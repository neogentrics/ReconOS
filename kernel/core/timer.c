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
	const u64 tick_ns = 1000000000ull / TIME_TICK_HZ;
	u64 ticks = (ns * TIME_TICK_HZ + 999999999ull) / 1000000000ull;
	u64 flags, now;

	/* Read once, here, before anything else this function does.
	 *
	 * The caller asked at some instant before this line, so this is at or
	 * after the moment they meant -- which is the direction that keeps the
	 * promise below. Read later and the deadline drifts further from what
	 * they asked for; read before the lock and it cannot drift at all. */
	u64 asked_ns = time_monotonic_ns();

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

	/* **From the clock, not from the wheel's hand.**
	 *
	 * These were one number until KF-204: both were stepped by the tick
	 * interrupt, so `wheel_now` and `time_ticks()` could not disagree and
	 * either would do here. The tick count is derived from the monotonic
	 * counter now, which means it advances continuously while the hand only
	 * moves when `timer_tick` runs -- so between two interrupts the clock
	 * reads up to one tick ahead.
	 *
	 * Filing against the hand then means a caller who read `time_ticks()`
	 * gets a deadline one tick *before* the one they asked for, and a timer
	 * that fires early is the fault KF-204 was about. (KF-206) */
	now = time_ticks();

	/* The wheel is never ahead of the clock -- `timer_tick` stops when it
	 * catches up -- and this says so rather than assuming it. A hand that
	 * had somehow overrun would otherwise produce a negative distance and a
	 * timer filed into the slot being processed right now. */
	if ((i64)(now - wheel_now) < 0)
		now = wheel_now;

	/* **The first tick at or after the instant asked for.** (KF-234)
	 *
	 * This was `now + ticks`, and `now` is `time_ticks()`, which is the
	 * monotonic counter *truncated* to a tick. That names the tick the
	 * caller is in, not the moment they asked at -- so the delay was
	 * measured from up to a whole tick in the past, and a fifty millisecond
	 * sleep could come due after forty.
	 *
	 * A timer may fire late. A tick is the wheel's resolution and nothing
	 * here pretends otherwise. It may never be *due before the instant it
	 * was asked for*, because a caller who then reads a clock to see
	 * whether the time has passed is told yes when it has not, and that is
	 * a wrong answer rather than a coarse one.
	 *
	 * Ceiling, on the sum, in nanoseconds: `expires * tick_ns` is then at
	 * or after `asked_ns + ns` by construction, at every phase, rather than
	 * at the phases where truncation happened to lose nothing. */
	t->expires = (asked_ns + ns + tick_ns - 1) / tick_ns;

	/* And never the slot the hand is on.
	 *
	 * The rounding above already puts a sub-tick delay on the next tick,
	 * which is what `ticks == 0 -> 1` says at the top of this function. This
	 * is the other end of the same rule and it is about the *wheel* rather
	 * than the request: `place` files relative to `wheel_now`, and a
	 * deadline at or behind the hand aliases onto a slot that has already
	 * been emptied -- where it would sit for a full turn of the wheel. It
	 * takes a clock that has not moved a tick since the hand last did, so
	 * it is rare and it is not hypothetical. */
	if ((i64)(t->expires - now) < 1)
		t->expires = now + 1;

	/* The reach, re-checked against the distance the wheel actually has to
	 * cover. `ticks < WHEEL_REACH` above was the whole precondition while
	 * the deadline was the hand plus the delay; it is up to one tick short
	 * of it now, and at the edge that is a timer that aliases onto the top
	 * level's own hand instead of being refused. */
	if (t->expires - wheel_now >= WHEEL_REACH) {
		__atomic_add_fetch(&refused, 1, __ATOMIC_RELAXED);
		spin_unlock_irq(&timer_lock, flags);
		return false;
	}

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

/* When the next filed timer is due, in wheel ticks. False where nothing is
 * filed at all, which is the ordinary state of an idle machine and is the
 * answer that lets a processor sleep indefinitely.
 *
 * **The obvious scan, on purpose.** Three hundred and twenty lists, every slot
 * of every level. The alternative is a minimum maintained as timers are filed
 * and cancelled and cascaded -- three places to keep in step, in exchange for
 * saving work on a processor whose next act is to halt. The cheap version is
 * the one that has to be right.
 *
 * Compared by difference against the wheel's own position rather than by
 * magnitude, so a deadline that has wrapped past 2^64 is still nearer than one
 * that has not -- the same rule sequence numbers use in tcp.c.
 */
bool timer_next_deadline(u64 *when)
{
	u64 flags = spin_lock_irq(&timer_lock);
	bool found = false;
	u64 best = 0;
	unsigned level, index;

	for (level = 0; level < WHEEL_LEVELS; level++) {
		for (index = 0; index < WHEEL_SIZE; index++) {
			const struct timer *t = slots[level][index];

			for (; t; t = t->next) {
				if (!found || (i64)(t->expires - best) < 0) {
					best = t->expires;
					found = true;
				}
			}
		}
	}

	spin_unlock_irq(&timer_lock, flags);

	if (found && when)
		*when = best;

	return found;
}

