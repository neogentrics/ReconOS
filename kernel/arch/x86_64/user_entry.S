/* The x86_64 system-call entry point, and the first program to use it.
 *
 * SYSCALL is fast because it does almost nothing. It saves the return address
 * in RCX and the flags in R11, masks the flags with one MSR, loads CS and SS
 * from another, jumps to a third -- and stops. In particular it does NOT
 * switch stacks. The processor arrives in the kernel still standing on the
 * user program's stack, which the user program chose, and which it may have
 * pointed anywhere at all.
 *
 * So the very first thing this code does is get off it. SWAPGS exchanges GS's
 * base address with a value kept in a fourth MSR, which is how the kernel finds
 * anything at all before it has a stack to keep things on: this processor's own
 * two words live at GS:0 and GS:8.
 *
 * The invariant SWAPGS depends on, written down because getting it backwards is
 * both easy and silent: while user code runs, GS_BASE is zero and
 * KERNEL_GS_BASE holds this processor's block. One SWAPGS on the way in, one on
 * the way out, and nothing between them may take a second one.
 */

	.section .text

	.set CPU_KERNEL_RSP, 0		/* GS:0  -- where to stand */
	.set CPU_USER_RSP,   8		/* GS:8  -- where we were standing */

	.global x86_syscall_entry
x86_syscall_entry:
	swapgs
	movq	%rsp, %gs:CPU_USER_RSP
	movq	%gs:CPU_KERNEL_RSP, %rsp

	/* RCX and R11 are not saved because the convention is polite about
	 * them; they are saved because SYSRET *reads* them. Losing either one
	 * means returning to the wrong address or with the wrong flags. */
	pushq	%rcx			/* the user program's RIP */
	pushq	%r11			/* and its RFLAGS */

	/* The rest is the promise the convention makes: a system call clobbers
	 * RAX, RCX and R11, and gives everything else back untouched. Keeping
	 * that promise is what lets a user program call one from the middle of a
	 * function without spilling half its registers first. */
	pushq	%rdi
	pushq	%rsi
	pushq	%rdx
	pushq	%r10
	pushq	%r8
	pushq	%r9

	/* Eight pushes, so the stack is still sixteen-byte aligned. The seventh
	 * C argument goes on the stack and would break that alignment on its
	 * own, hence the gap pushed with it. */
	subq	$8, %rsp
	pushq	%r9			/* a5 */

	/* Shuffle from the system-call convention into the C one. Right to left,
	 * so that no register is overwritten before it has been read.
	 *
	 * user: rax=number rdi rsi rdx r10 r8  r9
	 * C:    rdi=number rsi rdx rcx r8  r9  stack
	 *
	 * R10 rather than RCX for the fourth argument is not a whim: SYSCALL
	 * destroys RCX before the kernel sees anything, so the fourth argument
	 * could not have been passed there. Linux moved it to R10 for the same
	 * reason, and this kernel matches Linux here deliberately. */
	movq	%r8,  %r9
	movq	%r10, %r8
	movq	%rdx, %rcx
	movq	%rsi, %rdx
	movq	%rdi, %rsi
	movq	%rax, %rdi

	/* Interrupts on. A system call that cannot be interrupted is a system
	 * call a user program can use to stop the machine, by asking for
	 * something slow and never giving the timer a chance to fire. */
	sti
	call	syscall_dispatch
	cli

	addq	$16, %rsp		/* the seventh argument and its gap */

	popq	%r9
	popq	%r8
	popq	%r10
	popq	%rdx
	popq	%rsi
	popq	%rdi
	popq	%r11
	popq	%rcx

	/* Back onto the user stack while GS still points at the kernel's block,
	 * and only then swap it back. The other order reads GS:8 through the
	 * user program's GS base, which is zero, and loads RSP from address 8. */
	movq	%gs:CPU_USER_RSP, %rsp
	swapgs
	sysretq

/* --- The first program ReconOS runs at a lower privilege level ------------
 *
 * In the kernel image because there is nowhere else yet; checkpoint 15 gives
 * user programs a filesystem to be loaded from. Position-independent, because
 * the kernel copies these bytes into a fresh page and maps them at USER_BASE,
 * which is nowhere near where the linker put them -- every address it forms is
 * RIP-relative, and RIP-relative distances survive being moved.
 */
	.section .rodata
	.balign 16

	.global user_test_program
user_test_program:
	/* write(1, msg, msg_end - msg). The length is computed from two
	 * RIP-relative addresses rather than assembled as a constant, so that
	 * nothing depends on where the string was placed. */
	leaq	.Lmsg(%rip), %rsi
	leaq	.Lmsg_end(%rip), %rdx
	subq	%rsi, %rdx
	movq	$1, %rdi		/* the console */
	movq	$1, %rax		/* SYS_WRITE */
	syscall

	/* getpid(): a call with no arguments and a value coming back, which
	 * fails differently from one with three arguments going in. */
	movq	$2, %rax		/* SYS_GETPID */
	syscall

	/* yield(): returns to the scheduler and comes back, which is what
	 * proves user mode is a place the kernel can leave and return to
	 * rather than a one-way door. */
	movq	$4, %rax		/* SYS_YIELD */
	syscall

	/* exit(42). Forty-two rather than zero on purpose: a test that expects
	 * zero also passes when the value never arrives at all. */
	movq	$42, %rdi
	movq	$0, %rax		/* SYS_EXIT */
	syscall

	/* Not reached. Spinning beats falling off the end of the page into
	 * whatever the allocator left behind. */
.Lhang:
	jmp	.Lhang

.Lmsg:
	.ascii	"hello from user mode -- x86_64, ring 3\n"
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
	movq	$1, %rdi
	movabsq	$0xFFFF800000000000, %rsi
	movq	$8, %rdx
	movq	$1, %rax		/* SYS_WRITE */
	syscall

	/* A negative return is a refusal, and a refusal is the correct answer.
	 * A non-negative one means eight bytes of kernel memory just went to the
	 * console because a user program asked for them. */
	testq	%rax, %rax
	jns	.Lleaked

	/* Refused. Now try it without asking. */
	movabsq	$0xFFFF800000000000, %rsi
	movq	(%rsi), %rax

	/* Not reached: the load above faults. */
	movq	$2, %rdi
	movq	$0, %rax
	syscall

.Lleaked:
	movq	$1, %rdi
	movq	$0, %rax		/* SYS_EXIT: exiting at all is the failure */
	syscall
.Lbadhang:
	jmp	.Lbadhang

	.global user_test_bad_end
user_test_bad_end:

	.balign 8
	.global user_test_bad_len
user_test_bad_len:
	.quad	user_test_bad_end - user_test_bad
