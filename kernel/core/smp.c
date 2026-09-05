#include <recon/kernel/smp.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/time.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/arch.h>

struct cpu_local cpus[MAX_CPUS];

static unsigned cpu_count = 1;
static unsigned dropped;
static u64 cpu_ids[MAX_CPUS];

/* Set by each secondary once it is running, read by the boot processor while it
 * waits. Volatile because the two are different processors and the compiler has
 * no reason to expect this to change under it. */
static volatile unsigned online_count = 1;

unsigned smp_cpu_count(void)   { return cpu_count; }
unsigned smp_cpus_online(void) { return online_count; }

/* What a processor runs when nothing else will have it.
 *
 * Not a spin loop: `arch_wait_for_interrupt` stops the processor until
 * something happens. On a machine with four cores and one thread of work, three
 * of them sit here drawing almost nothing, which is the difference between an
 * idle machine that is cool and one that is warm.
 */
static void idle_loop(void *arg)
{
	for (;;) {
		arch_wait_for_interrupt();

		/* The tick woke us. If there is real work now, take it; if not,
		 * this returns immediately and we wait again. */
		sched_yield();
	}
}

void smp_init(void)
{
	unsigned found;

	for (unsigned i = 0; i < MAX_CPUS; i++) {
		cpus[i].id = i;
		cpus[i].online = false;
	}

	cpus[arch_cpu_id()].online = true;

	found = arch_smp_discover(cpu_ids, MAX_CPUS);

	if (found > MAX_CPUS) {
		/* Reported, never silently truncated. The memory map's region
		 * cap taught this: a limit that quietly drops what it cannot
		 * hold produces a wrong number rather than an error. */
		dropped = found - MAX_CPUS;
		found = MAX_CPUS;
	}

	cpu_count = found ? found : 1;

	for (unsigned i = 1; i < cpu_count; i++) {
		paddr_t stack = pmm_alloc_pages(4);
		char name[THREAD_NAME_MAX] = "idle-0";
		struct thread *idle;

		if (!stack) {
			kprintf("  smp: no memory for processor %u's stack\n", i);
			break;
		}

		/* An idle thread of its own, created here rather than there,
		 * because the scheduler's structures are guarded by a lock this
		 * processor already holds the right to take -- and the one being
		 * started does not yet exist as far as the scheduler knows. */
		name[5] = (char)('0' + i);
		idle = thread_create(name, idle_loop, 0);
		if (!idle) {
			pmm_free_pages(stack, 4);
			kprintf("  smp: no memory for processor %u's idle thread\n", i);
			break;
		}

		cpus[i].idle = idle;

		if (!arch_smp_start(cpu_ids[i], i,
				    (u8 *)phys_to_virt(stack) + 4 * PAGE_SIZE)) {
			kprintf("  smp: the firmware refused to start processor %u\n", i);
			pmm_free_pages(stack, 4);
			continue;
		}
	}

	/* Wait for them, but not forever. A processor that never reports is a
	 * fact worth printing, and hanging the boot to wait for it would be the
	 * least useful possible response. */
	{
		u64 deadline = time_monotonic_ns() + 1000000000ULL;

		while (online_count < cpu_count &&
		       time_monotonic_ns() < deadline)
			arch_cpu_relax();
	}
}

void smp_secondary_main(unsigned cpu)
{
	/* Page tables, interrupt controller, timer -- everything this processor
	 * needs that the boot processor could not do on its behalf. */
	arch_smp_cpu_init();

	cpus[cpu].online = true;
	__atomic_add_fetch(&online_count, 1, __ATOMIC_RELEASE);

	/* Adopt this processor's idle thread as what is running here, then hand
	 * over to the scheduler. From this point the processor is an equal: the
	 * tick preempts it and the run queue feeds it. */
	sched_adopt_idle(cpus[cpu].idle);

	idle_loop(0);

	/* idle_loop does not return. */
	for (;;)
		arch_wait_for_interrupt();
}

void smp_print_summary(void)
{
	kprintf("\nProcessors\n");
	kprintf("  found        : %u, %u online\n", cpu_count, online_count);

	if (dropped)
		kprintf("  WARNING      : %u more than this kernel can hold\n", dropped);

	for (unsigned i = 0; i < cpu_count && i < MAX_CPUS; i++)
		kprintf("  cpu %u        : %s, %lu ticks, %lu switches\n",
			i, cpus[i].online ? "online" : "did not start",
			cpus[i].ticks, cpus[i].switches);
}

bool smp_self_test(void)
{
	bool ok = true;

	if (online_count > cpu_count) {
		kputs("  smp: more processors reported online than were found\n");
		ok = false;
	}

	if (online_count < cpu_count) {
		kprintf("  smp: %u of %u processors did not start\n",
			cpu_count - online_count, cpu_count);
		ok = false;
	}

	/* On a machine with more than one processor, the others must actually be
	 * taking ticks -- a processor that is "online" but never interrupted is
	 * a processor whose timer was never started, and it would sit in its
	 * idle loop forever looking perfectly healthy. */
	if (cpu_count > 1) {
		u64 deadline = time_monotonic_ns() + 500000000ULL;
		bool all_ticking = false;

		while (time_monotonic_ns() < deadline) {
			all_ticking = true;
			for (unsigned i = 1; i < cpu_count; i++)
				if (cpus[i].online && cpus[i].ticks == 0)
					all_ticking = false;
			if (all_ticking)
				break;
			arch_cpu_relax();
		}

		if (!all_ticking) {
			kputs("  smp: a processor is online but its timer never "
			      "fired, so nothing can ever preempt it\n");
			ok = false;
		}
	}

	return ok;
}
