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
#define AML_MAX_METHODS 192
#define AML_MAX_NAMES   256
#define AML_NAME_LEN    5	/* four characters and a terminator */
#define AML_HID_LEN     16

/* A method the table declares: what it is called, how many arguments it takes,
 * and where its body is.
 *
 * The body is a pointer into the DSDT rather than a copy. That table is ACPI
 * reclaim memory reached through `phys_to_virt`, mapped for the life of the
 * machine -- the same lifetime every other pointer in this file has, and the
 * reason nothing here owns any memory.
 *
 * `device` is the enclosing device's name where there is one, because a machine
 * has many methods called `_CRS` and the only thing that tells them apart is
 * what they are inside. */
struct aml_method {
	char name[AML_NAME_LEN];
	char device[AML_NAME_LEN];
	u8 args;			/* 0 to 7, from the flags byte */
	const u8 *body;
	u32 body_len;
};

struct aml_device {
	char name[AML_NAME_LEN];	/* its own name segment */
	char hid[AML_HID_LEN];		/* _HID, decoded; empty if it has none */
};

/* A named data object -- `Name (BUF0, Buffer () {...})` -- kept as a pointer
 * to the encoded object rather than a decoded copy.
 *
 * Sound only because the evaluator refuses to store to a name. A table built
 * once at boot describes what the machine *declared*; if a method could write
 * one, that table would be a stale answer from the first write onward. The
 * refusal is what makes reading these correct rather than probably correct, and
 * it is enforced in aml_eval.c's Store. */
struct aml_name {
	char name[AML_NAME_LEN];
	char device[AML_NAME_LEN];
	const u8 *object;		/* the encoded data object */
	u32 object_len;
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

	/* Conditional blocks walked past without entering.
	 *
	 * Not an error and not nothing: whatever they declare is missing from
	 * everything below, so a reader comparing this namespace against a
	 * machine needs the number to know how much of the table was not
	 * looked at. */
	unsigned conditionals;

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
	unsigned names_kept;
	struct aml_name name_obj[AML_MAX_NAMES];

	/* Every method the table declares, up to the cap, and how many there
	 * were. The two differ on a machine with more than AML_MAX_METHODS,
	 * and both are printed: a count that silently equals the cap is a
	 * machine whose namespace is larger than this kernel will admit. */
	unsigned methods;
	unsigned methods_kept;
	struct aml_method method[AML_MAX_METHODS];
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
