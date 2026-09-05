/* Waking the other processors on x86_64 -- not yet.
 *
 * Reported rather than pretended, because a kernel that says it has four
 * processors and runs on one is worse than a kernel that says it has one.
 *
 * What it needs, so the size of it is on the record rather than in somebody's
 * head:
 *
 *   READING ACPI. The processors are listed in the MADT, one of the tables the
 *   root pointer leads to. Checkpoint 1 already finds that pointer on all three
 *   x86 boot paths; nothing walks the tables yet.
 *
 *   A REAL-MODE TRAMPOLINE. A processor started by the local APIC begins in
 *   16-bit real mode at a page below one megabyte -- not because anything needs
 *   it to, but because the startup message carries a page number in one byte.
 *   So the kernel has to place code in low memory that walks the same road the
 *   boot trampoline walks: protected mode, page tables, long mode.
 *
 *   THE STARTUP SEQUENCE ITSELF. INIT, then a delay, then twice a start-up
 *   message. It is hand-timed, the timings come from a manual, and a processor
 *   that misses one simply does not appear.
 *
 * None of that is hard so much as long, and every part of it is testable
 * separately. ARM needed none of it: PSCI is one call into firmware.
 */
#include "x86_64.h"

#include <recon/kernel/smp.h>
#include <recon/kernel/console.h>

unsigned arch_smp_discover(u64 *ids, unsigned max)
{
	ids[0] = 0;
	return 1;
}

bool arch_smp_start(u64 id, unsigned cpu, void *stack_top)
{
	return false;
}

void arch_smp_cpu_init(void)
{
	/* Nothing calls this: no secondary processor is ever started. */
}
