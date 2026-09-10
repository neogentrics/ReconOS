/* Waking the other processors on x86_64.
 *
 * ARM needed none of this: PSCI is one call into firmware, and the firmware
 * does the rest. On x86 the kernel does all of it, in three separate pieces,
 * and each one fails in its own way:
 *
 *   FINDING THEM. The processors are listed in the MADT, one of the tables the
 *   ACPI root pointer leads to. This part is here.
 *
 *   A REAL-MODE TRAMPOLINE. A processor started by the local APIC begins in
 *   16-bit real mode at a page below one megabyte -- not because anything needs
 *   it to, but because the startup message carries a page number in one byte.
 *   So the kernel has to place code in low memory that walks the same road the
 *   boot trampoline walks: protected mode, page tables, long mode.
 *
 *   THE STARTUP SEQUENCE. INIT, a delay, then twice a start-up message. It is
 *   hand-timed, the timings come from a manual, and a processor that misses one
 *   simply does not appear.
 *
 * --- Why the count is reported even before they can be started ---
 *
 * Discovery is useful on its own: it is the difference between "this machine
 * has one processor" and "this machine has eight and this kernel uses one",
 * and only the second of those tells anybody what is missing.
 *
 * It is also the piece that can be got wrong quietly. A MADT walk that stops
 * early, or misreads an entry length, reports a plausible number -- and a
 * plausible number is indistinguishable from a correct one without a machine
 * whose processor count is already known. Which is why the verification rig
 * boots at 1, 2, 4 and 8 processors and compares.
 */
#include "x86_64.h"

#include <recon/kernel/smp.h>
#include <recon/kernel/acpi.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/time.h>
#include <recon/kernel/user.h>
#include <recon/kernel/vm.h>

/* --- The MADT ------------------------------------------------------------
 *
 * "Multiple APIC Description Table". After the common header it carries the
 * local APIC's physical address, a flags word, and then a list of variable
 * length entries, each one a type byte and a length byte followed by whatever
 * that type means.
 *
 * The length byte is the only thing that says where the next entry begins, so a
 * zero length is not a malformed entry -- it is an endless loop. Checked.
 */
#define MADT_LOCAL_APIC		0	/* a processor, 8 bytes */
#define MADT_LOCAL_X2APIC	9	/* a processor with an id above 255 */

/* Entry type 0: ACPI processor id, APIC id, flags. */
#define MADT_APIC_ENABLED	(1u << 0)

/* Set even for a processor that is not enabled *now* but could be brought
 * online later. Treated as present, because it is: a kernel that ignores it is
 * one that silently uses half a machine. */
#define MADT_APIC_ONLINE_CAPABLE (1u << 1)

/* struct madt now lives in x86_64.h: ioapic.c walks the same table. */

/* Which logical processor each APIC identifier belongs to.
 *
 * The two are not the same number and must not be assumed to be. An APIC
 * identifier is assigned by the machine and is allowed to be sparse -- 0, 2, 4,
 * 6 on a machine with hyper-threading disabled is ordinary. The kernel's
 * processor number is a dense index into its own arrays. Treating one as the
 * other indexes past the end of those arrays on exactly the machines that do
 * this, which are real machines and not the rig's. */
static u32 apic_id_for_cpu[MAX_CPUS];
static bool cpu_map_ready;

/* And the same map read the other way round, because the forward one is read
 * far more often than it is written.
 *
 * x86_cpu_index() is on the path of every this_cpu(), which means every lock
 * acquisition and every scheduler decision. It used to search apic_id_for_cpu
 * from the front, which was free when that array had eight entries and is a
 * 256-iteration scan per lock now that it has 256. Raising the ceiling without
 * this would have made a bigger machine slower at the thing it does most.
 *
 * Entries are the kernel index plus one, so zero means "not a processor we
 * know about" and the array needs no separate initialisation pass. The size is
 * a compromise: identifiers below it are answered by one load, and anything
 * above falls back to the scan -- correct either way, and the scan is then
 * over a machine large enough that it has earned it. */
