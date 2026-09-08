/* The local APIC: how one processor talks to another, and how each one gets a
 * clock of its own.
 *
 * Everything before checkpoint 9b used the two chips the IBM PC shipped with:
 * an 8259 interrupt controller and an 8254 timer. Both are singular. There is
 * one 8254 in the machine and its output goes to one processor, so a kernel
 * that keeps time with it has exactly one processor that can be preempted --
 * which is fine while there is one processor and useless the moment there are
 * four.
 *
 * The local APIC is per-processor by construction. Each one has its own
 * identifier, its own timer, and its own interrupt inputs, and the way to reach
 * another processor is to ask yours to send it a message. So this file arrives
 * with 9b rather than earlier: nothing needed it until there was somebody to
 * talk to.
 *
 * --- What is deliberately not here ---
 *
 * The **I/O APIC**, which is what replaces the 8259 for routing *device*
 * interrupts. The 8259 still works, still delivers the disk and the keyboard to
 * the boot processor, and replacing it is a separate piece of work with its own
 * discovery problem. Until then device interrupts land on one processor and the
 * timer does not, which is an honest halfway house rather than a finished one.
 */
#include "x86_64.h"

#include <recon/kernel/console.h>
#include <recon/kernel/time.h>
#include <recon/kernel/vm.h>

/* Registers, as byte offsets from the base. Each is 32 bits and each must be
 * accessed as exactly 32 bits -- a byte or word access to one is undefined,
 * which is why nothing here takes a u8 pointer to them. */
#define APIC_ID			0x020
#define APIC_VERSION		0x030
#define APIC_TPR		0x080	/* task priority */
#define APIC_EOI		0x0B0
#define APIC_SPURIOUS		0x0F0
#define APIC_ICR_LOW		0x300	/* interrupt command, and the write that sends */
#define APIC_ICR_HIGH		0x310	/* ... its destination */
#define APIC_LVT_TIMER		0x320
#define APIC_TIMER_INITIAL	0x380
#define APIC_TIMER_CURRENT	0x390
#define APIC_TIMER_DIVIDE	0x3E0

/* Spurious-interrupt vector register. Bit 8 is what turns the APIC on at all;
 * the low eight bits are the vector it raises when an interrupt is withdrawn
 * between being raised and being taken, which is rare and must still land
 * somewhere that returns. */
#define APIC_SOFTWARE_ENABLE	(1u << 8)
#define VECTOR_SPURIOUS		0xFF

/* The per-processor timer's vector. Chosen above the sixteen the 8259 occupies
 * so that the two can coexist while the I/O APIC does not exist yet. */
#define VECTOR_APIC_TIMER	0x40

/* Interrupt command register, low half. */
#define ICR_DELIVERY_INIT	(5u << 8)
#define ICR_DELIVERY_STARTUP	(6u << 8)
#define ICR_LEVEL_ASSERT	(1u << 14)
#define ICR_DELIVERY_PENDING	(1u << 12)

/* IA32_APIC_BASE. Bit 11 enables the APIC globally, bit 8 marks the processor
 * that booted the machine, and the address is in the top of the register --
 * which is why the base is read from here rather than assumed to be the
 * architectural default. Firmware is allowed to move it. */
#define MSR_APIC_BASE		0x1Bu
#define APIC_BASE_ENABLE	(1ull << 11)

static volatile u8 *lapic;
static u32 timer_ticks_per_interval;

static inline u32 apic_read(unsigned reg)
{
	return *(volatile u32 *)(lapic + reg);
}

static inline void apic_write(unsigned reg, u32 value)
{
	*(volatile u32 *)(lapic + reg) = value;
}

bool x86_apic_present(void)
{
	return lapic != 0;
}

/* Maps the APIC and turns it on for the processor that calls it.
 *
 * Called once by the boot processor and again by every secondary, because the
 * *mapping* is shared and the *enabling* is not: each local APIC is a different
 * piece of hardware behind the same physical address, and each one is off until
 * its own processor says otherwise. */
bool x86_apic_init(void)
{
	u64 base_msr = x86_rdmsr(MSR_APIC_BASE);
	paddr_t base = base_msr & 0xFFFFFF000ull;

	if (!base)
		return false;

	if (!lapic) {
		vaddr_t va = (vaddr_t)(uintptr_t)phys_to_virt(base);

		/* Device memory, not ordinary memory: writes must reach the
		 * hardware in the order they were made and must not be held in
		 * a cache line waiting for company. Mapped global, because
		 * every processor reaches its own APIC through this one
		 * address and the mapping must survive an address-space
		 * change. */
		if (!vm_lookup(va) &&
		    !vm_map(va, base, PAGE_SIZE,
			    VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL)) {
			kputs("  apic: could not map the local APIC\n");
			return false;
		}

		lapic = (volatile u8 *)(uintptr_t)va;
	}

	/* Enabled in the MSR before the register block is touched. On a
	 * processor where firmware left it off, every read below would return
	 * ones and every write would go nowhere -- silently. */
	x86_wrmsr(MSR_APIC_BASE, base_msr | APIC_BASE_ENABLE);

	/* Accept every priority. Firmware sometimes leaves this raised, and a
	 * raised task priority silently discards interrupts below it -- which
	 * looks exactly like a timer that was never programmed. */
	apic_write(APIC_TPR, 0);

	apic_write(APIC_SPURIOUS, APIC_SOFTWARE_ENABLE | VECTOR_SPURIOUS);

	return true;
}

