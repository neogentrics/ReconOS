#include <recon/kernel/boot.h>
#include <recon/kernel/time.h>

#include <efi.h>
#include <recon/kernel/timer.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>

static volatile u64 ticks;

/* Interrupts actually taken, as opposed to ticks believed to have passed.
 *
 * These two numbers are the same thing today and stop being the same thing the
 * moment a processor is allowed to stop its tick: `ticks` is resynced from the
 * hardware clock on waking, because the timer wheel is turned by comparing
 * itself against it, so it goes on counting through any amount of idleness.
 * That is correct and it is also why `ticks` cannot measure what a tickless
 * idle is *for*.
 *
 * This one is never resynced. It counts wakeups, which is what a stopped tick
 * saves and what a warm laptop is made of. Every processor's interrupt counts
 * here, not just processor 0's -- a secondary spinning its own timer costs the
 * same power as the boot processor doing it. */
static volatile u64 tick_interrupts;

/* There is no resync any more, and that is the fix rather than a tidy-up.
 *
 * A count derived from the clock cannot fall behind it, so there is nothing to
 * catch up; `time_tick_resync` existed only because the count was kept
 * separately, and keeping it separately is what let it drift. See time_ticks().
 */

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
	/* Before the processor-0 test, deliberately: this is a count of
	 * interrupts taken by the machine, and every processor's timer costs
	 * the same power. `ticks` below is a count of intervals that have
	 * passed, of which there is one regardless of how many processors
	 * noticed -- which is KF's reason for the test in the first place. */
	tick_interrupts++;

	if (arch_cpu_id() != 0)
		return;

	/* And the timer wheel, turned by the same processor for the same reason:
	 * there is one wheel, and a wheel turned by four processors would run
	 * each slot four times. Everything filed on it therefore fires here, in
	 * interrupt context on processor 0 -- which is why deferred work exists
	 * and why timer.h says a callback must not do anything substantial. */
	timer_tick();
}

u64 time_ticks(void)
{
	/* **Derived, not counted.**
	 *
	 * This used to be a variable that `time_tick` incremented, which was
	 * right while the tick was the only thing that moved it. When a
	 * processor became able to suspend its tick, a second author appeared:
	 * a resync that set the count from the clock on waking. Each was
	 * correct and together they double-counted -- the resync moved the
	 * count up to the clock, the next interrupt added one more, and the
	 * resync would not move it back because it only ever went forward.
	 *
	 * Every idle-and-wake cycle therefore added a tick that no time had
	 * passed for, the wheel outran the clock, and filed timers fired early:
	 * a 100 ms deadline arriving after 53 ms. (KF-204)
	 *
	 * One fact, one author. The hardware counter is the authority and this
	 * is that counter in coarser units. */
	return arch_monotonic_ns() / (1000000000ull / TIME_TICK_HZ);
}

u64 time_tick_interrupts(void)
{
	return tick_interrupts;
}

u64 time_monotonic_ns(void)
{
	return arch_monotonic_ns();
}

/* What the firmware said the time was, and when it said it.
 *
 * Zero until somebody asks, and zero for ever on a machine with no UEFI. */
static u64 firmware_wall_ns;
static u64 firmware_wall_at;

