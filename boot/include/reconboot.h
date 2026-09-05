/* ReconBoot -- what our own loader hands the kernel.
 *
 * Shared by both halves: the loader fills it in, the kernel reads it, and this
 * file is the only place its shape is written down. It is included by code
 * built with two different compilers for two different ABIs, so it contains no
 * function pointers, no bitfields, no enums whose width could be argued about,
 * and every field is explicitly sized.
 *
 * --- Why a third protocol ---
 *
 * The kernel already understands Multiboot2 and PVH. Both of those hand over in
 * 32-bit protected mode, and the kernel's trampoline builds page tables and
 * switches to long mode itself. UEFI does not: it has already put the machine
 * in 64-bit mode with paging on and an identity map in place. So this protocol
 * enters at a different place, and the kernel image names where.
 *
 * --- Why the kernel names its own entry point ---
 *
 * An ELF has one entry point and it is already spoken for by the 32-bit
 * trampoline. Rather than fork the image, the kernel carries a small header in
 * a known section; the loader finds it by its magic and jumps where it says.
 * That keeps one kernel binary bootable by every protocol it supports, which is
 * the property that makes "BIOS, UEFI, or both" a build with one output rather
 * than three.
 */
#ifndef RECON_RECONBOOT_H
#define RECON_RECONBOOT_H

#include <stdint.h>

/* "RCNBOOT\0" read as a little-endian 64-bit value. */
#define RECONBOOT_MAGIC   0x00544f4f424e4352ULL
#define RECONBOOT_VERSION 1

/* --- The header the kernel carries ---------------------------------------
 *
 * Lives in its own section within the first 64KB of the image, found by the
 * loader scanning for the magic. Deliberately tiny: it answers one question. */
struct reconboot_kernel_header {
	uint64_t magic;
	uint32_t version;
	uint32_t header_size;

	/* Where to jump, as a virtual address in the image as linked. The
	 * loader loads at the ELF's own addresses, so this needs no relocation
	 * today; when the kernel becomes relocatable it becomes an offset and
	 * this comment becomes wrong, which is the point of saying it. */
	uint64_t entry;
};

/* --- Memory ------------------------------------------------------------- */

#define RECONBOOT_MEM_USABLE       0
#define RECONBOOT_MEM_RESERVED     1
#define RECONBOOT_MEM_ACPI_RECLAIM 2
#define RECONBOOT_MEM_ACPI_NVS     3
#define RECONBOOT_MEM_BAD          4
#define RECONBOOT_MEM_BOOTLOADER   5

struct reconboot_region {
	uint64_t base;
	uint64_t size;
	uint32_t kind;
	uint32_t reserved;
};

/* --- Framebuffer ---------------------------------------------------------
 *
 * The thing UEFI gives back that is hardest to obtain any other way: a linear
 * framebuffer at a known address, already in a mode the display accepted, with
 * no driver written. It is why the loader is built before the drivers are. */

#define RECONBOOT_PIXEL_NONE 0	/* no framebuffer: text-only or headless */
#define RECONBOOT_PIXEL_BGRA 1	/* blue, green, red, unused -- what x86 firmware usually gives */
#define RECONBOOT_PIXEL_RGBA 2	/* red, green, blue, unused */

struct reconboot_framebuffer {
	uint64_t base;
	uint64_t size;
	uint32_t width;
	uint32_t height;

	/* Bytes from the start of one row to the start of the next. NOT width
	 * times four: firmware routinely pads rows, and treating pitch as
	 * derivable from width is the classic way to get a picture that shears
	 * progressively down the screen. */
	uint32_t pitch;

	uint32_t format;	/* RECONBOOT_PIXEL_* */
};

/* --- The handoff --------------------------------------------------------- */

#define RECONBOOT_FIRMWARE_UEFI 1

struct reconboot {
	uint64_t magic;
	uint32_t version;
	uint32_t size;		/* of this structure, so it can grow compatibly */

	uint32_t firmware;
	uint32_t region_count;

	/* Physical address of an array of reconboot_region. Separate from this
	 * structure because the count is not known until the memory map is
	 * read, and the memory map cannot be read for the last time until
	 * everything else has finished allocating. */
	uint64_t regions;

	struct reconboot_framebuffer framebuffer;

	uint64_t acpi_rsdp;	/* 0 if the firmware did not publish one */
	uint64_t dtb;		/* 0 if there is no device tree */

	char loader[32];
	char cmdline[128];
};

#endif /* RECON_RECONBOOT_H */
