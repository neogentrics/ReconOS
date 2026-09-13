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

/* How many processors this kernel can hold.
 *
 * It was 8, which was the size of the test rig and not the size of anything
 * else. A current server part is two orders of magnitude past that: EPYC
 * reaches 128 cores a socket and 192 on Turin, Xeon 128 P-cores or 288
 * E-cores, and a two-socket board is routinely 256 to 576 *logical*
 * processors. Eight of those would have been used and the rest reported as
 * dropped -- correct behaviour, and not a useful machine.
 *
 * 256 is not the next round number up. **255 is the largest processor an
 * 8-bit APIC identifier can name**, so 256 is exactly where the xAPIC
 * interrupt controller stops being able to address what it can see. Going
 * past it is not a bigger array; it is x2APIC, which is a different set of
 * registers -- see arch/x86_64/apic.c, which now implements it, and
 * MAX_CPUS_X2APIC below.
 *
 * The cost is static memory and it is worth writing down, because it is the
 * reason not to simply pick a huge number. struct thread is the expensive one
 * at roughly a kilobyte, so boot_threads alone is about a quarter of a
 * megabyte of BSS; the per-processor blocks, task-state segments and
 * identifier arrays add tens of kilobytes more. All of it is zero-filled and
 * none of it is in the image on disk.
 *
 * **Untested above 32.** No machine with more than that has run this kernel.
 * The count is checked and a machine with more processors than this reports
 * the ones it dropped rather than silently using a subset -- the same rule the
 * memory map's region cap learned the hard way. */
#define MAX_CPUS 256

/* The ceiling with x2APIC in use, stated separately because the two limits
 * have different causes: MAX_CPUS is how many this kernel has room for, and
 * this is how many the addressing can name. x2APIC identifiers are 32 bits, so
 * the second number is no longer the binding one on x86_64 -- which is the
 * whole point of implementing it. */
#define MAX_CPUS_XAPIC 256

struct thread;

/* Everything a processor knows about itself. Indexed by arch_cpu_id(). */
struct cpu_local {
	unsigned id;
	bool online;

	/* What the *machine* calls this processor, as opposed to what the
	 * kernel calls it. An APIC identifier on x86_64, packed MPIDR affinity
	 * on aarch64.
	 *
	 * Recorded and printed because the difference between the two numbers
	 * is where KF-152 and KF-153 both live, and because it was invisible:
	 * a summary that shows only the dense index looks identical on a
	 * machine whose identities alias and one whose do not. */
	u64 hw_id;

	struct thread *current;		/* the thread running *here* */
	struct thread *idle;		/* what runs here when nothing else will */

	/* The thread that gave up this processor at the last switch, still
	 * to be released. Read and cleared by whoever runs here next, which
	 * is the first moment it is true that the outgoing thread has
	 * actually stopped using its stack. */
	struct thread *leaving;

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

/* Discovers the processors, filling `ids` with whatever identifier the
 * architecture needs to start each one -- an MPIDR value on aarch64, a local
 * APIC identifier on x86_64. Index 0 is this processor.
 *
 * **Returns how many exist, which may be more than `max`.** Only `max` of them
 * are written to `ids`; the return value is the true count, so the caller can
 * say how many it had to drop.
 *
 * That distinction is the whole point of the signature. It used to return the
 * number it had *stored*, which made `found > MAX_CPUS` unreachable in
 * smp_init -- a machine with sixteen processors reported eight and said
 * nothing, underneath a comment promising it would never silently truncate. */
unsigned arch_smp_discover(u64 *ids, unsigned max);

/* Starts one processor, which begins at the architecture's secondary entry
 * point with the given stack. Returns false if it could not be started -- which
 * on aarch64 means firmware refused, and on x86_64 means this kernel has not
 * written the startup sequence yet. */
bool arch_smp_start(u64 id, unsigned cpu, void *stack_top);

/* Called once, after every processor that is going to arrive has arrived.
 * Releases whatever bring-up needed and the running system does not -- which
 * on x86_64 is a page of real-mode code below one megabyte, and on aarch64 is
 * nothing, because PSCI does the equivalent inside firmware.
 *
 * It exists for a reason beyond tidiness: that trampoline page is mapped in the
 * half of the address space a *process* is meant to own, and every mapping left
 * down there is one that every per-process address space would have to carry a
 * copy of. */
/* What the machine calls the processor this is called on -- an APIC
 * identifier on x86_64, packed MPIDR affinity on aarch64. Sparse, possibly
 * large, and never an array index: arch_cpu_id() is the index. */
u64 arch_cpu_hw_id(void);

/* That processors can be told apart. Architecture-specific because what
 * threatens it is: on aarch64 an identity taken from one affinity field, on
 * x86_64 two maps of one fact drifting apart. */
bool arch_identity_self_test(void);

void arch_smp_bringup_done(void);

/* Brings this processor onto the kernel's page tables, its interrupt
 * controller interface, and its timer. Called by each secondary on itself. */
void arch_smp_cpu_init(void);

#endif /* RECON_KERNEL_SMP_H */