/* Days into the year at the first of each month, for a non-leap year. */
static const u16 month_start[12] = {
	0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

static u64 days_since_epoch(unsigned y, unsigned m, unsigned d)
{
	u64 days = 0;
	unsigned year;

	if (y < 1970 || m < 1 || m > 12 || d < 1)
		return 0;

	for (year = 1970; year < y; year++)
		days += (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
			? 366 : 365;

	days += month_start[m - 1];

	/* This year's leap day, but only once March has been reached. A
	 * February date in a leap year must not be moved forward by the leap
	 * day it precedes. */
	if (m > 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
		days += 1;

	return days + (d - 1);
}

/* Ask the firmware what time it is, once, while its own mappings are still
 * the ones in force.
 *
 * **Must run before vm_init.** The ReconBoot path does not build an address
 * space -- it adds one entry to the firmware's -- so until the kernel replaces
 * those tables, firmware runtime code is mapped where firmware expects to find
 * it. After that it is not, and reaching it would mean SetVirtualAddressMap,
 * which can be called once and cannot be undone.
 *
 * Returns false where there is nothing to ask, which is every BIOS machine and
 * is not a failure.
 */
bool time_capture_firmware_clock(void)
{
	const EFI_RUNTIME_SERVICES *rt;
	EFI_TIME t;
	u64 days;

	rt = (const EFI_RUNTIME_SERVICES *)(uintptr_t)
		boot_info()->runtime_services;

	if (!rt || !rt->GetTime)
		return false;

	if (rt->GetTime(&t, 0) != EFI_SUCCESS)
		return false;

	/* A firmware that answers with a year it cannot have meant is not a
	 * clock. Refused rather than turned into a plausible date -- the whole
	 * reason time_wall_ns returns zero on a machine with no clock is that a
	 * confident wrong date is worse than an admitted absent one. */
	if (t.Year < 1970 || t.Year > 2200 || t.Month < 1 || t.Month > 12 ||
	    t.Day < 1 || t.Day > 31 || t.Hour > 23 || t.Minute > 59 ||
	    t.Second > 59)
		return false;

	days = days_since_epoch(t.Year, t.Month, t.Day);
	if (!days)
		return false;

	firmware_wall_ns = ((days * 24 + t.Hour) * 60ull + t.Minute) * 60ull;
	firmware_wall_ns = (firmware_wall_ns + t.Second) * 1000000000ull;
	firmware_wall_at = arch_monotonic_ns();

	return true;
}

/* --- the wall clock, carried on the monotonic counter (KF-251) -------------
 *
 * `arch_wall_ns` on x86_64 reads the CMOS RTC, whose finest field is
 * **seconds**. So it answers a whole number of seconds multiplied by a billion,
 * and `SYS_WALLTIME` -- which is declared in nanoseconds -- moved once a
 * second for as long as it existed. The server session found it building an
 * NTP client: both of a round trip's local timestamps land in the same second,
 * the delay computes to zero, and `round trip 0 ms` to a server across the
 * internet is quantisation wearing the clothes of a measurement.
 *
 * **The fix was already in this function.** The branch below it -- for a
 * machine with no CMOS at all -- latches what the firmware said against the
 * monotonic counter and adds the difference, which has exactly the resolution
 * the interface promises. The coarse clock was preferred over the fine one
 * whenever a coarse clock was present. That is the whole fault.
 *
 * So the RTC stops being the clock and becomes the thing that *sets* it.
 */
static u64 wall_base_ns;	/* what the time was at the latch */
static u64 wall_base_at;	/* the monotonic counter at the latch */
static u64 wall_seen;		/* the last whole second the RTC showed */
static u64 wall_probed_at;	/* when the RTC was last consulted */
static u64 wall_fixups;		/* how many observations improved the phase */

/* How often the RTC is worth asking.
 *
 * **Not on every call, and that matters for the exact use that found this.**
 * `arch_wall_ns` spins while the RTC's update-in-progress bit is set -- up to
 * two milliseconds by specification -- and then does seven port reads. A
 * program timing a network round trip calls this twice in quick succession,
 * and a clock that costs two milliseconds to read cannot measure anything
 * shorter than that. Between probes the answer comes from the counter, which
 * is a register read.
 *
 * A quarter of a second is short enough that the second below is caught
 * turning promptly and long enough that the port I/O is not on any hot path. */
#define WALL_PROBE_NS 250000000ull

u64 time_wall_ns(void)
{
	u64 mono = arch_monotonic_ns();
	u64 coarse;

	/* Established, and not yet due another look: answer from the counter. */
	if (wall_base_ns && (mono - wall_probed_at) < WALL_PROBE_NS)
		return wall_base_ns + (mono - wall_base_at);

	coarse = arch_wall_ns();

	if (coarse) {
		wall_probed_at = mono;

		if (!wall_base_ns) {
			/* The first reading. The fraction is unknown -- the RTC
			 * says which second it is and nothing about where in it
			 * -- so this starts up to a second *behind* and never
			 * ahead, which is the safe direction and is corrected
			 * below. */
			wall_base_ns = coarse;
			wall_base_at = mono;
			wall_seen    = coarse;
		} else if (coarse != wall_seen) {
			/* **The second turned, and that instant is the one
			 * moment the fraction is known to be zero.**
			 *
			 * Chasing this edge at boot would cost up to a second of
			 * every boot. Watching for it instead costs nothing.
			 *
			 * **This clock is always late, never early**, and that
			 * is what makes the rule below safe. The reading says
			 * which second it is and nothing about where in it, so
			 * the first latch starts behind true time by the
			 * fraction it could not see, and adding a monotonic
			 * delta preserves exactly that lateness. Every later
			 * observation offers a *different* lateness -- however
			 * long after the turn it was noticed.
			 *
			 * So this keeps the best estimate seen: take the new one
			 * only when it is ahead of what is already being
			 * reported, which is precisely when it is less late.
			 * It converges toward the probe interval and stops. */
			u64 running = wall_base_ns + (mono - wall_base_at);

			wall_seen = coarse;

			/* **Forward only**, and the comparison above is what
			 * enforces it. A wall clock that steps backwards is a
			 * worse fault than the one being fixed here: a duration
			 * measured across the step comes out negative, and every
			 * caller computing `after - before` on unsigned
			 * nanoseconds gets a number near 2^64.
			 *
			 * Refusing the step rather than clamping the output
			 * keeps this function's answer a pure function of the
			 * counter, so it is monotonic by construction rather
			 * than by a comparison somebody could later remove. */
			if (coarse >= running) {
				wall_base_ns = coarse;
				wall_base_at = mono;
				wall_fixups++;
			}
		}

		return wall_base_ns + (mono - wall_base_at);
	}

	/* No clock this architecture knows how to read. The firmware told us
	 * once, before its mappings went away, and the monotonic counter has
	 * been running since -- so the date is that plus however long ago it
	 * was. */
	if (firmware_wall_ns)
		return firmware_wall_ns +
		       (arch_monotonic_ns() - firmware_wall_at);

	return 0;
}

/* How many observations of the turning second improved the phase.
 *
 * **Zero is a normal and correct answer, and reading it as "the branch never
 * ran" is the mistake this comment exists to prevent** -- it was very nearly
 * made here. The clock keeps the least-late estimate it has seen, so zero means
 * no later observation beat the first latch, which is what happens when the
 * first latch was already close to the turn.
 *
 * Measured on a boot: `arch_wall_ns` read 1,548,460 times over 1600 ms, its
 * value changed twice, and one of the two changes improved the phase. The other
 * was offered and correctly refused for being further from the turn than the
 * estimate already in hand.
 */
u64 time_wall_fixups(void)
{
	return wall_fixups;
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

	/* Where the clock comes from, before what it says.
	 *
	 * Both architectures have had a function for exactly this since their
	 * timers were written, and **neither was ever called from anywhere**.
	 * On the first machine whose counter could not be calibrated, that line
	 * would have said so directly above a monotonic reading of 0.000, and
	 * the diagnosis would have been on the screen rather than in a
	 * photograph of one. */
	arch_time_print_source();

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

		/* **What the clock's resolution actually is, measured.**
		 *
		 * The line above says the date and says nothing about how finely
		 * it can be read, and that gap is the whole of KF-251: the call
		 * behind it was declared in nanoseconds, delivered whole
		 * seconds, and every boot printed a date that looked perfectly
		 * correct. A resolution nobody measures is a resolution nobody
		 * can be wrong about.
		 *
		 * Two reads back to back. The number is small and noisy and is
		 * not meant to be precise -- it is meant to be *non-zero*, on
		 * every boot, where a clock counting whole seconds would show a
		 * flat zero almost every time. */
		{
			u64 a = time_wall_ns();
			u64 b = time_wall_ns();

			kprintf("  resolution   : two reads apart by %lu ns, "
				"phase corrected %lu time(s)\n",
				(u64)(b - a), time_wall_fixups());
		}
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

	/* --- and the wall clock has to have a fraction (KF-251) -------------
	 *
	 * This is the check that was missing, and its absence is why
	 * `SYS_WALLTIME` shipped declaring nanoseconds and delivering seconds.
	 * Nothing here ever looked below the decimal point, so a clock whose
	 * low nine digits were always zero passed every test the kernel had.
	 *
	 * **Asserted on the fraction, not on the difference.** Two reads far
	 * enough apart differ even with a one-second clock, eventually -- so a
	 * test comparing two readings passes on the broken clock about as often
	 * as the machine is slow, which is a test that measures the host. The
	 * fraction being non-zero is a property of one reading and cannot be
	 * satisfied by a clock that only counts seconds.
	 *
	 * Several reads, because a correct clock genuinely lands on a whole
	 * second sometimes -- about one read in a billion, and rather more than
	 * that on an emulator whose counter is coarse. One zero proves nothing;
	 * all of them zero is the fault.
	 */
	{
		u64 wall = time_wall_ns();

		if (!wall) {
			/* No clock at all is a normal answer on a machine with
			 * neither an RTC nor UEFI, and saying so is what keeps
			 * this from quietly measuring nothing. */
			kputs("  time: this machine has no wall clock, so its "
			      "resolution was not tested\n");
		} else {
			unsigned tries;
			bool fraction = false;

			for (tries = 0; tries < 8 && !fraction; tries++) {
				if (time_wall_ns() % 1000000000ull)
					fraction = true;

				for (volatile unsigned i = 0; i < 50000; i++)
					;
			}

			if (!fraction) {
				kputs("  time: the wall clock is declared in "
				      "nanoseconds and its low nine digits were "
				      "zero on eight reads -- it is counting "
				      "whole seconds\n");
				ok = false;
			}
		}
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
	 * what this is for.
	 *
	 * --- and it counted the wrong thing for two days (KF-235) ------------
	 *
	 * It asked `time_ticks()`. That was the interrupt's own count when this
	 * was written, and KF-204 made it `arch_monotonic_ns() / tick`, derived
	 * from the very counter the loop below times itself against. Substitute
	 * and the whole check collapses:
	 *
	 *     rate = (dmono / tick) * 1e9 / dmono  ==  1e9 / tick  ==  TICK_HZ
	 *
	 * **Exactly TIME_TICK_HZ, on every machine, whatever the hardware is
	 * doing.** The one thing this exists to catch -- a tick arriving at some
	 * rate other than the one every conversion in the kernel assumes -- had
	 * become the one thing it could not report. A 201 Hz tick would pass it
	 * now, which is the fault it was written for, by name, in the paragraph
	 * above.
	 *
	 * `time_tick_interrupts()` is the interrupt's own count and has been all
	 * along; `smp.c` and `power.c` already ask it whether the tick is alive.
	 * Nothing asked it how fast. It does now. */
	{
		u64 t0 = time_tick_interrupts();
		u64 n0 = time_monotonic_ns();
		u64 elapsed, rate;

		while (time_monotonic_ns() - n0 < 200000000ULL)
			arch_cpu_relax();

		elapsed = time_monotonic_ns() - n0;
		rate = (time_tick_interrupts() - t0) * 1000000000ULL / elapsed;

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
