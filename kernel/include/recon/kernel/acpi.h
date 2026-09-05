/* ACPI: the other way a machine describes itself.
 *
 * A device tree is a blob the firmware hands over saying what exists. ACPI is a
 * set of tables the firmware leaves in memory saying the same thing, reached
 * through a pointer the firmware publishes. Which one a machine has is decided
 * by its firmware, not by its architecture: an x86 PC has ACPI, an ARM board
 * booted from U-Boot has a device tree, and an ARM server booted through UEFI
 * has ACPI. This kernel meets all three.
 *
 * --- What is implemented, and what is deliberately not ---
 *
 * The tables are a directory: a root pointer leads to a list of tables, each
 * with a four-character signature saying what it is. Finding one by signature
 * and checking that it has not been corrupted is what this file does, and it is
 * enough for everything this kernel needs from ACPI so far:
 *
 *   MCFG  where PCI configuration space is memory-mapped
 *   MADT  which processors exist, and where their interrupt controllers are
 *
 * What is *not* implemented is AML -- the bytecode in the DSDT that describes
 * power management, hotplug and most of the machine's real configuration.
 * Interpreting it means writing an interpreter for a language, and nothing
 * here needs one yet. When something does, it will be a separate piece of work
 * with its own name, and not something that grew quietly out of a table walker.
 */
#ifndef RECON_KERNEL_ACPI_H
#define RECON_KERNEL_ACPI_H

#include <recon/kernel/types.h>
#include <recon/kernel/compiler.h>

/* The header every table starts with. The length includes the header, and is
 * what the checksum covers -- a table whose length is wrong is a table whose
 * checksum will not match, which is the point. */
struct acpi_sdt_header {
	char signature[4];
	u32  length;
	u8   revision;
	u8   checksum;
	char oem_id[6];
	char oem_table_id[8];
	u32  oem_revision;
	u32  creator_id;
	u32  creator_revision;
} RK_PACKED;

/* Finds the tables from the root pointer the firmware published. Safe to call
 * with zero, which is what a machine with no ACPI reports. */
void acpi_init(paddr_t rsdp_phys);

/* True when there are tables to look in. */
bool acpi_available(void);

/* The table with this signature, checksum-verified, or null.
 *
 * Signatures are four characters and are *not* NUL-terminated in the table, so
 * this takes exactly four and compares exactly four -- passing a C string with
 * its terminator would compare five bytes against four and never match. */
const struct acpi_sdt_header *acpi_find(const char signature[4]);

void acpi_print_summary(void);

/* --- What is read out of the tables ---------------------------------------
 *
 * Kept here rather than in the caller so that the knowledge of a table's shape
 * lives beside the code that validates it. */

/* Where PCI configuration space is memory-mapped, from MCFG. False on a machine
 * whose firmware does not publish one, which includes every PC that expects the
 * two-I/O-port mechanism to be used instead. */
bool acpi_pci_ecam(u64 *base, u8 *start_bus, u8 *end_bus);

#endif /* RECON_KERNEL_ACPI_H */
