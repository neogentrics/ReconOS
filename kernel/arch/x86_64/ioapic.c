/* The I/O APIC: where a device interrupt goes.
 *
 * The 8259 pair this kernel has used since checkpoint 4 has one output, and it
 * goes to one processor. That is not a limitation of how it is programmed; it
 * is what the chip is. So on a machine with two processors, or with two hundred
 * and eighty-eight, every disk completion, every keypress and every network
 * packet lands on processor 0, and the others cannot be given any of it.
 *
 * The I/O APIC is the other half of the local APIC's story. Each local APIC is
 * per-processor and handles interrupts that *arrive*; the I/O APIC is one chip
 * per bus that decides *which* processor they arrive at. It holds a table --
 * one entry per input line -- and each entry says: what vector to raise, which
 * processor to raise it on, and how the line is wired.
 *
 * --- What makes this harder than it sounds ---
 *
 * The inputs are not the sixteen ISA interrupt requests. They are Global System
 * Interrupts, a flat numbering across every I/O APIC in the machine, and the
 * firmware is free to wire an ISA line to a different one. It usually does: on
 * almost every PC, ISA IRQ 0 -- the timer -- arrives on GSI 2. A kernel that
 * assumes IRQ n is GSI n programs the wrong entry, masks the timer by accident
 * and stops.
 *
 * That mapping is in the MADT, as "interrupt source override" entries, and each
 * one also carries the polarity and trigger mode of the line. Reading them is
 * not optional and there is no sensible default: the entries exist precisely
 * because the default is wrong on that machine.
 *
 * --- And why the 8259 does not simply go away ---
 *
 * It is masked rather than removed. A machine with no I/O APIC in its tables --
 * or with one this code fails to map -- keeps working exactly as it did, which
 * matters because that is every machine this kernel ran on before today. The
 * switch is made only when there is something to switch to, and the boot report
 * says which one is in use rather than leaving it to be inferred.
 */
#include "x86_64.h"

#include <recon/kernel/console.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/smp.h>
#include <recon/kernel/time.h>
#include <recon/kernel/arch.h>

/* Two memory-mapped registers, and every access is a pair: write the index,
 * then read or write the data. The rest of the chip is behind them. */
#define IOAPIC_INDEX		0x00
#define IOAPIC_DATA		0x10

#define IOAPIC_REG_ID		0x00
#define IOAPIC_REG_VERSION	0x01
#define IOAPIC_REG_TABLE	0x10	/* two 32-bit words per entry */

/* A redirection entry's low word. */
#define ENTRY_MASKED		(1u << 16)
#define ENTRY_LEVEL		(1u << 15)	/* level triggered */
#define ENTRY_ACTIVE_LOW	(1u << 13)
#define ENTRY_LOGICAL		(1u << 11)	/* destination is a set, not one */

#define MADT_IOAPIC		1
#define MADT_INTERRUPT_OVERRIDE	2

struct madt_ioapic {
	u8 type;
	u8 length;
	u8 id;
	u8 reserved;
	u32 address;
	u32 gsi_base;
} RK_PACKED;

struct madt_override {
	u8 type;
	u8 length;
	u8 bus;
	u8 source;		/* the ISA interrupt request number */
	u32 gsi;		/* where it actually arrives */
	u16 flags;		/* polarity in bits 1:0, trigger in bits 3:2 */
} RK_PACKED;

#define IOAPICS_MAX 8

struct ioapic {
	volatile u8 *base;
	u32 gsi_base;
	unsigned entries;
};

static struct ioapic apics[IOAPICS_MAX];
static unsigned apic_count;

/* One per ISA interrupt request, filled from the override entries and
 * defaulting to the identity -- which is what the specification says to assume
 * for a line nobody overrode. */
static u32 isa_to_gsi[16];
static u16 isa_flags[16];
static bool routed_through_ioapic;

static u32 reg_read(struct ioapic *a, unsigned reg)
{
	*(volatile u32 *)(a->base + IOAPIC_INDEX) = reg;
	return *(volatile u32 *)(a->base + IOAPIC_DATA);
}

static void reg_write(struct ioapic *a, unsigned reg, u32 value)
{
	*(volatile u32 *)(a->base + IOAPIC_INDEX) = reg;
	*(volatile u32 *)(a->base + IOAPIC_DATA) = value;
}

/* Which chip owns a given global interrupt, or nothing. A machine with several
 * I/O APICs splits the numbering between them, and the entry for GSI 24 on the
 * second chip is its entry 0 -- not its entry 24, which does not exist. */
static struct ioapic *owner_of(u32 gsi, unsigned *index)
{
	unsigned i;

