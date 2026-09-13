/* Asking for something to happen later.
 *
 * Until now the kernel could tell the time and could be interrupted by it, and
 * could not *arrange* anything: there was no way to say "call this in fifty
 * milliseconds", and no way for a thread to sleep for a while. Every wait in
 * the kernel was either a spin against a deadline -- which burns a processor
 * for the whole wait -- or nothing at all.
 *
 * That was not an oversight so much as an ordering. A timer that fires while a
 * thread is asleep needs a thread that can *be* asleep, and threads could not
 * block until wait queues existed. Three separate rows of the architecture
 * audit said "waits on: wait queues", and this is two of them.
 *
 * --- Why a wheel ---
 *
 * The obvious structure is a list kept in expiry order: cheap to check, since
 * only the head can be due, and O(n) to insert. That is the wrong way round.
 * Timers are inserted and cancelled constantly -- every network timeout, every
 * disk command, every sleep -- and almost none of them ever expire, because the
 * thing they were guarding against usually does not happen. The operation to
 * make cheap is the one that happens most, and that is *inserting*.
 *
 * A wheel makes insertion O(1) by not sorting at all. Time is chopped into
 * ticks, and a timer is filed in the slot for the tick it is due in; expiry is
 * then reading one slot. The cost is that a slot per tick over any useful range
 * is far too many slots, which is what the levels are for: five wheels of 64
 * slots, each counting in units 64 times coarser than the one below. A timer
 * due a long time from now sits in a coarse slot and is *cascaded* down into
 * finer ones as its time approaches -- so the sorting still happens, but only
 * for the timers that survive long enough to need it.
 *
 * Reach is 64^5 ticks, which at a hundred ticks a second is about 124 days. A
 * request beyond that is refused rather than clamped, for the reason every
 * other limit in this kernel is: a clamped timer fires at the wrong time and
 * looks like it worked.
 */
#ifndef RECON_KERNEL_TIMER_H
#define RECON_KERNEL_TIMER_H

#include <recon/kernel/types.h>

/* One pending callback.
 *
 * Provided by the caller rather than allocated here, and deliberately: a timer
 * is nearly always a field of the thing it belongs to -- a request, a socket, a
 * thread -- and allocating one would mean a timer that can fail to start
 * because memory was short, at exactly the moments memory is short.
 *
 * Zeroed is valid and idle. Every field is private to timer.c. */
struct timer {
	struct timer *next;
	struct timer **pprev;	/* what points at this one, for O(1) removal */

	u64 expires;		/* absolute, in ticks */
	void (*fn)(void *);
	void *arg;
	bool pending;
};

void timer_init(struct timer *t, void (*fn)(void *), void *arg);

/* Files a timer to fire in `ns` nanoseconds.
 *
 * Returns false if the delay is beyond the wheel's reach, or if `t` is already
 * pending -- restarting a live timer is almost always a bug in the caller and
 * is never what it silently means.
 *
 * **The callback runs in interrupt context, on the processor that owns the
 * tick.** It must not block, and it must not do enough work to matter; if it
 * needs to, it should queue deferred work, which exists for exactly this. */
bool timer_start(struct timer *t, u64 ns);

/* Cancels a pending timer. Returns true if it was still pending -- that is, if
 * this call is what stopped it from running.
 *
 * False means it has already fired or was never started, and the difference
 * matters: a caller that frees the object a timer points at must know whether
 * the callback has been. */
bool timer_cancel(struct timer *t);

/* Sleeps this thread for at least `ns`. Returns false if the caller cannot
 * sleep -- no current thread, or the idle thread, which must never block. */
bool timer_sleep_ns(u64 ns);

/* Called once per tick by whichever processor owns the tick count. */
void timer_tick(void);

void timer_print_summary(void);
bool timer_self_test(void);

#endif /* RECON_KERNEL_TIMER_H */
