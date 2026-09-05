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

void spin_init(struct spinlock *l, const char *name)
{
	l->held = 0;
	l->owner = 0;
	l->name = name;
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

	return ok;
}
