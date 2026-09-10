#include <recon/kernel/timer.h>
#include <recon/kernel/time.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/wait.h>
#include <recon/kernel/console.h>
#include <recon/kernel/arch.h>

/* Five wheels of 64 slots. The numbers are not arbitrary: 64 is a power of two,
 * so the slot for a tick is a mask rather than a division, and five levels is
 * where reach stops being the limiting factor -- four would give under two
 * days, which is short enough that something would eventually want more. */
#define WHEEL_BITS	6
#define WHEEL_SIZE	(1u << WHEEL_BITS)
#define WHEEL_MASK	(WHEEL_SIZE - 1)
#define WHEEL_LEVELS	5

/* The furthest ahead a timer can be filed: one full turn of the top wheel. */
#define WHEEL_REACH	(1ull << (WHEEL_BITS * WHEEL_LEVELS))

static struct timer *slots[WHEEL_LEVELS][WHEEL_SIZE];
static struct spinlock timer_lock = SPINLOCK_INIT("timer");

/* How far the wheel has been turned, in ticks. Not the same as time_ticks():
 * this one moves only when the wheel has actually processed a tick, so a tick
 * that arrives while the wheel is behind is caught up rather than skipped. */
static u64 wheel_now;

static u64 started, fired, cancelled, refused;

/* --- the lists ------------------------------------------------------------
 *
 * Singly linked, with a pointer to whatever points at this entry -- which is
 * what makes removal O(1) without a back pointer per node. A non-null `pprev`
 * is also the definition of "on a list", so there is one fact here rather than
 * two that can disagree with each other.
 */
static void link_at(struct timer **head, struct timer *t)
{
	t->next = *head;
	if (t->next)
		t->next->pprev = &t->next;
	*head = t;
	t->pprev = head;
}

static void unlink_from(struct timer *t)
{
	if (!t->pprev)
		return;

	*t->pprev = t->next;
	if (t->next)
		t->next->pprev = t->pprev;

	t->next = 0;
	t->pprev = 0;
}

/* --- filing ---------------------------------------------------------------
 *
 * Which wheel a timer belongs on is decided by how far away it is, and which
 * slot by its expiry at that wheel's granularity. A timer 70 ticks out does not
 * fit level 0's 64 slots, so it goes on level 1, whose slots are 64 ticks wide
 * -- and it is cascaded down into level 0 when level 1's hand reaches it, by
 * which time it is less than 64 ticks away.
 *
 * The caller holds the lock.
 */
static void place(struct timer *t)
{
	u64 delta = t->expires - wheel_now;
	unsigned level, index;

	if ((i64)delta <= 0) {
		/* Already due, or overdue because the wheel fell behind. It
		 * goes in the slot being processed right now, which runs it on
		 * this turn rather than 64 turns from now -- the difference
		 * between a late timer and one that looks lost. */
		level = 0;
		index = (unsigned)(wheel_now & WHEEL_MASK);
	} else {
		for (level = 0; level < WHEEL_LEVELS; level++)
			if (delta < (1ull << (WHEEL_BITS * (level + 1))))
				break;

		/* timer_start refuses anything past the reach, so this cannot
		 * be hit from there. It can be hit by a timer cascading down
		 * near the wrap, and the top level is where it belongs. */
		if (level >= WHEEL_LEVELS)
			level = WHEEL_LEVELS - 1;

		index = (unsigned)((t->expires >> (WHEEL_BITS * level)) &
				   WHEEL_MASK);
	}

	link_at(&slots[level][index], t);
}

/* Empties one slot of a coarse wheel back into the finer ones.
 *
 * Returns the index it emptied, because the caller needs to know whether that
 * hand has wrapped: a wheel advances only when the one below it has come all
 * the way round, which is exactly the case where this returns zero.
 */
