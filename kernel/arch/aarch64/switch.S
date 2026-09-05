/* The context switch.
 *
 * Called on one stack and returns on another -- which C cannot express, because
 * the language's model is that a function returns where it was called from with
 * the stack it was called with. See the x86_64 file for the longer version of
 * the argument; it is the same argument.
 *
 * AAPCS64 says a function must preserve x19 through x28, the frame pointer x29,
 * and the link register x30. Those twelve, and nothing else -- the caller-saved
 * registers need no attention, because whoever called this on the other thread
 * already knows they may have changed.
 *
 * x30 is the one to notice. On this architecture the return address lives in a
 * register rather than on the stack, so saving and restoring it *is* what makes
 * the function return somewhere else.
 */

	.section .text

/* void arch_context_switch(void **save_to, void *new_sp);
 *   x0 = where to write the outgoing stack pointer
 *   x1 = the incoming stack pointer
 */
	.global arch_context_switch
arch_context_switch:
	sub	sp, sp, #(12 * 8)
	stp	x19, x20, [sp, #(0 * 8)]
	stp	x21, x22, [sp, #(2 * 8)]
	stp	x23, x24, [sp, #(4 * 8)]
	stp	x25, x26, [sp, #(6 * 8)]
	stp	x27, x28, [sp, #(8 * 8)]
	stp	x29, x30, [sp, #(10 * 8)]

	mov	x2, sp
	str	x2, [x0]		/* the outgoing thread is now saved entirely */
	mov	sp, x1			/* and from here we are the other thread */

	ldp	x19, x20, [sp, #(0 * 8)]
	ldp	x21, x22, [sp, #(2 * 8)]
	ldp	x23, x24, [sp, #(4 * 8)]
	ldp	x25, x26, [sp, #(6 * 8)]
	ldp	x27, x28, [sp, #(8 * 8)]
	ldp	x29, x30, [sp, #(10 * 8)]
	add	sp, sp, #(12 * 8)

	ret				/* returns into the *other* thread's caller */

/* Where a brand-new thread begins.
 *
 * The stack built by arch_thread_stack_init() has this address in the saved
 * x30 slot and the thread's function and argument in x19 and x20 -- so the RET
 * above lands here with both already loaded, for free, because the restore
 * sequence puts them there.
 */
	.global thread_trampoline
thread_trampoline:
	/* Unmask interrupts. The switch that brought us here ran with them
	 * masked, and a thread that starts masked can never be preempted -- it
	 * would run to completion and make the scheduler look broken for a
	 * reason that has nothing to do with the scheduler. */
	msr	daifclr, #2

	mov	x0, x20			/* the argument */
	blr	x19			/* the thread's function */

	/* Returning from a thread's function ends the thread. */
	bl	thread_exit

1:	wfi
	b	1b