#define APIC_REVERSE_MAP 4096
static u16 cpu_for_apic_id[APIC_REVERSE_MAP];

static void remember_apic_id(unsigned cpu, u32 id)
{
	if (cpu >= MAX_CPUS)
		return;

	apic_id_for_cpu[cpu] = id;

	if (id < APIC_REVERSE_MAP)
		cpu_for_apic_id[id] = (u16)(cpu + 1);
}

/* This processor's kernel index, found by asking its APIC who it is.
 *
 * Called before the APIC exists, by code that runs during early boot, and the
 * answer then is zero -- which is correct, because until the APIC is up there
 * is only one processor running. */
/* x86_64 has kept APIC identifiers and kernel indices apart since the map was
 * written -- see apic_id_for_cpu -- so the aliasing BG-153 describes cannot
 * happen here. What can, and what this checks, is the reverse map going out
 * of step with the forward one: they are two arrays holding one fact, and the
 * fast path reads only one of them. */
bool arch_identity_self_test(void)
{
	unsigned i;
	bool ok = true;

	for (i = 0; i < MAX_CPUS; i++) {
		u32 id = apic_id_for_cpu[i];

		/* Slot 0 is the only one legitimately holding APIC 0. */
		if (i && !id)
			continue;

		if (id < APIC_REVERSE_MAP && cpu_for_apic_id[id] != (u16)(i + 1)) {
			kprintf("  smp: APIC 0x%x belongs to processor %u but the "
				"fast map says %u\n", id, i,
				(unsigned)cpu_for_apic_id[id]);
			ok = false;
		}
	}

	/* --- and the x2APIC arithmetic, which no machine here can run -------
	 *
	 * x2APIC is what lifts the ceiling from 255 processors to 32 bits of
	 * them, and it has never executed: QEMU 8.2 does not implement it under
	 * TCG, and KVM is not reachable from this machine. See the note on the
	 * `x2apic` flag in apic.c.
	 *
	 * Two things about it are arithmetic rather than hardware, and getting
	 * either wrong is silent: which MSR each register becomes, and where
	 * the destination sits in the 64-bit command. Both are checked here, so
	 * that when the mode is finally entered on a real machine the failure
	 * -- if there is one -- is in the part that needed the hardware.
	 *
	 * This does not make x2APIC tested. It makes the untested part
	 * smaller. */
	if (x86_apic_msr_for(0x020u) != 0x802u ||		/* identifier */
	    x86_apic_msr_for(0x0B0u) != 0x80Bu ||		/* end of interrupt */
	    x86_apic_msr_for(0x300u) != 0x830u ||		/* command */
	    x86_apic_msr_for(0x3E0u) != 0x83Eu) {		/* timer divide */
		kputs("  smp: the x2APIC register numbering is wrong, so every "
			"access would go to the wrong register\n");
		ok = false;
	}

	/* The destination is the *high* half of the command. In the low half it
	 * would be read as delivery mode and vector: an INIT to processor 5
	 * would become some other message to everybody. */
	if (x86_apic_command_word(0x1234u, 0x4500u) !=
	    ((u64)0x1234u << 32 | 0x4500u)) {
		kputs("  smp: the x2APIC command word puts the destination in the "
			"wrong half\n");
		ok = false;
	}

	/* And that an identifier a byte cannot hold survives it, which is the
	 * entire purpose: 300 is an ordinary processor number on a machine with
	 * two server-size sockets. */
	if ((u32)(x86_apic_command_word(300u, 0) >> 32) != 300u) {
		kputs("  smp: a processor above 255 loses its identity in the "
			"command word\n");
		ok = false;
	}

	return ok;
}

u64 arch_cpu_hw_id(void)
{
	if (!x86_apic_present())
		return 0;

	return x86_apic_id();
}

