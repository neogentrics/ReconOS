#include <recon/kernel/time.h>
#include <recon/kernel/timer.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>

static volatile u64 ticks;

void time_tick(void)
{
	/* Counted once per interval, not once per processor.
	 *
	 * Every processor's timer calls this, and on a four-processor machine
	 * that made the reported count four times the number of intervals that
	 * had actually passed. Nothing depended on it -- monotonic time comes
	 * from a hardware counter, not from here -- so it was a number on a
	 * summary being wrong rather than a clock being wrong, and it would
	 * have stayed wrong quietly for exactly that reason.
	 *
	 * The boot processor is the one that counts. Not because its ticks are
	 * special, but because there is exactly one of it. */
	if (arch_cpu_id() != 0)
		return;

	ticks++;

	/* And the timer wheel, turned by the same processor for the same reason:
	  * there is one wheel, and a wheel turned by four processors would run
	  * each slot four times. Everything filed on it therefore fires here, in
	  * interrupt context on processor 0 -- which is why deferred work exists
	  * and why timer.h says a callback must not do anything substantial. */
	timer_tick();
}

u64 time_ticks(void)
{
	return ticks;
}

u64 time_monotonic_ns(void)
{
	return arch_monotonic_ns();
}

u64 time_wall_ns(void)
{
	return arch_wall_ns();
}

void time_init(void)
{
	ticks = 0;
	arch_time_init();
}

/* Whole seconds and the fraction, without dividing a 64-bit value more than
 * twice -- the kernel has no 128-bit helpers and no floating point, and a
 * printf that quietly needs either is a printf that does not link. */
static void print_ns(u64 ns)
{
	u64 secs = (u64)(ns / 1000000000ULL);
	u64 ms   = (u64)((ns % 1000000000ULL) / 1000000ULL);

	kprintf("%lu.%lu%lu%lu s", secs,
		(u64)((ms / 100) % 10), (u64)((ms / 10) % 10), (u64)(ms % 10));
}

void time_print_summary(void)
{
	u64 wall = time_wall_ns();

	kprintf("\nTime\n");
	kprintf("  monotonic    : ");
	print_ns(time_monotonic_ns());
	kprintf(" since boot\n");

	if (wall) {
		/* Days since the epoch is enough to show it is a real date
		 * without a calendar in the kernel. Turning it into a year and
		 * a month is the desktop's job and it already does it. */
		/* Cast, because a `ULL` literal makes the whole expression
		 * unsigned long long, and on this target that is a different
		 * type from the unsigned long that %lu names -- even though both
		 * are sixty-four bits. The warning is right to insist: on a
		 * target where they differ in width it would be a real bug. */
		kprintf("  wall clock   : %lu seconds since 1970 (day %lu)\n",
			(u64)(wall / 1000000000ULL),
			(u64)(wall / (86400ULL * 1000000000ULL)));
	} else {
		kputs("  wall clock   : no source of the date on this machine\n");
	}

	kprintf("  tick         : %u Hz, %lu so far\n",
		TIME_TICK_HZ, time_ticks());
}

bool time_self_test(void)
{
	u64 first, second;
	u64 ticks_before;
	bool ok = true;

	/* The monotonic clock has to move, and has to move forwards. A clock
	 * that reads the same twice is a clock that was never started. */
	first = time_monotonic_ns();
	for (volatile unsigned i = 0; i < 200000; i++)
		;
	second = time_monotonic_ns();

	if (second <= first) {
		kprintf("  time: the monotonic clock did not advance (%lu then %lu)\n",
			first, second);
		ok = false;
	}

	/* And the tick has to fire. Waiting on the *clock* rather than on a
	 * spin count, because a spin count is a guess about how fast the machine
	 * is and this has to pass on a slow one and a fast one alike.
	 *
	 * Two ticks rather than one: the first may be a fraction of a period
	 * away, and waiting for it proves only that the timer was already
	 * running. */
	/* --- and that it fires at the rate it claims to ---------------------
	  *
	  * TIME_TICK_HZ is not a measurement, it is a promise: every timer, every
	  * sleep and every scheduling slice in the kernel converts nanoseconds to
	  * ticks with it. If the hardware is actually delivering at some other
	  * rate, all of that is wrong by the same factor and *nothing counting
	  * ticks notices* -- every ordering assertion still holds, every timer
	  * still fires in the right sequence, and every sleep is simply the wrong
	  * length.
	  *
	  * That is not hypothetical. Moving the interrupt lines onto the I/O APIC
	  * did exactly this: the 8254 was programmed as a square wave, which
	  * changes its output twice a period, and the new controller counted both
	  * transitions where the old one counted one. The kernel ran at 201 Hz
	  * against a 100 Hz constant, and every tick-counting test passed.
	  *
	  * Measured against the monotonic clock, which comes from a counter and
	  * not from this tick. A fifth of a second is long enough to tell 100 from
	  * 200 and short enough to pay for on every boot; the tolerance is wide
	  * because a loaded guest genuinely loses ticks, and a factor of two is
	  * what this is for. */
	{
		u64 t0 = time_ticks();
		u64 n0 = time_monotonic_ns();
		u64 elapsed, rate;

		while (time_monotonic_ns() - n0 < 200000000ULL)
			arch_cpu_relax();

		elapsed = time_monotonic_ns() - n0;
		rate = (time_ticks() - t0) * 1000000000ULL / elapsed;

		if (rate > (u64)TIME_TICK_HZ * 3 / 2 ||
		    rate < (u64)TIME_TICK_HZ / 2) {
			kprintf("  time: the tick arrives at about %lu Hz, and every "
				"conversion in the kernel assumes %u\n",
				(unsigned long)rate, (unsigned)TIME_TICK_HZ);
			ok = false;
		}
	}

	ticks_before = time_ticks();
	{
		u64 deadline = time_monotonic_ns() + 500000000ULL;	/* half a second */

		while (time_ticks() < ticks_before + 2) {
			if (time_monotonic_ns() > deadline) {
				kprintf("  time: the tick did not fire in half a second "
					"(%lu ticks)\n", time_ticks() - ticks_before);
				return false;
			}
			arch_wait_for_interrupt();
		}
	}

	return ok;
}
