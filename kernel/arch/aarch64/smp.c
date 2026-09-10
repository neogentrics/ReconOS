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
#define PSCI_SYSTEM_OFF 0x84000008u

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

/* Turning the machine off, by the same interface that turns processors on.
 *
 * There is no ACPI on a machine booted from a device tree, so the portable
 * power_off() -- which reads a register out of the FADT and a value out of the
 * vendor's bytecode -- has nothing to work with and correctly says so. PSCI is
 * this architecture's answer, and it was already here: the call that wakes a
 * secondary processor is one function number away from the call that stops the
 * machine.
 *
 * The probe is the SMP one and it has to have run. A machine started with one
 * processor still probes, because smp_init runs on every boot -- but if that
 * ever stops being true, this returns false and the caller says the machine
 * could not be turned off, which is the right failure rather than a hang in an
 * unprobed call.
 *
 * SYSTEM_OFF does not return. Reaching the line after it means firmware
 * declined, and that is worth reporting rather than spinning on.
 */
bool arch_power_off(void)
{
	if (!psci_available)
		return false;

	psci_call(PSCI_SYSTEM_OFF, 0, 0, 0, psci_use_hvc);

	return false;
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

	/* Probes past `max`, so a machine with more processors than this kernel
	 * can hold is *reported* rather than quietly halved. Twice the array is
	 * enough to notice; probing without a bound would ask forever. */
	for (u64 candidate = 0; candidate < (u64)max * 2; candidate++) {
		if (candidate == ids[0])
			continue;

		/* Ask whether it is affine to us -- CPU_ON with a null entry
		 * point would start it somewhere useless, so the probe is
		 * AFFINITY_INFO (0xC4000004), which only reports. */
		if (psci_call(0xC4000004u, candidate, 0, 0, psci_use_hvc) >= 0) {
			if (count < max)
				ids[count] = candidate;
			count++;
		}
	}

	return count;
}

/* Nothing to release. Starting a processor here is one call into firmware,
 * which needs no code of ours in low memory to do it -- and since the stacks
 * stopped being identity mapped there is nothing of this kernel's below the
 * kernel at all. Present so that the portable caller does not have to know
 * which architecture it is on. */
void arch_smp_bringup_done(void)
{
}

bool arch_smp_start(u64 id, unsigned cpu, void *stack_top)
{
	i64 r;

	if (!psci_available)
		return false;

	if (cpu >= MAX_CPUS)
		return false;

	/* THE STACK IS A DIRECT-MAP ADDRESS, AND THIS USED TO BE AN IDENTITY
	 * MAPPING INSTEAD.
	 *
	 * The comment that stood here argued that a processor started by PSCI
	 * begins with its MMU off, so the address must be physical and must stay
	 * valid across the switch -- and it identity-mapped four pages to make
	 * one pointer work on both sides. It cost one entry, and one entry was
	 * not worth an assembly stack switch to avoid.
	 *
	 * The argument was wrong about *when*. `secondary_entry` calls
	 * `mmu_install` before it sets a stack pointer at all, and mmu_install
	 * keeps its return address in a register and touches no memory -- so the
	 * processor never uses this stack while its MMU is off. And the boot
	 * tables already carry the direct map at entry 256, put there so a
	 * secondary could reach a device before the real tables exist, which
	 * means a direct-map stack is addressable from the first instruction
	 * after the switch.
	 *
	 * What made the entry worth removing is not that it was unnecessary. It
	 * is that it was in the half a *process* is meant to own, and every
	 * mapping down there is one a per-process address space would have to
	 * carry a copy of. See vm_user_half_report. */
	secondary_stacks[cpu] = (u64)(uintptr_t)stack_top;

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

	/* Its own vector unit: CPACR_EL1 is per-processor, and firmware's
	 * setting of it is not something to rely on. */
	arch_vector_enable();
}
