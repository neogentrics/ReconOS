/*
 * Waiting.
 *
 * `src/recon_loop.c` reads no clock and makes no system call -- the time
 * arrives as an argument -- which is what lets every interesting question
 * about a timer be asked here rather than by watching a machine and counting.
 *
 * --- What is worth holding ---
 *
 * Eight thousand lines depend on this now: the session's boot progress, the
 * player's frame ticker, the task manager's refresh, the photo viewer's settle
 * timer, the audio top-up. All of them are **one-shot timers that re-arm
 * themselves**, which is the shape with the interesting failures in it:
 *
 *   - **a timer that fires twice** makes everything happen at double rate,
 *     which reads as a machine that feels busy rather than one that is broken.
 *   - **a callback re-arming itself for zero** must not run away inside one
 *     tick. Deciding what is due as we go would let it, and the loop would
 *     never return.
 *   - **disarming before the callback runs**, not after, or a callback that
 *     arms itself has its arming thrown away -- a film that plays one frame.
 *   - **a timer is armed relative to the clock its caller is reading**, even
 *     when that clock has just gone backwards. The version that clamped the
 *     clock instead made a timer armed after a jump wait for the old time to
 *     come round again -- fifty seconds late, for a fifty-second jump.
 *
 * Run with: ./build/recon_loop_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "recon_loop.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
	g_checks++;
	if (!condition) {
		g_failures++;
		printf("  FAIL: %s\n", what);
	}
}

/* --- what the callbacks do ---------------------------------------------- */

struct counter {
	int fired;

	/* Re-arm for this many milliseconds after firing; 0 does not re-arm. */
	int again;
	struct recon_timer *self;

	/* Destroy this one when fired, which is what a window closing does. */
	struct recon_timer *kill;
};

static void count(void *user) {
	struct counter *c = user;

	c->fired++;
	if (c->again > 0) {
		recon_timer_after(c->self, c->again);
	}
	if (c->kill != NULL) {
		recon_timer_destroy(c->kill);
		c->kill = NULL;
	}
}

static void test_a_timer_starts_disarmed(void) {
	printf("a timer is created disarmed\n");

	struct recon_loop *loop = recon_loop_create();
	struct counter c = { 0 };
	struct recon_timer *t = recon_timer_create(loop, count, &c);

	check(t != NULL, "it is made");
	check(!recon_timer_is_armed(t), "and it is not armed");

	/*
	 * Two steps rather than one, because that is how every caller uses it:
	 * the timer is made when a window opens and armed when something
	 * starts happening, and those are rarely the same moment. A timer that
	 * armed itself on creation would fire once for every window ever
	 * opened.
	 */
	check(recon_loop_tick(loop, 1000000) == 0, "and a tick fires nothing");
	check(c.fired == 0, "so nothing happened");

	recon_loop_destroy(loop);
}

static void test_it_fires_at_the_deadline_and_not_before(void) {
	printf("a timer fires when it is due, and not before\n");

	struct recon_loop *loop = recon_loop_create();
	struct counter c = { 0 };
	struct recon_timer *t = recon_timer_create(loop, count, &c);

	c.self = t;
	recon_loop_tick(loop, 1000);
	recon_timer_after(t, 250);

	check(recon_loop_tick(loop, 1100) == 0, "not at 100ms");
	check(c.fired == 0, "nothing fired");
	check(recon_loop_tick(loop, 1249) == 0, "not one millisecond early");
	check(c.fired == 0, "still nothing");

	check(recon_loop_tick(loop, 1250) == 1, "it fires at exactly 250ms");
	check(c.fired == 1, "once");

	/*
	 * And **not again**, which is the whole of what one-shot means. Every
	 * caller re-arms, so a timer that fired twice would double every rate
	 * in the desktop -- and what that looks like is a machine that feels
	 * busy, not one that is obviously broken.
	 */
	check(recon_loop_tick(loop, 1251) == 0, "and not again at 251");
	check(recon_loop_tick(loop, 9999) == 0, "nor ever");
	check(c.fired == 1, "still just once");
	check(!recon_timer_is_armed(t), "and it is disarmed");

	recon_loop_destroy(loop);
}

