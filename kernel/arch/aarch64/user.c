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

void arch_enter_user(u64 entry, u64 stack_top)
{
	__asm__ volatile(
		"msr	sp_el0, %[sp]\n"	/* the program's own stack */
		"msr	elr_el1, %[pc]\n"	/* where it starts */
		"msr	spsr_el1, %[st]\n"	/* at EL0, with interrupts on */
		"isb\n"
		"eret\n"
		:
		: [sp] "r"(stack_top), [pc] "r"(entry), [st] "r"(SPSR_EL0T)
		: "memory");

	/* eret does not return. */
	for (;;)
		__asm__ volatile("wfi");
}
