/* x86_64 entry.
 *
 * Unlike aarch64, an x86 CPU does not start in the mode the kernel wants. It
 * starts in 32-bit protected mode with paging off, and getting to 64-bit long
 * mode is something the kernel has to do for itself: build page tables, turn
 * on PAE, set the long-mode-enable bit, turn on paging, then far-jump through
 * a 64-bit code segment. That switch is the whole of this file, and it is why
 * this architecture's boot code is four times the length of ARM's.
 *
 * Two boot protocols land in the same trampoline:
 *
 *   Multiboot2 -- what GRUB uses, and therefore how this boots from an ISO or
 *   from real hardware. Hands us a pointer in EBX and a magic number in EAX.
 *
 *   PVH -- an ELF note that QEMU's -kernel understands directly, so a build
 *   can be booted in a second without producing a disc image first. Hands us
 *   a different structure, also in EBX.
 *
 * Both arrive in 32-bit protected mode with a flat address space, so the code
 * below only has to record which one called and where its data is.
 */

#define MULTIBOOT2_MAGIC        0xE85250D6
#define MULTIBOOT2_ARCH_I386    0

/* --- Multiboot2 header ------------------------------------------------- */

	.section .multiboot, "a"
	.align 8
multiboot_header:
	.long	MULTIBOOT2_MAGIC
	.long	MULTIBOOT2_ARCH_I386
	.long	multiboot_header_end - multiboot_header
	.long	-(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH_I386 + \
		  (multiboot_header_end - multiboot_header))

	/* End tag: type 0, flags 0, size 8. */
	.align 8
	.short	0
	.short	0
	.long	8
multiboot_header_end:

/* --- PVH entry note ---------------------------------------------------- */

/* XEN_ELFNOTE_PHYS32_ENTRY (18). QEMU reads this out of the ELF and jumps
 * straight to the address it names, which is what makes `make run` fast. */
	.section .note.Xen, "a"
	.align 4
	.long	4			/* namesz: "Xen\0" */
	.long	4			/* descsz: one 32-bit address */
	.long	18			/* type */
	.asciz	"Xen"
	.align 4
	.long	pvh_entry
	.align 4

/* --- 32-bit trampoline ------------------------------------------------- */

	.section .text.boot, "ax"
	.code32

	.global _start
_start:					/* Multiboot2 lands here */
	movl	$1, %esi		/* protocol tag: 1 = multiboot2 */
	jmp	common_entry

	.global pvh_entry
pvh_entry:				/* QEMU's PVH loader lands here */
	movl	$2, %esi		/* protocol tag: 2 = PVH */
	jmp	common_entry

common_entry:
	cli
	cld

	/* Record who booted us and where their data is, before any of this is
	 * clobbered. Both live in .data, which is not zeroed later. */
	movl	%esi, boot_protocol
	movl	%ebx, boot_info_phys
	movl	%eax, boot_magic

	/* A stack, so the rest of this can use one. */
	movl	$__stack_top, %esp

	/* Zero the three page tables. They are outside the BSS markers on
	 * purpose: BSS gets cleared further down, and clearing the page tables
	 * the CPU is currently walking would end the boot immediately. */
	movl	$__pagetables_start, %edi
	xorl	%eax, %eax
	movl	$((__pagetables_end - __pagetables_start) / 4), %ecx
	rep stosl

	/* One PML4 entry -> one PDPT entry -> a page directory of 512 entries,
	 * each a 2MB page. That identity-maps the first gigabyte, which is more
	 * than enough to hold a kernel that has not yet learned to allocate.
	 * Bits: 0 present, 1 writable, 7 page-size. */
	movl	$boot_pdpt, %eax
	orl	$0x03, %eax
	movl	%eax, boot_pml4

	movl	$boot_pd, %eax
	orl	$0x03, %eax
	movl	%eax, boot_pdpt

	movl	$boot_pd, %edi
	movl	$0x83, %eax
	movl	$512, %ecx
1:	movl	%eax, (%edi)
	movl	$0, 4(%edi)
	addl	$0x200000, %eax
	addl	$8, %edi
	loop	1b

	/* PAE. Long mode requires it; there is no 64-bit paging without it. */
	movl	%cr4, %eax
	orl	$(1 << 5), %eax
	movl	%eax, %cr4

	movl	$boot_pml4, %eax
	movl	%eax, %cr3

	/* EFER.LME -- "long mode enable". The CPU is not in long mode yet; this
	 * only says that turning on paging should enter it. */
	movl	$0xC0000080, %ecx
	rdmsr
	orl	$(1 << 8), %eax
	wrmsr

	/* Paging on. The CPU enters long mode at this instruction, but is still
	 * executing through a 32-bit code segment (compatibility mode). */
	movl	%cr0, %eax
	orl	$0x80000001, %eax	/* PG | PE */
	movl	%eax, %cr0

	/* The far jump through a 64-bit code descriptor is what actually leaves
	 * 32-bit code behind. */
	lgdt	gdt64_descriptor
	ljmp	$0x08, $long_mode_entry

/* --- 64-bit --------------------------------------------------------- */

	.code64
long_mode_entry:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	movq	$__stack_top, %rsp
	xorq	%rbp, %rbp

	/* Clear BSS. Now, and not before, because everything the trampoline
	 * needed to survive lives outside it. */
	movq	$__bss_start, %rdi
	movq	$__bss_end, %rcx
	subq	%rdi, %rcx
	xorq	%rax, %rax
	rep stosb

	call	kmain

halt:
	cli
	hlt
	jmp	halt

/* --- GDT ------------------------------------------------------------- */

	.section .rodata
	.align 16
gdt64:
	.quad	0				/* null */
	.quad	0x00AF9A000000FFFF		/* code: exec/read, L=1 */
	.quad	0x00CF92000000FFFF		/* data: read/write */
gdt64_end:

gdt64_descriptor:
	.word	gdt64_end - gdt64 - 1
	.long	gdt64		/* 32-bit base: lgdt runs before long mode,
				 * and the image lives below 4GB */

/* --- State handed over by the bootloader ------------------------------- */

	.section .data
	.align 8
	.global boot_protocol
boot_protocol:
	.long	0
	.global boot_info_phys
boot_info_phys:
	.long	0
	.global boot_magic
boot_magic:
	.long	0
