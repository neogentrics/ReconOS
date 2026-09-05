/* aarch64 exception vectors.
 *
 * ARM's table is not a list of addresses like x86's -- it is sixteen slots of
 * 128 bytes each, and the CPU *jumps into the slot*. So each slot holds
 * instructions, not a pointer, and there is room for exactly thirty-two of
 * them before the next slot begins. Overrunning a slot does not fail to
 * assemble in any obvious way; it silently becomes the next exception's
 * handler. So every slot here does the same two things: record which slot it
 * was, and branch out.
 *
 * The sixteen are four kinds of exception -- synchronous, IRQ, FIQ, SError --
 * times four origins: current level on the boot stack, current level on its own
 * stack, a lower level in 64-bit mode, a lower level in 32-bit mode. All
 * sixteen are filled. An unfilled slot is not an error the CPU reports; it is
 * whatever bytes happen to be there, executed.
 */

	.section .text

	.macro VECTOR_ENTRY index
	.align 7			/* each slot is 128 bytes: 2^7 */
	mov	x0, #\index
	b	vector_common
	.endm

/* The table has to be 2048-byte aligned, because VBAR_EL1 ignores the low
 * eleven bits of whatever it is given -- a misaligned table does not fault,
 * it silently lands somewhere else. */
	.align 11
	.global exception_vectors
exception_vectors:
	VECTOR_ENTRY 0		/* current EL, SP0:  synchronous */
	VECTOR_ENTRY 1		/* current EL, SP0:  IRQ */
	VECTOR_ENTRY 2		/* current EL, SP0:  FIQ */
	VECTOR_ENTRY 3		/* current EL, SP0:  SError */
	VECTOR_ENTRY 4		/* current EL, SPx:  synchronous  <- where kernel faults land */
	VECTOR_ENTRY 5		/* current EL, SPx:  IRQ */
	VECTOR_ENTRY 6		/* current EL, SPx:  FIQ */
	VECTOR_ENTRY 7		/* current EL, SPx:  SError */
	VECTOR_ENTRY 8		/* lower EL, 64-bit: synchronous  <- system calls, later */
	VECTOR_ENTRY 9		/* lower EL, 64-bit: IRQ */
	VECTOR_ENTRY 10		/* lower EL, 64-bit: FIQ */
	VECTOR_ENTRY 11		/* lower EL, 64-bit: SError */
	VECTOR_ENTRY 12		/* lower EL, 32-bit: synchronous */
	VECTOR_ENTRY 13		/* lower EL, 32-bit: IRQ */
	VECTOR_ENTRY 14		/* lower EL, 32-bit: FIQ */
	VECTOR_ENTRY 15		/* lower EL, 32-bit: SError */

/* Saves the general registers and the exception state, then calls into C.
 *
 * x0 already holds the slot index, so it is saved last and passed on. The
 * frame's layout has to match struct trap_frame in trap.c exactly; they are
 * two descriptions of the same bytes.
 */
vector_common:
	/* Thirty-six slots, not thirty-five: thirty-one general registers, ELR,
	   SPSR, ESR, FAR, and one of padding to keep the stack sixteen-byte
	   aligned, which the ABI requires at a call.

	   This was thirty-four, which is what happens when you count the fields
	   you are saving and forget that the C structure counts from zero. ESR
	   landed on top of SPSR; the handler then restored SPSR from the ESR
	   value and ERET returned into a nonsense processor state. It presented
	   as a *second* fault after the first was handled correctly, which is a
	   long way from the cause. */
	sub	sp, sp, #(36 * 8)

	stp	x1,  x2,  [sp, #(1 * 8)]
	stp	x3,  x4,  [sp, #(3 * 8)]
	stp	x5,  x6,  [sp, #(5 * 8)]
	stp	x7,  x8,  [sp, #(7 * 8)]
	stp	x9,  x10, [sp, #(9 * 8)]
	stp	x11, x12, [sp, #(11 * 8)]
	stp	x13, x14, [sp, #(13 * 8)]
	stp	x15, x16, [sp, #(15 * 8)]
	stp	x17, x18, [sp, #(17 * 8)]
	stp	x19, x20, [sp, #(19 * 8)]
	stp	x21, x22, [sp, #(21 * 8)]
	stp	x23, x24, [sp, #(23 * 8)]
	stp	x25, x26, [sp, #(25 * 8)]
	stp	x27, x28, [sp, #(27 * 8)]
	stp	x29, x30, [sp, #(29 * 8)]

	/* The slot index lands in the x0 slot, which is where trap.c looks for
	 * it -- the original x0 is gone, and saying so is better than saving a
	 * value the handler cannot use anyway. */
	str	x0, [sp]

	/* Where it faulted, and the processor state it faulted in. ELR is the
	 * one the handler may rewrite to resume somewhere else. */
	mrs	x1, elr_el1
	mrs	x2, spsr_el1
	stp	x1, x2, [sp, #(31 * 8)]

	/* Why, and on what address. */
	mrs	x1, esr_el1
	mrs	x2, far_el1
	stp	x1, x2, [sp, #(33 * 8)]

	mov	x0, sp
	bl	trap_dispatch

	/* ELR may have been changed by the handler to resume elsewhere. Reload
	 * it either way: reloading an unchanged value costs nothing, and a
	 * conditional here would be a branch on state the handler owns. */
	ldp	x1, x2, [sp, #(31 * 8)]
	msr	elr_el1, x1
	msr	spsr_el1, x2

	ldp	x1,  x2,  [sp, #(1 * 8)]
	ldp	x3,  x4,  [sp, #(3 * 8)]
	ldp	x5,  x6,  [sp, #(5 * 8)]
	ldp	x7,  x8,  [sp, #(7 * 8)]
	ldp	x9,  x10, [sp, #(9 * 8)]
	ldp	x11, x12, [sp, #(11 * 8)]
	ldp	x13, x14, [sp, #(13 * 8)]
	ldp	x15, x16, [sp, #(15 * 8)]
	ldp	x17, x18, [sp, #(17 * 8)]
	ldp	x19, x20, [sp, #(19 * 8)]
	ldp	x21, x22, [sp, #(21 * 8)]
	ldp	x23, x24, [sp, #(23 * 8)]
	ldp	x25, x26, [sp, #(25 * 8)]
	ldp	x27, x28, [sp, #(27 * 8)]
	ldp	x29, x30, [sp, #(29 * 8)]

	add	sp, sp, #(36 * 8)
	eret
