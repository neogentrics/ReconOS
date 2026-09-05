/* kmain -- the first portable code that runs.
 *
 * Everything above this point was written for one machine. Everything below
 * it is written once. The whole point of the arch/ split is that this file
 * looks identical no matter what booted it.
 */
#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

#ifndef RECONOS_KERNEL_VERSION
#define RECONOS_KERNEL_VERSION "0.0.0"
#endif

static void banner(void)
{
	char cpu[128];

	kputs("\n");
	kputs("ReconOS kernel " RECONOS_KERNEL_VERSION "\n");
	kprintf("  architecture : %s\n", arch_name());

	arch_cpu_identify(cpu, sizeof(cpu));
	kprintf("  cpu          : %s\n", cpu);
}

void kmain(void)
{
	arch_early_init();
	banner();

	kputs("\nNothing else is implemented yet. Idling.\n");

	/* The idle loop, and the first thing in the kernel that has to be right
	 * about the project's central claim: it sleeps until hardware wakes it,
	 * rather than spinning. There are no interrupt sources configured yet,
	 * so this parks the CPU for good -- which is correct, and measurable. */
	for (;;)
		arch_wait_for_interrupt();
}
