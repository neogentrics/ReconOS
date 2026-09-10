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

/* --- the identity arithmetic, on machines this rig does not have ----------
 *
 * WHAT THIS PROVES AND WHAT IT DOES NOT.
 *
 * BG-153 was that arch_cpu_id() returned MPIDR affinity level 0, which is
 * unique on a one-cluster machine and not on any other. The fix is that the
 * identity no longer comes from MPIDR at all -- it is a dense index handed to
 * each processor in TPIDR_EL1 -- so on this machine there is nothing left to
 * catch.
 *
 * And this machine cannot be made into the other kind. QEMU's virt board
 * numbers its processors 0,1,2,3 whatever topology it is asked for: -smp
 * 8,sockets=2,cores=4 and -smp 8,clusters=2,cores=4 both produce flat
 * affinities, which was measured rather than assumed. So there is no way here
 * to boot the machine the bug was about.
 *
 * What is testable is the arithmetic, against MPIDR values written down from
 * the specification. The table below is a two-socket, four-core-per-socket
 * machine and a big.LITTLE one. The old expression is run beside the new one
 * and the test requires that the old produces collisions and the new does
 * not -- so the test would fail if somebody quietly restored the old
 * behaviour, and it fails loudly today if the packing is wrong.
 *
 * It is a test of a calculation, not of a machine, and it is labelled that
 * way so nobody reads a pass here as this kernel having run on two sockets.
 */
static u64 pack_affinity(u64 mpidr)
{
	return (mpidr & 0x00FFFFFFull) | (((mpidr >> 32) & 0xFFull) << 24);
}

bool arch_identity_self_test(void)
{
	/* Bit 31 is RES1 in MPIDR_EL1 and is set on every real value, which is
	  * why it is here: an implementation that masked the whole register
	  * rather than the affinity fields would produce identical rubbish for
	  * every processor and pass a test built from bare numbers. */
	static const u64 machines[] = {
		/* two sockets, four cores each: Aff1 is the cluster */
		0x80000000ull, 0x80000001ull, 0x80000002ull, 0x80000003ull,
		0x80000100ull, 0x80000101ull, 0x80000102ull, 0x80000103ull,
		/* big.LITTLE: two clusters again, and a third at Aff2 */
		0x80010000ull, 0x80010001ull,
		/* and one with Aff3 set, which lives at bits 39:32 and is the
		  * field an implementation is most likely to drop */
		0x8000000000ull | 0x80000000ull,
	};
	const unsigned n = (unsigned)(sizeof(machines) / sizeof(machines[0]));
	unsigned i, j, old_collisions = 0, new_collisions = 0;
	bool ok = true;

	for (i = 0; i < n; i++)
		for (j = 0; j < i; j++) {
			if ((machines[i] & 0xFFull) == (machines[j] & 0xFFull))
				old_collisions++;

			if (pack_affinity(machines[i]) ==
			    pack_affinity(machines[j]))
				new_collisions++;
		}

	/* The control. If the old expression does *not* alias on this table
	  * then the table is not a multi-cluster machine and the rest of this
	  * test proves nothing -- which is the failure mode that let BG-153
	  * exist, arriving here as a failure rather than a silent pass. */
	if (old_collisions == 0) {
		kputs("  smp: the identity table has no aliases under the old "
			"rule, so it is not testing anything\n");
		ok = false;
	}

	if (new_collisions != 0) {
		kprintf("  smp: %u pairs of processors would share an identity\n",
			new_collisions);
		ok = false;
	}

	/* Aff3 specifically, because dropping it is silent: it only matters on
	  * machines with more than 65536 processors per Aff2 group, and the two
	  * values below differ in nothing else. */
	if (pack_affinity(0x80000000ull) ==
	    pack_affinity(0x8000000000ull | 0x80000000ull)) {
		kputs("  smp: affinity level 3 is being dropped\n");
		ok = false;
	}

	/* And that a packed value is what the hardware would give: Aff0 in the
	  * low byte and Aff3 in the top one, not merely something unique. */
	if (pack_affinity(0x80000103ull) != 0x103ull ||
	    pack_affinity(0x8200000000ull) != 0x82000000ull) {
		kputs("  smp: affinity fields are not packed where they were "
			"promised to be\n");
		ok = false;
	}

	return ok;
}

u64 arch_cpu_hw_id(void)
{
	return arch_cpu_affinity();
}

u64 arch_cpu_affinity(void)
{
	u64 mpidr;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));

	/* All four affinity levels packed into one 32-bit value: Aff0 in the low
	 * byte, then Aff1 and Aff2, and Aff3 -- which lives up at bits 39:32 of
	 * MPIDR -- brought down to the top byte.
	 *
	 * This is the processor's *position in the machine*, and it is what PSCI
	 * is given to start one. It is not an array index and must never be used
	 * as one: the values are sparse, and a second socket may begin at a large
	 * number. arch_cpu_id() is the index. */
	return (mpidr & 0x00FFFFFFull) | (((mpidr >> 32) & 0xFFull) << 24);
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
	ids[0] = arch_cpu_affinity();

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
