/* What the machine was like when we arrived.
 *
 * Every architecture, and every boot protocol within an architecture, answers
 * the same questions in a different format: what memory exists, who loaded us,
 * what firmware is underneath. This header is the one answer the rest of the
 * kernel reads. The translating happens in arch/, once per protocol, and
 * nothing above this line ever learns that E820 or the device tree exist.
 *
 * It is filled in during arch_early_init() and never changes afterwards. The
 * memory map in particular describes the machine at the instant of boot; once
 * the page allocator owns memory, this is history, not state.
 */
#ifndef RECON_KERNEL_BOOT_H
#define RECON_KERNEL_BOOT_H

#include <recon/kernel/types.h>

/* What is underneath us. This is the question the project actually cares
 * about, because ReconOS intends to boot both kinds itself rather than ask
 * GRUB to do it. */
enum boot_firmware {
	BOOT_FIRMWARE_UNKNOWN = 0,
	BOOT_FIRMWARE_BIOS,		/* legacy PC BIOS: real mode, E820, no runtime services */
	BOOT_FIRMWARE_UEFI,		/* UEFI: GetMemoryMap, runtime services, GPT */
	BOOT_FIRMWARE_DEVICETREE,	/* firmware described the machine in an FDT */
	BOOT_FIRMWARE_PARAVIRT,		/* no firmware at all: the hypervisor placed us */
};

/* Deliberately not E820's numbering, nor UEFI's, nor the device tree's absence
 * of one. Each arch maps its own into these. */
enum mem_kind {
	MEM_USABLE = 0,		/* free for the allocator to take */
	MEM_RESERVED,		/* firmware or hardware owns it, forever */
	MEM_ACPI_RECLAIM,	/* ACPI tables; usable once they have been read */
	MEM_ACPI_NVS,		/* must be preserved across sleep */
	MEM_BAD,		/* firmware says this RAM is faulty */
	MEM_BOOTLOADER,		/* whoever loaded us is using it; reclaimable later */
	MEM_KERNEL,		/* the kernel image itself */
};

struct mem_region {
	paddr_t base;
	u64     size;
	enum mem_kind kind;
};

/* Fixed, because there is no allocator yet -- this structure has to exist
 * before memory management does, which is the whole chicken-and-egg of early
 * boot.
 *
 * It was 64, on the reasoning that "real machines report well under thirty
 * regions". That was true of every machine tested at the time and wrong the
 * first time our own UEFI loader ran: OVMF reported 86, and 68 of them were
 * dropped. Firmware fragments its map as it allocates, so the count reflects
 * how much the firmware did before handing over, not how much memory exists.
 *
 * 256 at 24 bytes each is 6KB of BSS, which is nothing, and the dropped-region
 * counter stays -- because the lesson is that this number was guessed once and
 * a guess that fails silently is worse than a large array. */
#define BOOT_MAX_REGIONS 256

/* A linear framebuffer, where the firmware left us one. Zero width means there
 * is none, which is the normal answer on a serial-only machine and is not an
 * error. `pitch` is bytes per row and is *not* width times four -- firmware
 * routinely pads rows, and deriving one from the other is the classic way to
 * get a picture that shears progressively down the screen. */
enum fb_format {
	FB_FORMAT_NONE = 0,
	FB_FORMAT_BGRA,
	FB_FORMAT_RGBA,
};

struct framebuffer {
	paddr_t base;
	u64 size;
	u32 width;
	u32 height;
	u32 pitch;
	enum fb_format format;
};

struct boot_info {
	enum boot_firmware firmware;
	const char *protocol;	/* "Multiboot2", "PVH", "Device Tree" */
	const char *loader;	/* what the bootloader called itself, if it said */
	const char *cmdline;

	unsigned region_count;
	unsigned regions_dropped;	/* non-zero means BOOT_MAX_REGIONS was too small */
	struct mem_region regions[BOOT_MAX_REGIONS];

	u64 usable_bytes;
	u64 total_bytes;

	struct framebuffer fb;

	paddr_t acpi_rsdp;	/* 0 if the firmware did not point at one */
	paddr_t dtb;		/* 0 if there is no device tree */
};

/* Reads the structure our own bootloader left us. Portable: the ReconBoot
 * protocol is the same on every architecture, which is most of why it exists.
 * Returns false if what was handed over is not one. */
bool reconboot_parse(paddr_t handoff);

/* The one copy. Written only by arch/ during early init. */
struct boot_info *boot_info(void);

/* --- Used by arch/ while translating -------------------------------------- */

void boot_info_reset(const char *protocol, enum boot_firmware firmware);
void boot_add_region(paddr_t base, u64 size, enum mem_kind kind);

/* Sorts by address and merges adjacent regions of the same kind, then
 * recomputes the totals. Called once, after the last boot_add_region(). */
void boot_finish_regions(void);

/* --- Used by core/ while reporting ---------------------------------------- */

const char *boot_firmware_name(enum boot_firmware f);
const char *mem_kind_name(enum mem_kind k);
void boot_print_summary(void);

/* Where the kernel image itself sits. Provided by every linker script, and the
 * one region the kernel knows about without being told. */
/* Where the image sits in *physical* memory. Not where it runs: since
 * checkpoint 10 the kernel is linked high and loaded low, and these two are
 * the loaded bounds -- which is what the memory map and the page allocator
 * care about. Nothing that allocates has any interest in the virtual ones.
 *
 * Defined by the linker script as absolute symbols, so the address of the
 * symbol is the value; there is nothing at it to read. */
extern char __kernel_phys_start[];
extern char __kernel_phys_end[];

#endif /* RECON_KERNEL_BOOT_H */