unsigned x86_cpu_index(void)
{
	u32 id;
	unsigned i;

	if (!cpu_map_ready || !x86_apic_present())
		return 0;

	id = x86_apic_id();

	if (id < APIC_REVERSE_MAP) {
		u16 slot = cpu_for_apic_id[id];

		if (slot)
			return slot - 1u;
	} else {
		for (i = 0; i < MAX_CPUS; i++)
			if (apic_id_for_cpu[i] == id)
				return i;
	}

	/* An APIC that is not in the table. Zero is wrong, and so is anything
	 * else; what matters is that it is in range, because the caller is
	 * about to index an array with it. */
	return 0;
}

unsigned arch_smp_discover(u64 *ids, unsigned max)
{
	const struct madt *madt;
	const u8 *p, *end;
	unsigned found = 0, stored = 0;

	if (!max)
		return 0;

	/* The boot processor is always first, and is always present whatever
	 * the tables say. A machine with no ACPI at all still has one.
	 *
	 * `found` counts every processor the table describes; `stored` counts
	 * the ones that fit in the caller's array. Returning `found` is what
	 * lets the caller report a machine bigger than this kernel can use,
	 * rather than quietly using part of it. */
	ids[0] = 0;
	found = 1;
	stored = 1;

	madt = (const struct madt *)acpi_find("APIC");
	if (!madt)
		return found;

	if (madt->header.length < sizeof(*madt))
		return found;

	p   = (const u8 *)madt + sizeof(*madt);
	end = (const u8 *)madt + madt->header.length;

	while (p + 2 <= end) {
		u8 type = p[0];
		u8 len  = p[1];
		u64 id;
		u32 flags;
		unsigned i;

		/* A length of zero would make this loop forever, and a length
		 * that runs past the table would read somebody else's memory.
		 * A table that says either is not one to keep walking. */
		if (len < 2 || p + len > end) {
			kputs("smp: the processor table is malformed; "
			      "stopping where it stopped making sense\n");
			break;
		}

		if (type == MADT_LOCAL_APIC && len >= 8) {
			id    = p[3];			/* APIC id */
			flags = (u32)p[4] | ((u32)p[5] << 8) |
				((u32)p[6] << 16) | ((u32)p[7] << 24);
		} else if (type == MADT_LOCAL_X2APIC && len >= 16) {
			id    = (u32)p[4] | ((u32)p[5] << 8) |
				((u32)p[6] << 16) | ((u32)p[7] << 24);
			flags = (u32)p[8] | ((u32)p[9] << 8) |
				((u32)p[10] << 16) | ((u32)p[11] << 24);
		} else {
			p += len;
			continue;
		}

		if (!(flags & (MADT_APIC_ENABLED | MADT_APIC_ONLINE_CAPABLE))) {
			p += len;
			continue;
		}

		/* The boot processor appears in the table too, and this kernel
		 * has already counted it. Skipped by identity rather than by
		 * position: nothing says it is listed first. */
		{
			bool seen = false;

			for (i = 0; i < stored; i++)
				if (ids[i] == id)
					seen = true;

			/* Only the stored ones can be compared, so a duplicate
			 * beyond `max` is not detected. It is counted once,
			 * which is the honest answer: the table said it exists.
			 */
			if (!seen) {
				if (stored < max)
					ids[stored++] = id;
				found++;
			}
		}

		p += len;
	}

	/* The boot processor's own APIC, brought up here because this is the
	 * one place on x86_64 that runs on the boot processor during SMP start,
	 * and because it must happen after time_init: the APIC timer's
	 * frequency is not reported anywhere and has to be measured against a
	 * clock that is already trusted.
	 *
	 * The boot processor does *not* then start its APIC timer. It already
	 * has a tick, from the 8254, and two timers on one processor is two
	 * ticks -- a scheduler running at twice the rate it believes, which
	 * looks like it works. */
	if (x86_apic_init()) {
		remember_apic_id(0, x86_apic_id());
		cpu_map_ready = true;
		x86_apic_calibrate_timer();
	}

	return found;
}