static void test_a_callback_can_rearm_itself(void) {
	printf("a callback can arm itself again\n");

	/*
	 * The ordinary use, and the reason a timer is disarmed *before* its
	 * callback runs rather than after. Disarming afterwards would throw
	 * away whatever the callback just asked for, and `recon_player.c`'s
	 * ticker -- which ends by re-arming -- would play one frame and stop.
	 */
	struct recon_loop *loop = recon_loop_create();
	struct counter c = { 0 };
	struct recon_timer *t = recon_timer_create(loop, count, &c);

	c.self = t;
	c.again = 100;

	recon_loop_tick(loop, 0);
	recon_timer_after(t, 100);

	for (int i = 1; i <= 5; i++) {
		recon_loop_tick(loop, (uint64_t)i * 100);
	}
	check(c.fired == 5, "it fired once per period, five times");
	check(recon_timer_is_armed(t), "and is armed for the sixth");

	recon_loop_destroy(loop);
}

static void test_a_runaway_callback_cannot_hang_a_tick(void) {
	printf("a callback that re-arms for zero does not run away\n");

	/*
	 * `recon_timer_after(t, 0)` disarms, so this is really a callback
	 * asking for one millisecond and a tick that is already past it. If
	 * what is due were decided as the loop went, this would fire, re-arm,
	 * be found due again in the same pass, and never return.
	 *
	 * The list is taken **before** anything runs, so a timer fires at most
	 * once per tick however it behaves. That is the whole reason for the
	 * two-pass shape in `recon_loop_tick`.
	 */
	struct recon_loop *loop = recon_loop_create();
	struct counter c = { 0 };
	struct recon_timer *t = recon_timer_create(loop, count, &c);

	c.self = t;
	c.again = 1;

	recon_loop_tick(loop, 0);
	recon_timer_after(t, 1);

	/* If this hangs, it hangs -- and a suite that never finishes is a
	 * clearer report than one that fails. */
	check(recon_loop_tick(loop, 1000) == 1, "one tick fires it once");
	check(c.fired == 1, "exactly once, though it asked for more");
	check(recon_timer_is_armed(t), "and it is armed again for the next");

	recon_loop_destroy(loop);
}

static void test_zero_disarms(void) {
	printf("zero disarms, and so does a delay that went backwards\n");

	struct recon_loop *loop = recon_loop_create();
	struct counter c = { 0 };
	struct recon_timer *t = recon_timer_create(loop, count, &c);

	recon_loop_tick(loop, 0);
	recon_timer_after(t, 100);
	check(recon_timer_is_armed(t), "armed");

	recon_timer_after(t, 0);
	check(!recon_timer_is_armed(t), "and zero disarms it");
	check(recon_loop_tick(loop, 10000) == 0, "so it never comes due");
	check(c.fired == 0, "and never fires");

	/*
	 * A negative delay is not something a caller passes on purpose. It is
	 * what a caller passes having subtracted two times the wrong way
	 * round, and the choice is between "never" and "immediately, for
	 * ever". A timer firing in a tight loop because of a sign is a machine
	 * that stops responding; one that does not fire is a feature that does
	 * not work, and the second can be diagnosed.
	 */
	recon_timer_after(t, -5);
	check(!recon_timer_is_armed(t), "a negative delay disarms rather than firing");
	check(recon_loop_tick(loop, 20000) == 0, "and nothing comes due");

	recon_loop_destroy(loop);
}

static void test_rearming_replaces_the_delay(void) {
	printf("arming an armed timer replaces the delay\n");

	/*
	 * What `recon_player.c` relies on when a film is paused and resumed:
	 * the ticker is armed again with a fresh delay, and must not fire at
	 * whatever the old one would have been.
	 */
	struct recon_loop *loop = recon_loop_create();
	struct counter c = { 0 };
	struct recon_timer *t = recon_timer_create(loop, count, &c);

	recon_loop_tick(loop, 0);
	recon_timer_after(t, 100);
	recon_timer_after(t, 500);

	check(recon_loop_tick(loop, 100) == 0, "the first deadline is gone");
	check(c.fired == 0, "nothing fired at it");
	check(recon_loop_tick(loop, 500) == 1, "and the second is the one that counts");
	check(c.fired == 1, "fired once");

	recon_loop_destroy(loop);
}

