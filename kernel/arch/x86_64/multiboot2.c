/* Multiboot2, as GRUB speaks it.
 *
 * This is scaffolding, and is meant to be. The project's aim is to boot itself
 * from its own bootloader on both BIOS and UEFI machines; GRUB is here so that
 * there is something to compare against and something to boot from while that
 * is being written. The value it provides in the meantime is real, though:
 * GRUB will tell us whether it was itself started by a BIOS or by UEFI, which
 * is the same question our own loader will have to answer.
 */
#include "x86_64.h"

#include <recon/kernel/boot.h>
#include <recon/kernel/compiler.h>
#include <recon/kernel/kstring.h>

/* Tag types. Only the ones that are read are named. */
#define MB2_TAG_END              0
#define MB2_TAG_CMDLINE          1
#define MB2_TAG_LOADER_NAME      2
#define MB2_TAG_MMAP             6
#define MB2_TAG_EFI32_SYSTABLE  11
#define MB2_TAG_EFI64_SYSTABLE  12
#define MB2_TAG_ACPI_RSDP_V1    14
#define MB2_TAG_ACPI_RSDP_V2    15
#define MB2_TAG_EFI_MMAP        17
#define MB2_TAG_EFI32_HANDLE    19
#define MB2_TAG_EFI64_HANDLE    20

struct mb2_tag {
	u32 type;
	u32 size;
} RK_PACKED;

struct mb2_mmap_tag {
	u32 type;
	u32 size;
	u32 entry_size;
	u32 entry_version;
} RK_PACKED;

struct mb2_mmap_entry {
	u64 addr;
	u64 len;
	u32 type;
	u32 reserved;
} RK_PACKED;

/* The strings GRUB hands over live in memory it owns. Copied out, because
 * that memory becomes reclaimable and a pointer into it would rot. */
static char cmdline_copy[256];
static char loader_copy[64];

bool mb2_parse(u32 info_phys)
{
	const u8 *base = (const u8 *)(uintptr_t)info_phys;
	u32 total_size;
	bool saw_efi = false;

	if (!info_phys)
		return false;

	total_size = *(const u32 *)base;
	if (total_size < 8 || total_size > (16u << 20))
		return false;

	/* Firmware is decided after the walk, because the evidence for UEFI is
	 * the presence of a tag rather than the value of one. */
	boot_info_reset("Multiboot2", BOOT_FIRMWARE_UNKNOWN);

	for (u32 off = 8; off + sizeof(struct mb2_tag) <= total_size;) {
		const struct mb2_tag *tag = (const struct mb2_tag *)(base + off);

		if (tag->type == MB2_TAG_END)
			break;
		if (tag->size < sizeof(struct mb2_tag))
			break;	/* malformed: stop rather than loop forever */

		switch (tag->type) {
		case MB2_TAG_CMDLINE:
			kstrlcpy(cmdline_copy, (const char *)(tag + 1),
				 sizeof(cmdline_copy));
			boot_info()->cmdline = cmdline_copy;
			break;

		case MB2_TAG_LOADER_NAME:
			kstrlcpy(loader_copy, (const char *)(tag + 1),
				 sizeof(loader_copy));
			boot_info()->loader = loader_copy;
			break;

		case MB2_TAG_MMAP: {
			const struct mb2_mmap_tag *m = (const struct mb2_mmap_tag *)tag;

			/* entry_size is read from the tag rather than assumed to be
			 * sizeof(entry): the specification says it may grow, and a
			 * kernel that steps by its own idea of the size would walk
			 * off into nonsense on a future loader. */
			if (m->entry_size < sizeof(struct mb2_mmap_entry))
				break;

			for (u32 e = sizeof(*m); e + m->entry_size <= m->size;
			     e += m->entry_size) {
				const struct mb2_mmap_entry *entry =
					(const struct mb2_mmap_entry *)((const u8 *)tag + e);

				x86_add_e820_region(entry->addr, entry->len, entry->type);
			}
			break;
		}

		case MB2_TAG_ACPI_RSDP_V1:
		case MB2_TAG_ACPI_RSDP_V2:
			/* The tag contains a copy of the table, not a pointer to it,
			 * so the address recorded is the copy's. */
			boot_info()->acpi_rsdp = (paddr_t)(uintptr_t)(tag + 1);
			break;

		case MB2_TAG_EFI32_SYSTABLE:
		case MB2_TAG_EFI64_SYSTABLE:
		case MB2_TAG_EFI32_HANDLE:
		case MB2_TAG_EFI64_HANDLE:
		case MB2_TAG_EFI_MMAP:
			saw_efi = true;
			break;

		default:
			break;
		}

		/* Tags are padded to 8 bytes. */
		off += (tag->size + 7) & ~7u;
	}

	/* GRUB emits EFI tags only when EFI is what started it. Their absence,
	 * on a machine that produced a Multiboot2 memory map at all, means the
	 * loader came up through a legacy BIOS. */
	boot_info()->firmware = saw_efi ? BOOT_FIRMWARE_UEFI : BOOT_FIRMWARE_BIOS;

	return true;
}
