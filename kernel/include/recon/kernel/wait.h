/* A thread that can wait, and the three things built on one that can.
 *
 * This is the primitive several parts of the kernel have been doing without,
 * and doing without it has a name: every caller that must wait for something
 * spins, yielding, and burns a scheduling slice per poll. The block layer does
 * it waiting for a disk. The USB driver does it waiting for a transfer. The
 * scheduler's own self-test does it waiting for three threads to finish.
 *
 * It is also what four separate rows of the architecture checklist are waiting
 * on -- mutexes, semaphores, condition variables, deferred interrupt work and
 * software timers all need a thread to be able to stop and be woken, and none
 * of them can be built on a spin.
 *
 * --- The one thing that has to be right ---
 *
 * **The gap between deciding to wait and actually waiting.** A thread checks a
 * condition, finds it false, and goes to sleep. If the wake happens in between,
 * it is delivered to a thread that is not yet asleep, and then the thread
 * sleeps for ever waiting for something that already happened. That is the lost
 * wakeup, and it is the only hard part of this file.
 *
 * It is closed by making the caller hold a lock across both: the condition is
 * tested and the thread is queued under the same lock the waker must take to
 * signal. `wait_sleep` releases it as part of going to sleep, at the point
 * where the scheduler has already committed, and takes it again on the way
 * back. A caller that does not hold the lock has a race, and the interface is
 * shaped so that not holding it is awkward rather than possible.
 */
#ifndef RECON_KERNEL_WAIT_H
#define RECON_KERNEL_WAIT_H

#include <recon/kernel/lock.h>
#include <recon/kernel/timer.h>
#include <recon/kernel/types.h>

struct thread;

/* A list of threads waiting for one thing. Zeroed is empty and valid, so one
 * declared static needs no initialiser. */
struct wait_queue {
	struct thread *head;
	struct thread *tail;
};

/* Sleeps until somebody wakes this queue.
 *
 * `lock` must be held on entry and is held again on return. It is released
 * while the thread is actually asleep, which is what lets the waker take it --
 * and it is released *after* the thread is queued, which is what makes the
 * wakeup impossible to lose.
 *
 * `flags` is the interrupt state spin_lock_irq returned, and is what the lock
 * is re-taken with.
 *
 * Returns false if it could not sleep -- there is no current thread, or the
 * caller is the idle thread, which must never block because there is nothing
 * else for its processor to run. A caller that ignores that has a processor
 * that has stopped. */
bool wait_sleep(struct wait_queue *q, struct spinlock *lock, u64 flags);

/* --- a sleep that gives up -------------------------------------------------
 *
 * `wait_sleep` waits for ever, and for a thread waiting on another thread that
 * is the right answer: if the signal never comes, the program is wrong and
 * hanging is the honest outcome. For a thread waiting on **hardware** it is the
 * wrong answer. A disk that never answers is not a bug in this kernel, it is a
 * disk, and a driver that waits for ever on one turns a broken device into a
 * machine that stops with nothing on the screen.
 *
 * So every such caller has polled instead -- burning a slice per look, and
 * unable to have more than one request outstanding, because the only way it
 * knows a request finished is that it went and looked.
 *
 * --- Why the caller owns this and it is not a parameter ---
 *
 * The obvious interface is `wait_sleep_timeout(q, lock, flags, ns)`, with the
 * timer on the stack. `timer_sleep_ns` does exactly that and is correct,
 * because it drops its lock **and restores interrupts** before waiting out a
 * callback that lost the cancel race.
 *
 * A deadline sleep cannot. It has to return the way `wait_sleep` does -- lock
 * held, interrupts still off -- so if the tick belongs to this processor, a
 * spin waiting for that callback is waiting for something that cannot run. It
 * would be a deadlock that appears only when the timer fires in the same
 * instant as the wakeup, on the processor that owns the tick.
 *
 * Putting the timer in the caller's own structure removes the question rather
 * than answering it. A callback that fires late points at something that is
 * still there, so nothing has to be waited for -- and this kernel has already
 * paid once for a pointer to a stack frame that had gone, in the lock registry.
 *
 * --- And why there is a generation number ---
 *
 * `timer_cancel` can lose, which means a disarmed deadline may still fire. If
 * "expired" were a flag, that late callback would set it on a structure whose
 * next request has already started, and that request would time out
 * immediately -- leaking the descriptors its device still owns, for a timeout
 * that belongs to the request before it.
 *
 * So the callback records *which* arming it belongs to, and only the current
 * one counts. A stale one writes a number nobody is looking at.
 */
