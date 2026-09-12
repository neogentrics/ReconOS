/* The High Precision Event Timer, and why a kernel with a working clock wants
 * a second one.
 *
 * `arch_monotonic_ns` reads the time stamp counter, which is one instruction
 * and the best clock on the machine -- **when the processor says it is
 * invariant**. That word is doing a great deal of work. A counter that is not
 * invariant changes rate with the clock speed, so it runs slow under power
 * management and fast under turbo, and nothing about reading it tells you that
 * happened. Checkpoint 3 already asks the question; until now nothing could act
 * on a "no".
 *
 * The HPET is the answer to a no. It is a counter in the chipset rather than in
 * the core: it ticks at a fixed femtosecond period the hardware reports, it does
 * not care what the processor is doing, and it is the same counter on every
 * processor in the machine. It is slower to read -- a memory access to a device
 * rather than one instruction -- which is exactly why it is not simply used for
 * everything.
 *
 * --- What this does and does not do ---
 *
 * It finds the HPET, maps it, checks it is actually running, and offers its
 * counter as a nanosecond clock. It does **not** take over the tick. The tick
 * comes from the PIT and works; replacing a working interrupt source with an
 * unproven one, in the same change that introduces the unproven one, is how a
 * machine stops booting for a reason nobody can see.
 *
 * So the comparators -- the part of the HPET that can raise an interrupt -- are
 * deliberately untouched. They are what a tickless kernel would want, and
 * checkpoint 24 is where that question belongs.
 *
 * --- The period, and the one piece of arithmetic worth reading twice ---
 *
 * Capability bits 63:32 hold the period of one tick in **femtoseconds**, which
 * is 10^-15 seconds. Nanoseconds are 10^-9. So
 *
 *     ns = ticks * period_fs / 1000000
 *
 * and the multiplication overflows a 64-bit number long before the counter
 * does: a 100 MHz HPET has a period around 10,000,000 fs, so `ticks *
 * period_fs` passes 2^64 after about 18,000 seconds -- five hours. A kernel
 * whose clock silently wraps five hours after boot is worse than one with no
 * second clock at all.
 *
 * So the division is done first where it is exact, and the remainder is carried
 * separately. Ticks are split into whole microseconds' worth and what is left
 * over, and neither part can overflow on any period a real HPET reports.
 */
#include "x86_64.h"

#include <recon/kernel/acpi.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/console.h>
#include <recon/kernel/hpet.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>

/* The ACPI table. Fixed layout, and the address is a Generic Address Structure
 * of which only the address itself is used -- an HPET is always memory-mapped,
 * and a firmware claiming otherwise is one this does not want to believe. */
struct acpi_hpet {
	struct acpi_sdt_header header;
	u32 event_timer_block_id;
	u8  address_space_id;
	u8  register_bit_width;
	u8  register_bit_offset;
	u8  reserved;
	u64 address;
	u8  hpet_number;
	u16 minimum_tick;
	u8  page_protection;
} __attribute__((packed));

#define HPET_CAPABILITIES	0x000
#define HPET_CONFIGURATION	0x010
#define HPET_MAIN_COUNTER	0x0F0

#define HPET_CFG_ENABLE		(1ull << 0)

static volatile u8 *base;
static u32 period_fs;
static u64 at_boot;
static bool running;

static u64 reg_read(unsigned off)
{
	return *(volatile u64 *)(base + off);
}

static void reg_write(unsigned off, u64 v)
{
	*(volatile u64 *)(base + off) = v;
}

bool hpet_present(void)
{
	return running;
}

u32 hpet_period_fs(void)
{
	return period_fs;
}

/* Ticks since this was started, as nanoseconds.
 *
 * Split so neither half can overflow. `whole` counts complete millions of
 * femtoseconds -- that is, whole nanoseconds' worth of period -- and `part`
 * carries what is left, multiplied before it is divided because it is small
 * enough to survive it. */
u64 hpet_now_ns(void)
{
	u64 ticks;
	u64 whole, part;

	if (!running)
		return 0;

	ticks = reg_read(HPET_MAIN_COUNTER) - at_boot;

	whole = ticks / 1000000ull;
	part  = ticks % 1000000ull;

	return whole * (u64)period_fs + (part * (u64)period_fs) / 1000000ull;
}

