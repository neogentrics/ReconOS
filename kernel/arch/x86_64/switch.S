/* The context switch.
 *
 * This is the function that cannot be written in C, and it is worth being
 * precise about why. It is called on one stack and *returns on another*. C has
 * no way to say that: the language's whole model is that a function returns to
 * the place it was called from, with the stack it was called with.
 *
 * What makes it work is that the calling convention already says which
 * registers a function must preserve. So saving exactly those, swapping the
 * stack pointer, and restoring exactly those from the new stack leaves the CPU
 * in a state that the *other* thread's compiler-generated code believes is its
 * own. The caller-saved registers need no attention at all -- whoever called
 * this function on the other thread already knows they may have changed.
 *
 * The System V calling convention for x86_64 says a function must preserve
 * RBX, RBP, R12, R13, R14 and R15. Those six, and nothing else.
 */

	.section .text

/* void arch_context_switch(void **save_to, void *new_sp);
 *   RDI = where to write the outgoing stack pointer
 *   RSI = the incoming stack pointer
 */
	.global arch_context_switch
arch_context_switch:
	pushq	%rbx
	pushq	%rbp
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	movq	%rsp, (%rdi)		/* the outgoing thread is now saved entirely */
	movq	%rsi, %rsp		/* and from here we are the other thread */

	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbp
	popq	%rbx
	ret				/* returns into the *other* thread's caller */

/* Where a brand-new thread begins.
 *
 * arch_thread_stack_init() builds a stack whose return address is this label
 * and whose saved R12 and R13 hold the function to run and its argument. So the
 * first switch into a new thread arrives here through the RET above, with those
 * two registers already loaded -- which is why they are the two chosen: they
 * are callee-saved, so the restore sequence puts them in place for free.
 */
	.global thread_trampoline
thread_trampoline:
	/* Interrupts are masked, because the switch that brought us here
	 * happened inside an interrupt handler or with them off. A thread that
	 * starts with interrupts masked can never be preempted, so it would run
	 * to completion and the scheduler would look broken in a way that had
	 * nothing to do with the scheduler. */
	sti

	movq	%r13, %rdi		/* the argument */
	callq	*%r12			/* the thread's function */

	/* Returning from a thread's function ends the thread. Nothing has to
	 * remember to call this. */
	callq	thread_exit

	/* thread_exit does not return. If it somehow does, stop here rather
	 * than execute whatever follows. */
1:	hlt
	jmp	1b
