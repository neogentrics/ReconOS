/* What the machine says it is.
 *
 * ACPI describes what the machine *has*. SMBIOS describes what it *is* --
 * manufacturer, model, serial number, and which firmware is in it. Two
 * different questions, two different tables, and this kernel could answer
 * neither about the machine as opposed to the processor.
 *
 * It matters for more than a summary line. `SYS_MACHINE` reports a processor
 * vendor and model today and has nothing to say about the machine around it, so
 * a desktop asking "what am I running on" gets an answer about the CPU. And a
 * bug report from a real machine is worth a great deal more when it names the
 * model it happened on.
 *
 * --- Why the entry point is an architecture's problem ---
 *
 * The table is *found* differently everywhere. On a PC it is anchored somewhere
 * in the sixty-four kilobytes below one megabyte and is found by scanning for a
 * signature; everywhere else it comes from a UEFI configuration table the
 * firmware hands over. The scan is machine-specific and so it lives in arch/;
 * the structures inside are the same on every machine and so they live in
 * core/, which is the line `make check-portable` enforces.
 */
#ifndef RECON_KERNEL_SMBIOS_H
#define RECON_KERNEL_SMBIOS_H

#include <recon/kernel/types.h>

#define SMBIOS_STRING_MAX 64

struct smbios_facts {
	bool present;

	char manufacturer[SMBIOS_STRING_MAX];
	char product[SMBIOS_STRING_MAX];
	char version[SMBIOS_STRING_MAX];
	char serial[SMBIOS_STRING_MAX];

	char bios_vendor[SMBIOS_STRING_MAX];
	char bios_version[SMBIOS_STRING_MAX];

	u8 major, minor;		/* the SMBIOS version itself */
	unsigned structures;		/* how many were walked */
};

/* Finds and reads the tables. Safe to call on a machine that has none, which
 * is reported rather than treated as a failure. */
void smbios_init(void);

/* What was found. False when there is nothing, which is different from a
 * machine whose fields are empty strings -- and the difference is worth
 * keeping, because one means "no tables" and the other means "the vendor left
 * them blank", which vendors do. */
bool smbios_read(struct smbios_facts *out);

void smbios_print_summary(void);

/* --- what the architecture provides --------------------------------------
 *
 * The physical address of the entry-point structure, or zero when this machine
 * has none or has not told us where it is. */
paddr_t arch_smbios_anchor(void);

#endif /* RECON_KERNEL_SMBIOS_H */
