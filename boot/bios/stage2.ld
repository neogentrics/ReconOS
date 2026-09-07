/* Stage 2 is read to 0x8000 by stage 1 and links there.
 *
 * Above stage 1 at 0x7C00, below the stack at 0x7000 growing down, and inside
 * the conventional memory every PC has. It has a megabyte of partition to grow
 * into but only what it loads to work with, so the load address is the number
 * that matters and it is the same one stage 1 writes into its disk address
 * packet -- two files that must agree, which is why both name it in a comment.
 */
ENTRY(_start)

SECTIONS
{
	. = 0x8000;

	.text : {
		*(.text)
		*(.rodata*)
		*(.data)
	}

	/DISCARD/ : {
		*(.bss)
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