void hpet_init(void)
{
	const struct acpi_hpet *t =
		(const struct acpi_hpet *)acpi_find("HPET");
	vaddr_t va;
	u64 caps;

	if (!t || !t->address)
		return;

	va = (vaddr_t)(uintptr_t)phys_to_virt((paddr_t)t->address);

	/* Device memory and global, for the reasons the I/O APIC's page gives:
	 * the reads must actually reach the chip rather than a cache line, and
	 * the mapping has to survive an address-space change because time is
	 * asked for from every context there is. */
	if (!vm_lookup(va) &&
	    !vm_map(va, (paddr_t)t->address, PAGE_SIZE,
		    VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL))
		return;

	base = (volatile u8 *)(uintptr_t)va;

	caps = reg_read(HPET_CAPABILITIES);
	period_fs = (u32)(caps >> 32);

	/* A period of zero is a chip that did not answer, and a period above
	 * 100 nanoseconds is outside what the specification permits. Both are
	 * refused rather than used: a clock that is wrong is worse than a
	 * clock that is absent, because something will believe it. */
	if (!period_fs || period_fs > 100000000u) {
		base = NULL;
		period_fs = 0;
		return;
	}

	/* Start it if firmware left it stopped, then check that it is actually
	 * moving. **The enable bit reading back as set is not evidence.** A
	 * counter that never advances satisfies every register check and hands
	 * out the same nanosecond for ever. */
	reg_write(HPET_CONFIGURATION,
		  reg_read(HPET_CONFIGURATION) | HPET_CFG_ENABLE);

	{
		u64 first = reg_read(HPET_MAIN_COUNTER);
		u64 spin;
		u64 second;

		for (spin = 0; spin < 100000; spin++)
			__asm__ __volatile__("" ::: "memory");

		second = reg_read(HPET_MAIN_COUNTER);

		if (second == first) {
			kputs("  hpet: the counter is not advancing, so it is "
			      "not being used\n");
			base = NULL;
			period_fs = 0;
			return;
		}

		at_boot = second;
	}

	running = true;
}

void hpet_print_summary(void)
{
	if (!running) {
		kputs("  hpet         : none on this machine\n");
		return;
	}

	/* The frequency, worked out from the period, because that is the number
	 * a person recognises. Femtoseconds per tick is what the hardware says
	 * and megahertz is what it means. */
	kprintf("  hpet         : %u.%u MHz, %u fs per tick\n",
		(unsigned)(1000000000ull / period_fs),
		(unsigned)((1000000000ull * 10 / period_fs) % 10),
		period_fs);
}

/* --- the self-test --------------------------------------------------------
 *
 * Two claims, and the second is the one that could plausibly be wrong.
 *
 * That the counter moves is nearly free to check and would catch a chip that
 * was mapped but dead. That it *agrees with the other clock* is the real test:
 * two independent counters, one in the core and one in the chipset, measuring
 * the same wait. If the femtosecond arithmetic above is wrong -- and it is the
 * only part of this file that could be wrong quietly -- the two disagree by a
 * factor, not by a rounding error.
 *
 * A generous bound, deliberately. The point is to catch a scale error, and
 * under an emulator with a software timer the two clocks genuinely do drift.
 * Asserting tightly here would produce a test that fails for a reason that is
 * not a fault.
 */
bool hpet_self_test(void)
{
	u64 h1, h2, t1, t2;
	u64 spin;
	u64 hd, td;

	if (!running) {
		kputs("  hpet: none on this machine, so there is nothing to "
		      "check\n");
		return true;
	}

	h1 = hpet_now_ns();
	t1 = arch_monotonic_ns();

	for (spin = 0; spin < 2000000; spin++)
		__asm__ __volatile__("" ::: "memory");

	h2 = hpet_now_ns();
	t2 = arch_monotonic_ns();

	if (h2 <= h1) {
		kputs("  hpet: the counter did not advance across a wait\n");
		return false;
	}

	hd = h2 - h1;
	td = t2 > t1 ? t2 - t1 : 0;

	if (!td)
		return true;		/* the other clock said nothing */

	/* Within a factor of eight either way. A scale error in the
	 * femtosecond conversion is a factor of a million. */
	if (hd > td * 8 || td > hd * 8) {
		kprintf("  hpet: measured %lu ns where the counter measured "
			"%lu -- the two clocks disagree by more than a "
			"rounding\n", (unsigned long)hd, (unsigned long)td);
		return false;
	}

	return true;
}