	for (i = 0; i < apic_count; i++)
		if (gsi >= apics[i].gsi_base &&
		    gsi < apics[i].gsi_base + apics[i].entries) {
			*index = (unsigned)(gsi - apics[i].gsi_base);
			return &apics[i];
		}

	return 0;
}

static void write_entry(struct ioapic *a, unsigned index, u32 low, u32 high)
{
	/* The high word first, and then the low one.
	 *
	 * The low word holds the mask bit, so writing it last is what arms the
	 * entry -- and arming an entry whose destination has not been written
	 * yet sends the next interrupt on that line to whatever processor the
	 * firmware left in the register. */
	reg_write(a, IOAPIC_REG_TABLE + index * 2 + 1, high);
	reg_write(a, IOAPIC_REG_TABLE + index * 2, low);
}

bool x86_ioapic_init(void)
{
	const struct madt *madt = (const struct madt *)acpi_find("APIC");
	const u8 *p, *end;
	unsigned i;

	for (i = 0; i < 16; i++) {
		isa_to_gsi[i] = i;	/* until the tables say otherwise */
		isa_flags[i] = 0;
	}

	if (!madt)
		return false;

	p = (const u8 *)madt + sizeof(*madt);
	end = (const u8 *)madt + madt->header.length;

	while (p + 2 <= end) {
		u8 type = p[0];
		u8 len = p[1];

		/* A zero length is not a malformed entry, it is an endless
		 * loop. The same check smp.c makes on the same table. */
		if (len < 2 || p + len > end)
			break;

		if (type == MADT_IOAPIC && len >= sizeof(struct madt_ioapic)) {
			const struct madt_ioapic *e =
				(const struct madt_ioapic *)p;

			if (apic_count < IOAPICS_MAX && e->address) {
				struct ioapic *a = &apics[apic_count];
				vaddr_t va = (vaddr_t)(uintptr_t)
					phys_to_virt((paddr_t)e->address);

				/* Device memory and global, for the same
				 * reasons the local APIC's page is: writes
				 * must reach the chip in order, and the
				 * mapping has to survive an address-space
				 * change because an interrupt can be
				 * reprogrammed from any process's context. */
				if (vm_lookup(va) ||
				    vm_map(va, (paddr_t)e->address, PAGE_SIZE,
					   VM_READ | VM_WRITE | VM_DEVICE |
					   VM_GLOBAL)) {
					a->base = (volatile u8 *)(uintptr_t)va;
					a->gsi_base = e->gsi_base;
					a->entries = ((reg_read(a,
						IOAPIC_REG_VERSION) >> 16) &
						0xFF) + 1;
					apic_count++;
				} else {
					kputs("  ioapic: could not map one; "
					      "staying on the 8259\n");
				}
			}
		} else if (type == MADT_INTERRUPT_OVERRIDE &&
			   len >= sizeof(struct madt_override)) {
			const struct madt_override *e =
				(const struct madt_override *)p;

			if (e->source < 16) {
				isa_to_gsi[e->source] = e->gsi;
				isa_flags[e->source] = e->flags;
			}
		}

		p += len;
	}

	if (!apic_count)
		return false;

	/* Every entry masked before any is armed. Firmware leaves this table in
	 * whatever state its own code wanted, and an entry left armed pointing
	 * at a vector this kernel has not filled in is an interrupt storm on
	 * the first device that raises it. */
	for (i = 0; i < apic_count; i++) {
		unsigned e;

		for (e = 0; e < apics[i].entries; e++)
			write_entry(&apics[i], e, ENTRY_MASKED, 0);
	}

	return true;
}

bool x86_ioapic_present(void)
{
	return apic_count != 0;
}

bool x86_ioapic_in_use(void)
{
	return routed_through_ioapic;
}

/* Points one ISA interrupt request at a vector on one processor. */
bool x86_ioapic_route_isa(unsigned irq, u8 vector, u32 destination)
{
	u32 gsi, low = vector;
	unsigned index;
	struct ioapic *a;
	u16 flags;

	if (irq >= 16)
		return false;

	gsi = isa_to_gsi[irq];
	flags = isa_flags[irq];
	a = owner_of(gsi, &index);

	if (!a)
		return false;

	/* Polarity and trigger come from the override entry when there is one.
	 * Bits 1:0 are the polarity and 3:2 the trigger, and in both fields
	 * zero means "whatever the bus normally does" -- which for ISA is
	 * active high and edge triggered, so zero needs nothing set. */
	if ((flags & 0x3) == 0x3)
		low |= ENTRY_ACTIVE_LOW;
	if (((flags >> 2) & 0x3) == 0x3)
		low |= ENTRY_LEVEL;

	/* Physical destination, one named processor. Logical mode can address a
	 * set of processors at once and is how interrupts get balanced, and
	 * that is a policy this kernel has not got yet -- so the destination is
	 * a processor somebody chose, rather than a group whose membership
	 * nothing maintains. */
	write_entry(a, index, low, destination << 24);
	return true;
}