/* --- starting them -------------------------------------------------------
 *
 * The sequence is INIT, a wait, a startup message, a wait, and a second startup
 * message. It comes from a manual, the delays are in it, and a processor that
 * misses one simply does not appear -- there is no error and nothing to read.
 *
 * The second startup message is not a retry for a failure anybody has seen. It
 * is in the specification because some processors ignore the first, and a
 * processor that has already started treats the second as noise. So it is sent
 * unconditionally rather than only when the first appears not to have worked,
 * which would be a condition nothing can evaluate.
 */

/* Where the trampoline is copied to: a page below one megabyte, because the
 * startup message says where to begin as a page number in a single byte. */
static paddr_t trampoline_page;

struct trampoline_params {
	u64 cr3;
	u64 stack;
	u64 entry;
	u32 cpu;
	u32 pad;
};

extern u8 x86_trampoline_start[];
extern u8 x86_trampoline_end[];
extern u8 x86_trampoline_params[];

static void spin_ns(u64 ns)
{
	u64 until = time_monotonic_ns() + ns;

	while (time_monotonic_ns() < until)
		arch_cpu_relax();
}

/* Copies the trampoline into low memory and makes it reachable from the address
 * it will run at.
 *
 * The identity mapping is the part that is easy to leave out and impossible to
 * debug afterwards. Paging comes on inside the trampoline, and the *next
 * instruction* is fetched from the page it is running in -- at its physical
 * address, because that is what the processor has been executing all along. If
 * the kernel's tables do not map that address to itself, the fetch faults, and
 * it faults on a processor with no interrupt table, which triple-faults it into
 * a reset with nothing printed anywhere. */
static bool prepare_trampoline(void)
{
	u64 size = (u64)(x86_trampoline_end - x86_trampoline_start);

	if (trampoline_page)
		return true;

	if (size > PAGE_SIZE) {
		kputs("  smp: the trampoline outgrew the page it must fit in\n");
		return false;
	}

	trampoline_page = pmm_alloc_page_below(0x100000);
	if (!trampoline_page) {
		kputs("  smp: no free page below one megabyte for the "
		      "trampoline\n");
		return false;
	}

	kmemcpy(phys_to_virt(trampoline_page), x86_trampoline_start,
		(size_t)size);

	if (!vm_map((vaddr_t)trampoline_page, trampoline_page, PAGE_SIZE,
		    VM_READ | VM_WRITE | VM_EXEC | VM_GLOBAL)) {
		kputs("  smp: could not map the trampoline where it will "
		      "run\n");
		pmm_free_page(trampoline_page);
		trampoline_page = 0;
		return false;
	}

	return true;
}

/* The trampoline has done its work, so it stops being mapped and the page goes
 * back to the allocator.
 *
 * Two reasons, and the second is the one that made it worth doing now. It is a
 * page of memory that is readable, writable AND executable at a fixed low
 * address for the life of the machine -- the shape of thing an exploit looks
 * for. And it is the only mapping this kernel keeps in the half a process is
 * meant to own, which is what stands between here and giving each process an
 * address space of its own. `vm_user_half_report` counts it, and the number it
 * prints is zero once this has run.
 *
 * Unmapping before freeing, in that order: a freed page can be handed to
 * somebody else immediately, and a page that is somebody else's while still
 * mapped executable at a low address is worse than either problem alone.
 *
 * A processor started after this would find nothing to start from. Nothing
 * does -- processors are started once, here, at boot -- and if hot-plug ever
 * arrives it re-prepares rather than assuming the page survived.
 */
void arch_smp_bringup_done(void)
{
	if (!trampoline_page)
		return;

	vm_unmap((vaddr_t)trampoline_page, PAGE_SIZE);
	pmm_free_page(trampoline_page);
	trampoline_page = 0;
}

