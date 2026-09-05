/* The other processors.
 *
 * Until now every processor but one has been asleep in boot.S, parked at the
 * entry point since the firmware released them. This is where they are given
 * something to do.
 *
 * --- What actually has to be true ---
 *
 * Waking a processor is the small part. What makes it safe is that everything
 * two processors can touch at once is guarded, and that anything a processor
 * needs to know about *itself* stops being a global. Both of those are changes
 * to code that already worked, which is why the locking came first.
 *
 * `current` is the clearest example. It was one pointer meaning "the running
 * thread"; on four processors there are four running threads and the question
 * has no single answer. It becomes per-processor, and every reader that said
 * `current` now has to say *whose*.
 */
#ifndef RECON_KERNEL_SMP_H
#define RECON_KERNEL_SMP_H

#include <recon/kernel/types.h>
#include <recon/kernel/compiler.h>
#include <recon/kernel/arch.h>

/* Enough for anything this kernel will meet before it can read a firmware table
 * that says otherwise. The count is checked, and a machine with more processors
 * than this reports the ones it dropped rather than silently using a subset --
 * the same rule the memory map's region cap learned the hard way. */
#define MAX_CPUS 8

struct thread;

/* Everything a processor knows about itself. Indexed by arch_cpu_id(). */
struct cpu_local {
	unsigned id;
	bool online;

	struct thread *current;		/* the thread running *here* */
	struct thread *idle;		/* what runs here when nothing else will */

	u64 switches;
	u64 ticks;
};

extern struct cpu_local cpus[MAX_CPUS];

static inline struct cpu_local *this_cpu(void)
{
	return &cpus[arch_cpu_id()];
}

/* How many processors the machine has, including this one. One until the others
 * are found. */
unsigned smp_cpu_count(void);
unsigned smp_cpus_online(void);

/* Finds the other processors and starts them. Runs after the scheduler, because
 * a processor with nowhere to be scheduled has nothing to do but spin. */
void smp_init(void);

/* Entered by every secondary processor once it is running on the kernel's own
 * page tables with a stack of its own. Does not return. */
RK_NORETURN void smp_secondary_main(unsigned cpu);

void smp_print_summary(void);
bool smp_self_test(void);

/* --- What the architecture provides -------------------------------------- */

/* Discovers the processors and returns how many there are, filling `ids` with
 * whatever identifier the architecture needs to start each one -- an MPIDR
 * value on aarch64, a local APIC identifier on x86_64. Index 0 is this
 * processor. */
unsigned arch_smp_discover(u64 *ids, unsigned max);

/* Starts one processor, which begins at the architecture's secondary entry
 * point with the given stack. Returns false if the firmware refused. */
bool arch_smp_start(u64 id, unsigned cpu, void *stack_top);

/* Brings this processor onto the kernel's page tables, its interrupt
 * controller interface, and its timer. Called by each secondary on itself. */
void arch_smp_cpu_init(void);

#endif /* RECON_KERNEL_SMP_H */
