/* Dropping to EL0 on aarch64, and coming back.
 *
 * ARM makes this remarkably direct. There are no descriptors, no segment
 * registers, no special instruction pair to set up. A privilege level is a
 * *number in the saved processor state*, and the same `eret` that returns from
 * an exception is what enters user mode: set the state to say EL0, set the
 * return address, set the stack, and return to somewhere you were never called
 * from.
 *
 * Coming back is the exception mechanism already built at checkpoint 7. `svc`
 * raises a synchronous exception from a lower level, which lands in vector slot
 * 8 -- one of the sixteen that file fills and that nothing has used until now.
 */
#include "aarch64.h"

#include <recon/kernel/user.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/console.h>

/* SPSR_EL1's low bits select the level to return to. Zero is EL0 using SP_EL0,
 * which is what a user program wants: its own stack, unrelated to the kernel's.
 *
 * The DAIF bits are left clear, which enables interrupts on entry to user mode.
 * That is not a detail -- a user program that runs with interrupts masked
 * cannot be preempted, and one loop would stop the machine. */
#define SPSR_EL0T 0x00000000ULL

void arch_user_init(void)
{
	/* Nothing to set up. The exception vectors installed at checkpoint 7
	 * already cover exceptions from a lower level, and there is no
	 * equivalent of x86's descriptor tables or SYSCALL registers.
	 *
	 * This function exists so that the portable side has one shape on both
	 * architectures. An empty implementation that says why is better than a
	 * conditional at the call site. */
}

void arch_thread_switched_in(struct thread *t)
{
	/* Nothing, and this is not a stub -- it is a real difference between the
	  * two architectures.
	  *
	  * x86_64 has to be told where a trap from user mode should land, in a
	  * task-state segment and a per-processor block, and both belong to the
	  * processor while the stack they name belongs to the thread. That gap
	  * is what a preemptible system call falls into when the thread is
	  * rescheduled onto another processor.
	  *
	  * Here there is no gap, because there is no second place holding the
	  * answer. An exception from EL0 switches to SP_EL1, and SP_EL1 *is* the
	  * kernel stack pointer this processor is using -- which the context
	  * switch has just set to the incoming thread's own stack. The value is
	  * right by construction rather than by being copied somewhere in time.
	  */
	(void)t;
}

void arch_enter_user(u64 entry, u64 stack_top)
{
	__asm__ volatile(
		"msr	sp_el0, %[sp]\n"	/* the program's own stack */
		"msr	elr_el1, %[pc]\n"	/* where it starts */
		"msr	spsr_el1, %[st]\n"	/* at EL0, with interrupts on */
		"isb\n"
	
	/* Every register the program has no business seeing, zeroed.
	 *
	 * Without this a program's first instruction reads whatever the
	 * kernel last left behind: kernel stack addresses, pointers into
	 * kernel structures, whatever the last call was holding. None of it
	 * is secret the way a key is; all of it is the map somebody needs to
	 * aim at the kernel with.
	 *
	 * After the system registers are set, because the operands live in
	 * some of these -- and x30 included, because the link register holds
	 * a kernel return address and is as much of a leak as any other.
	 * SP_EL0 is untouched: it was just given the program's own stack. */
	"mov	x0, xzr\n"
	"mov	x1, xzr\n"
	"mov	x2, xzr\n"
	"mov	x3, xzr\n"
	"mov	x4, xzr\n"
	"mov	x5, xzr\n"
	"mov	x6, xzr\n"
	"mov	x7, xzr\n"
	"mov	x8, xzr\n"
	"mov	x9, xzr\n"
	"mov	x10, xzr\n"
	"mov	x11, xzr\n"
	"mov	x12, xzr\n"
	"mov	x13, xzr\n"
	"mov	x14, xzr\n"
	"mov	x15, xzr\n"
	"mov	x16, xzr\n"
	"mov	x17, xzr\n"
	"mov	x18, xzr\n"
	"mov	x19, xzr\n"
	"mov	x20, xzr\n"
	"mov	x21, xzr\n"
	"mov	x22, xzr\n"
	"mov	x23, xzr\n"
	"mov	x24, xzr\n"
	"mov	x25, xzr\n"
	"mov	x26, xzr\n"
	"mov	x27, xzr\n"
	"mov	x28, xzr\n"
	"mov	x29, xzr\n"
	"mov	x30, xzr\n"
	
		"eret\n"
		:
		: [sp] "r"(stack_top), [pc] "r"(entry), [st] "r"(SPSR_EL0T)
		: "memory");

	/* eret does not return. */
	for (;;)
		__asm__ volatile("wfi");
}
