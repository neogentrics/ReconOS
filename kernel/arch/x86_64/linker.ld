/* x86_64 kernel image layout.
 *
 * The image is loaded low and runs high. Those are two different addresses for
 * the same bytes, and the split is what this file exists to express.
 *
 * 1MB is where it is *loaded*, because below it lies the real-mode memory map
 * -- BIOS data area, video memory, option ROMs -- which is not contiguous RAM
 * and never was. Every loader that will ever start this kernel puts it there.
 *
 * 0xFFFFFFFF80000000 is where it *runs*, and the reason is user processes. A
 * kernel identity-mapped at 1MB occupies part of the same address space every
 * program has to live in; every program's address space would then have a hole
 * in it, in the same place, that it must not touch. Moving the kernel to the
 * top hands the entire lower canonical half to whatever is running -- which is
 * the point of checkpoint 10 and the reason it was deferred to it rather than
 * done at checkpoint 5 for tidiness.
 *
 * The specific address is not free. -mcmodel=kernel generates references that
 * assume the code lives in the top two gigabytes, which is exactly
 * 0xFFFFFFFF80000000 upward, and it is the only model that produces sensible
 * code for a high kernel. So the direct map at 0xFFFF800000000000 and the
 * kernel image are two separate windows rather than one.
 *
 * --- The low region ---
 *
 * Not all of it can move. The trampoline that gets from 32-bit protected mode
 * to 64-bit long mode runs before paging exists, so it must be linked at the
 * address it is loaded at; so must the page tables it builds and the descriptor
 * table it loads. Those live in .boot and stay low. Everything after them is
 * linked high and loaded low, which is what AT() below says.
 */

ENTRY(_start)

KERNEL_LOAD_ADDR = 0x100000;
KERNEL_VMA       = 0xFFFFFFFF80000000;

SECTIONS
{
	. = KERNEL_LOAD_ADDR;

	__kernel_phys_start = .;

	/* Everything that runs before paging, and everything it touches. */
	.boot : {
		KEEP(*(.multiboot))   /* must be within the first 32KB of the file */
		KEEP(*(.text.boot))
		KEEP(*(.rodata.boot))
		. = ALIGN(8);
	}

	/* Kept, not discarded: QEMU finds the PVH entry point by reading this
	 * note out of the ELF. Discarding it costs direct -kernel booting. */
	.note.Xen : { KEEP(*(.note.Xen)) }

	. = ALIGN(8);
	/* Kept and placed early: the loader finds it by scanning the first part
	 * of the image for its magic. */
	.reconboot : { KEEP(*(.reconboot)) }

	/* Scratch the trampoline needs and the file does not carry: a stack for
	 * it, and the page tables it builds. Both are outside every BSS marker,
	 * because BSS is cleared after the switch and the CPU is walking these
	 * tables by then. */
	. = ALIGN(4096);
	.boot.bss (NOLOAD) : {
		boot_stack_bottom = .; . += 4096; boot_stack_top = .;

		/* Three words the trampoline writes and the kernel proper
		 * reads once. Here rather than in an initialised section so
		 * that .boot stays read-and-execute: a loaded region holding
		 * both code and writable data is one the linker warns about,
		 * and it is warning about something real. Nothing reads these
		 * before they are written, so having no initial value costs
		 * nothing. */
		boot_scratch_protocol = .; . += 8;
		boot_scratch_info     = .; . += 8;
		boot_scratch_magic    = .; . += 8;

		/* Back onto a page boundary before the tables. A page table
		 * whose address is not page-aligned is not a page table: the
		 * processor ignores the low twelve bits of every pointer to
		 * one, so the entries land twenty-four bytes into a table the
		 * hardware then reads from the wrong place. It costs nothing
		 * to be careful here and the failure is silent. */
		. = ALIGN(4096);

		__pagetables_start = .;
		boot_pml4    = .; . += 4096;
		boot_pdpt    = .; . += 4096;	/* the low half, identity */
		boot_pdpt_hi = .; . += 4096;	/* the top half, the kernel */
		boot_pd      = .; . += 4096;	/* shared: both point at it */
		__pagetables_end = .;
	}
	/* An absolute symbol whose *value* is the size. boot.S cannot subtract
	 * two linker-defined symbols itself -- the assembler sees both as
	 * undefined and refuses -- so the arithmetic happens here instead. */
	__pagetables_size = __pagetables_end - __pagetables_start;

	. = ALIGN(4096);
	__low_end = .;

	/* --- Everything below here is linked high and loaded low ---------- */

	. = __low_end + KERNEL_VMA;

	.text : AT(ADDR(.text) - KERNEL_VMA) {
		*(.text .text.*)
	}

	. = ALIGN(4096);
	.rodata : AT(ADDR(.rodata) - KERNEL_VMA) {
		*(.rodata .rodata.*)
	}

	. = ALIGN(4096);
	.data : AT(ADDR(.data) - KERNEL_VMA) {
		*(.data .data.*)
	}

	. = ALIGN(4096);
	.bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VMA) {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		__bss_end = .;
	}

	. = ALIGN(4096);
	.stack (NOLOAD) : AT(ADDR(.stack) - KERNEL_VMA) {
		__stack_bottom = .;
		. += 64K;
		__stack_top = .;
	}

	/* Where the image begins and ends *in physical memory*, which is what
	 * the memory map and the page allocator care about. The virtual bounds
	 * are of no interest to either: nothing allocates virtual addresses
	 * yet, and the physical bytes are the ones that must not be handed
	 * out. */
	__kernel_phys_end = . - KERNEL_VMA;

	/* The 32-bit trampoline needs the descriptor table's *loaded* address,
	 * and cannot compute it: subtracting two symbols the assembler has
	 * never seen is not something it will do. The linker can, so it does. */
	gdt64_phys = gdt64 - KERNEL_VMA;

	/DISCARD/ : {
		*(.comment)
		*(.note.GNU-stack)
		*(.note.gnu.*)
		*(.eh_frame)
	}
}
