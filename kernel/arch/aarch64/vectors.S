/* aarch64 exception vectors.
 *
 * ARM's table is not a list of addresses like x86's -- it is sixteen slots of
 * 128 bytes each, and the CPU *jumps into the slot*. So each slot holds
 * instructions, not a pointer, and there is room for exactly thirty-two of them
 * before the next slot begins. Overrunning a slot does not fail to assemble in
 * any obvious way; it silently becomes the next exception's handler.
 *
 * The sixteen are four kinds of exception -- synchronous, IRQ, FIQ, SError --
 * times four origins: current level on the boot stack, current level on its own
 * stack, a lower level in 64-bit mode, a lower level in 32-bit mode. All
 * sixteen are filled. An unfilled slot is not an error the CPU reports; it is
 * whatever bytes happen to be there, executed.
 *
 * --- x0 is saved before anything else, and that is a fix ---
 *
 * Each slot used to begin `mov x0, #index`, which destroyed x0 before the frame
 * existed. That was harmless while every exception was a fault: nobody wanted
 * the register, only the report. It stopped being harmless the moment user mode
 * arrived, because a system call's *first argument* is in x0 -- so every call
 * would have received the vector number in place of its first argument, and
 * `write(1, buf, len)` would have been `write(8, buf, len)`.
 *
 * So the frame is opened and x0 stored in it before the index is loaded. Four
 * instructions, which still fits a slot with room to spare.
 */

	.section .text

/* Thirty-six slots: x0 through x30, ELR, SPSR, ESR, FAR, the vector index, and
 * one of padding to keep the stack sixteen-byte aligned. Must match struct
 * trap_frame in trap.c exactly; they are two descriptions of the same bytes. */
	.set FRAME, (36 * 8)
	.set F_ELR,    (31 * 8)
	.set F_SPSR,   (32 * 8)
	.set F_ESR,    (33 * 8)
	.set F_FAR,    (34 * 8)
	.set F_VECTOR, (35 * 8)

	.macro VECTOR_ENTRY index
	.align 7			/* each slot is 128 bytes: 2^7 */
	sub	sp, sp, #FRAME
	str	x0, [sp]		/* the real x0, before anything clobbers it */
	mov	x0, #\index
	b	vector_common
	.endm

/* The table has to be 2048-byte aligned, because VBAR_EL1 ignores the low
 * eleven bits of whatever it is given -- a misaligned table does not fault, it
 * silently lands somewhere else. */
	.align 11
	.global exception_vectors
exception_vectors:
	VECTOR_ENTRY 0		/* current EL, SP0:  synchronous */
	VECTOR_ENTRY 1		/* current EL, SP0:  IRQ */
	VECTOR_ENTRY 2		/* current EL, SP0:  FIQ */
	VECTOR_ENTRY 3		/* current EL, SP0:  SError */
	VECTOR_ENTRY 4		/* current EL, SPx:  synchronous  <- kernel faults */
	VECTOR_ENTRY 5		/* current EL, SPx:  IRQ          <- the tick */
	VECTOR_ENTRY 6		/* current EL, SPx:  FIQ */
	VECTOR_ENTRY 7		/* current EL, SPx:  SError */
	VECTOR_ENTRY 8		/* lower EL, 64-bit: synchronous  <- system calls */
	VECTOR_ENTRY 9		/* lower EL, 64-bit: IRQ */
	VECTOR_ENTRY 10		/* lower EL, 64-bit: FIQ */
	VECTOR_ENTRY 11		/* lower EL, 64-bit: SError */
	VECTOR_ENTRY 12		/* lower EL, 32-bit: synchronous */
	VECTOR_ENTRY 13		/* lower EL, 32-bit: IRQ */
	VECTOR_ENTRY 14		/* lower EL, 32-bit: FIQ */
	VECTOR_ENTRY 15		/* lower EL, 32-bit: SError */

/* The frame is already open and x0 already in it. x0 now holds the slot index.
 */
vector_common:
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

	str	x0, [sp, #F_VECTOR]	/* which slot we came through */

	/* Where it happened and the state it happened in. ELR is the one a
	 * handler may rewrite, to resume elsewhere or to step over a call. */
	mrs	x1, elr_el1
	mrs	x2, spsr_el1
	stp	x1, x2, [sp, #F_ELR]

	/* Why, and on what address. */
	mrs	x1, esr_el1
	mrs	x2, far_el1
	stp	x1, x2, [sp, #F_ESR]

	mov	x0, sp
	bl	trap_dispatch

	/* Reload unconditionally. The handler may have changed ELR to resume
	 * somewhere else; reloading an unchanged value costs nothing, and a
	 * conditional here would be a branch on state the handler owns. */
	ldp	x1, x2, [sp, #F_ELR]
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

	/* x0 last, and from the frame rather than preserved in a register --
	 * because a system call's *return value* was written there by the
	 * handler, and this is how it reaches the program that asked. */
	ldr	x0, [sp]

	add	sp, sp, #FRAME
	eret
