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

/* --- the deadline --------------------------------------------------------- */

static void deadline_reached(void *arg)
{
	struct wait_deadline *d = arg;
	u64 flags;

	/* Under the caller's own lock, because the thing being changed is read
	 * in the same breath as the condition it competes with. Without it this
	 * is the lost wakeup with extra steps: the sleeper tests "not expired",
	 * this sets expired, and the sleeper then queues itself and waits for a
	 * wake that has already been delivered. */
	flags = spin_lock_irq(d->lock);

	d->expired_at = d->armed_for;

	/* All of them, not one. Several threads may be waiting on one device,
	 * and the deadline has passed for every one of them -- waking a single
	 * waiter would leave the rest asleep on a clock that has already run
	 * out and will not be restarted. */
	wait_wake_all(d->q);

	spin_unlock_irq(d->lock, flags);
}

bool wait_deadline_arm(struct wait_deadline *d, struct wait_queue *q,
		       struct spinlock *lock, u64 ns)
{
	if (!d || !q || !lock)
		return false;

	/* A new arming, which is what makes any callback still in flight from
	 * the previous one harmless: it will write a generation that is no
	 * longer the current one. */
	d->generation++;
	d->q = q;
	d->lock = lock;

	timer_init(&d->timer, deadline_reached, d);

	if (!timer_start(&d->timer, ns))
		return false;

	/* After the timer is filed, never before. Set first, it would name an
	 * arming that then failed to start -- and a caller looking at a
	 * deadline nothing is going to reach would wait for ever while
	 * believing it had a clock. */
	d->armed_for = d->generation;
	return true;
}

bool wait_deadline_passed(const struct wait_deadline *d)
{
	/* Not "did a callback run". A callback from the arming before this one
	 * ran out of time for a request that is already finished. */
	return d && d->armed_for == d->generation &&
	       d->expired_at == d->generation;
}

void wait_deadline_disarm(struct wait_deadline *d)
{
	if (!d)
		return;

	/* The result is deliberately ignored, and that is the whole point of
	 * the generation number. It says whether this call is what stopped the
	 * callback from running -- which matters enormously to a caller whose
	 * timer points at a stack frame, and not at all to one whose timer
	 * points at itself. */
	timer_cancel(&d->timer);

	/* And the arming is retired, so a callback that fires after this
	 * returns writes a number that is no longer current. */
	d->armed_for = d->generation + 1;
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

/* Names a mutex, so its guard shows up as something in the lock summary
 * rather than as a question mark.
 *
 * A mutex whose fields are all zero already works -- the guard is free, the
 * queue is empty and there is no owner -- which is why nothing needed this
 * before. What zero does not give it is a name, and the moment locks started
 * reporting what they cost, every mutex in the kernel became an unnamed row.
 *
 * Optional, and a mutex that is never named still works. */
void mutex_init(struct mutex *m, const char *name)
{
	spin_init(&m->guard, name);
	m->waiters.head = 0;
	m->waiters.tail = 0;
	m->owner = 0;
}

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

/* --- the deadline's own test ---------------------------------------------
 *
 * A deadline that never fires and a deadline that fires immediately both look
 * like "the call returned". So every assertion here is about *when*, measured
 * against a clock that does not depend on the thing being tested, or about
 * *which arming* -- never about the call having come back.
 */
static struct spinlock deadline_lock = SPINLOCK_INIT("deadline-test");
static struct wait_queue deadline_queue;
static struct wait_deadline deadline_under_test;

static void signal_the_queue(void *arg)
{
	u64 flags = spin_lock_irq(&deadline_lock);

	*(volatile bool *)arg = true;
	wait_wake_all(&deadline_queue);

	spin_unlock_irq(&deadline_lock, flags);
}

/* Sleeps until the flag is set or the deadline passes. Returns the elapsed
 * nanoseconds, and reports through `gave_up` which of the two ended it. */
static u64 sleep_until(volatile bool *flag, bool *gave_up)
{
	u64 started = time_monotonic_ns();
	u64 flags;
	bool slept = true;

	flags = spin_lock_irq(&deadline_lock);

	while (!*flag && !wait_deadline_passed(&deadline_under_test) && slept)
		slept = wait_sleep(&deadline_queue, &deadline_lock, flags);

	*gave_up = wait_deadline_passed(&deadline_under_test) && !*flag;

	spin_unlock_irq(&deadline_lock, flags);

	return time_monotonic_ns() - started;
}

static bool deadline_self_test(void)
{
	volatile bool flag = false;
	struct timer signaller;
	bool gave_up = false;
	bool ok = true;
	u64 took;

	/* --- it gives up, and not before it said it would ---------------- */
	if (!wait_deadline_arm(&deadline_under_test, &deadline_queue,
			       &deadline_lock, 100000000ull)) {	/* 100 ms */
		kputs("  wait: a 100ms deadline could not be armed\n");
		return false;
	}

	took = sleep_until(&flag, &gave_up);
	wait_deadline_disarm(&deadline_under_test);

	if (!gave_up) {
		kputs("  wait: a deadline nobody signalled did not run out\n");
		ok = false;
	}

	/* The half that matters. A deadline that expires the instant it is
	 * armed ends every wait correctly and waits for nothing -- which is a
	 * driver that reports every disk broken, and a test that only checked
	 * "it gave up" would call it a pass. */
	if (took < 90000000ull) {
		kprintf("  wait: a 100ms deadline ran out after %llu ns\n",
			(unsigned long long)took);
		ok = false;
	}

	/* --- an arming that ran out does not poison the next one --------- */
	if (!wait_deadline_arm(&deadline_under_test, &deadline_queue,
			       &deadline_lock, 2000000000ull)) {
		kputs("  wait: a 2s deadline could not be armed\n");
		return false;
	}

	if (wait_deadline_passed(&deadline_under_test)) {
		kputs("  wait: a freshly armed deadline says it has already "
		      "run out, so the arming before it leaked\n");
		ok = false;
	}

	/* --- and it does not give up when it is signalled ---------------- */
	flag = false;
	timer_init(&signaller, signal_the_queue, (void *)&flag);

	if (!timer_start(&signaller, 50000000ull)) {	/* 50 ms */
		kputs("  wait: the signaller could not be started\n");
		wait_deadline_disarm(&deadline_under_test);
		return false;
	}

	took = sleep_until(&flag, &gave_up);
	wait_deadline_disarm(&deadline_under_test);

	if (gave_up) {
		kputs("  wait: a signal arrived well inside the deadline and "
		      "the sleeper gave up anyway\n");
		ok = false;
	}

	/* Woken by the signal rather than by the clock, which is the same
	 * distinction again: had it slept the full two seconds and then
	 * noticed the flag, every assertion above would still hold. */
	if (took > 1000000000ull) {
		kprintf("  wait: a signal at 50ms was not acted on for "
			"%llu ns\n", (unsigned long long)took);
		ok = false;
	}

	return ok;
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

	/* --- and a sleep that gives up ----------------------------------- */
	if (!deadline_self_test())
		ok = false;

	return ok;
}
