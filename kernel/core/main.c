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

	/* Run at boot rather than in a test harness, because there is no test
	 * harness that can run a kernel yet, and an allocator that is quietly
	 * wrong is the kind of fault that surfaces three checkpoints later as
	 * something else's bug. */
	kputs("\nSelf-tests\n");
	kprintf("  physical allocator : %s\n",
		pmm_self_test() ? "pass" : "FAIL");

	kputs("\nNothing else is implemented yet. Idling.\n");

	/* The idle loop, and the first thing in the kernel that has to be right
	 * about the project's central claim: it sleeps until hardware wakes it,
	 * rather than spinning. There are no interrupt sources configured yet,
	 * so this parks the CPU for good -- which is correct, and measurable. */
	for (;;)
		arch_wait_for_interrupt();
}
