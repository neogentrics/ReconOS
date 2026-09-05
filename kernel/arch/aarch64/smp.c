/* Waking the other processors on aarch64.
 *
 * ARM does not have an instruction that starts a processor. What it has is
 * PSCI -- the Power State Coordination Interface -- a call into whatever is
 * running at a higher exception level than the kernel: a hypervisor, secure
 * firmware, or in QEMU's case the machine model itself. The kernel asks, and
 * something more privileged does the actual starting.
 *
 * That is a better arrangement than x86's, where the kernel drives the
 * interrupt controller through a hand-timed startup sequence. It also means the
 * kernel cannot start a processor on a machine whose firmware will not, which
 * is a real limitation and an honest one.
 */
#include "aarch64.h"

#include <recon/kernel/smp.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/console.h>
#include <recon/kernel/trap.h>
#include <recon/kernel/pmm.h>

/* PSCI function identifiers. The 64-bit forms, because this kernel is 64-bit
 * and the 32-bit ones take different argument widths. */
#define PSCI_CPU_ON     0xC4000003u

#define PSCI_SUCCESS            0
#define PSCI_NOT_SUPPORTED      (-1)
#define PSCI_INVALID_PARAMETERS (-2)
#define PSCI_ALREADY_ON         (-4)

/* Where a secondary begins, in boot.S. A *physical* address: a processor that
 * has just been started has its MMU off, so nothing else would mean anything to
 * it. The kernel is identity mapped, so its link address is its physical
 * address -- and when the kernel moves to the higher half at checkpoint 10 this
 * becomes a translation and this comment becomes wrong, which is why it is
 * here. */
extern void secondary_entry(void);

/* One slot per processor, written by the boot processor and read by that
 * processor before its MMU is on.
 *
 * An array rather than a single word, and that is a fix rather than a
 * preference: one shared word is a race, because the boot processor writes the
 * next processor's stack pointer while the previous one may not have read its
 * own yet. Starting them one at a time and waiting would also work and would be
 * slower for no reason. */
u64 secondary_stacks[MAX_CPUS];

