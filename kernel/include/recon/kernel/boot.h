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

	/* A filesystem image the loader put in memory, or zero. Zero is the
	 * ordinary case and not a failure. */
	u64 initrd_base;
	u64 initrd_size;

	/* EFI_RUNTIME_SERVICES, or zero on a machine with no UEFI. The half of
	 * the firmware that outlives ExitBootServices, and on a board with no
	 * battery-backed clock it is the only source of the date. */
	u64 runtime_services;

	/* Where the firmware said its SMBIOS tables are, or zero. Zero is
	 * ordinary rather than a fault: a BIOS machine has no configuration
	 * table to ask, and the scan below one megabyte answers there. */
	u64 smbios;

	/* The volume this kernel was read from, as the loader saw it.
	 *
	 * `boot_part_lba` is where that partition starts inside its disk, which
	 * is the same number `struct block_device` keeps in `first_lba` -- so a
	 * device can be matched against it without converting anything.
	 * `boot_part_guid` is the GPT unique partition GUID where there is one,
	 * and is all zero where there is not, which includes every BIOS boot.
	 *
	 * An LBA is unique within one disk and a GUID is unique across all of
	 * them, so a match on the LBA alone is a strong hint and a match on
	 * both is an answer. Any code deciding something irreversible should
	 * say which of the two it had. */
	u64 boot_part_lba;
	u8  boot_part_guid[16];
	u32 boot_disk;

	/* What the loader's boot menu offered and what happened to it.
	 *
	 * Here so the boot report can print it, and the boot report is written
	 * to disk -- which is the only reason this travels at all. The loader
	 * says the same thing on the firmware console and then draws the menu
	 * over it, so on a machine with no serial port nobody ever reads it.
	 *
	 * `menu_shown` is separate from `menu_entries` being zero because those
	 * are different facts: no menu at all means the loader gave up before
	 * offering anything, which is KF-233's failure, and a menu with entries
	 * nobody chose is an ordinary boot. A single count says the same number
	 * for both. */
	/* Whether the loader said anything about a menu, which is not the same
	 * as there having been one. The BIOS loader has no menu, GRUB has its
	 * own, and a ReconBoot older than this field wrote nothing here -- all
	 * three arrive as zeroes, and zeroes are also what "the menu never
	 * ran" looks like. Without this flag the report would accuse three
	 * ordinary loaders of KF-233's fault. */
	bool menu_known;
	bool menu_shown;
	bool menu_drawn;
	bool menu_key;			/* somebody was at the keyboard */
	bool menu_picked;		/* and chose an entry */
	u32  menu_entries;

	paddr_t acpi_rsdp;	/* 0 if the firmware did not point at one */
	paddr_t dtb;		/* 0 if there is no device tree */
};

/* Reads the structure our own bootloader left us. Portable: the ReconBoot
 * protocol is the same on every architecture, which is most of why it exists.
 * Returns false if what was handed over is not one. */
bool reconboot_parse(paddr_t handoff);

/* The one copy. Written only by arch/ during early init. */
struct boot_info *boot_info(void);

/* Whether a whole word appears on the kernel command line. */
bool boot_cmdline_has(const char *word);

/* --- Used by arch/ while translating -------------------------------------- */

void boot_info_reset(const char *protocol, enum boot_firmware firmware);

/* --- what the loader left in the registers ---------------------------------
 *
 * The ReconBoot path records every register the handoff arrived with, before
 * the kernel has touched one, and this reports whether they were all clear.
 *
 * False with `*dirty` naming the first one that was not. True on every other
 * boot path, with `*dirty` left null -- GRUB and a hypervisor make no such
 * promise and are not being held to one, and saying "pass" for a promise
 * nobody made would be a test that cannot fail.
 *
 * `*checked` receives how many registers the answer covers, because it is not
 * all of them: the loader has to name the address it jumps to in some
 * register, and that one necessarily still holds it. */
bool boot_handoff_registers_clear(const char **dirty, unsigned *checked);
void boot_add_region(paddr_t base, u64 size, enum mem_kind kind);

/* Sorts by address and merges adjacent regions of the same kind, then
 * recomputes the totals. Called once, after the last boot_add_region(). */
void boot_finish_regions(void);

/* --- Used by core/ while reporting ---------------------------------------- */

const char *boot_firmware_name(enum boot_firmware f);
const char *mem_kind_name(enum mem_kind k);
/* Which volume the loader read this kernel from, on its own so it can be
 * printed twice: in the boot report, and again at the end of a `noinit` boot
 * where a long report has scrolled the first copy off a small screen. */
void boot_print_medium(void);
void boot_print_menu(void);

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