static unsigned cascade(unsigned level)
{
	unsigned index = (unsigned)((wheel_now >> (WHEEL_BITS * level)) &
				    WHEEL_MASK);
	struct timer *list = slots[level][index];

	slots[level][index] = 0;

	while (list) {
		struct timer *next = list->next;

		list->next = 0;
		list->pprev = 0;
		place(list);
		list = next;
	}

	return index;
}

void timer_init(struct timer *t, void (*fn)(void *), void *arg)
{
	t->next = 0;
	t->pprev = 0;
	t->expires = 0;
	t->fn = fn;
	t->arg = arg;
	t->pending = false;
}

bool timer_start(struct timer *t, u64 ns)
{
	u64 ticks = (ns * TIME_TICK_HZ + 999999999ull) / 1000000000ull;
	u64 flags;

	if (!t->fn)
		return false;

	/* Rounded up, and never to zero: a timer asked for in less than a tick
	 * is due on the next one and not on this one. Firing immediately would
	 * mean a "wake me in a microsecond" that ran before its caller had
	 * finished setting up whatever the callback touches. */
	if (ticks == 0)
		ticks = 1;

	if (ticks >= WHEEL_REACH) {
		/* Refused rather than clamped. A clamped timer fires at a time
		 * nobody asked for and reports success, which is the shape of
		 * fault this project keeps deciding not to build. */
		__atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
		return false;
	}

	flags = spin_lock_irq(&timer_lock);

	if (t->pending) {
		/* Restarting a live timer is almost always a caller that has
		 * lost track of one, and there is no reading of "start" that
		 * makes it obvious whether the old expiry or the new one
		 * should win. Refused, so the bug is at the call site. */
		spin_unlock_irq(&timer_lock, flags);
		return false;
	}

	t->expires = wheel_now + ticks;
	t->pending = true;
	place(t);
	started++;

	spin_unlock_irq(&timer_lock, flags);
	return true;
}

bool timer_cancel(struct timer *t)
{
	u64 flags = spin_lock_irq(&timer_lock);
	bool was = t->pending;

	if (was) {
		unlink_from(t);
		t->pending = false;
		cancelled++;
	}

	spin_unlock_irq(&timer_lock, flags);
	return was;
}

void timer_tick(void)
{
	u64 flags = spin_lock_irq(&timer_lock);

	/* A loop rather than a single step, because the wheel can fall behind:
	 * a processor that spent two ticks with interrupts off owes two turns,
	 * and turning once would leave a slot unexamined until the hand came
	 * back round to it 64 ticks later. */
	while ((i64)(time_ticks() - wheel_now) >= 0) {
		unsigned index = (unsigned)(wheel_now & WHEEL_MASK);
		struct timer *due;

		/* Level 0 has come all the way round, so the wheel above it
		 * advances by one -- and if that one has also wrapped, the one
		 * above it, and so on. This is the whole of the cascade, and it
		 * is why a timer can be filed in O(1) and still come out in the
		 * right order. */
		if (index == 0) {
			unsigned level = 1;

			while (level < WHEEL_LEVELS && cascade(level) == 0)
				level++;
		}

		due = slots[0][index];
		slots[0][index] = 0;
		wheel_now++;

		while (due) {
			struct timer *next = due->next;
			void (*fn)(void *) = due->fn;
			void *arg = due->arg;

			due->next = 0;
			due->pprev = 0;
			due->pending = false;
			fired++;

			/* The lock is dropped across the callback, because a
			 * callback that starts another timer is ordinary -- a
			 * retry, anything periodic -- and holding the lock
			 * would make that a deadlock instead.
			 *
			 * `next` is read before the callback runs, since the
			 * callback may free the object the timer is embedded
			 * in. That is not hypothetical: it is what a request
			 * timeout does. */
			spin_unlock_irq(&timer_lock, flags);
			fn(arg);
			flags = spin_lock_irq(&timer_lock);

			due = next;
		}
	}

	spin_unlock_irq(&timer_lock, flags);
}

/* --- sleeping -------------------------------------------------------------
 *
 * A thread that wants to wait a while, without a processor spinning for it.
 */
