/* Where a ReconOS program is linked, and why it is two segments and not one.
 *
 * The kernel's loader reads program headers, so this script's job is to produce
 * headers worth reading: one that is readable and executable, one that is
 * readable and writable, and neither that is both. A single segment carrying
 * everything would load correctly and would prove nothing about a loader that
 * ignores `p_flags` -- and a loader that ignores p_flags maps a program's code
 * writable, which is the thing the kernel spent checkpoint 10 arranging not to
 * do.
 *
 * PHDRS is written out rather than left to the linker. Left alone, the linker
 * is entitled to merge sections into whatever segments it likes, and the
 * arrangement this file exists to test would then depend on a default.
 *
 * The base address is the same USER_BASE the kernel has mapped programs at
 * since checkpoint 10 -- above the never-mapped first page, so a null pointer
 * in a program faults exactly as it does in the kernel, and well below the
 * stack. It is written here as a number rather than shared with the kernel's
 * header because this file is read by the linker and that one by a C compiler;
 * if they disagree the loader refuses the program by address range and says so,
 * which is a better failure than a silent one.
 *
 * The alignment is a page, and that matters to the loader rather than to the
 * program: a segment whose file offset and virtual address are not congruent
 * modulo a page cannot be mapped by copying whole pages, and this kernel's
 * loader refuses such a program rather than doing a partial-page copy it has
 * no caller for.
 */

ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);		/* R-X */
	data PT_LOAD FLAGS(6);		/* RW- */
}

SECTIONS
{
	. = 0x400000;

	.text : ALIGN(4096) {
		*(.text .text.*)
	} :text

	.rodata : {
		*(.rodata .rodata.*)
	} :text

	/* A page boundary between the two, because they become two mappings
	 * with different permissions and a page cannot have two. Without this
	 * the writable segment starts inside the last page of the executable
	 * one, and whichever is mapped second decides what the shared page is
	 * -- which is a program whose code is writable, or whose data is not,
	 * depending on the order the loader happens to walk the headers in. */
	. = ALIGN(4096);

	.data : {
		*(.data .data.*)
	} :data

	.bss : {
		*(.bss .bss.*)
		*(COMMON)
	} :data

	/* Nothing here is loaded, and saying so keeps them out of the program
	 * headers rather than leaving the loader to recognise and skip them. */
	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
