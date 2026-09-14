/* The last thing this loader runs, and the only part of it that has to be
 * somewhere else.
 *
 * --- why this exists --------------------------------------------------------
 *
 * The kernel goes where its ELF says: 0x100000 upward, fixed. This loader is
 * wherever the firmware decided to put it, which is a choice nothing here
 * influences and firmware is not obliged to explain. **They have never
 * overlapped, and nothing guarantees they never will.**
 *
 * Copying a kernel segment over the loader while the loader is running is a
 * machine that executes whatever the kernel just wrote. Until now the loader
 * checked for that and refused -- correct, loud, and a machine that will not
 * start.
 *
 * So the copy happens from here instead: a few dozen bytes moved to a page the
 * kernel will not land on, run after `ExitBootServices` when nothing else needs
 * the loader to still exist. Overwriting the loader is then harmless, because
 * nothing ever returns to it.
 *
 * --- what it must not do ----------------------------------------------------
 *
 * **No absolute addresses, no relocations, no calls, no data outside its
 * arguments.** It is copied byte for byte to an address chosen at run time, so
 * anything the linker would have fixed up is wrong the moment it moves. Every
 * branch below is relative and every address comes from a register.
 *
 * --- the arguments, in the SysV registers the C caller uses -----------------
 *
 *   RDI  the copy table: {dest, src, len} triples, ending with len == 0
 *   RSI  the handoff, which the kernel wants in RDI
 *   RDX  where to jump when the copying is done, which the kernel wants in RAX
 */
	.section .text
	.globl	reloc_trampoline
	.globl	reloc_trampoline_end

	.balign 16
reloc_trampoline:
	/* Keep the two that are not the table: the copy loop needs RDI and RSI
	 * for the move itself, which is what `rep movsb` insists on. */
	movq	%rsi, %r8		/* the handoff */
	movq	%rdx, %r9		/* the entry point */
	movq	%rdi, %r10		/* the table */

1:	movq	16(%r10), %rcx		/* len */
	testq	%rcx, %rcx
	jz	2f

	movq	0(%r10), %rdi		/* dest */
	movq	8(%r10), %rsi		/* src */

	/* A zero source means fill rather than copy.
	 *
	 * A segment's .bss tail has to be zeroed at the destination, and the
	 * destination is exactly the memory that might be this loader -- so it
	 * cannot be done while the loader is still needed any more than the
	 * copy can. One table, two kinds of entry, and no second pass. */
	testq	%rsi, %rsi
	jnz	3f

	xorl	%eax, %eax
	cld
	rep	stosb
	jmp	4f

3:

	/* Forwards, and that is safe here for a reason worth stating: the
	 * source is a buffer the firmware allocated for the whole kernel image
	 * and the destination is where the kernel belongs. They are different
	 * allocations, so they do not overlap, and a backwards copy would only
	 * matter if they did. */
	cld
	rep	movsb

4:	addq	$24, %r10		/* the next triple */
	jmp	1b

2:	/* The handoff goes in RDI, which is the first argument of the SysV ABI
	 * the kernel's entry point is declared with.
	 *
	 * **And every other register is cleared here rather than by the
	 * caller**, because the caller can no longer do it: the loader set
	 * three of them to reach this code. The reasoning is the loader's and
	 * is unchanged -- a kernel that accidentally reads a leftover works on
	 * the firmware it was written against and fails on the next, and that
	 * failure arrives as a machine that will not boot with no console to
	 * say why.
	 *
	 * One register has to arrive dirty, because naming an address without a
	 * register to hold it is not something this architecture offers -- and
	 * **which one is the kernel's to say.** It names RDI and RAX in boot.c
	 * and checks the other thirteen. R9 carries the entry point through the
	 * copy loop, which needs RAX for `rep stosb`, and hands it over at the
	 * last moment: RAX set, R9 cleared, then the jump. */
	movq	%r8, %rdi
	xorl	%ebx, %ebx
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	xorl	%esi, %esi
	xorl	%ebp, %ebp
	xorl	%r8d, %r8d
	xorl	%r10d, %r10d
	xorl	%r11d, %r11d
	xorl	%r12d, %r12d
	xorl	%r13d, %r13d
	xorl	%r14d, %r14d
	xorl	%r15d, %r15d
	movq	%r9, %rax
	xorl	%r9d, %r9d
	jmp	*%rax

reloc_trampoline_end:
