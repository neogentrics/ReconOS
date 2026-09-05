/* Reading what our own bootloader left us.
 *
 * Portable, and that is the point: ReconBoot is one protocol on every
 * architecture, so unlike Multiboot2 and the device tree this needs no
 * per-machine translation. The loader has already done the translating, on the
 * far side of the boundary, while it still had a console to report a mistake
 * on.
 *
 * The structure is shared with boot/include/reconboot.h -- one file, included
 * by two compilers producing two different ABIs, which is why every field in it
 * is explicitly sized and there is not a function pointer or a bitfield in it.
 */
#include <recon/kernel/boot.h>
#include <recon/kernel/kstring.h>

#include <reconboot.h>

static char loader_name[32];
static char cmdline_copy[128];

static enum mem_kind translate_kind(uint32_t kind)
{
	switch (kind) {
	case RECONBOOT_MEM_USABLE:       return MEM_USABLE;
	case RECONBOOT_MEM_ACPI_RECLAIM: return MEM_ACPI_RECLAIM;
	case RECONBOOT_MEM_ACPI_NVS:     return MEM_ACPI_NVS;
	case RECONBOOT_MEM_BAD:          return MEM_BAD;
	case RECONBOOT_MEM_BOOTLOADER:   return MEM_BOOTLOADER;
	case RECONBOOT_MEM_RESERVED:
	default:                         return MEM_RESERVED;
	}
}

bool reconboot_parse(paddr_t handoff)
{
	const struct reconboot *bi = (const struct reconboot *)(uintptr_t)handoff;
	const struct reconboot_region *regions;

	if (!handoff || bi->magic != RECONBOOT_MAGIC)
		return false;

	/* A version we do not know is a structure whose fields may have moved.
	 * Refused rather than read optimistically: the failure of reading it
	 * anyway would be a memory map that is subtly wrong, which is the worst
	 * kind of wrong there is at this point in the boot. */
	if (bi->version != RECONBOOT_VERSION)
		return false;

	boot_info_reset("ReconBoot", BOOT_FIRMWARE_UEFI);

	kstrlcpy(loader_name, bi->loader, sizeof(loader_name));
	boot_info()->loader = loader_name;

	if (bi->cmdline[0]) {
		kstrlcpy(cmdline_copy, bi->cmdline, sizeof(cmdline_copy));
		boot_info()->cmdline = cmdline_copy;
	}

	boot_info()->acpi_rsdp = (paddr_t)bi->acpi_rsdp;
	boot_info()->dtb       = (paddr_t)bi->dtb;

	if (bi->framebuffer.width && bi->framebuffer.format != RECONBOOT_PIXEL_NONE) {
		boot_info()->fb.base   = (paddr_t)bi->framebuffer.base;
		boot_info()->fb.size   = bi->framebuffer.size;
		boot_info()->fb.width  = bi->framebuffer.width;
		boot_info()->fb.height = bi->framebuffer.height;
		boot_info()->fb.pitch  = bi->framebuffer.pitch;
		boot_info()->fb.format =
			bi->framebuffer.format == RECONBOOT_PIXEL_BGRA
				? FB_FORMAT_BGRA : FB_FORMAT_RGBA;

		/* The framebuffer is device memory, not RAM, and the allocator
		 * must never hand it out. Recording it here rather than relying
		 * on the firmware's map to have marked it: it usually does, and
		 * "usually" is not a basis for deciding what memory is safe to
		 * hand a program. */
		boot_add_region(boot_info()->fb.base, boot_info()->fb.size,
				MEM_RESERVED);
	}

	regions = (const struct reconboot_region *)(uintptr_t)bi->regions;
	if (regions)
		for (uint32_t i = 0; i < bi->region_count; i++)
			boot_add_region((paddr_t)regions[i].base,
					regions[i].size,
					translate_kind(regions[i].kind));

	return true;
}
