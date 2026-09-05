/* Internal to arch/aarch64. Nothing above arch/ includes this. */
#ifndef RECON_ARCH_AARCH64_H
#define RECON_ARCH_AARCH64_H

#include <recon/kernel/types.h>

/* Saved by boot.S from x0, where the firmware left it. */
extern u64 arch_dtb_pointer;

/* Set by reconboot_entry from x0. Zero when the firmware started us directly. */
extern u64 reconboot_handoff;

/* Where the first PL011 UART is on QEMUs virt machine and on a great many ARM
 * boards, because the address came from the same reference design. Hardcoded
 * until there is a device tree parser that can report why it failed; needed
 * here as well as in arch.c because the console has to be mapped before the
 * kernel switches to its own translation tables. */
#define PL011_BASE 0x09000000UL

/* Reads the memory and the command line out of a flattened device tree.
 * Returns false if the blob is not one. */
bool fdt_parse(u64 dtb_phys);

#endif /* RECON_ARCH_AARCH64_H */