struct sleeper {
	struct wait_queue q;
	struct spinlock lock;
	volatile bool done;
};

static void wake_sleeper(void *arg)
{
	struct sleeper *s = arg;
	u64 flags = spin_lock_irq(&s->lock);

	s->done = true;
	wait_wake_all(&s->q);

	spin_unlock_irq(&s->lock, flags);
}

bool timer_sleep_ns(u64 ns)
{
	struct sleeper s;
	struct timer t;
	u64 flags;
	bool slept = true;

	s.q.head = 0;
	s.q.tail = 0;
	spin_init(&s.lock, "sleep");
	s.done = false;

	timer_init(&t, wake_sleeper, &s);

	if (!timer_start(&t, ns))
		return false;

	flags = spin_lock_irq(&s.lock);

	/* A loop, because a woken thread is one that has been told to look
	 * again rather than one whose condition is known true -- and because it
	 * is the flag, not the wakeup, that says the time has passed. */
	while (!s.done && slept)
		slept = wait_sleep(&s.q, &s.lock, flags);

	spin_unlock_irq(&s.lock, flags);

	if (!slept) {
		/* Could not sleep: the idle thread, or no thread at all. The
		 * timer is still filed and points at a structure on a stack
		 * that is about to go, so it has to come off -- and if it has
		 * already fired, the wait for the flag is bounded, because the
		 * callback is either running now or has finished. */
		if (!timer_cancel(&t))
			while (!s.done)
				arch_cpu_relax();

		return false;
	}

	return true;
}

void timer_print_summary(void)
{
	kprintf("\nTimers\n");
	kprintf("  wheel        : %u levels of %u slots, %lu ticks of reach\n",
		(unsigned)WHEEL_LEVELS, (unsigned)WHEEL_SIZE,
		(unsigned long)WHEEL_REACH);
	kprintf("  activity     : %lu started, %lu fired, %lu cancelled\n",
		started, fired, cancelled);

	if (refused)
		kprintf("  refused      : %lu, as further off than the wheel "
			"reaches\n", refused);
}

/* --- the self-test --------------------------------------------------------
 *
 * Four things, and the fourth is the one that costs time and is worth it.
 *
 * A timer that fires is easy to check and proves little on its own: a wheel
 * that ran every slot on every tick would pass it. So the assertions are about
 * *when* -- not before its time, in the right order relative to other timers,
 * not at all once cancelled -- and about the cascade, which is the only part of
 * this file that is not a linked-list operation and the only part that can be
 * subtly wrong.
 *
 * The cascade case waits about three quarters of a second, because that is what
 * it takes for a timer filed on level 1 to be walked down to level 0 and run.
 * There is no way to test it faster that is still testing this code: driving
 * the wheel by hand from the test would be testing a second implementation of
 * the loop.
 */
static volatile unsigned fire_order[8];
static volatile unsigned fire_count;
static volatile u64 fired_at_tick[8];

static void note_firing(void *arg)
{
	unsigned me = (unsigned)(uintptr_t)arg;

	if (fire_count < 8) {
		fired_at_tick[fire_count] = time_ticks();
		fire_order[fire_count++] = me;
	}
}

static bool wait_for_tick(u64 target)
{
	u64 deadline = time_monotonic_ns() + 5000000000ULL;

	while (time_ticks() < target) {
		if (time_monotonic_ns() > deadline)
			return false;
		sched_yield();
	}

	return true;
}

