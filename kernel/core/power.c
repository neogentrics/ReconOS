/* Turning the machine off.
 *
 * Until this existed the kernel could only *halt*: stop executing, with the
 * power supply still on, the fans still turning and the machine still warm. A
 * person who asks a computer to shut down and watches it sit there lit up has
 * been told the operating system does not work, and they are not wrong.
 *
 * It takes three separate pieces of knowledge, which is why it arrives now
 * rather than earlier and why the two before it had to be built first:
 *
 *   WHERE the power management control register is. That is in the FADT.
 *
 *   WHAT to write into it. That is in the DSDT, as bytecode, under the name
 *   \_S5 -- a package whose first two values are the sleep-type codes for
 *   "off" on this particular machine. There is no standard value; every
 *   chipset has its own, which is the whole reason it is in the firmware's
 *   description rather than in a specification.
 *
 *   HOW to write it, which is an I/O port on one architecture and does not
 *   exist on the other.
 *
 * --- What it does not do ---
 *
 * It does not run \_PTS, the method firmware expects to be called before a
 * sleep transition so it can prepare. Running it means executing AML, which
 * aml.c deliberately does not do. On the machines this has been tried on the
 * transition works without it; on a machine where it does not, the symptom
 * will be a shutdown that hangs, and this paragraph is where to start.
 */
#include <recon/kernel/power.h>
#include <recon/kernel/identity.h>

#include <recon/kernel/acpi.h>
#include <recon/kernel/aml.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>
#include <recon/kernel/time.h>
#include <recon/kernel/timer.h>

/* --- an idle that does not wake for nothing --------------------------------
 *
 * See power.h. The counters are here rather than in the architecture because
 * every architecture would otherwise keep its own and they would disagree.
 */

/* The longest this will sleep with nothing at all filed.
 *
 * Not "for ever", deliberately. Preemption comes from the tick, so a processor
 * that has suspended its own tick has also suspended the thing that would
 * notice work arriving. One second turns a hundred wakeups a second into one --
 * which is the whole of the saving to three significant figures -- and leaves
 * no way to build a machine that has stopped listening. */
#define IDLE_CEILING_NS		1000000000ull

static u64 idle_tickless;	/* times the tick was actually suspended */
static u64 idle_ordinary;	/* times it could not be, and we waited anyway */

void power_idle_wait(void)
{
	u64 deadline_ticks;
	u64 now = time_monotonic_ns();
	u64 deadline = now + IDLE_CEILING_NS;

	/* The next filed timer, if there is one and if it lands sooner than the
	 * ceiling. `timer_next_deadline` answers in ticks since boot, which is
	 * the wheel's own unit; the architecture wants nanoseconds. */
	if (timer_next_deadline(&deadline_ticks)) {
		u64 when = deadline_ticks * (1000000000ull / TIME_TICK_HZ);

		if ((i64)(when - deadline) < 0)
			deadline = when;
	}

	if (arch_wait_tickless(deadline)) {
		idle_tickless++;

		/* The wheel is turned by comparing itself against the tick
		 * count, and the tick count only moves when the tick fires. So
		 * a processor coming back from a suspended tick owes the wheel
		 * however many intervals passed while it was not looking --
		 * without this, a stopped tick is a wheel that never turns
		 * again and every filed timer simply never runs.
		 *
		 * Processor 0 only, and the function says why. */
		if (time_tick_resync())
			timer_tick();

		return;
	}

	idle_ordinary++;
	arch_wait_for_interrupt();
}


/* Does the tick actually stop?
 *
 * Sleeping this thread is what makes the processor idle, so the sleep is both
 * the thing being timed and the thing that causes the condition. A fixed tick
 * costs one wakeup every ten milliseconds; a suspended one costs the wakeups
 * of whatever else the machine is doing, plus the one that ends the sleep.
 *
 * The threshold is deliberately generous. This is a shared machine under an
 * emulator, other processors are running their own idle loops, and devices
 * interrupt when they please -- so the claim is *far fewer than a fixed tick
 * would cost*, not an exact count. A test that demanded exactly one would fail
 * for reasons that have nothing to do with the tick.
 */
bool power_idle_self_test(void)
{
	const u64 nap_ns = 200000000ull;	/* 200 ms */
	u64 before_wakeups, before_ns, before_tickless;
	u64 wakeups, elapsed, would_have_been;

	before_wakeups  = time_tick_interrupts();
	before_tickless = power_idle_tickless();
	before_ns       = time_monotonic_ns();

	if (!timer_sleep_ns(nap_ns)) {
		kputs("power: this thread cannot sleep, so nothing can be "
		      "measured about idling\n");
		return false;
	}

	elapsed  = time_monotonic_ns() - before_ns;
	wakeups  = time_tick_interrupts() - before_wakeups;

	/* The sleep has to have actually happened. A timer that returned early
	 * would make the wakeup count look wonderful for the wrong reason --
	 * which is the same trap as a mode sweep that skipped every shape. */
	if (elapsed < nap_ns / 2) {
		kprintf("power: asked to sleep %lu ms and slept %lu -- nothing "
			"was measured\n",
			(unsigned long)(nap_ns / 1000000),
			(unsigned long)(elapsed / 1000000));
		return false;
	}

	would_have_been = (elapsed * TIME_TICK_HZ) / 1000000000ull;

	/* A machine that cannot suspend its tick is a normal machine. Saying so
	 * is what keeps this from being a test that quietly measures nothing. */
	if (power_idle_tickless() == before_tickless) {
		kprintf("power: the tick was not suspended on this machine; "
			"%lu wakeup(s) in %lu ms\n",
			(unsigned long)wakeups,
			(unsigned long)(elapsed / 1000000));
		return true;
	}

	kprintf("power: %lu wakeup(s) in %lu ms, against %lu a fixed tick "
		"would have cost\n",
		(unsigned long)wakeups, (unsigned long)(elapsed / 1000000),
		(unsigned long)would_have_been);

	/* Half is a long way from the real figure and a long way from noise. */
	if (wakeups * 2 >= would_have_been) {
		kputs("power: the tick was suspended and cost no fewer wakeups, "
		      "which means it did not stay suspended\n");
		return false;
	}

	return true;
}

