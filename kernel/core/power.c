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

#include <recon/kernel/acpi.h>
#include <recon/kernel/aml.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>

/* Bit 13 of the control register: "go now". The sleep type occupies bits 10
 * to 12, which is why the value is shifted there rather than written plainly. */
#define SLP_EN		(1u << 13)
#define SLP_TYP_SHIFT	10

enum power_result power_off(void)
{
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

	/* The write does not return on a machine that obeys it. Reaching here
	 * means the machine declined, which is a fact worth reporting rather
	 * than a reason to spin. */
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
