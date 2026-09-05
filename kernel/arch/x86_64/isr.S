/* Exception entry points.
 *
 * The CPU pushes a small frame -- RIP, CS, RFLAGS, RSP, SS -- and, for some
 * exceptions but not others, an error code. That inconsistency is the reason
 * for two macros below rather than one: the vectors that do not push an error
 * code push a zero in its place, so that everything downstream sees one shape.
 *
 * Getting that wrong does not produce a wrong number in a dump. It produces a
 * frame shifted by eight bytes, an IRETQ to a nonsense address, and a machine
 * that resets while reporting a fault that never happened.
 */

	.section .text

/* The order these are pushed in is the order struct trap_frame declares them,
 * read from the bottom of the structure upward. The two have to agree exactly. */
	.macro SAVE_REGS
	pushq	%rax
	pushq	%rbx
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%rbp
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	.endm

	.macro RESTORE_REGS
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rbp
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rbx
	popq	%rax
	.endm

/* A vector the CPU does not give an error code for: push a zero so the frame
 * is the same shape as the ones it does. */
	.macro ISR_NOERR n
	.global isr_\n
isr_\n:
	pushq	$0
	pushq	$\n
	jmp	isr_common
	.endm

/* A vector the CPU pushes an error code for. Pushing another would put the
 * frame eight bytes out. */
	.macro ISR_ERR n
	.global isr_\n
isr_\n:
	pushq	$\n
	jmp	isr_common
	.endm

	ISR_NOERR 0		/* divide error */
	ISR_NOERR 1		/* debug */
	ISR_NOERR 2		/* non-maskable interrupt */
	ISR_NOERR 3		/* breakpoint */
	ISR_NOERR 4		/* overflow */
	ISR_NOERR 5		/* bound range */
	ISR_NOERR 6		/* invalid opcode */
	ISR_NOERR 7		/* device not available */
	ISR_ERR   8		/* double fault */
	ISR_NOERR 9
	ISR_ERR   10		/* invalid TSS */
	ISR_ERR   11		/* segment not present */
	ISR_ERR   12		/* stack fault */
	ISR_ERR   13		/* general protection */
	ISR_ERR   14		/* page fault */
	ISR_NOERR 15
	ISR_NOERR 16		/* x87 floating point */
	ISR_ERR   17		/* alignment check */
	ISR_NOERR 18		/* machine check */
	ISR_NOERR 19		/* SIMD floating point */
	ISR_NOERR 20		/* virtualisation */
	ISR_ERR   21		/* control protection */
	ISR_NOERR 22
	ISR_NOERR 23
	ISR_NOERR 24
	ISR_NOERR 25
	ISR_NOERR 26
	ISR_NOERR 27
	ISR_NOERR 28
	ISR_ERR   29		/* VMM communication */
	ISR_ERR   30		/* security */
	ISR_NOERR 31

isr_common:
	SAVE_REGS

	/* The frame is on the stack and the stack pointer is its address. The
	 * System V ABI puts the first argument in RDI. */
	movq	%rsp, %rdi
	call	trap_dispatch

	RESTORE_REGS

	/* Drop the vector and the error code the stubs pushed. The CPU's own
	 * IRETQ frame is what is left, which is what IRETQ expects to find. */
	addq	$16, %rsp
	iretq

/* --- Loading our own descriptor tables ---------------------------------
 *
 * Called with the GDT descriptor in RDI and the IDT descriptor in RSI.
 *
 * Reloading CS cannot be done with a MOV -- there is no instruction that writes
 * CS directly. The way to do it in 64-bit mode is to push the new selector and
 * a return address and execute a far return, which pops both. That is what the
 * three pushes and the LRETQ below are.
 */
	.global x86_load_tables
x86_load_tables:
	lgdt	(%rdi)
	lidt	(%rsi)

	/* Data segments first: they can be written directly, and doing them
	 * before the far return means the far return lands somewhere consistent. */
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	/* Far return to reload CS: push the selector, push where to continue. */
	leaq	.Lreloaded(%rip), %rax
	pushq	$0x08
	pushq	%rax
	lretq

.Lreloaded:
	ret