bool timer_self_test(void)
{
	struct timer a, b, c, gone;
	u64 start;
	bool ok = true;


	fire_count = 0;

	timer_init(&a, note_firing, (void *)(uintptr_t)1);
	timer_init(&b, note_firing, (void *)(uintptr_t)2);
	timer_init(&c, note_firing, (void *)(uintptr_t)3);
	timer_init(&gone, note_firing, (void *)(uintptr_t)9);

	/* Out of order on purpose. A wheel that simply ran things in the order
	 * they were filed would pass a test that started them in order. */
	start = time_ticks();
	timer_start(&b, 40000000ull);		/* 4 ticks */
	timer_start(&c, 70000000ull);		/* 7 ticks */
	timer_start(&a, 20000000ull);		/* 2 ticks */
	timer_start(&gone, 50000000ull);	/* 5 ticks, and cancelled */

	if (!timer_cancel(&gone)) {
		kputs("  timer: a timer that was just started reported that it "
		      "was not pending\n");
		ok = false;
	}

	if (!wait_for_tick(start + 10)) {
		kputs("  timer: the tick stopped, so nothing here was "
		      "tested\n");
		return false;
	}

	if (fire_count != 3) {
		kprintf("  timer: %u of 3 timers fired\n", fire_count);
		ok = false;
	}

	/* The cancelled one, named separately: "three fired" would also be true
	 * if the cancelled one had run and one of the others had not. */
	for (unsigned i = 0; i < fire_count; i++)
		if (fire_order[i] == 9) {
			kputs("  timer: a cancelled timer fired anyway\n");
			ok = false;
		}

	if (fire_count == 3 &&
	    (fire_order[0] != 1 || fire_order[1] != 2 || fire_order[2] != 3)) {
		kprintf("  timer: fired in the order %u %u %u rather than by "
			"expiry\n", fire_order[0], fire_order[1],
			fire_order[2]);
		ok = false;
	}

	/* Not early. A timer that fires immediately fires in the right order
	 * with every other timer that fires immediately, so order alone does
	 * not catch a wheel that has lost its sense of time. */
	if (fire_count == 3 && fired_at_tick[0] < start + 2) {
		kprintf("  timer: a 2-tick timer fired after %lu\n",
			fired_at_tick[0] - start);
		ok = false;
	}

	/* Beyond the wheel's reach: refused, and refused visibly. */
	{
		struct timer far;
		u64 before = refused;

		timer_init(&far, note_firing, (void *)(uintptr_t)7);

		if (timer_start(&far, 0xFFFFFFFFFFFFFFull) ||
		    refused == before) {
			kputs("  timer: a delay past the end of the wheel was "
			      "accepted\n");
			timer_cancel(&far);
			ok = false;
		}
	}

	/* --- the cascade ---
	 *
	 * 70 ticks is past level 0's 64 slots, so this timer is filed on level
	 * 1 and can only run if it is walked back down. Without the cascade it
	 * would sit there and never fire, which is precisely the failure a
	 * shorter test cannot see. */
	{
		struct timer deep;
		u64 at = time_ticks();

		fire_count = 0;
		timer_init(&deep, note_firing, (void *)(uintptr_t)5);
		timer_start(&deep, 700000000ull);	/* 70 ticks */

		if (!wait_for_tick(at + 75)) {
			kputs("  timer: the tick stopped during the cascade "
			      "test\n");
			timer_cancel(&deep);
			return false;
		}

		if (fire_count != 1 || fire_order[0] != 5) {
			kputs("  timer: a timer filed on the second wheel "
			      "never came down to the first\n");
			timer_cancel(&deep);
			ok = false;
		} else if (fired_at_tick[0] < at + 70) {
			kprintf("  timer: the cascaded timer fired %lu ticks "
				"early\n", at + 70 - fired_at_tick[0]);
			ok = false;
		}
	}

	/* --- and a thread that sleeps ---
	 *
	 * The assertion is on elapsed time rather than on returning at all: a
	 * timer_sleep_ns that returned immediately would pass any test that
	 * only checked it came back. */
	{
		u64 before = time_monotonic_ns();

		if (!timer_sleep_ns(50000000ull)) {	/* 50 ms */
			kputs("  timer: a thread could not sleep\n");
			ok = false;
		} else if (time_monotonic_ns() - before < 45000000ull) {
			kprintf("  timer: a 50 ms sleep took %lu us\n",
				(time_monotonic_ns() - before) / 1000);
			ok = false;
		}
	}

	return ok;
}