bool arch_smp_start(u64 id, unsigned cpu, void *stack_top)
{
	struct trampoline_params *p;
	unsigned before;
	u64 deadline;

	if (!x86_apic_present())
		return false;

	if (!prepare_trampoline())
		return false;

	/* The parameter block lives inside the copied page, at the offset the
	 * assembler put it at. Derived rather than written down twice: an
	 * offset agreed by hand between an assembler and a C file is an offset
	 * that drifts the first time either grows. */
	p = (struct trampoline_params *)
		((u8 *)phys_to_virt(trampoline_page) +
		 (x86_trampoline_params - x86_trampoline_start));

	remember_apic_id(cpu, (u32)id);

	p->cr3   = x86_read_cr3();
	p->stack = (u64)(uintptr_t)stack_top;
	p->entry = (u64)(uintptr_t)smp_secondary_main;
	p->cpu   = cpu;

	/* Written before the processor is told to look at them. */
	__atomic_thread_fence(__ATOMIC_RELEASE);

	before = smp_cpus_online();

	x86_apic_send_init((u32)id);
	spin_ns(10000000ull);				/* 10 ms */

	x86_apic_send_startup((u32)id, (u8)(trampoline_page >> 12));
	spin_ns(200000ull);				/* 200 us */

	x86_apic_send_startup((u32)id, (u8)(trampoline_page >> 12));
	spin_ns(200000ull);

	/* Waited for here rather than in the portable caller, and this is not
	 * politeness: there is one trampoline page and one parameter block, and
	 * the next processor's parameters overwrite this one's. Starting them
	 * all and then waiting would hand several processors the same stack.
	 *
	 * The timeout is what keeps a processor that never arrives from
	 * stopping the boot. It is reported as a failure to start, which is
	 * what it is -- not as a processor that is running and silent. */
	deadline = time_monotonic_ns() + 500000000ull;	/* 500 ms */
	while (smp_cpus_online() == before && time_monotonic_ns() < deadline)
		arch_cpu_relax();

	return smp_cpus_online() > before;
}

/* Everything a processor needs that the boot processor could not do for it.
 *
 * The order matters and each line has a reason to be where it is. Tables first,
 * because until they are loaded a fault is a reset. Then the address space,
 * then this processor's own APIC, then the things that can raise an interrupt
 * -- because an interrupt arriving before the table that describes it is the
 * one failure that leaves nothing to read. */
void arch_smp_cpu_init(void)
{
	x86_load_tables_this_cpu();

	vm_activate_this_cpu();

	/* Its own APIC. The mapping is shared and already made; what is
	 * per-processor is the enabling, because each of these is a different
	 * piece of hardware behind one address. */
	x86_apic_init();

	/* Its own task-state segment and its own SYSCALL registers. Both are
	 * per-processor state that looks global: the model-specific registers
	 * are written per processor by the architecture, and the TSS holds the
	 * stack a system call lands on, which two processors cannot share. */
	arch_user_init();

	/* Last, and only now: this is what makes the processor preemptible, and
	 * everything above had to be in place before the first tick. */
	/* Its own vector unit. The control registers that enable it are
	 * per-processor, so a secondary that skipped this would take an
	 * undefined-instruction fault on the first thread that used one. */
	arch_vector_enable();

	x86_apic_start_timer();

	/* And interrupts on, which is the line whose absence is hardest to see.
	 *
	 * The trampoline began with `cli`, because a processor part-way through
	 * changing mode cannot take an interrupt, and nothing since has undone
	 * it. Without this the processor comes online, reports healthy, adopts
	 * its idle thread, arms its timer -- and halts forever, because
	 * arch_wait_for_interrupt is a halt and no interrupt can arrive to end
	 * it. Every count reads zero and nothing anywhere says why.
	 *
	 * Which is the same failure aarch64 had at 9b for a different reason:
	 * there the vectors were the firmware's, here the flag is clear. Both
	 * present as processors that are online and take not one tick. */
	__asm__ volatile("sti");
}
