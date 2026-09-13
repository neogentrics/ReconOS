/* Where a woken processor begins, and the three modes it walks through.
 *
 * A processor started by a startup message begins in **16-bit real mode**, at
 * an address the message carries as a page number in a single byte. That is the
 * whole reason this code exists and the whole reason it has to live below one
 * megabyte: not because anything about it needs sixteen bits, but because the
 * only way to say where to start is a number that cannot exceed 255.
 *
 * So this walks the same road boot.S walks -- real mode, protected mode, page
 * tables, long mode -- for a processor that missed it. It is short and it is
 * fiddly, and the fiddliness is all one problem: **this code is assembled at
 * one address and executed at another.** It is copied to a page the allocator
 * chose, so no absolute address in it is right, and the two far jumps a mode
 * change requires both take absolute addresses.
 *
 * Rather than have C patch the copy -- which means the assembler and the C
 * agreeing about offsets, in two files, silently -- everything is computed at
 * run time from where the code finds itself. A processor in real mode can read
 * its own CS, and CS << 4 is the physical address this page was copied to.
 * Nothing here needs to be told where it is.
 *
 * The parameter block at the end is written by C before the processor is
 * started: page tables, a stack, an entry point, and which processor this is.
 */

/* Offsets into the page. C reads them from the symbols below via
 * x86_trampoline_size and friends, so they cannot drift. */

	.section .rodata.trampoline, "a"
	.balign 4096
	.code16
	.global x86_trampoline_start
x86_trampoline_start:
	cli
	cld

	/* CS is the page we were copied to, divided by sixteen. Everything
	 * below addresses this page through DS, and computes the page's
	 * physical base in EBX for the descriptors that need a linear one. */
	movw	%cs, %ax
	movw	%ax, %ds

	movzwl	%ax, %ebx
	shll	$4, %ebx		/* EBX = physical base of this page */

	/* The GDT descriptor's base field, filled in now that the address is
	 * known. Written as a 32-bit store two bytes into the descriptor,
	 * because the first two bytes are the limit. */
	movl	%ebx, %eax
	addl	$(tramp_gdt - x86_trampoline_start), %eax
	movl	%eax, %ds:(tramp_gdt_desc - x86_trampoline_start + 2)

	/* Both far-jump targets, likewise. Each is a 32-bit offset followed by
	 * a 16-bit selector, and only the offset depends on where we are. */
	movl	%ebx, %eax
	addl	$(protected_entry - x86_trampoline_start), %eax
	movl	%eax, %ds:(jump_to_protected - x86_trampoline_start)

	movl	%ebx, %eax
	addl	$(long_entry - x86_trampoline_start), %eax
	movl	%eax, %ds:(jump_to_long - x86_trampoline_start)

	lgdtl	%ds:(tramp_gdt_desc - x86_trampoline_start)

	/* Protected mode, and nothing else in CR0 yet: paging comes after the
	 * page tables are loaded, and turning both on at once would fetch the
	 * next instruction through tables that are not there. */
	movl	%cr0, %eax
	orl	$1, %eax
	movl	%eax, %cr0

	ljmpl	*%ds:(jump_to_protected - x86_trampoline_start)

	.code32
protected_entry:
	movw	$0x10, %ax		/* the flat data selector below */
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	/* Physical address extension, which long mode requires. */
	movl	%cr4, %eax
	orl	$(1 << 5), %eax
	movl	%eax, %cr4

	/* The page tables the boot processor is already using. They must map
	 * this page at its own address -- the instruction after paging comes on
	 * is fetched from here, and a translation that does not exist for it
	 * faults on the very instruction that would have reported the fault. */
	movl	%ds:(tramp_cr3 - x86_trampoline_start)(%ebx), %eax
	movl	%eax, %cr3

	/* Long mode enable, and no-execute enable in the same write.
	 *
	 * NXE is not optional here even though nothing in this file is marked
	 * no-execute: the *kernel's* page tables use bit 63, and on a processor
	 * where EFER.NXE is clear that bit is reserved rather than ignored. So
	 * every kernel mapping would fault on first use, on the secondary only,
	 * with tables that are demonstrably correct. boot.S learned this at
	 * checkpoint 5; it is the same fact and it is per-processor. */
	movl	$0xC0000080, %ecx	/* EFER */
	rdmsr
	orl	$((1 << 8) | (1 << 11)), %eax
	wrmsr

	/* Paging, and protection, in one write. */
	movl	%cr0, %eax
	orl	$((1 << 31) | (1 << 0)), %eax
	movl	%eax, %cr0

	ljmpl	*(jump_to_long - x86_trampoline_start)(%ebx)

	.code64
long_entry:
	/* EBX was written in 32-bit mode, where the top half of RBX is
	 * preserved but invisible. It becomes visible here, and nothing has
	 * promised it is zero -- so it is zeroed, by a 32-bit write to EBX,
	 * which in 64-bit mode clears the upper half as a side effect. Without
	 * it every parameter below is read from an address 4 GB or more away
	 * from the one intended, on a processor that has no way to say so. */
	movl	%ebx, %ebx

	/* Out of the compatibility segment and into a 64-bit one. The stack is
	 * this processor's own, written by C before it was started. */
	movq	(tramp_stack - x86_trampoline_start)(%rbx), %rsp

	/* The entry point is a full 64-bit address in the higher half, which
	 * cannot be reached by a relative jump from down here. Loaded into a
	 * register and jumped to indirectly -- and this is the last instruction
	 * fetched from low memory, so the identity mapping is not needed after
	 * it. */
	movl	(tramp_cpu - x86_trampoline_start)(%rbx), %edi
	movq	(tramp_entry - x86_trampoline_start)(%rbx), %rax
	jmpq	*%rax

	/* Unreachable. A processor that arrives here has been handed an entry
	 * point of zero, and stopping is better than executing whatever is at
	 * the bottom of memory. */
1:	hlt
	jmp	1b

	/* --- the descriptors this file needs, and no more --------------------
	 *
	 * Three entries: null, a flat 32-bit code segment to reach protected
	 * mode with, and a 64-bit code segment to reach long mode with, plus the
	 * flat data segment protected mode needs for its selectors. The kernel's
	 * real GDT is loaded later, by C, once this processor can run C. */
	.balign 8
tramp_gdt:
	.quad	0
	.quad	0x00CF9A000000FFFF	/* 0x08 32-bit code, flat */
	.quad	0x00CF92000000FFFF	/* 0x10 32-bit data, flat */
	.quad	0x00AF9A000000FFFF	/* 0x18 64-bit code */
tramp_gdt_end:

tramp_gdt_desc:
	.word	tramp_gdt_end - tramp_gdt - 1
	.long	0			/* filled in at run time */

jump_to_protected:
	.long	0			/* offset, filled in at run time */
	.word	0x08

jump_to_long:
	.long	0			/* offset, filled in at run time */
	.word	0x18

	/* --- what C writes before starting a processor ---------------------- */
	.balign 16
	.global x86_trampoline_params
x86_trampoline_params:
tramp_cr3:
	.quad	0
tramp_stack:
	.quad	0
tramp_entry:
	.quad	0
tramp_cpu:
	.long	0
	.long	0

	.global x86_trampoline_end
x86_trampoline_end:

	.section .note.GNU-stack, "", @progbits
