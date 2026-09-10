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

/* --- x2APIC ---------------------------------------------------------------
 *
 * The same local APIC reached through model-specific registers instead of a
 * page of memory, and the reason to care is not the access method: **in xAPIC
 * mode a processor identifier is eight bits.** The identifier register holds
 * eight, and the interrupt command register has eight bits of destination, so
 * 255 is the highest processor this kernel could ever speak to -- while the
 * MADT walk in smp.c has always read x2APIC entries and would happily
 * enumerate a thousand. Found and unable to be started is what a two-socket
 * server would have hit.
 *
 * x2APIC identifiers are 32 bits. The register block becomes MSRs 0x800
 * upward, one per 16-byte offset of the old block, and the interrupt command
 * register becomes a single 64-bit write with the destination in the high
 * half -- which also removes the two-write sequence the old path had to get
 * in the right order.
 *
 * **It cannot be turned off again without resetting the processor.** So the
 * decision is made once, at boot, from what CPUID reports -- and every
 * processor makes the same decision, because they must agree: a secondary
 * left in xAPIC mode would have a different identifier from the one the boot
 * processor used to start it.
 */
#define APIC_BASE_X2APIC	(1ull << 10)
#define MSR_X2APIC_BASE		0x800u
#define MSR_X2APIC_ICR		0x830u

/* Off on every machine this has run on, and the reason is worth recording
 * where somebody reading the code will find it rather than in a commit
 * message: **QEMU 8.2 does not implement x2APIC under TCG.** Asking for it
 * with -cpu qemu64,+x2apic produces
 *
 *     TCG doesn't support requested feature: CPUID.01H:ECX.x2apic [bit 21]
 *
 * and even -cpu max reports the bit clear, which was measured rather than
 * assumed. KVM would provide it and this machine cannot reach /dev/kvm.
 *
 * So every line below that runs only when x2apic is true has never executed.
 * It is here because the alternative is a kernel that cannot start the
 * processors on a two-socket server at all, and because the arithmetic it
 * depends on -- which register becomes which MSR, where the destination sits
 * in the 64-bit command -- is testable without the hardware and is tested in
 * arch_identity_self_test. What is untested is whether the mode switch takes
 * and whether a real processor answers afterwards. */
static bool x2apic;
static bool announced;

/* An offset in the memory-mapped block, as the MSR holding the same register.
 * The block is 16-byte spaced and the MSRs are consecutive, so the mapping is
 * a shift -- stated here once rather than at each register. */
static inline u32 x2apic_msr(unsigned reg)
{
	return MSR_X2APIC_BASE + (reg >> 4);
}

/* Both of the above, exposed so they can be tested on a machine that cannot
 * enter the mode -- see arch_identity_self_test. Neither touches hardware. */
u32 x86_apic_msr_for(unsigned reg)
{
	return x2apic_msr(reg);
}

u64 x86_apic_command_word(u32 target, u32 command)
{
	return ((u64)target << 32) | command;
}

static volatile u8 *lapic;
static u32 timer_ticks_per_interval;

static inline u32 apic_read(unsigned reg)
{
	if (x2apic)
		return (u32)x86_rdmsr(x2apic_msr(reg));

	return *(volatile u32 *)(lapic + reg);
}

static inline void apic_write(unsigned reg, u32 value)
{
	if (x2apic) {
		x86_wrmsr(x2apic_msr(reg), value);
		return;
	}

	*(volatile u32 *)(lapic + reg) = value;
}

bool x86_apic_present(void)
{
	/* In x2APIC mode there is no mapping to have made -- the registers are
	  * MSRs -- so `lapic` stays null on a machine where the APIC is very
	  * much present. Asking the wrong question here would have made every
	  * caller believe there was no interrupt controller at all. */
	return x2apic || lapic != 0;
}

bool x86_apic_is_x2(void)
{
	return x2apic;
}

