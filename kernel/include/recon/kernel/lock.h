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
};

#define SPINLOCK_INIT(n) { 0, 0, (n) }

void spin_init(struct spinlock *l, const char *name);

void spin_lock(struct spinlock *l);
void spin_unlock(struct spinlock *l);

/* Masks interrupts, then takes the lock; returns the previous interrupt state,
 * which must be handed back to spin_unlock_irq(). Used for anything an
 * interrupt handler also touches -- the scheduler's run queue, chiefly. */
u64  spin_lock_irq(struct spinlock *l);
void spin_unlock_irq(struct spinlock *l, u64 flags);

bool lock_self_test(void);

#endif /* RECON_KERNEL_LOCK_H */
