#include <recon/kernel/work.h>
#include <recon/kernel/timer.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/wait.h>
#include <recon/kernel/time.h>
#include <recon/kernel/console.h>

static struct spinlock work_lock = SPINLOCK_INIT("work");
static struct wait_queue work_waiters;	/* the worker, waiting for something */
static struct wait_queue drain_waiters;	/* anybody waiting for the queue to empty */

static struct work *head;
static struct work *tail;

static struct thread *worker;
static u64 queued_total, ran_total, already_queued;

/* Bumped every time an item finishes. work_drain reads it and waits for it to
 * pass a mark, which is what makes "everything queued before this call" a
 * question with an answer -- as opposed to "the queue is empty right now",
 * which is true in the gap between two items and means nothing. */
static u64 completed;

void work_init(struct work *w, void (*fn)(void *), void *arg)
{
	w->next = 0;
	w->fn = fn;
	w->arg = arg;
	w->queued = false;
}

bool work_schedule(struct work *w)
{
	u64 flags;

	if (!w || !w->fn)
		return false;

	flags = spin_lock_irq(&work_lock);

	if (w->queued) {
		/* Already waiting its turn. Queueing it twice would put one
		 * item on the list twice, which is a loop in a singly linked
		 * list and a machine that stops -- and the caller almost never
		 * wants two runs anyway: it wants the work done after the most
		 * recent event, which the one already queued will do. */
		already_queued++;
		spin_unlock_irq(&work_lock, flags);
		return false;
	}

	w->next = 0;
	w->queued = true;

	if (tail)
		tail->next = w;
	else
		head = w;
	tail = w;

	queued_total++;

	/* The wake happens under the same lock the worker tests the queue
	 * under, which is what makes it impossible to lose: the worker cannot
	 * be between "found the queue empty" and "gone to sleep" without
	 * holding this lock. */
	wait_wake_one(&work_waiters);

	spin_unlock_irq(&work_lock, flags);
	return true;
}

static void worker_loop(void *arg)
{
	u64 flags = spin_lock_irq(&work_lock);

	for (;;) {
		struct work *w;

		while (!head)
			if (!wait_sleep(&work_waiters, &work_lock, flags))
				break;

		if (!head) {
			/* Could not sleep and there is nothing to do. Yield
			 * rather than spin: this should not happen -- the
			 * worker is an ordinary thread -- and if it does, a
			 * processor burning on an empty queue is worse than a
			 * loop that gives way. */
			spin_unlock_irq(&work_lock, flags);
			sched_yield();
			flags = spin_lock_irq(&work_lock);
			continue;
		}

		w = head;
		head = w->next;
		if (!head)
			tail = 0;

		w->next = 0;
		w->queued = false;

		/* Outside the lock, because the work is arbitrary code: it can
		 * take locks, sleep, and queue more work. Holding this one
		 * across it would make each of those a deadlock. */
		spin_unlock_irq(&work_lock, flags);
		w->fn(w->arg);
		flags = spin_lock_irq(&work_lock);

		ran_total++;
		completed++;
		wait_wake_all(&drain_waiters);
	}
}

void work_init_queue(void)
{
	worker = thread_create("kworker", worker_loop, 0);

	if (!worker)
		kputs("  work: no memory for the worker thread, so deferred "
		      "work will never run\n");
}

bool work_drain(void)
{
	u64 flags = spin_lock_irq(&work_lock);
	u64 mark = queued_total;
	bool ok = true;

	/* Everything queued *before this call* -- so the mark is a count of
	 * arrivals, not a state of the list. Waiting for the list to be empty
	 * would return in the instant between two items with work still to do,
	 * and would never return at all on a queue that keeps being fed. */
	while (completed < mark && ok)
		ok = wait_sleep(&drain_waiters, &work_lock, flags);

	spin_unlock_irq(&work_lock, flags);
	return ok && completed >= mark;
}

void work_print_summary(void)
{
	kprintf("\nDeferred work\n");
	kprintf("  queue        : %lu queued, %lu run", queued_total,
		ran_total);

	if (already_queued)
		kprintf(", %lu asked for while already queued", already_queued);

	kputs("\n");
}

/* --- the self-test --------------------------------------------------------
 *
 * The assertion that matters is not that the work runs. It is *where* it runs.
 *
 * A "deferred work" implementation that called the function immediately would
 * pass any test that only checked the function was called, and would be exactly
 * the thing this exists to replace. So each item records which thread ran it,
 * and the test requires that to be the worker and not the caller -- and the
 * items are queued from inside a timer callback, which is real interrupt
 * context and the situation this was built for.
 */
static struct work items[4];
static volatile unsigned ran_order[4];
static volatile unsigned ran_count;
static volatile u64 ran_on_thread[4];
static volatile u64 queued_from_thread;
static volatile bool queued_from_timer;

static void note_work(void *arg)
{
	unsigned me = (unsigned)(uintptr_t)arg;
	struct thread *t = sched_current();

	if (ran_count < 4) {
		ran_on_thread[ran_count] = t ? t->id : 0;
		ran_order[ran_count++] = me;
	}
}

static void queue_from_interrupt(void *arg)
{
	struct thread *t = sched_current();
	unsigned i;

	queued_from_thread = t ? t->id : 0;
	queued_from_timer = true;

	for (i = 0; i < 4; i++)
		work_schedule(&items[i]);
}

bool work_self_test(void)
{
	struct timer t;
	unsigned i;
	bool ok = true;

	if (!worker) {
		kputs("  work: there is no worker thread\n");
		return false;
	}

	ran_count = 0;
	queued_from_timer = false;

	for (i = 0; i < 4; i++)
		work_init(&items[i], note_work, (void *)(uintptr_t)i);

	/* Queued from a timer callback rather than from here, because that is
	 * interrupt context and it is the case this exists for. Queueing them
	 * directly would test the list and not the point. */
	timer_init(&t, queue_from_interrupt, 0);

	if (!timer_start(&t, 20000000ull)) {	/* 20 ms */
		kputs("  work: could not arrange to queue from an interrupt\n");
		return false;
	}

	{
		u64 deadline = time_monotonic_ns() + 3000000000ULL;

		while (!queued_from_timer && time_monotonic_ns() < deadline)
			sched_yield();
	}

	if (!queued_from_timer) {
		kputs("  work: the timer that was to queue the work never "
		      "fired\n");
		timer_cancel(&t);
		return false;
	}

	if (!work_drain()) {
		kputs("  work: could not wait for the queue to empty\n");
		return false;
	}

	if (ran_count != 4) {
		kprintf("  work: %u of 4 items ran\n", ran_count);
		return false;
	}

	/* In order. One worker running one item at a time is a guarantee this
	 * makes, and a caller may depend on it. */
	for (i = 0; i < 4; i++)
		if (ran_order[i] != i) {
			kprintf("  work: item %u ran %u%s\n", ran_order[i], i,
				"th rather than in the order it was queued");
			ok = false;
		}

	/* And the whole point: on the worker, not on whoever queued it. */
	for (i = 0; i < 4; i++) {
		if (ran_on_thread[i] != worker->id) {
			kprintf("  work: an item ran on thread %lu rather than "
				"the worker (%lu), so it was not deferred at "
				"all\n", ran_on_thread[i], worker->id);
			ok = false;
			break;
		}

		if (ran_on_thread[i] == queued_from_thread) {
			kputs("  work: an item ran on the thread that queued "
			      "it\n");
			ok = false;
			break;
		}
	}

	return ok;
}
