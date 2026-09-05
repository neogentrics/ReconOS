/* PVH: booted directly by a hypervisor, with no firmware underneath at all.
 *
 * Worth keeping distinct from the BIOS and UEFI cases rather than pretending
 * to be one of them. A PVH guest has no BIOS to call and no UEFI runtime
 * services to keep mapped, so "which firmware" has the honest answer "none",
 * and code that later wants to call firmware has to check.
 *
 * It exists here because it makes the build-and-boot loop about a second long:
 * QEMU reads the entry point out of an ELF note and jumps to it, with no disc
 * image in between.
 */
#include "x86_64.h"

#include <recon/kernel/boot.h>
#include <recon/kernel/compiler.h>
#include <recon/kernel/kstring.h>

struct hvm_start_info {
	u32 magic;
	u32 version;
	u32 flags;
	u32 nr_modules;
	u64 modlist_paddr;
	u64 cmdline_paddr;
	u64 rsdp_paddr;
	/* version 1 and later */
	u64 memmap_paddr;
	u32 memmap_entries;
	u32 reserved;
} RK_PACKED;

struct hvm_memmap_entry {
	u64 addr;
	u64 size;
	u32 type;	/* E820 numbering */
	u32 reserved;
} RK_PACKED;

static char cmdline_copy[256];

bool pvh_parse(u32 info_phys)
{
	const struct hvm_start_info *si =
		(const struct hvm_start_info *)(uintptr_t)info_phys;

	if (!info_phys || si->magic != PVH_START_INFO_MAGIC)
		return false;

	boot_info_reset("PVH", BOOT_FIRMWARE_PARAVIRT);
	boot_info()->loader = "hypervisor";

	if (si->cmdline_paddr) {
		kstrlcpy(cmdline_copy, (const char *)(uintptr_t)si->cmdline_paddr,
			 sizeof(cmdline_copy));
		boot_info()->cmdline = cmdline_copy;
	}

	if (si->rsdp_paddr)
		boot_info()->acpi_rsdp = (paddr_t)si->rsdp_paddr;

	/* The memory map arrived in version 1. A version 0 guest gets no map,
	 * which is a thing to report rather than to guess around. */
	if (si->version >= 1 && si->memmap_paddr && si->memmap_entries) {
		const struct hvm_memmap_entry *e =
			(const struct hvm_memmap_entry *)(uintptr_t)si->memmap_paddr;

		for (u32 i = 0; i < si->memmap_entries; i++)
			x86_add_e820_region(e[i].addr, e[i].size, e[i].type);
	}

	return true;
}
