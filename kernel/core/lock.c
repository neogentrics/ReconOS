#include <recon/kernel/lock.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>
#include <recon/kernel/panic.h>

/* How many times to spin before deciding this is not contention but a
 * deadlock. A critical section here is tens of instructions; ten million spins
 * is many milliseconds, which no correct caller will ever reach.
 *
 * Reporting rather than hanging matters more than the exact number. A kernel
 * that stops with no message is the least informative failure there is, and a
 * deadlock is otherwise exactly that. */
#define SPIN_LIMIT 10000000u

/* --- the registry ----------------------------------------------------
 *
 * A lock adds itself the first time it is taken. There is no list anybody
 * has to keep up to date, so a lock added to this kernel next year appears
 * in the summary without anybody remembering to put it there -- and a lock
 * that never appears is a lock nothing ever took, which is a fact worth
 * seeing rather than an omission.
 *
 * The index moves with an atomic add and the claim on the lock is a
 * test-and-set, because two processors reaching the same never-before-used
 * lock at the same moment is exactly the situation a lock exists for.
 */
static const struct spinlock *registry[LOCK_REGISTRY_MAX];
static volatile unsigned registry_used;
static volatile unsigned registry_overflow;

static void register_lock(struct spinlock *l)
{
	unsigned at;

	/* The common case, by an enormous margin: already there. Read
	 * without an atomic, because once it is set it never changes
	 * back, so the only wrong answer this can give is "not yet" on a
	 * lock that is being registered right now -- and the test-and-set
	 * below settles that. */
	if (l->registered)
		return;

	if (__atomic_test_and_set(&l->registered, __ATOMIC_ACQ_REL))
		return;	/* somebody else got there first */

	at = __atomic_fetch_add(&registry_used, 1, __ATOMIC_RELAXED);

	if (at >= LOCK_REGISTRY_MAX) {
		/* Counted, not dropped silently. A summary missing a lock
		 * would be read as a lock that is never taken, which is the
		 * opposite of the truth about the one that arrived last. */
		__atomic_fetch_add(&registry_overflow, 1, __ATOMIC_RELAXED);
		return;
	}

	registry[at] = l;
}

unsigned lock_registry_count(void)
{
	unsigned n = registry_used;

	return (n > LOCK_REGISTRY_MAX) ? LOCK_REGISTRY_MAX : n;
}

const struct spinlock *lock_registry_at(unsigned i)
{
	return (i < lock_registry_count()) ? registry[i] : 0;
}

void spin_init(struct spinlock *l, const char *name)
{
	l->held = 0;
	l->owner = 0;
	l->name = name;
	l->taken = 0;
	l->contended = 0;
	l->worst_spins = 0;

	/* Marked as already registered, which means *never* registered:
	 * register_lock finds the claim taken and leaves it alone.
	 *
	 * The registry holds pointers and the summary reads through them
	 * long afterwards, so a lock it points at must outlive the kernel
	 * that reports on it. A lock initialised at runtime is usually one
	 * on a stack or inside a structure that comes and goes -- and the
	 * first version of this file learned that the hard way: the lock
	 * self-test's own stack locks registered themselves, the frame was
	 * reused, and printing the summary faulted on a name pointer
	 * reading 0x282.
	 *
	 * A static lock never comes here, because SPINLOCK_INIT leaves this
	 * zero and the first acquisition claims it. So the rule is exactly
	 * "the registry holds locks that live as long as the kernel", and
	 * it is enforced rather than described. */
	l->registered = 1;
}