u32 x86_apic_id(void)
{
	if (!lapic)
		return 0;

	return apic_read(APIC_ID) >> 24;
}

void x86_apic_eoi(void)
{
	if (lapic)
		apic_write(APIC_EOI, 0);
}

/* Waits for the last interrupt command to be accepted.
 *
 * The command register holds one message at a time and reports its progress in
 * a bit. Writing a second before the first has gone loses it -- and losing a
 * startup message produces a processor that simply never appears, with nothing
 * anywhere saying why. */
static void wait_for_delivery(void)
{
	unsigned spins = 0;

	while ((apic_read(APIC_ICR_LOW) & ICR_DELIVERY_PENDING) &&
	       spins < 1000000)
		spins++;
}

static void send_command(u32 target, u32 command)
{
	/* Destination first. The low half is what sends, so writing it first
	 * would send the message to whatever destination was last used. */
	apic_write(APIC_ICR_HIGH, target << 24);
	apic_write(APIC_ICR_LOW, command);
	wait_for_delivery();
}

void x86_apic_send_init(u32 target)
{
	send_command(target, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT);
}

/* The startup message carries the address to begin at as a *page number in one
 * byte*, which is the entire reason the trampoline has to live below one
 * megabyte. Vector 8 means start at 0x8000. */
void x86_apic_send_startup(u32 target, u8 page)
{
	send_command(target, ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT | page);
}

/* --- the per-processor timer ---------------------------------------------
 *
 * The APIC timer counts down at the bus frequency, which nothing reports. So it
 * is measured against a clock that is already trusted: run it flat out for a
 * known interval and see how far it got. That is the same method time.c uses to
 * calibrate the time stamp counter against the 8254, for the same reason --
 * a frequency nobody states has to be observed.
 */
#define TIMER_PERIODIC		(1u << 17)
#define TIMER_MASKED		(1u << 16)
#define TIMER_DIVIDE_BY_16	0x3

void x86_apic_calibrate_timer(void)
{
	u64 start, elapsed;
	u32 remaining;

	if (!lapic)
		return;

	apic_write(APIC_TIMER_DIVIDE, TIMER_DIVIDE_BY_16);

	/* Masked while calibrating: the count is wanted, the interrupt is not,
	 * and an interrupt taken here would land on a vector nothing has been
	 * told about yet. */
	apic_write(APIC_LVT_TIMER, TIMER_MASKED | VECTOR_APIC_TIMER);
	apic_write(APIC_TIMER_INITIAL, 0xFFFFFFFFu);

	start = time_monotonic_ns();
	while (time_monotonic_ns() - start < 20000000ull)	/* 20 ms */
		;
	elapsed = time_monotonic_ns() - start;

	remaining = apic_read(APIC_TIMER_CURRENT);
	apic_write(APIC_TIMER_INITIAL, 0);

	if (elapsed == 0 || remaining == 0xFFFFFFFFu) {
		/* It did not count. Reported rather than papered over with a
		 * plausible constant: a made-up frequency gives every processor
		 * a tick at the wrong rate, and a wrong tick rate is a
		 * scheduler that looks like it works. */
		timer_ticks_per_interval = 0;
		kputs("  apic: the timer did not count; secondaries will not "
		      "be preempted\n");
		return;
	}

	{
		u64 counted = 0xFFFFFFFFull - remaining;
		u64 per_second = counted * 1000000000ull / elapsed;

		timer_ticks_per_interval = (u32)(per_second / TIME_TICK_HZ);

		if (timer_ticks_per_interval == 0)
			timer_ticks_per_interval = 1;
	}
}

/* Starts this processor's timer at the kernel's tick rate. Every processor
 * calls it for itself; the calibration above is shared because the bus
 * frequency is a property of the machine rather than of one processor. */
void x86_apic_start_timer(void)
{
	if (!lapic || !timer_ticks_per_interval)
		return;

	apic_write(APIC_TIMER_DIVIDE, TIMER_DIVIDE_BY_16);
	apic_write(APIC_LVT_TIMER, TIMER_PERIODIC | VECTOR_APIC_TIMER);
	apic_write(APIC_TIMER_INITIAL, timer_ticks_per_interval);
}

bool x86_apic_timer_ready(void)
{
	return lapic != 0 && timer_ticks_per_interval != 0;
}
