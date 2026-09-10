/* Waiting, and the three things built on it. See wait.h for why it exists and
 * what the hard part is.
 *
 * The hard part, restated where the code is: between a thread deciding to wait
 * and actually being asleep there is a window, and a wake that lands in it is
 * lost. This closes it by ordering two things exactly:
 *
 *   the thread is put on the queue *while still holding the lock the waker
 *   must take*, so a waker cannot be running concurrently with the decision;
 *
 *   and interrupts stay off from that moment until the context switch, so
 *   nothing on this processor can intervene either.
 *
 * The lock is released between those two, which is safe: by then the thread is
 * queued and marked, so a waker that takes the lock immediately finds it and
 * marks it runnable. The thread then switches away as a runnable thread, which
 * costs one extra switch and loses nothing.
 */
#include <recon/kernel/wait.h>

#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/time.h>

static void enqueue(struct wait_queue *q, struct thread *t)
{
	t->wait_next = NULL;

	if (q->tail)
		q->tail->wait_next = t;
	else
		q->head = t;

	q->tail = t;
}

static struct thread *dequeue(struct wait_queue *q)
{
	struct thread *t = q->head;

	if (!t)
		return NULL;

	q->head = t->wait_next;
	if (!q->head)
		q->tail = NULL;

	t->wait_next = NULL;
	return t;
}

bool wait_sleep(struct wait_queue *q, struct spinlock *lock, u64 flags)
{
	struct thread *t = sched_current();

	if (!t)
		return false;

	/* An idle thread must never block. It is what its processor runs when
	 * there is nothing else, so a blocked one is a processor that has
	 * stopped -- and it would stop while holding nothing and waiting for
	 * something no one is going to signal. */
	if (t->idle_for >= 0)
		return false;

	enqueue(q, t);
	t->state = THREAD_BLOCKED;

	/* The lock goes, the interrupt state does not. Releasing with
	 * spin_unlock_irq here would let this processor take a tick between
	 * the release and the switch -- harmless in itself, and one more thing
	 * to reason about for no gain. The caller's flags are restored at the
	 * bottom, after the lock is taken again. */
	spin_unlock(lock);

	sched_switch();

	/* Woken, and running again. Interrupts are still off -- they were off
	 * on the way in and the switch preserved that -- so the lock is taken
	 * without touching them, which leaves exactly the state the caller had
	 * before: lock held, interrupts masked, `flags` still the value to
	 * restore with.
	 *
	 * The condition is *not* known to be true. A woken thread is a thread
	 * that has been told to look again, and the caller loops. */
	spin_lock(lock);
	(void)flags;

	return true;
}

/* Takes a thread off the queue and makes it runnable.
 *
 * Not a context switch: the woken thread runs when the scheduler next picks it,
 * which may be on another processor and may be immediately. Switching to it
 * here would mean a waker that holds a lock handing the processor to a thread
 * that wants the same lock. */
static void wake(struct thread *t)
{
	if (!t)
		return;

	if (t->state == THREAD_BLOCKED)
		t->state = THREAD_READY;
}

void wait_wake_one(struct wait_queue *q)
{
	wake(dequeue(q));
}

void wait_wake_all(struct wait_queue *q)
{
	struct thread *t;

	while ((t = dequeue(q)) != NULL)
		wake(t);
}

unsigned wait_queue_length(const struct wait_queue *q)
{
	const struct thread *t = q->head;
	unsigned n = 0;

	while (t) {
		n++;
		t = t->wait_next;
	}

	return n;
}

/* --- a lock that sleeps --------------------------------------------------- */

void mutex_lock(struct mutex *m)
{
	u64 flags = spin_lock_irq(&m->guard);

	while (m->owner) {
		if (!wait_sleep(&m->waiters, &m->guard, flags)) {
			/* Nothing that can sleep is running -- early boot, or
			 * an idle thread. Spinning is the only thing left, and
			 * it is what the caller would have had to do anyway. */
			spin_unlock_irq(&m->guard, flags);
			arch_cpu_relax();
			flags = spin_lock_irq(&m->guard);
		}
	}

	m->owner = sched_current();
	spin_unlock_irq(&m->guard, flags);
}

void mutex_unlock(struct mutex *m)
{
	u64 flags = spin_lock_irq(&m->guard);

	m->owner = NULL;

	/* One, not all. Waking every waiter for a lock only one can hold is the
	 * thundering herd: they all run, all but one find it taken, and all but
	 * one go back to sleep having achieved nothing but a context switch. */
	wait_wake_one(&m->waiters);

	spin_unlock_irq(&m->guard, flags);
}

bool mutex_held(const struct mutex *m)
{
	return m->owner != NULL;
}

/* --- a count ------------------------------------------------------------- */

void semaphore_init(struct semaphore *s, unsigned count)
{
	s->guard.held = 0;
	s->waiters.head = NULL;
	s->waiters.tail = NULL;
	s->count = count;
}

