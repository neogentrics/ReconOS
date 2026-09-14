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

/* The same three questions, on aarch64. See the x86_64 copy for why the values
 * live in stack slots rather than in registers across a system call. */
	.global user_test_facts
user_test_facts:
	sub	sp, sp, #128		/* buffer at [sp], saved copy at [sp,#64] */

	/* random(buf, 16), which must return exactly 16. */
	mov	x0, sp
	mov	x1, #16
	mov	x8, #5			/* SYS_RANDOM */
	svc	#0
	cmp	x0, #16
	b.ne	.Lfacts_1

	ldr	x9, [sp]
	str	x9, [sp, #64]

	/* Again. Identical reads mean a generator that is not advancing. */
	mov	x0, sp
	mov	x1, #16
	mov	x8, #5
	svc	#0
	cmp	x0, #16
	b.ne	.Lfacts_1

	ldr	x9, [sp]
	ldr	x10, [sp, #64]
	cmp	x9, x10
	b.eq	.Lfacts_2

	/* machine(buf, 128): a positive size back, and version 1 at offset 4. */
	mov	x0, sp
	mov	x1, #128
	mov	x8, #6			/* SYS_MACHINE */
	svc	#0
	cmp	x0, #0
	b.le	.Lfacts_3

	ldr	w9, [sp, #4]
	cmp	w9, #1
	b.ne	.Lfacts_4

	/* walltime(), a positive number of nanoseconds. */
	mov	x8, #7			/* SYS_WALLTIME */
	svc	#0
	cmp	x0, #0
	b.le	.Lfacts_5

	/* create() handed a kernel address for its path. See the x86_64 copy:
	 * the pointer check has to happen before anything else, because this is
	 * the one call that both walks a path and sets permissions. */
	movz	x0, #0xFFFF, lsl #48
	movk	x0, #0x8000, lsl #32
	mov	x1, #8
	mov	x2, #0x180		/* 0600 */
	mov	x3, #0
	mov	x4, #0
	mov	x8, #8			/* SYS_CREATE */
	svc	#0
	cmn	x0, #2			/* SYS_EFAULT, and nothing else will do */
	b.ne	.Lfacts_6

	mov	x0, #77
	b	.Lfacts_exit
.Lfacts_1:
	mov	x0, #1
	b	.Lfacts_exit
.Lfacts_2:
	mov	x0, #2
	b	.Lfacts_exit
.Lfacts_3:
	mov	x0, #3
	b	.Lfacts_exit
.Lfacts_4:
	mov	x0, #4
	b	.Lfacts_exit
.Lfacts_5:
	mov	x0, #5
	b	.Lfacts_exit
.Lfacts_6:
	mov	x0, #6
.Lfacts_exit:
	mov	x8, #0			/* SYS_EXIT */
	svc	#0
.Lfacts_hang:
	b	.Lfacts_hang

	.global user_test_facts_end
user_test_facts_end:

	.balign 8
	.global user_test_facts_len
user_test_facts_len:
	.quad	user_test_facts_end - user_test_facts

/* --- a program that signals itself, on aarch64 -----------------------------
 *
 * The same test as the x86_64 one and the same exit codes, because what is
 * being checked is a property of the kernel rather than of an instruction set:
 * a handler runs on the program's own stack, gets the signal number, and the
 * program afterwards is still the program.
 *
 * x19 carries the count. Callee-saved on this ABI, which is what makes the
 * check meaningful -- a caller-saved register would prove nothing.
 */
	.global user_signal_program
user_signal_program:
	mov	x19, #0

	/* sigaction(15, HANDLER, handler, restorer) */
	mov	x0, #15			/* SIGTERM */
	mov	x1, #2			/* SIG_ACTION_HANDLER */
	adr	x2, .Lhandler
	adr	x3, .Lrestorer
	mov	x8, #20			/* SYS_SIGACTION */
	svc	#0

	cbnz	x0, .Lrefused

	/* getpid(), so there is somewhere to send it. */
	mov	x8, #2
	svc	#0
	mov	x20, x0

	/* kill(self, 15) */
	mov	x0, x20
	mov	x1, #15
	mov	x8, #19			/* SYS_KILL */
	svc	#0

	/* A call whose return is where delivery happens. */
	mov	x8, #4			/* SYS_YIELD */
	svc	#0

	/* exit(x19) */
	mov	x0, x19
	mov	x8, #0
	svc	#0

.Lrefused:
	mov	x0, #1
	mov	x8, #0
	svc	#0

.Lhandler:
	/* x0 holds the signal number. Checked, because a handler entered for
	 * the wrong reason looks identical from outside. */
	cmp	x0, #15
	b.ne	.Lwrong_signal

	add	x19, x19, #7
	ret				/* to the restorer, in x30 */

.Lwrong_signal:
	mov	x0, #2
	mov	x8, #0
	svc	#0

.Lrestorer:
	mov	x8, #22			/* SYS_SIGRETURN */
	svc	#0

.Lrhang:
	b	.Lrhang

	.global user_signal_program_end
user_signal_program_end:

	.balign 8
	.global user_signal_program_len
user_signal_program_len:
	.quad	user_signal_program_end - user_signal_program

/* --- a program that maps the screen and draws on it -------------------------
 *
 * The aarch64 half of checkpoint 21's second half. Same steps, same exit codes
 * as the x86_64 one in arch/x86_64/user_entry.S, so a failure means the same
 * thing whichever machine reported it.
 *
 *   1. opens /dev/fb0
 *   2. asks what the screen is -- above all its *pitch*
 *   3. maps the whole of it
 *   4. writes a marker at (0,0) and another at the start of row 1
 *   5. exits with 21
 *
 * The second marker is the point of the first: one pixel at offset zero proves
 * the mapping reached memory, and a pixel exactly `pitch` bytes in proves the
 * program was told the right stride.
 *
 * Most aarch64 machines in the verification rig have no framebuffer at all --
 * AAVMF provides none -- so this program does not run there and the self-test
 * says so rather than failing. It runs on a machine given a display adapter,
 * which is what the kernel's own mode setting is for.
 */
	.global user_fb_program
user_fb_program:
	/* open("/dev/fb0", len, OPEN_READ|OPEN_WRITE, 0) */
	adr	x0, .Lfbpath
	adr	x1, .Lfbpath_end
	sub	x1, x1, x0
	mov	x2, #3			/* OPEN_READ | OPEN_WRITE */
	mov	x3, #0
	mov	x8, #9			/* SYS_OPEN */
	svc	#0

	tbnz	x0, #63, .Lfb_no_open	/* the sign bit: an error */
	mov	x19, x0			/* the descriptor, callee-saved */

	sub	sp, sp, #32

	/* Touch it first, from here.
	 *
	 * The stack is a reserved region rather than mapped memory -- it exists
	 * when the program touches it and not before. So the page under sp does
	 * not exist yet, and the first write to it would otherwise be the
	 * kernel filling this structure in: a fault on a user address taken in
	 * kernel mode, which is a different path from the one demand paging is
	 * for. */
	str	xzr, [sp]

	mov	x0, sp
	mov	x1, #24			/* sizeof(struct fb_info) */
	mov	x8, #24			/* SYS_SCREEN */
	svc	#0

	cmp	x0, #24
	b.ne	.Lfb_no_screen

	/* map(fd, info.bytes) */
	mov	x0, x19
	ldr	x1, [sp, #16]		/* info.bytes */
	mov	x8, #23			/* SYS_MAP */
	svc	#0

	tbnz	x0, #63, .Lfb_no_map

	/* (0,0). Built with movz/movk because a 32-bit constant does not fit in
	 * an instruction on this architecture and a literal pool would have to
	 * be reached PC-relative anyway. */
	movz	w2, #0x434F
	movk	w2, #0x5245, lsl #16	/* 'RECO', little-endian */
	str	w2, [x0]

	/* The start of row 1, which is `pitch` bytes in and nowhere else. */
	ldr	w3, [sp, #8]		/* info.pitch, zero-extended into x3 */
	movz	w4, #0x534F
	movk	w4, #0x214E, lsl #16	/* 'OSN!' */
	str	w4, [x0, x3]

	/* One read back through the same mapping. A mapping that took a store
	 * and cannot return it is not memory. */
	ldr	w5, [x0]
	cmp	w5, w2
	b.ne	.Lfb_not_memory

	mov	x0, #21			/* checkpoint 21, and nothing else is */
	mov	x8, #0			/* SYS_EXIT */
	svc	#0

.Lfb_no_open:
	mov	x0, #1
	mov	x8, #0
	svc	#0
.Lfb_no_screen:
	mov	x0, #2
	mov	x8, #0
	svc	#0
.Lfb_no_map:
	mov	x0, #3
	mov	x8, #0
	svc	#0
.Lfb_not_memory:
	mov	x0, #4
	mov	x8, #0
	svc	#0

.Lfb_hang:
	b	.Lfb_hang

.Lfbpath:
	.ascii	"/dev/fb0"
.Lfbpath_end:

	.global user_fb_program_end
user_fb_program_end:

	.balign 8
	.global user_fb_program_len
user_fb_program_len:
	.quad	user_fb_program_end - user_fb_program


/* --- asking to stop the machine without the right to --------------------
 *
 * SYS_POWER is behind CAP_SHUTDOWN, and the kernel cannot test that refusal on
 * itself: a kernel thread's capabilities are the absence of a process rather
 * than a field, so `capability_drop` is a documented no-op there and
 * `capable(CAP_SHUTDOWN)` stays true. Asked to be refused, it would be obeyed.
 *
 * So the question is put by a process, which holds capabilities in a field.
 *
 * **What is actually being proved, stated exactly.** `process_create` gives a
 * process `(uid == UID_KERNEL) ? CAP_ALL : 0`, so this program starts holding
 * *nothing*. The drop below is therefore a no-op today. It is written anyway
 * because it is what a real program does -- give up what it will not need --
 * and because the line after it is not a no-op and never will be.
 *
 * **The read-back is the safety argument.** `sys_dropcap` returns what is
 * still held, and this asks nothing of the machine unless CAP_SHUTDOWN is
 * genuinely gone. That branch cannot be taken today, and saying so is the
 * point: this program runs inside the self-tests on every path in the matrix,
 * and a version that asked first and checked afterwards would power off
 * twenty-eight guests the first day a process started with capabilities.
 *
 * So the assertion is *a program that does not hold CAP_SHUTDOWN is refused*,
 * which is the property that matters, and the guard is there for a day that
 * has not come.
 */
	.global user_test_power
user_test_power:
	/* dropcap(CAP_SHUTDOWN). Bit 2, so the value is 4. */
	mov	x0, #4
	mov	x8, #17			/* SYS_DROPCAP */
	svc	#0

	/* x0 is what is still held. Z is set when the bit is clear, so a
	 * branch on "not equal" is a branch on "the drop did not take". */
	tst	x0, #4
	b.ne	.Lpower_still_held

	/* Now it must refuse, and this is the assertion. */
	mov	x0, #0			/* POWER_ACTION_OFF */
	mov	x8, #25			/* SYS_POWER */
	svc	#0

	/* cmn adds, so this sets Z when x0 is exactly -12, which is SYS_EPERM.
	 * The same idiom the facts program uses for SYS_EFAULT. */
	cmn	x0, #12
	b.ne	.Lpower_wrong

	mov	x0, #60			/* refused, as it must be */
	b	.Lpower_exit
.Lpower_still_held:
	mov	x0, #2
	b	.Lpower_exit
.Lpower_wrong:
	mov	x0, #1
.Lpower_exit:
	mov	x8, #0			/* SYS_EXIT */
	svc	#0
.Lpower_hang:
	b	.Lpower_hang

	.global user_test_power_end
user_test_power_end:

	.balign 8
	.global user_test_power_len
user_test_power_len:
	.quad	user_test_power_end - user_test_power
