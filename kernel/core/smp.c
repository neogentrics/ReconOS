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

/* What each processor says it is, written by that processor about itself.
 *
 * This exists because BG-153 was invisible to every test there was. On
 * aarch64 arch_cpu_id() returned MPIDR affinity level 0 -- the processor
 * within its cluster -- and on a two-socket or big.LITTLE machine two
 * processors answer the same number and index the same entry of every
 * per-processor array in the kernel. Nothing fails until they touch it at the
 * same moment, and no assertion anywhere would have noticed.
 *
 * So each processor writes down who it thinks it is, and the boot processor
 * checks afterwards that the answers are distinct and that each one matches
 * the slot that processor was started into. It is cheap and it is the only
 * thing standing between this kernel and a fault that presents as random
 * corruption on hardware nobody here owns. */
static unsigned claimed_id[MAX_CPUS];
static bool claimed[MAX_CPUS];

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
	cpus[arch_cpu_id()].hw_id = arch_cpu_hw_id();

	found = arch_smp_discover(cpu_ids, MAX_CPUS);

	if (found > MAX_CPUS) {
		/* Reported, never silently truncated. The memory map's region
		 * cap taught this: a limit that quietly drops what it cannot
		 * hold produces a wrong number rather than an error. */
		dropped = found - MAX_CPUS;
		found = MAX_CPUS;
	}

	cpu_count = found ? found : 1;

	/* An idle thread for the boot processor too, and this is new.
	  *
	  * The loop below starts at 1 because 1 upward are the processors that
	  * have to be *started*, and processor 0 quietly inherited a different
	  * arrangement: the boot thread itself was left marked as processor 0
	  * idle thread, so there was always something for it to fall back to.
	  *
	  * That worked and cost something invisible. An idle thread is not
	  * allowed to block -- a blocked idle thread is a processor that has
	  * stopped -- so the boot thread could not wait for anything either,
	  * and every piece of kernel code that runs on it inherited that. It
	  * went unnoticed for as long as nothing on the boot path waited.
	  *
	  * Now processor 0 has an idle thread of its own like every other
	  * processor, and the boot thread is an ordinary thread that can sleep.
	  */
	{
		struct thread *idle = thread_create("idle-000", idle_loop, 0);

		if (idle) {
			idle->idle_for = 0;
			cpus[0].idle = idle;
		} else {
			kputs("  smp: no memory for the boot processor's idle "
				"thread\n");
		}
	}

	for (unsigned i = 1; i < cpu_count; i++) {
	paddr_t stack = pmm_alloc_pages(4);
		char name[THREAD_NAME_MAX] = "idle-000";
		struct thread *idle;

		if (!stack) {
			kprintf("  smp: no memory for processor %u's stack\n", i);
			break;
		}

		/* An idle thread of its own, created here rather than there,
		 * because the scheduler's structures are guarded by a lock this
		 * processor already holds the right to take -- and the one being
		 * started does not yet exist as far as the scheduler knows. */
		/* Three digits, because there can now be 256 of these and
		' + i' past nine produces a colon, a semicolon and then
		 * letters. A name is read by a person. */
		name[5] = (char)('0' + (i / 100) % 10);
		name[6] = (char)('0' + (i / 10) % 10);
		name[7] = (char)('0' + i % 10);
		idle = thread_create(name, idle_loop, 0);
		if (!idle) {
			pmm_free_pages(stack, 4);
			kprintf("  smp: no memory for processor %u's idle thread\n", i);
			break;
		}

		/* Claimed for this processor before it can be scheduled
		 * anywhere. An idle thread in the ring with no owner is a
		 * thread another processor will happily take. */
		idle->idle_for = (int)i;
		cpus[i].idle = idle;

		if (!arch_smp_start(cpu_ids[i], i,
				    (u8 *)phys_to_virt(stack) + 4 * PAGE_SIZE)) {
			/* Not "the firmware refused": on x86_64 there is no firmware
			 * in this path at all, and a message that names the wrong
			 * culprit sends whoever reads it to the wrong place. */
			kprintf("  smp: could not start processor %u\n", i);
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

	/* Bring-up is over, so whatever it needed in low memory can go.
	 *
	 * This is called after the wait and not inside the loop: the scaffolding
	 * is shared by every processor being started, and taking it away while
	 * one of them is still walking through it is a processor that vanishes
	 * with nothing to read afterwards. A processor that never reported still
	 * gets the full deadline first -- if it arrives late it finds its
	 * trampoline gone, which is the same outcome as never arriving and is
	 * the one we already print. */
	arch_smp_bringup_done();
}

void smp_secondary_main(unsigned cpu)
{
	/* Page tables, interrupt controller, timer -- everything this processor
	 * needs that the boot processor could not do on its behalf. */
	arch_smp_cpu_init();

	/* Who this processor believes it is, asked of it rather than assumed.
	 * `cpu` is the slot the boot processor started it into; arch_cpu_id()
	 * is what every per-processor lookup in the kernel will use from here
	 * on. They have to be the same number. */
	if (cpu < MAX_CPUS) {
		claimed_id[cpu] = arch_cpu_id();
		cpus[cpu].hw_id = arch_cpu_hw_id();
		claimed[cpu] = true;
	}

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
		kprintf("  WARNING      : at least %u more than this kernel "
			"can hold\n", dropped);

	/* The machine identifier is printed beside the kernel index because the
	 * two being different is normal and their being *equal* is a coincidence
	 * of small machines. A reader who sees 0,1,2,3 against 0x0,0x1,0x100,
	 * 0x101 can see the topology; one who sees only the left-hand column
	 * cannot tell an aliased identity from a healthy one. */
	for (unsigned i = 0; i < cpu_count && i < MAX_CPUS; i++)
		kprintf("  cpu %u        : %s, hw 0x%lx, %lu ticks, "
			"%lu switches\n",
			i, cpus[i].online ? "online" : "did not start",
			cpus[i].hw_id, cpus[i].ticks, cpus[i].switches);
}

bool smp_self_test(void)
{
	bool ok = true;
	unsigned i, j;

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

	/* --- and that they are all different processors --------------------
	 *
	 * Each secondary recorded arch_cpu_id() about itself. Two requirements,
	 * and the second is the one that catches an aliased identity:
	 *
	 *   - every processor agrees with the slot it was started into, so the
	 *     arrays it indexes are the arrays the boot processor set up for it;
	 *   - no two agree with each other, which on a machine whose identity
	 *     came from one affinity field is exactly what would fail.
	 *
	 * The boot processor is checked too, and separately: it never passes
	 * through smp_secondary_main. */
	claimed_id[0] = arch_cpu_id();
	claimed[0] = true;

	if (claimed_id[0] != 0) {
		kprintf("  smp: the boot processor calls itself %u rather than "
			"0\n", claimed_id[0]);
		ok = false;
	}

	for (i = 0; i < cpu_count && i < MAX_CPUS; i++) {
		if (!claimed[i])
			continue;

		if (claimed_id[i] != i) {
			kprintf("  smp: processor started as %u calls itself %u\n",
				i, claimed_id[i]);
			ok = false;
		}

		for (j = 0; j < i; j++)
			if (claimed[j] && claimed_id[j] == claimed_id[i]) {
				kprintf("  smp: processors %u and %u both call "
					"themselves %u, so they share every "
					"per-processor structure there is\n",
					j, i, claimed_id[i]);
				ok = false;
			}
	}

	return ok;
}
