/* aarch64 kernel image layout.
 *
 * 0x40000000 is where RAM begins on QEMU's `virt` machine, and where its
 * -kernel loader places an ELF. Real hardware differs, which is exactly why
 * this address lives in one file per architecture rather than in the code.
 */

ENTRY(_start)

KERNEL_LOAD_ADDR = 0x40000000;

SECTIONS
{
	. = KERNEL_LOAD_ADDR;

	__kernel_start = .;

	.text : {
		KEEP(*(.text.boot))     /* _start must be the first byte of the image */
		*(.text .text.*)
	}

	. = ALIGN(4096);
	.rodata : { *(.rodata .rodata.*) }

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

	. = ALIGN(4096);
	.stack (NOLOAD) : {
		__stack_bottom = .;
		. += 64K;
		__stack_top = .;
	}

	__kernel_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
		*(.eh_frame)
	}
}