static void test_a_callback_may_destroy_a_timer(void) {
	printf("a callback may destroy a timer, including another one due now\n");

	/*
	 * What a window closing does: every timer it owns goes at once, and
	 * one of them may be mid-callback. A tick that had decided its list
	 * and then ran a freed timer would be reading memory nobody owns.
	 */
	struct recon_loop *loop = recon_loop_create();
	struct counter first = { 0 };
	struct counter second = { 0 };
	struct recon_timer *a = recon_timer_create(loop, count, &first);
	struct recon_timer *b = recon_timer_create(loop, count, &second);

	recon_loop_tick(loop, 0);
	recon_timer_after(a, 10);
	recon_timer_after(b, 10);
	first.kill = b;             /* a destroys b, which was also due */

	int fired = recon_loop_tick(loop, 10);

	check(fired == 2, "both were due");
	check(first.fired == 1, "the first ran");
	check(second.fired == 0, "and the second was destroyed before it could");

	recon_loop_destroy(loop);
}

static void test_a_clock_that_goes_backwards(void) {
	printf("the clock is taken as given, backwards or not\n");

	/*
	 * --- What this used to check, and why it checked nothing ---
	 *
	 * There was a clamp in `recon_loop_tick` that refused a time earlier
	 * than the last one, with a comment about unsigned arithmetic wrapping.
	 * A mutation deleting it changed no output, and looking at why showed
	 * the comment was describing a failure the file cannot have: every use
	 * of the time is an addition or a comparison, and the one subtraction
	 * is already guarded.
	 *
	 * The clamp also cost something. A timer armed *after* a backward jump
	 * was given a deadline relative to the old time and waited for the
	 * clock to catch up -- fifty seconds late, for a fifty-second jump.
	 *
	 * So the behaviour held here is the one that matters: **a timer is
	 * armed relative to the clock its caller is reading.** That is what
	 * "in 100 milliseconds" means, and it is what distinguishes the two
	 * versions.
	 */
	struct recon_loop *loop = recon_loop_create();
	struct counter c = { 0 };
	struct recon_timer *t = recon_timer_create(loop, count, &c);

	recon_loop_tick(loop, 100000);

	/* The clock jumps back fifty seconds, and only then is a timer armed. */
	recon_loop_tick(loop, 50000);
	recon_timer_after(t, 100);

	check(recon_loop_tick(loop, 50099) == 0, "it does not fire early");
	check(recon_loop_next_deadline(loop, 50000) == 100,
	      "and says it is 100ms away, not fifty seconds");
	check(recon_loop_tick(loop, 50100) == 1,
	      "it fires 100ms after it was armed, on the clock it was given");
	check(c.fired == 1, "once");

	/* And a timer armed before the jump keeps the deadline it was given,
	 * which the jump has made unreachable until the clock returns -- the
	 * honest consequence of a clock that lied, and not this file's to
	 * invent an answer for. */
	struct counter old = { 0 };
	struct recon_timer *before = recon_timer_create(loop, count, &old);

	recon_loop_tick(loop, 200000);
	recon_timer_after(before, 100);
	recon_loop_tick(loop, 100000);
	check(old.fired == 0, "a deadline in the future is still in the future");
	check(recon_loop_tick(loop, 200100) == 1, "and comes due when it arrives");

	recon_loop_destroy(loop);
}

static void test_how_long_to_sleep(void) {
	printf("the loop says how long until the next timer\n");

	/*
	 * What it is for is sleeping. A driver that ticks in a tight loop pins
	 * a processor for nothing; one that sleeps a fixed amount makes every
	 * timer late by up to that amount.
	 */
	struct recon_loop *loop = recon_loop_create();
	struct counter c = { 0 };
	struct recon_timer *a = recon_timer_create(loop, count, &c);
	struct recon_timer *b = recon_timer_create(loop, count, &c);

	recon_loop_tick(loop, 1000);
	check(recon_loop_next_deadline(loop, 1000) == -1,
	      "nothing armed answers -1, not 0 -- a caller must not busy-wait");

	recon_timer_after(a, 500);
	recon_timer_after(b, 200);
	check(recon_loop_next_deadline(loop, 1000) == 200,
	      "the soonest of the two, not the first or the last");

	check(recon_loop_next_deadline(loop, 1150) == 50,
	      "and it counts down as the clock moves");
	check(recon_loop_next_deadline(loop, 5000) == 0,
	      "something already overdue answers zero");

	recon_timer_destroy(b);
	check(recon_loop_next_deadline(loop, 1000) == 500,
	      "destroying the soonest leaves the other");

	recon_loop_destroy(loop);
}

