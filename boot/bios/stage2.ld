/* Stage 2 is read to 0x8000 by stage 1 and links there.
 *
 * Above stage 1 at 0x7C00, below the stack at 0x7000 growing down, and inside
 * the conventional memory every PC has. The load address is the one number two
 * files must agree on -- stage1.S writes it into its disk address packet -- so
 * both name it in a comment.
 *
 * `.text.entry` is placed first and on its own, because the first four bytes of
 * this image are the magic number stage 1 checks before jumping. If the linker
 * were free to order sections, that check would start failing on an unrelated
 * edit, and the symptom would be a machine that stops with E:S2 for no reason
 * anybody could see in the diff.
 */
ENTRY(_start)

SECTIONS
{
	. = 0x8000;

	.text : {
		*(.text.entry)
		*(.text*)
		*(.rodata*)
	}

	.data : {
		*(.data*)
	}

	/* Zeroed by the entry stub, because objcopy -O binary drops NOBITS
	 * sections: they occupy no space in the image, so whatever the firmware
	 * happened to leave at these addresses would be the initial value of
	 * every static in the C. */
	. = ALIGN(4);
	__bss_start = .;
	.bss : {
		*(.bss*)
		*(COMMON)
	}
	. = ALIGN(4);
	__bss_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}

	/* Real mode with a zero segment addresses 64KB, and every pointer in
	 * stage 2 is an offset into it. Code and data crossing 0x10000 would
	 * not fail to link and would not fault: the addresses would wrap and
	 * write over the interrupt vector table at zero, which is a machine
	 * that stops for reasons no message could explain.
	 *
	 * Checked here because it is the kind of limit something grows into
	 * quietly -- the sector count in stage 1 was such a limit, and it was
	 * found by a boot failure rather than by a build (KF-138).
	 */
	ASSERT(__bss_end <= 0x10000,
	       "stage 2 has grown past the 64KB real mode can address")
}
