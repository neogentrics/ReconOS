/* Spinlocks.
 *
 * Written before the second processor exists, deliberately. A run queue that
 * two processors can edit at once is a run queue that will eventually contain a
 * cycle, and the moment to make that safe is before there is a second processor
 * to prove it — retrofitting locks onto a working single-processor kernel means
 * finding every place that was safe only by accident.
 *
 * --- Why spinning, and why that is not the whole answer ---
 *
 * A spinlock does the simplest possible thing: loop until the lock is free. It
 * is correct, it is small, and it is the right primitive for a critical section
 * measured in tens of instructions -- which is what these guard.
 *
 * It is the *wrong* primitive for anything longer, because a spinning CPU is a
 * CPU doing nothing at full power, which is precisely the behaviour this
 * project argues against. Nothing here may hold a lock across anything slow.
 * When something needs to wait for a disk or a key, that wants a lock that
 * sleeps, and that needs the scheduler to have somewhere to put a waiting
 * thread -- which it does not yet.
 *
 * --- Interrupts ---
 *
 * A lock taken by ordinary code and also by an interrupt handler on the same
 * processor deadlocks: the handler spins for a lock the code it interrupted is
 * holding, and that code cannot continue until the handler returns. So the
 * interrupt-safe form masks interrupts while held, and the two forms are
 * separate functions rather than a flag, because getting it wrong produces a
 * machine that stops with no message.
 */
#ifndef RECON_KERNEL_LOCK_H
#define RECON_KERNEL_LOCK_H

#include <recon/kernel/types.h>

struct spinlock {
	/* Deliberately not a bool. `__atomic_test_and_set` is defined on a byte
	 * and works on every architecture without an assembly helper, which
	 * keeps this file portable -- and the built-in emits the right
	 * instruction on both: `lock xchg` on x86_64, `ldaxrb`/`stlxrb` on
	 * aarch64. */
	volatile unsigned char held;

	/* Which processor holds it, for reporting a deadlock rather than
	 * hanging. Meaningless when `held` is zero. */
	volatile unsigned owner;

	const char *name;

	/* --- what this lock has cost -------------------------------
	 *
	 * Written only by whoever holds the lock, after acquiring it, so
	 * they need no atomics: the holder is the only writer by
	 * definition. Read without one during the summary, which is a
	 * torn read at worst and a report, not a decision.
	 *
	 * On the same cache line as `owner`, which every acquisition
	 * already writes -- see the long note further down. That is what
	 * makes counting free rather than making the measurement into
	 * the thing being measured. */
	u64 taken;
	u64 contended;
	u32 worst_spins;

	/* Set once, by the first acquisition, to put this lock in the
	 * registry. A byte and a test-and-set rather than a bool and a
	 * check, because two processors can reach a lock for the first
	 * time together and the loser must not add it twice. */
	volatile unsigned char registered;
};

/* Designated rather than positional, so that adding a field above does not
 * turn every static lock in the kernel into a build error -- which is what
 * `{ 0, 0, (n) }` did the first time the counters were added. Everything not
 * named here is zero, which is what each of them should start as. */
#define SPINLOCK_INIT(n) { .name = (n) }

void spin_init(struct spinlock *l, const char *name);

void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

/* Masks interrupts, then takes the lock; returns the previous interrupt state,
 * which must be handed back to spin_unlock_irq(). Used for anything an
 * interrupt handler also touches -- the scheduler's run queue, chiefly. */
u64  spin_lock_irq(struct spinlock *l);
void spin_unlock_irq(struct spinlock *l, u64 flags);

/* --- What each lock has cost --------------------------------------------
 *
 * The question this answers is the one asked of every kernel eventually: *would
 * more processors help?* A machine with eight cores that spends its time
 * waiting on one lock runs at the speed of that lock, and nothing about the
 * other seven is visible from the outside -- the machine simply does not get
 * faster, and there is no message.
 *
 * Three numbers per lock. Taken, contended, and the worst wait seen:
 *
 *   - **taken** is lock traffic. A lock taken twice a second is not a
 *     bottleneck however contended it is;
 *   - **contended** is the count of acquisitions that had to spin at all. This
 *     is the number that says whether a lock is a wall;
 *   - **worst** is the longest spin any single acquisition suffered. A lock
 *     contended one time in a thousand but for a million spins is a different
 *     problem from one contended constantly but briefly, and an average would
 *     hide both.
 *
 * --- Why measuring this does not change the answer ---
 *
 * The trap in lock profiling is that **the instrument is made of the thing it
 * measures**. A counter shared by every processor is a cache line every
 * processor writes, which is exactly the traffic a contended lock produces --
 * so a naive lock profiler reports contention it created, and reports most on
 * the locks that were fine.
 *
 * This avoids it by not adding any sharing that is not there already:
 *
 *   - the counters live *in the lock*, on the cache line the lock itself is on.
 *     That line is already written by every acquirer -- `owner` is set on every
 *     acquisition -- so the line is already being taken exclusively by whoever
 *     took the lock, and a second write to it costs nothing extra;
 *   - only the holder writes them, after acquiring, so they need no atomics of
 *     their own;
 *   - `contended` and `worst` are written only when there *was* contention,
 *     which is a path that has already spent thousands of cycles spinning.
 *
 * The remaining cost is that `struct spinlock` is bigger, which changes which
 * locks share a cache line with each other and with the data they protect. Two
 * unrelated locks on one line contend for the line even when neither is
 * contended -- false sharing, and it looks exactly like real contention from
 * inside. Worth knowing before believing a surprising result.
 *
 * There is no way to switch this off. It is always on, for the same reason the
 * safety checks in this kernel are: a measurement that can be disabled is a
 * measurement somebody disabled, and the numbers then describe a kernel nobody
 * is running.
 */

/* Every lock that has ever been taken, so there is something to report.
 *
 * Filled in by the locks themselves, the first time each is used -- there is no
 * list to keep up to date and no way to add a lock and forget to register it.
 * A lock that has never been taken does not appear, which is itself worth
 * seeing. */
#define LOCK_REGISTRY_MAX 64

unsigned lock_registry_count(void);
const struct spinlock *lock_registry_at(unsigned i);

void lock_print_summary(void);

bool lock_self_test(void);

#endif /* RECON_KERNEL_LOCK_H */
