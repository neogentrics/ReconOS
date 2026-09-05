/* The first program ReconOS runs at a lower privilege level.
 *
 * It is written in assembly and lives inside the kernel image, which is not
 * where user programs will live for long -- checkpoint 15 gives them a
 * filesystem to be loaded from. Until then this is the only way to have a user
 * program at all, and having one is the point: the boundary is not built until
 * something has crossed it in both directions.
 *
 * Two properties matter. It is position-independent -- every address it forms
 * comes from `adr`, which is PC-relative -- because the kernel copies these
 * bytes into a fresh page and maps them at USER_BASE, nowhere near where the
 * linker put them. And it makes no assumption about registers on entry, because
 * nothing promises it any.
 */

	.section .rodata
	.balign 16

	.global user_test_program
user_test_program:

	/* write(1, msg, msg_end - msg)
	 *
	 * The length is computed from two PC-relative addresses rather than
	 * assembled as a constant, so that nothing here depends on where the
	 * assembler happened to place the string. */
	adr	x1, .Lmsg
	adr	x2, .Lmsg_end
	sub	x2, x2, x1
	mov	x0, #1			/* the console */
	mov	x8, #1			/* SYS_WRITE */
	svc	#0

	/* getpid(), whose only job is to prove that a call with no arguments
	 * and a value coming back works as well as one with three going in. */
	mov	x8, #2			/* SYS_GETPID */
	svc	#0

	/* yield(), which returns to the scheduler and comes back -- the part
	 * that proves user mode is preemptible and not a one-way door. */
	mov	x8, #4			/* SYS_YIELD */
	svc	#0

	/* exit(42). Forty-two rather than zero on purpose: a test that expects
	 * zero passes when the value never arrives at all. */
	mov	x0, #42
	mov	x8, #0			/* SYS_EXIT */
	svc	#0

	/* Not reached. If exit ever does return, spinning here is far better
	 * than falling off the end of the page into whatever follows. */
.Lhang:
	b	.Lhang

.Lmsg:
	.ascii	"hello from user mode -- aarch64, EL0\n"
.Lmsg_end:

	.global user_test_program_end
user_test_program_end:

	.balign 8
	.global user_test_program_len
user_test_program_len:
	.quad	user_test_program_end - user_test_program

/* --- The second program: the one that is supposed to fail ----------------
 *
 * A boundary that has only ever been crossed legally has not been tested. This
 * one tries both ways of getting at kernel memory, and the kernel's two
 * defences are different mechanisms that have to be checked separately.
 *
 * First it asks politely: a system call with a kernel address as its buffer.
 * That is stopped by code -- user_range_ok in core/user.c -- and stopping it is
 * the difference between a kernel and a library.
 *
 * Then, having been refused, it goes and reads the address itself. That one is
 * stopped by the processor, because the mapping does not carry the bit that
 * says user mode may reach it. Nothing in the kernel is consulted at all.
 */
	.section .rodata
	.balign 16

	.global user_test_bad
user_test_bad:
	/* write(1, 0xFFFF800000000000, 8) -- the direct map's base, which is
	 * every byte of physical memory, laid out conveniently. */
	mov	x0, #1
	movz	x1, #0xFFFF, lsl #48
	movk	x1, #0x8000, lsl #32
	mov	x2, #8
	mov	x8, #1			/* SYS_WRITE */
	svc	#0

	/* A negative return is a refusal, and a refusal is the correct answer.
	 * Bit 63 clear means the call succeeded, which would mean eight bytes of
	 * kernel memory just went to the console on a user program's say-so. */
	tbz	x0, #63, .Lleaked

	/* Refused. Now try it without asking. */
	movz	x1, #0xFFFF, lsl #48
	movk	x1, #0x8000, lsl #32
	ldr	x2, [x1]

	/* Not reached: the load above faults. If it somehow does not, say so
	 * loudly by exiting normally, which the test reads as a failure. */
	mov	x0, #2
	mov	x8, #0
	svc	#0

.Lleaked:
	mov	x0, #1
	mov	x8, #0			/* SYS_EXIT: exiting at all is the failure */
	svc	#0
.Lbadhang:
	b	.Lbadhang

	.global user_test_bad_end
user_test_bad_end:

	.balign 8
	.global user_test_bad_len
user_test_bad_len:
	.quad	user_test_bad_end - user_test_bad