static i64 psci_call(u32 function, u64 a1, u64 a2, u64 a3, bool use_hvc)
{
	register u64 x0 __asm__("x0") = function;
	register u64 x1 __asm__("x1") = a1;
	register u64 x2 __asm__("x2") = a2;
	register u64 x3 __asm__("x3") = a3;

	if (use_hvc)
		__asm__ volatile("hvc #0"
				 : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
	else
		__asm__ volatile("smc #0"
				 : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");

	return (i64)x0;
}

/* Which instruction reaches the firmware is a property of the machine, not of
 * the architecture: it depends on whether there is a hypervisor above us. The
 * device tree says which in its /psci node, and reading that needs a parser
 * that can walk to a named property -- which ours cannot yet.
 *
 * So it is discovered by trying. A call that reaches nothing returns
 * NOT_SUPPORTED rather than faulting, which makes the trial safe, and the
 * answer is remembered. */
static bool psci_use_hvc = true;
static bool psci_available;

static void psci_probe(void)
{
	/* PSCI_VERSION is 0x84000000 and takes no arguments -- the cheapest
	 * call that proves something is listening. */
	if (psci_call(0x84000000u, 0, 0, 0, true) >= 0) {
		psci_use_hvc = true;
		psci_available = true;
		return;
	}

	if (psci_call(0x84000000u, 0, 0, 0, false) >= 0) {
		psci_use_hvc = false;
		psci_available = true;
		return;
	}

	psci_available = false;
}

unsigned arch_cpu_id_real(void)
{
	u64 mpidr;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

	/* Affinity level 0 is the processor within its cluster. Enough for the
	 * machines this kernel runs on; a many-cluster machine needs the higher
	 * affinity fields folded in, and that is a change to this one function. */
	return (unsigned)(mpidr & 0xFF);
}

unsigned arch_smp_discover(u64 *ids, unsigned max)
{
	unsigned count = 1;

	psci_probe();

	if (!psci_available) {
		kputs("  smp: no PSCI on this machine, so the other processors "
		      "cannot be started\n");
		return 1;
	}

	/* Which processors exist is in the device tree's /cpus node, and our
	 * parser cannot walk to it yet. So they are discovered the same way the
	 * PSCI method was: by asking. CPU_ON for an identifier that does not
	 * exist returns INVALID_PARAMETERS, which is a definite answer.
	 *
	 * This is a placeholder for reading the device tree properly, and it is
	 * a placeholder that cannot silently be wrong -- a processor it fails to
	 * find is a processor that does not answer. */
	ids[0] = arch_cpu_id_real();

	for (u64 candidate = 0; candidate < max && count < max; candidate++) {
		if (candidate == ids[0])
			continue;

		/* Ask whether it is affine to us -- CPU_ON with a null entry
		 * point would start it somewhere useless, so the probe is
		 * AFFINITY_INFO (0xC4000004), which only reports. */
		if (psci_call(0xC4000004u, candidate, 0, 0, psci_use_hvc) >= 0)
			ids[count++] = candidate;
	}

	return count;
}

bool arch_smp_start(u64 id, unsigned cpu, void *stack_top)
{
	i64 r;

	if (!psci_available)
		return false;

	if (cpu >= MAX_CPUS)
		return false;

	/* THE STACK MUST BE A PHYSICAL ADDRESS, AND MUST STAY VALID WHEN THE MMU
	 * COMES ON.
	 *
	 * A processor started by PSCI begins with its MMU off, so the direct-map
	 * address the allocator hands back means nothing to it -- setting the
	 * stack pointer to one and pushing faults immediately. So it gets the
	 * physical address.
	 *
	 * But a physical address stops meaning anything the instant that
	 * processor turns its MMU on, because the kernel identity-maps only its
	 * own image and this stack is not in it. So the stack is identity mapped
	 * too, and the same pointer is correct on both sides of the switch.
	 *
	 * The alternative -- switching stacks immediately after enabling the MMU
	 * -- means doing it in assembly between two instructions that must not be
	 * separated, and is not worth avoiding one page table entry for. */
	{
		paddr_t phys = virt_to_phys((u8 *)stack_top - 4 * PAGE_SIZE);

		if (!vm_map((vaddr_t)phys, phys, 4 * PAGE_SIZE,
			    VM_READ | VM_WRITE | VM_GLOBAL))
			return false;

		secondary_stacks[cpu] = (u64)phys + 4 * PAGE_SIZE;
	}

	/* The processor being started has its data cache off, so it reads memory
	 * directly rather than through this processor's cache -- where the write
	 * above may still be sitting. Cleaning to the point of coherency is what
	 * makes it visible.
	 *
	 * On QEMU this is unnecessary and harmless; on real hardware it is the
	 * difference between a processor that starts and one that jumps to
	 * whatever happened to be in that memory before. */
	__asm__ volatile(
		"dc civac, %0\n"
		"dsb sy\n"
		: : "r"(&secondary_stacks[cpu]) : "memory");

	/* The context identifier is passed to the entry point in x0, and this
	 * kernel uses it for the processor's own index -- so a secondary knows
	 * which it is without reading memory it cannot yet trust. */
	/* A *physical* entry point. The processor arrives with the MMU off, so
	 * the linked address of secondary_entry would mean nothing to it -- and
	 * PSCI does not fail visibly when handed one, it starts a processor that
	 * never appears. */
	r = psci_call(PSCI_CPU_ON, id,
		      (u64)(uintptr_t)secondary_entry - KERNEL_VMA, cpu,
		      psci_use_hvc);

	if (r == PSCI_SUCCESS || r == PSCI_ALREADY_ON)
		return true;

	kprintf("  smp: PSCI refused to start processor %u (%ld)\n", cpu, r);
	return false;
}

void arch_smp_cpu_init(void)
{
	/* The page tables, which the boot processor built and this one now
	 * adopts. Every processor needs its own MMU turned on; the tables
	 * themselves are shared. */
	vm_activate_this_cpu();

	/* ITS OWN EXCEPTION VECTORS. VBAR_EL1 is per-processor, and a secondary
	 * that does not set its own is still using whatever the firmware left
	 * there. It comes online, reports healthy, arms its timer -- and when the
	 * interrupt arrives it vanishes into firmware code that has no idea what
	 * this kernel is.
	 *
	 * That was the bug: three processors online with interrupts enabled and
	 * timers armed, taking not one tick between them. Nothing failed. They
	 * simply never came back.
	 *
	 * The general form is the one this project keeps meeting: a register that
	 * looks global because there was only ever one processor to set it. */
	trap_init();

	/* This processor's own interrupt controller interface and its own timer.
	 * Both are per-processor on this architecture -- the distributor is
	 * shared and was configured once, but the CPU interface and the timer's
	 * private interrupt are banked, so each processor must enable its own or
	 * it will sit in its idle loop forever, online and uninterruptible. */
	aarch64_gic_cpu_init();
	aarch64_timer_cpu_init();
}
