/* Internal to arch/x86_64. Nothing above arch/ includes this. */
#ifndef RECON_ARCH_X86_64_H
#define RECON_ARCH_X86_64_H

#include <recon/kernel/types.h>

/* Set by boot.S before anything else runs. */
extern u32 boot_protocol;	/* 1 = Multiboot2, 2 = PVH */
extern u32 boot_info_phys;
extern u32 boot_magic;

#define BOOT_PROTOCOL_MULTIBOOT2 1
#define BOOT_PROTOCOL_PVH        2

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u
#define PVH_START_INFO_MAGIC        0x336ec578u

/* Each translates one boot protocol's account of the machine into the kernel's
 * own. Returns false if the structure did not look like what it claimed. */
bool mb2_parse(u32 info_phys);
bool pvh_parse(u32 info_phys);

/* E820's numbering, which both protocols above borrow. */
void x86_add_e820_region(u64 base, u64 len, u32 e820_type);

#endif /* RECON_ARCH_X86_64_H */
