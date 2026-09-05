/* x86_64 kernel image layout.
 *
 * 1MB is the traditional load address on this architecture: below it lies the
 * real-mode memory map -- the BIOS data area, video memory, option ROMs --
 * which is not contiguous RAM and never was.
 */

ENTRY(_start)

KERNEL_LOAD_ADDR = 0x100000;

SECTIONS
{
	. = KERNEL_LOAD_ADDR;

	__kernel_start = .;

	.text : {
		KEEP(*(.multiboot))   /* must be within the first 32KB of the image */
		KEEP(*(.text.boot))
		*(.text .text.*)
	}

	. = ALIGN(8);
	/* Kept and placed early: the loader finds it by scanning the first part
	 * of the image for its magic. */
	.reconboot : { KEEP(*(.reconboot)) }

	. = ALIGN(4096);
	.rodata : {
		*(.rodata .rodata.*)
	}

	/* Kept, not discarded: QEMU finds the PVH entry point by reading this
	 * note out of the ELF. Discarding it costs direct -kernel booting. */
	.note.Xen : { KEEP(*(.note.Xen)) }

	. = ALIGN(4096);
	.data : { *(.data .data.*) }

	. = ALIGN(4096);
	.bss : {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		__bss_end = .;
	}

	/* Outside the BSS markers deliberately: the CPU is walking these tables
	 * by the time BSS is cleared. */
	. = ALIGN(4096);
	.pagetables (NOLOAD) : {
		__pagetables_start = .;
		boot_pml4 = .; . += 4096;
		boot_pdpt = .; . += 4096;
		boot_pd   = .; . += 4096;
		__pagetables_end = .;
	}
	/* An absolute symbol whose *value* is the size. boot.S cannot subtract
	 * two linker-defined symbols itself -- the assembler sees both as
	 * undefined and refuses -- so the arithmetic happens here instead. */
	__pagetables_size = __pagetables_end - __pagetables_start;

	. = ALIGN(4096);
	.stack (NOLOAD) : {
		__stack_bottom = .;
		. += 64K;
		__stack_top = .;
	}

	__kernel_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note.GNU-stack)
		*(.note.gnu.*)
		*(.eh_frame)
	}
}