/* --- descriptors --------------------------------------------------------- */

struct reader {
	int calls;
	int last_fd;
};

static void on_readable(int fd, void *user) {
	struct reader *r = user;

	r->calls++;
	r->last_fd = fd;
}

static void test_a_watch_hears_its_own_descriptor(void) {
	printf("a watch hears its own descriptor and no other\n");

	struct recon_loop *loop = recon_loop_create();
	struct reader one = { 0, -1 };
	struct reader two = { 0, -1 };
	struct recon_watch *a = recon_watch_readable(loop, 7, on_readable, &one);
	struct recon_watch *b = recon_watch_readable(loop, 9, on_readable, &two);

	check(a != NULL && b != NULL, "both are watched");

	recon_loop_readable(loop, 7);
	check(one.calls == 1 && one.last_fd == 7, "the first heard its own");
	check(two.calls == 0, "and the second heard nothing");

	recon_loop_readable(loop, 11);
	check(one.calls == 1 && two.calls == 0, "a descriptor nobody watches is ignored");

	/*
	 * A watch is not disarmed by firing. A descriptor with more to read is
	 * the ordinary case, and a caller that had to re-register after every
	 * read would drop whatever arrived in between.
	 */
	recon_loop_readable(loop, 7);
	check(one.calls == 2, "and it keeps hearing until it is destroyed");

	recon_watch_destroy(a);
	recon_loop_readable(loop, 7);
	check(one.calls == 2, "destroyed, it hears no more");

	recon_watch_destroy(b);
	recon_loop_destroy(loop);
}

static void test_running_out(void) {
	printf("running out of room is refused, not overrun\n");

	struct recon_loop *loop = recon_loop_create();
	struct counter c = { 0 };
	int made = 0;

	while (recon_timer_create(loop, count, &c) != NULL && made < 1000) {
		made++;
	}

	/*
	 * A fixed table, and the answer when it is full is NULL. Every caller
	 * of `recon_timer_create` in the desktop checks -- and the one that
	 * did not would get a null dereference at its next arm rather than
	 * quietly sharing somebody else's timer.
	 */
	check(made > 0 && made < 1000, "it stops at a bound rather than growing");
	check(recon_timer_create(loop, count, &c) == NULL, "and then refuses");

	check(recon_timer_create(loop, NULL, &c) == NULL, "a callback is required");
	check(recon_timer_create(NULL, count, &c) == NULL, "and so is a loop");
	check(recon_watch_readable(loop, -1, on_readable, &c) == NULL,
	      "and a descriptor that is not one is refused");

	recon_loop_destroy(loop);
}

static void test_nothing_at_all(void) {
	printf("every call survives being handed nothing\n");

	/* Not defensiveness for its own sake: `recon_timer_create` answers
	 * NULL when it runs out, so a caller that did not check hands NULL to
	 * every one of these. */
	recon_timer_after(NULL, 100);
	recon_timer_destroy(NULL);
	recon_watch_destroy(NULL);
	recon_loop_destroy(NULL);
	recon_loop_readable(NULL, 3);

	check(!recon_timer_is_armed(NULL), "no timer is not armed");
	check(recon_loop_tick(NULL, 5) == 0, "no loop fires nothing");
	check(recon_loop_next_deadline(NULL, 5) == -1, "and has no deadline");
}

int main(void) {
	printf("ReconOS event loop tests\n\n");

	test_a_timer_starts_disarmed();
	test_it_fires_at_the_deadline_and_not_before();
	test_a_callback_can_rearm_itself();
	test_a_runaway_callback_cannot_hang_a_tick();
	test_zero_disarms();
	test_rearming_replaces_the_delay();
	test_a_callback_may_destroy_a_timer();
	test_a_clock_that_goes_backwards();
	test_how_long_to_sleep();
	test_a_watch_hears_its_own_descriptor();
	test_running_out();
	test_nothing_at_all();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
