/* Reading the machine's own description of itself.
 *
 * ACPI's fixed tables say where the fixed hardware is. Everything else -- which
 * devices exist, what resources they use, how to turn the machine off -- is in
 * the DSDT, and the DSDT is not a table. It is **bytecode**, in a language
 * called AML, compiled by the machine's vendor from a source language nobody
 * ships. There is no way to read it except to interpret it.
 *
 * --- What this does, and what it deliberately does not ---
 *
 * It **builds the namespace**: the tree of scopes, devices and named values the
 * bytecode declares. That is what answers "does this machine have a PS/2
 * controller" and "what is the value that turns it off".
 *
 * It **does not execute methods**. A Method is a subroutine with control flow,
 * arithmetic, and reads and writes of hardware registers through operation
 * regions; running one means an evaluator, an object model with reference
 * counting, and a way to touch hardware from the middle of it. Method bodies
 * are located and stepped over.
 *
 * That split is not arbitrary. It is the line between *what the machine is*,
 * which is declared, and *what the machine does*, which is executed -- and
 * everything this kernel needs today is on the declared side. Where it is not,
 * this says so rather than returning a plausible answer.
 *
 * **And it stops rather than guessing.** AML has no length prefix on most
 * opcodes, so an opcode this does not know cannot be skipped: the next byte
 * might be an operand or might be the next term, and there is no way to tell.
 * A parser that guessed would walk into the middle of an instruction and
 * report whatever it found there as a device. This one stops and records where,
 * so the summary can say the namespace is partial instead of implying it is
 * complete.
 */
#ifndef RECON_KERNEL_AML_H
#define RECON_KERNEL_AML_H

#include <recon/kernel/types.h>

/* S0 through S5. Indexed by the digit, so the zeroth is never used and
 * `sleep[3]` is the state spelled \_S3. */
#define AML_SLEEP_STATES 6

#define AML_MAX_DEVICES 64
#define AML_NAME_LEN    5	/* four characters and a terminator */
#define AML_HID_LEN     16

struct aml_device {
	char name[AML_NAME_LEN];	/* its own name segment */
	char hid[AML_HID_LEN];		/* _HID, decoded; empty if it has none */
};

struct aml_state {
	bool parsed;

	/* Where the walk stopped, and why. Zero when it reached the end. A
	 * partial namespace is useful and must not be mistaken for a whole
	 * one. */
	u32 stopped_at;
	u8  stopped_on_opcode;

	unsigned devices;
	struct aml_device device[AML_MAX_DEVICES];

	/* The sleep states this machine declares, indexed by their number.
	 *
	 * Each is a package of small integers naming what to write to the two
	 * power-management control registers. \_S5 is "off" -- without it a
	 * kernel can only halt, which leaves the machine drawing power with the
	 * fans running. \_S3 is "suspend to RAM", and it is the same shape,
	 * which is why this is a table rather than a second pair of fields.
	 *
	 * Indexed 0..5 with 0 unused, so `sleep[3]` is \_S3 and reads the way it
	 * is spelled. A state the machine does not declare has `have` false, and
	 * that is a fact about the machine rather than a failure to parse. */
	struct aml_sleep_state {
		bool have;
		u8 typ_a;
		u8 typ_b;
	} sleep[AML_SLEEP_STATES];

	unsigned names;			/* how many named objects were seen */
	unsigned methods;		/* how many method bodies were stepped over */
};

/* Parses the DSDT the FADT points at. Safe on a machine with neither. */
void aml_init(void);

const struct aml_state *aml(void);

/* Whether a device with this hardware identifier was declared -- "PNP0303" for
 * a keyboard controller, "PNP0F13" for a mouse. Case-sensitive, as the
 * identifiers are. */
bool aml_has_device(const char *hid);

void aml_print_summary(void);

#endif /* RECON_KERNEL_AML_H */
