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
#include <recon/kernel/console.h>

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

struct madt {
	struct acpi_sdt_header header;
	u32 local_apic_address;
	u32 flags;
	/* entries follow */
} RK_PACKED;

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

	return found;
}

bool arch_smp_start(u64 id, unsigned cpu, void *stack_top)
{
	/* Discovery works; waking them does not yet. Reported as a failure
	 * rather than as a processor that never checks in, so the count of
	 * processors *online* stays honest. */
	(void)id;
	(void)cpu;
	(void)stack_top;
	return false;
}

void arch_smp_cpu_init(void)
{
	/* Nothing calls this: no secondary processor is started yet. */
}
