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
 * KERNEL_GS_BASE holds this processor's block.
 *
 * --- AND WHY BOTH SWAPGS ARE AT THE TOP ---
 *
 * This used to swap on the way in and swap back on the way out, holding the
 * kernel's GS across the whole call and reading the saved user stack pointer
 * out of GS:8 at the end. That is the ordinary arrangement and it was wrong
 * here, for a reason that only exists on a kernel with more than one
 * processor and a preemptible system call.
 *
 * Interrupts are enabled across syscall_dispatch, deliberately -- see below.
 * So the thread can be preempted in the middle of a call and picked up by a
 * **different processor**. Two things then break at once, and both are
 * silent until they are not:
 *
 *   - GS_BASE is a register, one per processor, and it does not travel with
 *     the thread. The processor the call returns on has whatever GS_BASE it
 *     had -- zero, if it has been running kernel threads and never came from
 *     user mode -- so `movq %gs:8, %rsp` reads address 8 and takes a page
 *     fault in kernel mode, on the way out of a system call that worked.
 *
 *   - Even with GS right, GS:8 is the *other* processor's slot. The user
 *     stack pointer was written into processor A's block and would be read
 *     out of processor B's: the program returns standing on some other
 *     thread's stack.
 *
 * The fix is that per-processor storage is only used for the few instructions
 * where there is no alternative -- before this thread has a stack -- and the
 * user stack pointer is then carried on the kernel stack, which is the
 * thread's own and travels with it. GS goes back to the user's immediately
 * afterwards and the kernel never touches it again, so there is no swap on
 * the way out and no window at all in which a migration matters.
 *
 * The two instructions between the swaps run with interrupts masked, by
 * SYSCALL itself, through IA32_FMASK.
 */

	.section .text

	.set CPU_KERNEL_RSP, 0		/* GS:0  -- where to stand */
	.set CPU_USER_RSP,   8		/* GS:8  -- where we were standing */

	.global x86_syscall_entry
x86_syscall_entry:
	swapgs
	movq	%rsp, %gs:CPU_USER_RSP
	movq	%gs:CPU_KERNEL_RSP, %rsp

	/* Off the per-processor block and onto this thread's own stack, which
	  * is the whole point: from here on nothing this call needs lives
	  * anywhere that belongs to a processor rather than to the thread. */
	pushq	%gs:CPU_USER_RSP
	swapgs

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

	/* Nine pushes, which leaves the stack eight off sixteen-byte alignment
	  * -- so the seventh C argument, which goes on the stack, puts it back
	  * rather than breaking it. It used to be eight pushes and a gap pushed
	  * alongside the argument; carrying the user stack pointer here is the
	  * ninth, and it fills that gap exactly. */
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

	addq	$8, %rsp		/* the seventh argument */

	/* --- the one place a return to user mode asks a question -----------
	 *
	 * Everything below this is the same fixed sequence of pops and a
	 * SYSRET it has always been. What changed is that the registers those
	 * pops are about to read can now be *edited* first -- which is how a
	 * signal handler is entered without this path gaining a branch.
	 *
	 * The return value is in RAX and nothing here may disturb it, so it is
	 * pushed and popped around the call. RDI then points at the saved
	 * register block:
	 *
	 *   [0] r9  [1] r8  [2] r10  [3] rdx  [4] rsi  [5] rdi
	 *   [6] rflags      [7] user rip      [8] user rsp
	 *
	 * Interrupts are already off and stay off. A signal delivered here is
	 * delivered on the kernel stack with the user's registers still
	 * saved, which is the only moment where both are true. */
	pushq	%rax
	leaq	8(%rsp), %rdi
	call	signal_on_return
	popq	%rax

	popq	%r9
	popq	%r8
	popq	%r10
	popq	%rdx
	popq	%rsi
	popq	%rdi
	popq	%r11
	popq	%rcx

	/* Back onto the user stack, taken from where this thread left it rather
	  * than from whichever processor happens to be running now. GS is
	  * already the user's -- it was swapped back at the top -- so there is
	  * nothing left to undo.
	  *
	  * Interrupts are off, and must be: between this instruction and SYSRET
	  * the processor is in ring 0 standing on a user stack, and an interrupt
	  * taken there would push its frame onto memory the program chose. */
	popq	%rsp
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