void spin_lock(struct spinlock *l)
{
	unsigned spins = 0;

	/* Acquire ordering: nothing the caller does inside the critical section
	 * may be reordered to before the lock is taken. The built-in emits
	 * whatever that costs on this architecture -- nothing on x86_64, a
	 * barrier on aarch64 -- which is exactly the kind of difference that
	 * belongs in the compiler rather than in this file. */
	while (__atomic_test_and_set(&l->held, __ATOMIC_ACQUIRE)) {
		if (++spins > SPIN_LIMIT)
			panic("lock: deadlock");

		/* Tell the processor this is a spin loop. On x86 it stops the
		 * pipeline speculating its way through the loop and cuts the
		 * power the wait costs; on ARM it yields to the other thread of
		 * a multithreaded core. Without it a spinning CPU burns as much
		 * energy as a working one. */
		arch_cpu_relax();
	}

	l->owner = arch_cpu_id();

	/* Held now, so this processor is the only writer of everything
	 * below and no atomics are needed for any of it. */
	l->taken++;

	if (spins) {
		l->contended++;

		if (spins > l->worst_spins)
			l->worst_spins = spins;
	}

	register_lock(l);
}

void spin_unlock(struct spinlock *l)
{
	l->owner = 0;

	/* Release ordering: everything the caller did inside the critical
	 * section must be visible to the next holder before the lock appears
	 * free. Getting this wrong produces a lock that works and data that
	 * arrives late, which is far harder to find than a lock that does not
	 * work. */
	__atomic_clear(&l->held, __ATOMIC_RELEASE);
}

u64 spin_lock_irq(struct spinlock *l)
{
	u64 flags = arch_irq_save();

	spin_lock(l);
	return flags;
}

void spin_unlock_irq(struct spinlock *l, u64 flags)
{
	spin_unlock(l);
	arch_irq_restore(flags);
}


/* See the note in lock.h about why these numbers are worth having and why
 * collecting them does not change them. */
void lock_print_summary(void)
{
	unsigned n = lock_registry_count();
	unsigned i;
	u64 total = 0, total_contended = 0;

	kprintf("\nLocks\n");

	if (!n) {
		kputs("  none taken yet\n");
		return;
	}

	for (i = 0; i < n; i++) {
		const struct spinlock *l = lock_registry_at(i);

		if (!l)
			continue;

		total += l->taken;
		total_contended += l->contended;
	}

	kprintf("  %u locks, %llu acquisitions, %llu contended",
		n, (unsigned long long)total,
		(unsigned long long)total_contended);

	if (total)
		kprintf(" (%llu%%)",
			(unsigned long long)((total_contended * 100) / total));
	kprintf("\n");

	/* Only the locks that were ever waited for. A list of fourteen locks
	 * that were all uncontended is a page of zeroes somebody has to read
	 * past to find the one that was not -- and on a machine with one
	 * processor, that is every boot. */
	for (i = 0; i < n; i++) {
		const struct spinlock *l = lock_registry_at(i);

		if (!l || !l->contended)
			continue;

		kprintf("  %-10s %llu taken, %llu waited",
			l->name ? l->name : "?",
			(unsigned long long)l->taken,
			(unsigned long long)l->contended);

		if (l->taken)
			kprintf(" (%llu%%)",
				(unsigned long long)((l->contended * 100) / l->taken));

		kprintf(", worst %u spins\n", l->worst_spins);
	}

	if (registry_overflow)
		kprintf("  and %u more that did not fit in the registry\n",
			registry_overflow);
}

/* A lock that exists only to be measured, and that lives as long as the
 * kernel does -- which is what makes it safe to put in the registry. */
static struct spinlock probe = SPINLOCK_INIT("test-probe");