/* Whether this processor can do x2APIC at all: CPUID leaf 1, ECX bit 21. */
static bool x2apic_supported(void)
{
	u32 a, b, c, d;

	__asm__ volatile("cpuid"
			: "=a"(a), "=b"(b), "=c"(c), "=d"(d)
			: "a"(1u), "c"(0u));

	return (c & (1u << 21)) != 0;
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

	/* And then, if the processor has it, x2APIC.
	  *
	  * The order matters: the memory-mapped block above is set up first and
	  * unconditionally, because the decision below has to be made the same
	  * way by every processor and a processor that could not make it needs
	  * the old path working underneath it.
	  *
	  * The boot processor decides and the secondaries follow: `x2apic` is
	  * already true by the time any of them runs this, so they take the
	  * branch rather than re-deciding. Two processors reaching different
	  * answers would be a machine whose processors do not agree what their
	  * own identifiers are.
	  *
	  * Firmware may have enabled it already -- that is what the EXTD bit in
	  * the value just read would say -- and enabling it twice is harmless,
	  * while going the other way is not possible at all without a reset. */
	if (x2apic || x2apic_supported()) {
		u64 want = (base_msr | APIC_BASE_ENABLE | APIC_BASE_X2APIC);

		x86_wrmsr(MSR_APIC_BASE, want);

		/* Read back, because this is a mode switch and believing it
		  * happened when it did not means every register access after
		  * this line goes to the wrong place. A processor that refuses
		  * keeps the memory-mapped path, which still works. */
		if (x86_rdmsr(MSR_APIC_BASE) & APIC_BASE_X2APIC)
			x2apic = true;
		else if (x2apic)
			kputs("  apic: this processor would not enter x2APIC mode "
				"and the others have\n");
	}

	/* Accept every priority. Firmware sometimes leaves this raised, and a
	 * raised task priority silently discards interrupts below it -- which
	 * looks exactly like a timer that was never programmed. */
	apic_write(APIC_TPR, 0);

	apic_write(APIC_SPURIOUS, APIC_SOFTWARE_ENABLE | VECTOR_SPURIOUS);

	/* Said once, by whichever processor gets here first, because which mode
	  * this machine is in decides how many processors it can address and
	  * there was previously no way to tell from the outside. Every boot in
	  * the matrix now states it. */
	if (!announced) {
		announced = true;
		kprintf("  apic: %s, so processor identifiers are %s\n",
			x2apic ? "x2APIC" : "xAPIC",
			x2apic ? "32-bit, and this path is UNTESTED"
				: "8-bit (255 processors at most)");
	}

	return true;
}

u32 x86_apic_id(void)
{
	/* Thirty-two bits in x2APIC mode and eight in the top of the register
	  * in xAPIC mode. Not a cosmetic difference: shifting an x2APIC
	  * identifier down by 24 turns processors 0 through 255 into processor
	  * 0, which is every per-processor lookup in the kernel pointing at one
	  * slot. */
	if (x2apic)
		return (u32)x86_rdmsr(x2apic_msr(APIC_ID));

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

	/* There is nothing to wait for in x2APIC mode: the command is a single
	  * MSR write, which cannot be half-done, and the delivery-status bit is
	  * reserved and reads zero. Polling it here would be a loop that always
	  * exits immediately, which is harmless and is also a lie about what the
	  * hardware is doing. */
	if (x2apic)
		return;

	while ((apic_read(APIC_ICR_LOW) & ICR_DELIVERY_PENDING) &&
	       spins < 1000000)
		spins++;
}

static void send_command(u32 target, u32 command)
{
	if (x2apic) {
		/* One 64-bit write, destination in the high half -- and the
		  * full 32 bits of it, which is the entire point. */
		x86_wrmsr(MSR_X2APIC_ICR,
				x86_apic_command_word(target, command));
		return;
	}

	/* Above 255 there is no way to say who this is for: the destination
	  * field is eight bits wide. Refused rather than truncated, because a
	  * truncated destination is a message delivered to the wrong processor
	  * -- an INIT sent to somebody already running. */
	if (target > 0xFFu) {
		kprintf("  apic: processor 0x%x cannot be addressed without "
			"x2APIC\n", target);
		return;
	}

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

/* Sends a vector to every other processor, and to no one else.
 *
 * The destination shorthand does the addressing: bits 18 and 19 set to "all
 * excluding self" means the message goes to every processor on the bus without
 * this code having to know how many there are or what their identifiers are.
 * Enumerating them and sending one at a time would work and would be a list
 * that can go stale.
 *
 * Fixed delivery rather than the special modes, and edge triggered, because
 * this is an ordinary interrupt that happens to be sent by software. */
#define ICR_ALL_BUT_SELF	(3u << 18)

void x86_apic_send_ipi_all_but_self(u8 vector)
{
	if (!lapic)
		return;

	/* No destination field to write: the shorthand replaces it. Writing the
	 * low half is what sends. */
	apic_write(APIC_ICR_LOW, ICR_ALL_BUT_SELF | ICR_LEVEL_ASSERT | vector);
	wait_for_delivery();
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