void semaphore_take(struct semaphore *s)
{
	u64 flags = spin_lock_irq(&s->guard);

	while (s->count == 0) {
		if (!wait_sleep(&s->waiters, &s->guard, flags)) {
			spin_unlock_irq(&s->guard, flags);
			arch_cpu_relax();
			flags = spin_lock_irq(&s->guard);
		}
	}

	s->count--;
	spin_unlock_irq(&s->guard, flags);
}

void semaphore_give(struct semaphore *s)
{
	u64 flags = spin_lock_irq(&s->guard);

	s->count++;
	wait_wake_one(&s->waiters);

	spin_unlock_irq(&s->guard, flags);
}

/* --- wait until something is true ---------------------------------------- */

void condition_wait(struct condition *c, struct spinlock *lock, u64 flags)
{
	if (!wait_sleep(&c->waiters, lock, flags)) {
		/* Cannot sleep here. Release and retake so that whoever would
		 * signal has a chance to run and the caller's loop can test
		 * again -- which is the same shape as sleeping, more
		 * expensively. */
		spin_unlock_irq(lock, flags);
		arch_cpu_relax();
		spin_lock_irq(lock);
	}
}

void condition_signal(struct condition *c)
{
	wait_wake_one(&c->waiters);
}

void condition_broadcast(struct condition *c)
{
	wait_wake_all(&c->waiters);
}

/* --- the self-test --------------------------------------------------------
 *
 * The thing worth checking is not that a thread can sleep. It is that a thread
 * that sleeps is *actually descheduled*, and that a wake reaches it.
 *
 * Which means the test has to prove the sleep happened. Three threads take from
 * a semaphore that starts at zero, and if the three gives arrived first the
 * count would already be three and every take would succeed **without ever
 * sleeping** -- a test that passes while exercising none of this. So it waits
 * until all three are on the queue, asserts that they are, and only then gives.
 *
 * The second round is the opposite order on purpose: the gives arrive before
 * the takes. That is where a lost wakeup hides, and it must also work.
 */
static struct semaphore test_sem;
static volatile unsigned test_woke;

static void waiter_thread(void *arg)
{
	(void)arg;

	semaphore_take(&test_sem);
	__atomic_add_fetch(&test_woke, 1, __ATOMIC_RELEASE);
}

static bool wait_for(volatile unsigned *counter, unsigned target, u64 ns)
{
	u64 deadline = time_monotonic_ns() + ns;

	while (__atomic_load_n(counter, __ATOMIC_ACQUIRE) < target &&
	       time_monotonic_ns() < deadline)
		sched_yield();

	return __atomic_load_n(counter, __ATOMIC_ACQUIRE) >= target;
}

bool wait_self_test(void)
{
	unsigned i, queued = 0;
	u64 deadline;
	bool ok = true;

	/* --- round one: they sleep first, and are proved to have slept --- */

	semaphore_init(&test_sem, 0);
	test_woke = 0;

	for (i = 0; i < 3; i++)
		if (!thread_create("waiter", waiter_thread, 0)) {
			kputs("  wait: could not create a waiting thread\n");
			return false;
		}

	deadline = time_monotonic_ns() + 3000000000ULL;

	while (time_monotonic_ns() < deadline) {
		u64 flags = spin_lock_irq(&test_sem.guard);

		queued = wait_queue_length(&test_sem.waiters);
		spin_unlock_irq(&test_sem.guard, flags);

		if (queued == 3)
			break;

		sched_yield();
	}

	/* The assertion that makes the rest mean anything. Without it, three
	 * threads that never blocked would pass everything below. */
	if (queued != 3) {
		kprintf("  wait: %u of 3 threads actually blocked, so nothing "
			"was tested\n", queued);
		return false;
	}

	for (i = 0; i < 3; i++)
		semaphore_give(&test_sem);

	if (!wait_for(&test_woke, 3, 3000000000ULL)) {
		kprintf("  wait: %u of 3 sleeping threads woke\n",
			__atomic_load_n(&test_woke, __ATOMIC_ACQUIRE));
		ok = false;
	}

	/* --- round two: the gives arrive first --------------------------- */

	semaphore_init(&test_sem, 0);
	test_woke = 0;

	for (i = 0; i < 3; i++)
		semaphore_give(&test_sem);

	for (i = 0; i < 3; i++)
		if (!thread_create("waiter", waiter_thread, 0)) {
			kputs("  wait: could not create a waiting thread\n");
			return false;
		}

	if (!wait_for(&test_woke, 3, 3000000000ULL)) {
		kprintf("  wait: %u of 3 threads got through a semaphore that "
			"was already open\n",
			__atomic_load_n(&test_woke, __ATOMIC_ACQUIRE));
		ok = false;
	}

	if (test_sem.count != 0) {
		kprintf("  wait: the count ended at %u, not 0\n",
			test_sem.count);
		ok = false;
	}

	return ok;
}
