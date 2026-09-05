/* kmain -- the first portable code that runs.
 *
 * Everything above this point was written for one machine. Everything below
 * it is written once. The whole point of the arch/ split is that this file
 * looks identical no matter what booted it.
 */
#include <recon/kernel/arch.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/cpu.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/smp.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/time.h>
#include <recon/kernel/trap.h>
#include <recon/kernel/user.h>
#include <recon/kernel/vm.h>

#ifndef RECONOS_KERNEL_VERSION
#define RECONOS_KERNEL_VERSION "0.0.0"
#endif

static struct cpu_caps cpu;

static void banner(void)
{
	kputs("\n");
	kputs("ReconOS kernel " RECONOS_KERNEL_VERSION "\n");
	kprintf("  architecture : %s\n", arch_name());
}

void kmain(void)
{
	arch_early_init();
	banner();

	arch_cpu_caps(&cpu);
	cpu_print_caps(&cpu);

	boot_print_summary();

	pmm_init();
	pmm_print_summary();

	vm_init();
	vm_print_summary();

	heap_init();
	heap_print_summary();

	/* After the heap, because a fault report is more useful than a fault,
	 * and before anything that might fault. */
	trap_init();

	/* After the fault handlers, because enabling interrupts without
	 * somewhere for them to go is how a machine resets while telling you
	 * nothing. */
	/* The scheduler before the timer, so that the first tick has something
	 * to tick. Started here rather than earlier because it allocates. */
	sched_init();

	time_init();
	time_print_summary();

	/* After the timer, because a processor with no tick cannot be preempted
	 * and the test for that has to have a clock to wait on. */
	smp_init();
	smp_print_summary();

	/* Last, because it is the one thing that needs everything: pages to map,
	 * page tables that can express "user may reach this", a fault handler to
	 * catch the program when it is wrong, a thread to run it in, and a timer
	 * to take it back off the processor. */
	user_init();

	/* Run at boot rather than in a test harness, because there is no test
	 * harness that can run a kernel yet, and an allocator that is quietly
	 * wrong is the kind of fault that surfaces three checkpoints later as
	 * something else's bug. */
	kputs("\nSelf-tests\n");
	kprintf("  physical allocator : %s\n",
		pmm_self_test() ? "pass" : "FAIL");
	kprintf("  virtual memory     : %s\n",
		vm_self_test() ? "pass" : "FAIL");
	kprintf("  kernel heap        : %s\n",
		heap_self_test() ? "pass" : "FAIL");
	kprintf("  fault handling     : %s\n",
		trap_self_test() ? "pass" : "FAIL");
	kprintf("  clock and tick     : %s\n",
		time_self_test() ? "pass" : "FAIL");
	kprintf("  threads            : %s\n",
		sched_self_test() ? "pass" : "FAIL");
	kprintf("  locking            : %s\n",
		lock_self_test() ? "pass" : "FAIL");
	kprintf("  processors         : %s\n",
		smp_self_test() ? "pass" : "FAIL");
	kprintf("  user mode          : %s\n",
		user_self_test() ? "pass" : "FAIL");
	kprintf("  the boundary holds : %s\n",
		user_boundary_test() ? "pass" : "FAIL");

	sched_print_summary();
	user_print_summary();

	kputs("\nNothing else is implemented yet. Idling.\n");

	/* The idle loop, and the first thing in the kernel that has to be right
	 * about the project's central claim: it sleeps until hardware wakes it,
	 * rather than spinning.
	 *
	 * As of checkpoint 8 there is something to wake it: the tick, a hundred
	 * times a second. That is the simple correct thing and it is also, on an
	 * idle machine, a hundred wakeups a second doing nothing -- which is how
	 * a laptop runs warm with nothing running. Making the tick stop when
	 * there is nothing to wake for belongs with the scheduler, and is
	 * recorded in docs/KERNEL.md rather than left to be noticed. */
	for (;;)
		arch_wait_for_interrupt();
}