struct wait_deadline {
	struct timer timer;

	struct wait_queue *q;
	struct spinlock *lock;

	u32 generation;		/* arming n */
	u32 armed_for;		/* which arming the live timer belongs to */
	u32 expired_at;		/* which arming last ran out of time */
};

/* Starts the clock. `q` and `lock` are the caller's own, and the callback takes
 * that lock -- so it must be the lock the condition is tested under, or the
 * wakeup races the sleep exactly as it would without one.
 *
 * Must not be called with `lock` held: the arming is not the waiting, and a
 * timer filed under the lock it will later want is a needless way to be woken
 * by your own deadline while you hold it.
 *
 * False if the delay is beyond the timer wheel's reach, in which case nothing
 * is armed and the caller has no deadline -- which is worth handling rather
 * than ignoring, since a caller that ignores it waits for ever. */
bool wait_deadline_arm(struct wait_deadline *d, struct wait_queue *q,
		       struct spinlock *lock, u64 ns);

/* Whether *this* arming has run out. Safe to read under the caller's lock,
 * which is where it should be read: it is set by the callback holding that
 * lock, and tested in the same loop as the condition. */
bool wait_deadline_passed(const struct wait_deadline *d);

/* Stops the clock. Cheap, and safe whether or not the timer has fired -- there
 * is deliberately nothing to wait for. Must be called with `lock` not held,
 * for the same reason as arming. */
void wait_deadline_disarm(struct wait_deadline *d);

/* Wakes one waiter, or all of them. Safe to call with none waiting, which is
 * the common case and not an error: a signal with no waiter is a signal
 * nobody needed. Callers hold the same lock they tested the condition under. */
void wait_wake_one(struct wait_queue *q);
void wait_wake_all(struct wait_queue *q);

/* How many threads are on it, for summaries and tests. */
unsigned wait_queue_length(const struct wait_queue *q);

/* --- What is built on it -------------------------------------------------
 *
 * Each is small, and each exists because spinning is the wrong answer for it.
 */

/* A lock that sleeps rather than spins.
 *
 * A spinlock is right when the holder will release it in less time than a
 * context switch costs -- which is true of the ones this kernel already has,
 * all of which guard a few lines. It is wrong the moment the holder might wait
 * for hardware, because then every other processor burns its slice watching.
 */
struct mutex {
	struct spinlock guard;		/* protects the two fields below */
	struct wait_queue waiters;
	struct thread *owner;		/* null when free */
};

/* Optional: names the guard so it appears in the lock summary as something
 * other than a question mark. A zeroed mutex already works without it. */
void mutex_init(struct mutex *m, const char *name);

void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_held(const struct mutex *m);

/* A count, and threads that wait for it to be positive. The general form: a
 * mutex is one of these with a count of one and an owner, and a completion is
 * one that starts at zero. */
struct semaphore {
	struct spinlock guard;
	struct wait_queue waiters;
	unsigned count;
};

void semaphore_init(struct semaphore *s, unsigned count);
void semaphore_take(struct semaphore *s);
void semaphore_give(struct semaphore *s);

/* "Wait until something is true", where the thing is not a count.
 *
 * The condition belongs to the caller and is tested by the caller, under the
 * caller's own lock, in a loop -- because a woken thread is not a thread whose
 * condition is now true. It is a thread that has been told to look again. Any
 * interface that promises otherwise is one that breaks when two threads are
 * woken for one event.
 */
struct condition {
	struct wait_queue waiters;
};

void condition_wait(struct condition *c, struct spinlock *lock, u64 flags);
void condition_signal(struct condition *c);
void condition_broadcast(struct condition *c);

bool wait_self_test(void);

#endif /* RECON_KERNEL_WAIT_H */