/* Takes every ISA line off the 8259 and puts it on the I/O APIC.
 *
 * The vectors are the ones the 8259 was already using, so nothing downstream of
 * trap_dispatch has to change -- what changes is which chip has to be told the
 * interrupt was handled, and that is the one thing this gets wrong silently if
 * it is got wrong at all. See trap_dispatch.
 */
bool x86_ioapic_take_over(void)
{
	unsigned irq;
	u32 destination;

	if (!apic_count || !x86_apic_present())
		return false;

	destination = x86_apic_id();

	/* Masked at the 8259 first, and only then armed at the I/O APIC. The
	 * other order leaves a window in which both chips are delivering the
	 * same line, which on a level-triggered line is a duplicate the kernel
	 * acknowledges to the wrong controller. */
	x86_pic_mask_all();

	for (irq = 0; irq < 16; irq++) {
		/* IRQ 2 is where the second 8259 was cascaded into the first.
		 * There is no device on it and there is no second controller
		 * any more, so arming it would be arming nothing -- and on the
		 * usual machine GSI 2 is where the *timer* arrives, which makes
		 * this the one line where a mistake stops the clock. */
		if (irq == 2)
			continue;

		x86_ioapic_route_isa(irq, (u8)(32 + irq), destination);
	}

	routed_through_ioapic = true;

	/* And then check, rather than assume.
	  *
	  * Everything above is programmed from tables written by somebody
	  * else, describing a machine this code has not seen. If any of it is
	  * wrong the symptom is not an error: it is that the timer interrupt
	  * stops arriving, and a kernel whose clock has stopped does not get
	  * as far as reporting anything.
	  *
	  * So the tick is watched across the switch, against a clock that does
	  * not depend on it -- arch_monotonic_ns reads hardware. If it does
	  * not advance, the 8259 gets its lines back and the machine carries
	  * on exactly as it did before, having said so.
	  *
	  * This is not an off-switch for a check. It is the check: the
	  * alternative is a machine that hangs at boot on hardware nobody
	  * here owns, with no output to say why. */
	{
		u64 before = time_ticks();
		u64 deadline = arch_monotonic_ns() + 200000000ull;

		/* Interrupts on, because the thing being watched for is an
		  * interrupt. Written as the instruction rather than through
		  * arch_irq_restore, which takes a whole flags word: the first
		  * attempt passed 2 for "interrupts on" and the interrupt flag is
		  * bit 9, so it waited for a tick with interrupts masked, watched
		  * one never arrive, and reported that the I/O APIC had broken the
		  * clock. The check was right about what it saw and wrong about
		  * what it had done. */
		__asm__ volatile("sti");

		while (time_ticks() < before + 2 &&
		       arch_monotonic_ns() < deadline)
			arch_cpu_relax();

		if (time_ticks() < before + 2) {
			kputs("  ioapic: the tick stopped when the lines moved "
				"across, so they are going back to the 8259\n");

			for (irq = 0; irq < 16; irq++) {
				unsigned index;
				struct ioapic *a = owner_of(isa_to_gsi[irq], &index);

				if (a)
					write_entry(a, index, ENTRY_MASKED, 0);
			}

			routed_through_ioapic = false;
			x86_pic_restore_default();
			return false;
		}
	}

	return true;
}

/* The portable entry point: called once, after every processor is running. */
void arch_irq_route_init(void)
{
	if (!x86_ioapic_init())
		return;

	x86_ioapic_take_over();
}

void arch_irq_print_summary(void)
{
	x86_ioapic_print_summary();
	x86_msi_print_summary();
}

bool arch_irq_self_test(void)
{
	return x86_msi_self_test();
}

void x86_ioapic_print_summary(void)
{
	unsigned i;



	if (!apic_count) {
		kputs("  routing      : 8259, so every device interrupt lands "
		      "on the boot processor\n");
		return;
	}

	for (i = 0; i < apic_count; i++)
		kprintf("  I/O APIC %u   : %u inputs from global interrupt "
			"%u\n", i, apics[i].entries, apics[i].gsi_base);

	kprintf("  routing      : %s\n",
		routed_through_ioapic ? "I/O APIC, so device interrupts can be "
		"sent to any processor" : "8259, because the I/O APIC could "
		"not be armed");

	/* The overrides, because they are the part that is wrong on a machine
	 * nobody tested and there is no other way to see what was read. */
	for (i = 0; i < 16; i++)
		if (isa_to_gsi[i] != i)
			kprintf("  override     : ISA interrupt %u actually "
				"arrives on global interrupt %u\n",
				i, isa_to_gsi[i]);
}
