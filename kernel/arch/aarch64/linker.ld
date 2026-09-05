/* aarch64 kernel image layout.
 *
 * 0x40000000 is where RAM begins on QEMU's `virt` machine, and where its
 * -kernel loader places an ELF. Real hardware differs, which is exactly why
 * this address lives in one file per architecture rather than in the code.
 */

ENTRY(_start)

/* RAM begins at 0x40000000 on QEMU's `virt` machine; the 0x80000 is the
 * text_offset the Image header asks for, and leaves the loader room to put the
 * device tree below us. The two numbers must agree -- boot.S declares the
 * offset, this adds it to the base of RAM. */
KERNEL_LOAD_ADDR = 0x40080000;

SECTIONS
{
	. = KERNEL_LOAD_ADDR;

	__kernel_start = .;

	.text : {
		KEEP(*(.text.boot))     /* _start must be the first byte of the image */
		*(.text .text.*)
	}

	. = ALIGN(8);
	.reconboot : { KEEP(*(.reconboot)) }

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

	/* Named in the Image header so the loader reserves room for BSS and the
	 * stack, not merely for the bytes it copied out of the file. */
	__image_size = __kernel_end - __kernel_start;

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
		*(.eh_frame)
	}
}