/* How many times the tick was suspended, for the self-test. */
u64 power_idle_tickless(void)
{
	return idle_tickless;
}


/* Bit 13 of the control register: "go now". The sleep type occupies bits 10
 * to 12, which is why the value is shifted there rather than written plainly. */
#define SLP_EN		(1u << 13)
#define SLP_TYP_SHIFT	10

enum power_result power_off(void)
{
	/* Stopping the machine is a privileged act, and now there is somewhere
	 * to ask. The kernel holds this; a program has to have been given it.
	 *
	 * Its own result rather than a generic refusal, so that a caller which
	 * was not allowed can tell that apart from a machine that declined --
	 * the distinction KF-162 was about. */
	if (!capable(CAP_SHUTDOWN))
		return POWER_REFUSED;

	struct acpi_fadt_facts fadt;
	const struct aml_state *a = aml();

	/* The architecture's own way first, where it has one.
	 *
	 * This is not a preference between two equal mechanisms. ACPI is how an
	 * x86 machine is turned off and is *absent* on an ARM machine booted
	 * from a device tree -- so without this the kernel passed every test on
	 * aarch64 and then sat there with the power on, which is what a person
	 * calls broken. It does not return if it works. */
	if (arch_power_off())
		return POWER_REFUSED;

	if (!acpi_fadt(&fadt) || !fadt.pm1a_control)
		return POWER_NO_REGISTER;

	if (!a->have_s5)
		return POWER_NO_SLEEP_STATE;

	/* The second register first, where there is one.
	 *
	 * A machine with two control blocks needs both written, and the
	 * transition happens on the write to the *first*. Writing them the
	 * other way round starts the transition while the second half of the
	 * machine has not been told, which is a shutdown that works on the
	 * machines with one block and hangs on the machines with two -- and
	 * every machine anybody tests on has one. */
	if (fadt.pm1b_control)
		arch_acpi_write_control(fadt.pm1b_control,
					(u16)(((u32)a->s5_typ_b << SLP_TYP_SHIFT) |
					      SLP_EN));

	if (!arch_acpi_write_control(fadt.pm1a_control,
				     (u16)(((u32)a->s5_typ_a << SLP_TYP_SHIFT) |
					   SLP_EN)))
		return POWER_UNSUPPORTED;

	/* And then it waits, which this did not do and needed to.
	 *
	 * The comment here used to say that the write does not return on a
	 * machine that obeys it, so reaching the next line meant the machine
	 * had declined. That is true of the *instruction* and not of the
	 * machine: the write completes and the processor carries on, and the
	 * transition happens somewhere between one instruction and the next.
	 * How many instructions fit in that gap is a property of the hardware
	 * and, under emulation, of how busy the host is.
	 *
	 * So a loaded machine reported that its firmware had refused to turn
	 * off while it was in the middle of turning off. Found by the
	 * verification run, which boots four guests at once, and never by a
	 * boot on its own -- eight of eight of those were clean. Same family
	 * as KF-160: a timing assumption that held until the machine got
	 * busier. (KF-162)
	 *
	 * A fifth of a second, against a clock that does not depend on the
	 * tick. Long enough that no real machine is still deciding, short
	 * enough that one which genuinely cannot turn itself off still says
	 * so while somebody is watching. */
	{
		u64 deadline = arch_monotonic_ns() + 200000000ull;

		while (arch_monotonic_ns() < deadline)
			arch_cpu_relax();
	}

	return POWER_REFUSED;
}

void power_off_or_say_why(void)
{
	enum power_result r;

	kputs("\nPowering off.\n");

	r = power_off();

	switch (r) {
	case POWER_NO_REGISTER:
		kputs("  power: this machine's firmware published no power "
		      "management register\n");
		break;
	case POWER_NO_SLEEP_STATE:
		kputs("  power: no _S5 in this machine's description, so "
		      "there is no value that means off\n");
		break;
	case POWER_UNSUPPORTED:
		kputs("  power: this architecture has no way to reach that "
		      "register\n");
		break;
	case POWER_REFUSED:
		kputs("  power: the machine was told to turn off and did "
		      "not\n");
		break;
	default:
		break;
	}
}