/* --- a program that signals itself -----------------------------------------
 *
 * The end-to-end proof, and it has to be a real program in ring 3 because
 * every interesting part of signal delivery happens on the boundary between
 * there and here. A kernel-side test can check that the decision logic decides
 * correctly; only this can check that a handler *runs* and that the program
 * afterwards is still the program.
 *
 * What it does, in order:
 *
 *   1. registers a handler for signal 15, with a restorer
 *   2. asks the kernel to send signal 15 to itself
 *   3. makes any system call, which is what gives the kernel a return to
 *      deliver on -- the signal is pending from step 2 and arrives here
 *   4. the handler adds to a counter in a register the ABI says survives
 *   5. the restorer invokes SYS_SIGRETURN and the program continues
 *   6. exits with the counter, which is 7 only if all of that happened
 *
 * Seven rather than one: a counter that ends at one also ends at one when the
 * handler ran twice and the restore lost an increment, and zero is what a
 * program that never entered the handler at all reports.
 */
	.global user_signal_program
user_signal_program:
	/* RBX holds the count. Callee-saved, so the kernel's own save and
	 * restore across the handler is what is being checked -- a scratch
	 * register would prove nothing, because nothing promises to keep one. */
	xorq	%rbx, %rbx

	/* sigaction(15, HANDLER, .Lhandler, .Lrestorer) */
	movq	$15, %rdi		/* SIGTERM */
	movq	$2, %rsi		/* SIG_ACTION_HANDLER */
	leaq	.Lhandler(%rip), %rdx
	leaq	.Lrestorer(%rip), %r10
	movq	$20, %rax
	syscall

	/* It was refused. Exit with 1 so the failure is distinguishable from
	 * the handler never running, which exits with 0. */
	testq	%rax, %rax
	jnz	.Lrefused

	/* getpid(), so the signal has somewhere to be sent. */
	movq	$2, %rax		/* SYS_GETPID */
	syscall
	movq	%rax, %r12

	/* kill(self, 15) */
	movq	%r12, %rdi
	movq	$15, %rsi
	movq	$19, %rax
	syscall

	/* A system call whose return is where the signal gets delivered. The
	 * handler runs somewhere inside this one. */
	movq	$4, %rax		/* SYS_YIELD */
	syscall

	/* exit(rbx). Seven if the handler ran once and the restore put
	 * everything back. */
	movq	%rbx, %rdi
	movq	$0, %rax		/* SYS_EXIT */
	syscall

.Lrefused:
	movq	$1, %rdi
	movq	$0, %rax
	syscall

.Lhandler:
	/* RDI holds the signal number the kernel delivered. Checked, because a
	 * handler entered with the wrong number is a handler that would run for
	 * the wrong reason and look identical from outside. */
	cmpq	$15, %rdi
	jne	.Lwrong_signal

	addq	$7, %rbx
	ret				/* to the restorer the kernel pushed */

.Lwrong_signal:
	movq	$2, %rdi
	movq	$0, %rax
	syscall

.Lrestorer:
	movq	$22, %rax
	syscall

	/* Not reached: SYS_SIGRETURN does not come back here, it comes back
	 * where the handler interrupted. Spinning beats running on. */
.Lrhang:
	jmp	.Lrhang

	.global user_signal_program_end
user_signal_program_end:

	.global user_signal_program_len
user_signal_program_len:
	.quad	user_signal_program_end - user_signal_program

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

