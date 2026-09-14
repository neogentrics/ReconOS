/* What runs before `main`, and the whole of what a C program needs to start.
 *
 * The kernel enters a program at its ELF entry point with a stack and with
 * every general-purpose register cleared. That is enough for assembly, which
 * is what has run here until now. It is not quite enough for C, and the three
 * things missing are small and all of them are here:
 *
 *   - **The stack has to be aligned.** The System V ABI requires that RSP is
 *     sixteen-byte aligned *at the point of a call instruction*, which means
 *     it is 16n+8 on entry to the callee after the return address is pushed.
 *     Compilers rely on this to use aligned vector stores for ordinary struct
 *     copies, and a misaligned stack fails somewhere inside a function that
 *     has nothing to do with the alignment.
 *
 *   - **The frame pointer has to be zero**, so anything walking the stack --
 *     the kernel's backtrace, when a program faults -- has an end to find
 *     rather than a number left over from before.
 *
 *   - **`main` returning has to become `exit`.** Falling off the end of main
 *     executes whatever the linker put after it.
 *
 * There is no `.init_array` handling, no `argc`/`argv` and no `envp`. The
 * kernel passes no arguments yet, and inventing a convention here would mean
 * the one written later disagrees with the programs written against this one.
 * When the kernel grows a way to start a program with arguments, this file is
 * where they will be unpacked, and that is why it exists as its own file
 * rather than as a label at the top of a program.
 *
 * ONE FILE, TWO ARCHITECTURES, the same as hello.S -- described once,
 * assembled twice.
 */

#define SYS_EXIT 0

	.section .text
	.balign 16
	.global _start

#if defined(__x86_64__)

_start:
	/* End the chain a stack walker follows. */
	xorl	%ebp, %ebp

	/*
	 * The kernel hands over a stack pointer that is page-aligned, so it is
	 * already sixteen-byte aligned here -- and `call` will push eight
	 * bytes, leaving main's entry misaligned by exactly the eight the ABI
	 * expects. Masking anyway costs one instruction and removes the
	 * dependence on the kernel's choice of stack top, which is not this
	 * program's to assume.
	 */
	andq	$-16, %rsp

	call	main

	/* main's return value is in EAX; exit wants it in RDI. */
	movl	%eax, %edi
	movq	$SYS_EXIT, %rax
	syscall

	/* Not reached. Spinning beats running into whatever follows. */
.Lhang:
	jmp	.Lhang

#elif defined(__aarch64__)

_start:
	mov	x29, #0			/* the frame pointer chain ends here */
	mov	x30, #0			/* and so does the return address */

	/* AArch64 requires a sixteen-byte aligned SP at every point it is used
	 * as a base, not merely at a call, so this is not belt and braces. */
	mov	x9, sp
	and	x9, x9, #-16
	mov	sp, x9

	bl	main

	/* main's return value is already in w0, which is where exit wants it. */
	mov	x8, #SYS_EXIT
	svc	#0

.Lhang:
	b	.Lhang

#else
#error "ReconOS user programs are built for x86_64 or aarch64"
#endif

	/* No executable stack, said in the object rather than only on the
	 * linker's command line -- a program that says it here keeps saying it
	 * if the link line is ever rewritten. */
	.section .note.GNU-stack,"",%progbits