/* How far the wheel has actually been turned. A processor that has been asleep
 * compares this against the tick count to know what it owes. */
u64 timer_wheel_position(void)
{
	return wheel_now;
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
	 * only checked it came back.
	 *
	 * **Asked for at a chosen point inside a tick, and exact rather than
	 * within five milliseconds.** Both of those were slop and the slop was
	 * hiding a fault.
	 *
	 * A sleep may return late -- the wheel has a resolution and a tick is
	 * it. It may never return *early*: "wake me in 50 ms" that comes back
	 * in 41 is a promise broken, and every caller that then reads a clock
	 * to see whether it is time yet does the wrong thing quietly.
	 *
	 * The old form asked at whatever moment the boot happened to reach it
	 * and allowed 45 ms. Under QEMU that moment is close to the same on
	 * every run, which is why fifty-six of them agreed; on a laptop it is
	 * effectively random, and half of all phases return under 45 ms. **A
	 * test whose outcome depends on when it is run is not measuring the
	 * thing it names.** So the phase is now chosen: late in a tick, where
	 * the error is largest and a failure is certain rather than likely.
	 */
	/* --- the deadline itself, which is where the fault actually is ------
	 *
	 * Sleeping and timing it measures a race: the filing, the tick that
	 * fires the callback, and the scheduler getting back to the thread. Any
	 * of those three can add a tick and hide the error, which is why the
	 * end-to-end version below caught this one boot in four at a phase held
	 * to within twenty-five microseconds. **Printing the phase made it one
	 * in six, because the print itself took long enough to cross the
	 * boundary the test needed not to cross.** An instrument that changes
	 * the thing it measures by that much is measuring itself.
	 *
	 * The promise is arithmetic and can be checked as arithmetic. A timer
	 * filed for `ns` from now may fire late -- a tick is the resolution --
	 * and may never be *due* before the instant it was asked at. Read the
	 * deadline back and compare it against that instant. No interrupt, no
	 * scheduler, no race, and it holds at every phase rather than at a
	 * chosen one.
	 */
	{
		const u64 tick_ns = 1000000000ull / TIME_TICK_HZ;
		unsigned phase;

		/* Every tenth of a tick, so no single phase can be the lucky
		 * one. Each pass files a timer, reads its deadline and cancels
		 * it; nothing is ever allowed to fire. */
		for (phase = 0; phase < 10; phase++) {
			struct timer probe;
			u64 target = tick_ns * phase / 10;
			u64 asked, due_ns;

			while (time_monotonic_ns() % tick_ns < target)
				arch_cpu_relax();

			timer_init(&probe, note_firing, (void *)(uintptr_t)9);

			asked = time_monotonic_ns();
			if (!timer_start(&probe, 50000000ull)) {
				kputs("  timer: a probe could not be "
				      "filed\n");
				ok = false;
				break;
			}
			due_ns = probe.expires * tick_ns;
			timer_cancel(&probe);

			if (due_ns < asked + 50000000ull) {
				kprintf("  timer: a 50 ms timer filed at "
					"phase %u/10 is due %lu us before it "
					"was asked for\n", phase,
					(unsigned long)((asked + 50000000ull
							 - due_ns) / 1000));
				ok = false;
				break;
			}
		}
	}

	{
		const u64 tick_ns = 1000000000ull / TIME_TICK_HZ;
		u64 before, elapsed;

		/* Seven tenths into a tick, deliberately -- a band, not a
		 * threshold, and not as late as it will go.
		 *
		 * **The obvious choice defeats itself.** Asking at nine tenths
		 * leaves one tenth of a tick before the count rolls over, and
		 * `timer_start` reads the clock again a little after this
		 * does: lock, init, lock. If that crossing happens the base
		 * tick is one higher, the sleep is a whole tick longer than it
		 * asked for, and the test passes -- for the opposite of the
		 * right reason. Six boots did exactly that.
		 *
		 * Seven tenths keeps three of them in hand, which is room the
		 * setup cannot use, and the error is still 7 ms. The Gateway's
		 * own figure lands here: 43132 us is a phase of 6.868 ms. */
		while (time_monotonic_ns() % tick_ns < tick_ns * 7 / 10)
			arch_cpu_relax();

		before = time_monotonic_ns();

		if (!timer_sleep_ns(50000000ull)) {	/* 50 ms */
			kputs("  timer: a thread could not sleep\n");
			ok = false;
		} else if ((elapsed = time_monotonic_ns() - before)
			   < 50000000ull) {
			kprintf("  timer: a 50 ms sleep came back %lu us "
				"early, after %lu us\n",
				(unsigned long)((50000000ull - elapsed) / 1000),
				(unsigned long)(elapsed / 1000));
			ok = false;
		}
	}

	return ok;
}