/* A program that asks the kernel the three questions the desktop currently
 * asks the host's /proc.
 *
 * It checks its own answers and reports which one failed through its exit code,
 * because a program that exits normally after getting nonsense is a test that
 * passes. Every value it compares against is one the kernel cannot return by
 * accident: a length that must be exactly what was asked for, two random reads
 * that must differ, and a structure version that must be one.
 *
 * Values are kept in stack slots rather than in registers across a system
 * call. Which registers survive a syscall is a property of the entry path in
 * another file, and a test that quietly depends on it is a test that breaks
 * when that path is optimised.
 */
	.global user_test_facts
user_test_facts:
	subq	$128, %rsp		/* buffer at 0(%rsp), saved copy at 64 */

	/* random(buf, 16), which must return exactly 16. */
	movq	%rsp, %rdi
	movq	$16, %rsi
	movq	$5, %rax		/* SYS_RANDOM */
	syscall
	cmpq	$16, %rax
	jne	.Lfacts_1

	movq	(%rsp), %rax
	movq	%rax, 64(%rsp)

	/* Again. Two reads that come back identical mean a generator that is
	 * not advancing, which every other check here would pass. */
	movq	%rsp, %rdi
	movq	$16, %rsi
	movq	$5, %rax
	syscall
	cmpq	$16, %rax
	jne	.Lfacts_1

	movq	(%rsp), %rax
	cmpq	64(%rsp), %rax
	je	.Lfacts_2

	/* machine(buf, 128). The return is the size the kernel would write, so
	 * it must be positive; the version field at offset 4 must be 1. */
	movq	%rsp, %rdi
	movq	$128, %rsi
	movq	$6, %rax		/* SYS_MACHINE */
	syscall
	cmpq	$0, %rax
	jle	.Lfacts_3

	movl	4(%rsp), %eax
	cmpl	$1, %eax
	jne	.Lfacts_4

	/* walltime(), which must be a positive number of nanoseconds. */
	movq	$7, %rax		/* SYS_WALLTIME */
	syscall
	cmpq	$0, %rax
	jle	.Lfacts_5

	/* create() handed a kernel address for its path.
	 *
	 * The one thing about this call that must be true on every machine,
	 * including the ones with no filesystem at all: the pointer check
	 * happens before anything else. A kernel that walked a path through an
	 * address the caller does not own would do it while creating a file
	 * whose permissions the caller chose, which is the worst possible
	 * combination of the two.
	 *
	 * The direct map's base, which is every byte of physical memory. Fourth
	 * argument in R10 rather than RCX, because SYSCALL destroys RCX before
	 * the kernel sees it. */
	movabsq	$0xFFFF800000000000, %rdi
	movq	$8, %rsi
	movq	$0x180, %rdx		/* 0600 */
	movq	$0, %r10
	movq	$0, %r8
	movq	$8, %rax		/* SYS_CREATE */
	syscall
	cmpq	$-2, %rax		/* SYS_EFAULT, and nothing else will do */
	jne	.Lfacts_6

	movq	$77, %rdi
	jmp	.Lfacts_exit
.Lfacts_1:
	movq	$1, %rdi
	jmp	.Lfacts_exit
.Lfacts_2:
	movq	$2, %rdi
	jmp	.Lfacts_exit
.Lfacts_3:
	movq	$3, %rdi
	jmp	.Lfacts_exit
.Lfacts_4:
	movq	$4, %rdi
	jmp	.Lfacts_exit
.Lfacts_5:
	movq	$5, %rdi
	jmp	.Lfacts_exit
.Lfacts_6:
	movq	$6, %rdi
.Lfacts_exit:
	movq	$0, %rax		/* SYS_EXIT */
	syscall
.Lfacts_hang:
	jmp	.Lfacts_hang

	.global user_test_facts_end
user_test_facts_end:

	.balign 8
	.global user_test_facts_len
user_test_facts_len:
	.quad	user_test_facts_end - user_test_facts
