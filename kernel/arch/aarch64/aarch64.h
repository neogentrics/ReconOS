/* Internal to arch/aarch64. Nothing above arch/ includes this. */
#ifndef RECON_ARCH_AARCH64_H
#define RECON_ARCH_AARCH64_H

#include <recon/kernel/types.h>

/* Saved by boot.S from x0, where the firmware left it. */
extern u64 arch_dtb_pointer;

/* Reads the memory and the command line out of a flattened device tree.
 * Returns false if the blob is not one. */
bool fdt_parse(u64 dtb_phys);

#endif /* RECON_ARCH_AARCH64_H */
