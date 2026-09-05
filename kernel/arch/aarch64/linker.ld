/* aarch64 kernel image layout.
 *
 * The image is loaded low and runs high, and those are two addresses for the
 * same bytes.
 *
 * 0x40080000 is where it is *loaded*: 0x40000000 is where RAM begins on QEMU's
 * `virt` machine, and the 0x80000 is the text_offset the arm64 Image header
 * asks for, which leaves the loader room to put the device tree below us. The
 * two numbers must agree -- boot.S declares the offset, this adds it to the
 * base of RAM. Real hardware differs, which is exactly why this address lives
 * in one file per architecture rather than in the code.
 *
 * 0xFFFFFFFF80000000 is where it *runs*. The reason is user processes: a
 * kernel identity-mapped in the low half puts a hole in the middle of every
 * program's address space, in the same place, that no program may touch.
 * Moving the kernel out hands that half over entirely, which is what
 * checkpoint 10 is for.
 *
 * The address is the same one x86_64 uses, and that is a choice rather than a
 * coincidence: nothing on this architecture requires it -- aarch64 code is
 * PC-relative and links anywhere -- but x86_64's -mcmodel=kernel does, and one
 * number in two linker scripts is easier to hold than two.
 *
 * --- Nothing here is linked low ---
 *
 * Unlike x86_64, which needs a trampoline at its loaded address because the
 * processor starts in the wrong mode, this architecture starts in the right
 * one. Its boot code only has to avoid *absolute* addresses until the MMU is
 * on, and PC-relative addressing gives it every address it needs. So the whole
 * image is linked high and AT() places every byte of it low.
 */

ENTRY(_start)

KERNEL_LOAD_ADDR = 0x40080000;
KERNEL_VMA       = 0xFFFFFFFF80000000;

SECTIONS
{
	. = KERNEL_LOAD_ADDR + KERNEL_VMA;

	__kernel_start = .;

	.text : AT(ADDR(.text) - KERNEL_VMA) {
		KEEP(*(.text.boot))     /* _start must be the first byte of the image */
		*(.text .text.*)
	}

	. = ALIGN(8);
	.reconboot : AT(ADDR(.reconboot) - KERNEL_VMA) { KEEP(*(.reconboot)) }

	. = ALIGN(4096);
	.rodata : AT(ADDR(.rodata) - KERNEL_VMA) { *(.rodata .rodata.*) }

	. = ALIGN(4096);
	.data : AT(ADDR(.data) - KERNEL_VMA) { *(.data .data.*) }

	. = ALIGN(4096);
	.bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA) {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		__bss_end = .;
	}

	/* The translation tables boot.S builds before there is an allocator.
	 * Outside every BSS marker, because BSS is cleared after the MMU is on
	 * and the hardware is walking these by then.
	 *
	 * One top-level table serves both halves of the address space: entry 0
	 * is the identity map the switch itself stands on, entry 256 is the
	 * direct map, and entry 511 is the kernel's own image. TTBR0 and TTBR1
	 * are both pointed at it, and each simply never looks at the other's
	 * entries. */
	. = ALIGN(4096);
	.boot.tables (NOLOAD) : AT(ADDR(.boot.tables) - KERNEL_VMA) {
		__boot_tables_start = .;
		boot_l0    = .; . += 4096;
		boot_l1_id = .; . += 4096;	/* 512 blocks of 1GB, identity */
		boot_l1_hi = .; . += 4096;	/* the kernel image, up top */
		__boot_tables_end = .;
	}
	__boot_tables_size = __boot_tables_end - __boot_tables_start;

	. = ALIGN(4096);
	.stack (NOLOAD) : AT(ADDR(.stack) - KERNEL_VMA) {
		__stack_bottom = .;
		. += 64K;
		__stack_top = .;
	}

	__kernel_end = .;

	/* Where the image sits in physical memory, which is what the memory map
	 * and the page allocator care about. */
	__kernel_phys_start = __kernel_start - KERNEL_VMA;
	__kernel_phys_end   = __kernel_end - KERNEL_VMA;

	/* Named in the Image header so the loader reserves room for BSS, the
	 * tables and the stack, not merely for the bytes it copied out of the
	 * file. */
	__image_size = __kernel_end - __kernel_start;

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
		*(.eh_frame)
	}
}