bool lock_self_test(void)
{
	struct spinlock l;
	bool ok = true;

	spin_init(&l, "test");

	if (l.held) {
		kputs("  lock: a freshly initialised lock was already held\n");
		return false;
	}

	spin_lock(&l);
	if (!l.held) {
		kputs("  lock: taking a lock did not mark it held\n");
		ok = false;
	}

	spin_unlock(&l);
	if (l.held) {
		kputs("  lock: releasing a lock did not mark it free\n");
		ok = false;
	}

	/* The interrupt-safe form has to actually mask interrupts, and has to
	 * put them back exactly as it found them -- including leaving them
	 * masked if they were already masked, which is the case that a naive
	 * "disable then enable" gets wrong and that only shows up when one
	 * interrupt-safe lock is taken inside another. */
	{
		u64 flags;
		bool was_enabled = arch_irqs_enabled();

		flags = spin_lock_irq(&l);
		if (arch_irqs_enabled()) {
			kputs("  lock: the interrupt-safe form did not mask interrupts\n");
			ok = false;
		}

		/* Nested, to prove the restore is by saved state and not by
		 * unconditionally re-enabling. */
		{
			u64 inner;
			struct spinlock l2;

			spin_init(&l2, "test-inner");
			inner = spin_lock_irq(&l2);
			spin_unlock_irq(&l2, inner);

			if (arch_irqs_enabled()) {
				kputs("  lock: releasing an inner lock re-enabled "
				      "interrupts the outer one had masked\n");
				ok = false;
			}
		}

		spin_unlock_irq(&l, flags);
		if (arch_irqs_enabled() != was_enabled) {
			kputs("  lock: interrupts were not restored to how they "
			      "were found\n");
			ok = false;
		}
	}

	/* --- the counters -------------------------------------------
	 *
	 * Measured on `probe`, which is static. That matters: a lock on
	 * this stack would be gone by the time anything read it, and the
	 * last check below is the one that makes sure it never gets into
	 * the registry to be read.
	 *
	 * The contention check is the one that carries weight. A counter
	 * incremented on every acquisition rather than only on a spin
	 * would still produce a plausible summary -- every lock heavily
	 * contended -- and that is an answer somebody would act on.
	 *
	 * What is **not** covered: a genuinely contended lock. Producing
	 * one on demand needs two processors racing on a schedule, which
	 * is what the page allocator's concurrency test builds and what
	 * this would have to borrow. Said plainly rather than left as a
	 * gap somebody finds later: the spin path is not tested here. */
	{
		unsigned i;
		u64 before = probe.taken;
		bool found = false;

		for (i = 0; i < 100; i++) {
			spin_lock(&probe);
			spin_unlock(&probe);
		}

		if (probe.taken != before + 100) {
			kprintf("  lock: a lock taken 100 times counted "
			        "%llu\n",
				(unsigned long long)(probe.taken - before));
			ok = false;
		}

		if (probe.contended) {
			kprintf("  lock: a lock nothing was competing for "
			        "reported %llu waits, so contention is being "
			        "counted on every acquisition\n",
				(unsigned long long)probe.contended);
			ok = false;
		}

		if (probe.worst_spins) {
			kputs("  lock: an uncontended lock recorded a spin\n");
			ok = false;
		}

		/* It reached the registry, or none of the above is ever
		 * reported. A counter nothing reads is not instrumentation. */
		for (i = 0; i < lock_registry_count(); i++)
			if (lock_registry_at(i) == &probe)
				found = true;

		if (!found) {
			kputs("  lock: a static lock taken 100 times is not in "
			      "the registry, so nothing will ever report it\n");
			ok = false;
		}

		/* And the other direction, which is the check this file did
		 * not have the first time and needed: a lock on the stack
		 * must **not** be in the registry. The registry outlives
		 * every stack frame in this kernel, so a pointer to one is a
		 * fault waiting for the next summary -- which is exactly how
		 * this went wrong, reading a name pointer at 0x282. */
		{
			struct spinlock transient;

			spin_init(&transient, "test-transient");
			spin_lock(&transient);
			spin_unlock(&transient);

			for (i = 0; i < lock_registry_count(); i++) {
				if (lock_registry_at(i) != &transient)
					continue;

				kputs("  lock: a lock on the stack got into the "
				      "registry, which will fault the next time "
				      "anything reads it\n");
				ok = false;
				break;
			}
		}
	}

	return ok;
}
