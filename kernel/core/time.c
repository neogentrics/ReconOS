#include <recon/kernel/time.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>

static volatile u64 ticks;

void time_tick(void)
{
	ticks++;
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
