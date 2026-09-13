/* Stage 1 is loaded at 0x7C00 by the BIOS, and links there.
 *
 * Every label in it is an absolute address in the first megabyte, because real
 * mode has no relocation and no loader -- the BIOS reads 512 bytes to a fixed
 * place and jumps. Get this address wrong and every `movw $msg, %si` points at
 * whatever the firmware happened to leave there.
 */
ENTRY(_start)

SECTIONS
{
	. = 0x7C00;

	.text : {
		*(.text)
		*(.rodata*)
		*(.data)
	}

	/* There is no bss. A variable that needs zeroing needs code to zero it,
	 * and 440 bytes does not have room for a loop whose only job is to
	 * produce zeroes the assembler could have written into the image. */
	/DISCARD/ : {
		*(.bss)
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
