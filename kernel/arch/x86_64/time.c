/* x86_64 time: a counter, a tick, and the date.
 *
 * Three pieces of hardware, all of them older than most people reading this,
 * and all of them still exactly where the IBM PC left them:
 *
 *   THE TIME STAMP COUNTER, a 64-bit count of CPU cycles, read with one
 *   instruction. It is the only fine-grained clock worth using -- but it counts
 *   *cycles*, and cycles are not a fixed length of time unless the CPU says its
 *   counter is invariant. Checkpoint 3 already asks.
 *
 *   THE PROGRAMMABLE INTERVAL TIMER, a 1.19MHz counter from 1981, which is used
 *   here for two things: to calibrate the counter above, and to produce the
 *   tick. It is not the best timer on a modern machine -- the local APIC timer
 *   is -- but it needs no ACPI tables, no interrupt routing, and no discovery,
 *   and a kernel that cannot yet read a table should not depend on one.
 *
 *   THE CMOS REAL-TIME CLOCK, which holds the date across power loss.
 */
#include "x86_64.h"

#include <recon/kernel/arch.h>
#include <recon/kernel/time.h>
#include <recon/kernel/cpu.h>
#include <recon/kernel/console.h>
#include <recon/kernel/sched.h>

/* Port I/O is in x86_64.h, which this already includes. */

/* A short pause between writes to the same 1981 chip. Writing to an unused
 * port is the traditional way to spend a bus cycle. */
static inline void io_wait(void) { outb(0x80, 0); }

static inline u64 rdtsc(void)
{
	u32 lo, hi;

	/* The serialising form. Without it the CPU may execute the read before
	 * the work being measured, which is a fine way to measure a negative
	 * interval. */
	__asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
	return ((u64)hi << 32) | lo;
}

static u64 tsc_at_boot;
static u64 tsc_khz;

/* Whether tsc_khz came from the processor or from a measurement -- printed,
 * because the two have different failure modes and a reader deserves to know
 * which one produced the number. */
static bool tsc_from_cpuid;

/* What leaf 15h said, kept so the boot can show its working. Zero when the
 * leaf was absent or silent and the PIT was used instead. */
static u32 tsc_crystal_hz, tsc_ratio_num, tsc_ratio_den;
static u32 tsc_base_mhz;

/* --- The 8259 interrupt controllers --------------------------------------
 *
 * Two chips, cascaded, and their default vectors overlap the CPU's own
 * exceptions -- IRQ 0 arrives as vector 8, which is also the double fault. That
 * collision is a genuine historical accident and every x86 kernel begins by
 * fixing it. */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define IRQ_BASE  32	/* the first vector after the CPU's 32 exceptions */

static void pic_remap(void)
{
	/* Initialisation sequence: start, then three data bytes each. */
	outb(PIC1_CMD, 0x11); io_wait();	/* begin, expect a fourth byte */
	outb(PIC2_CMD, 0x11); io_wait();

	outb(PIC1_DATA, IRQ_BASE);      io_wait();	/* master vectors 32..39 */
	outb(PIC2_DATA, IRQ_BASE + 8);  io_wait();	/* slave vectors 40..47 */

	outb(PIC1_DATA, 0x04); io_wait();	/* slave is on master's line 2 */
	outb(PIC2_DATA, 0x02); io_wait();	/* slave's identity */

	outb(PIC1_DATA, 0x01); io_wait();	/* 8086 mode */
	outb(PIC2_DATA, 0x01); io_wait();

	/* Everything masked except IRQ 0, the timer. Nothing else has a handler
	 * yet, and an unmasked interrupt with no handler is a fault. */
	outb(PIC1_DATA, 0xFE);
	outb(PIC2_DATA, 0xFF);
}

/* The controller has to be told the interrupt was handled, or it never sends
 * another. A tick that fires exactly once is the signature of forgetting. */
void x86_pic_end_of_interrupt(unsigned irq)
{
	if (irq >= 8)
		outb(PIC2_CMD, 0x20);
	outb(PIC1_CMD, 0x20);
}

/* Every line masked at both chips. Used when the I/O APIC takes the lines
 * over: two controllers delivering one line is a duplicate interrupt that the
 * kernel then acknowledges to only one of them. */
/* The mask the 8259 is left with at boot: the timer open, everything else
 * shut. Used to put the lines back when the I/O APIC could not hold them. */
void x86_pic_restore_default(void)
{
	outb(PIC1_DATA, 0xFE);
	outb(PIC2_DATA, 0xFF);
}

/* One line opened at the 8259, without disturbing the others.
 *
 * Read-modify-write rather than a remembered value: the mask is a register
 * in a chip, and this kernel is not the only thing that has written to it
 * -- firmware left it in some state and x86_pic_restore_default has its own
 * opinion. Reading it is one instruction and removes the question. */
void x86_pic_unmask(unsigned irq)
{
	u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
	u8 bit = (u8)(1u << (irq & 7));

	if (irq >= 16)
		return;

	outb(port, (u8)(inb(port) & ~bit));

	/* A line on the second chip only arrives if the cascade is open
	 * too. Forgetting this is the classic way an interrupt above 7
	 * never appears with every other register correct. */
	if (irq >= 8)
		outb(PIC1_DATA, (u8)(inb(PIC1_DATA) & ~(1u << 2)));
}

/* One line closed at the 8259, without disturbing the others. The mirror of
 * x86_pic_unmask, and read-modify-write for the same reason. */
void x86_pic_mask(unsigned irq)
{
	u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
	u8 bit = (u8)(1u << (irq & 7));

	if (irq >= 16)
		return;

	outb(port, (u8)(inb(port) | bit));
}

void x86_pic_mask_all(void)
{
	outb(PIC1_DATA, 0xFF);
	outb(PIC2_DATA, 0xFF);
}

/* Whichever controller actually delivered it.
 *
 * This is the single line that has to change when the routing changes, and it
 * is worth having in one place rather than at each site: acknowledging to the
 * 8259 an interrupt the I/O APIC delivered unbalances a chip that is no longer
 * sending anything, and *failing* to acknowledge the local APIC leaves the
 * interrupt in service -- after which that processor takes no further
 * interrupt at or below its priority. Neither says anything; the machine
 * simply stops getting interrupts. */
void x86_irq_ack(unsigned irq)
{
	if (x86_ioapic_in_use()) {
		x86_apic_eoi();
		return;
	}

	x86_pic_end_of_interrupt(irq);
}

/* --- The programmable interval timer ------------------------------------- */

#define PIT_CH0   0x40
#define PIT_CH2   0x42
#define PIT_CMD   0x43
#define PIT_GATE2 0x61

#define PIT_HZ 1193182u

static void pit_start_tick(void)
{
	unsigned divisor = PIT_HZ / TIME_TICK_HZ;

	/* Mode 2, a rate generator, and NOT mode 3, a square wave.
	 *
	 * This was 0x36 -- mode 3 -- and it worked for as long as the 8259 was
	 * the only thing listening. A square wave holds its output high for
	 * half the period and low for the other half, so there are *two*
	 * transitions per tick; the 8259 as emulated counts one of them and
	 * the I/O APIC counts both. The first boot with the lines moved across
	 * ran the whole kernel at 201 Hz against a 100 Hz constant -- every
	 * sleep half as long as asked, every timer early, and every test that
	 * counted ticks rather than nanoseconds still passing.
	 *
	 * Mode 2 pulses the output low for one input cycle and leaves it high
	 * the rest of the period: one transition, one interrupt, whichever
	 * controller is listening. It is what the chip is for and what every
	 * other kernel uses it in. */
	outb(PIT_CMD, 0x34);			/* channel 0, both bytes, rate generator */
	outb(PIT_CH0, (u8)(divisor & 0xFF));
	outb(PIT_CH0, (u8)(divisor >> 8));
}

/* Measures how many cycles the counter advances in a known interval.
 *
 * Channel 2 is used rather than channel 0 because channel 2's gate can be
 * driven directly from a port, so the measurement needs no interrupt -- which
 * matters, because this runs before any interrupt handler exists. */
/* What the processor says its counter runs at, in kHz, or zero if it declines.
 *
 * Leaf 15h gives the ratio between the counter and the core crystal: EBX/EAX,
 * with ECX the crystal in hertz. ECX is frequently zero even when the ratio is
 * right -- the crystal is then a documented constant per processor family, and
 * 19.2 MHz is the one for the Atom-derived parts that are also the parts most
 * likely to have no PIT. Using it where ECX is silent is the specification's own
 * instruction, not an assumption about this machine.
 *
 * Leaf 16h is the fallback inside the fallback: the base frequency in MHz. It
 * is a nominal figure rather than the counter's exact rate, so it is only
 * reached when 15h says nothing.
 */
static u64 tsc_khz_from_cpuid(void)
{
	u32 max_leaf, a, b, c, d;

	cpuid_count(0, 0, &max_leaf, &b, &c, &d);

	if (max_leaf >= 0x15) {
		cpuid_count(0x15, 0, &a, &b, &c, &d);

		/* a is the denominator and b the numerator. A zero in either
		 * means the leaf is present and has nothing to say. */
		if (a && b) {
			u64 crystal = c;

			if (!crystal)
				crystal = 19200000ULL;	/* Atom-family default */

			tsc_crystal_hz = (u32)crystal;
			tsc_ratio_num  = b;
			tsc_ratio_den  = a;

			return (crystal / 1000ULL) * (u64)b / (u64)a;
		}
	}

	if (max_leaf >= 0x16) {
		cpuid_count(0x16, 0, &a, &b, &c, &d);

		if (a & 0xFFFF) {		/* base frequency, MHz */
			tsc_base_mhz = a & 0xFFFF;
			return (u64)tsc_base_mhz * 1000ULL;
		}
	}

	return 0;
}

static u64 calibrate_tsc(void)
{
	const unsigned ms = 50;
	unsigned count = (PIT_HZ * ms) / 1000;
	u64 start, end;
	u8 gate;

	/* Gate on, speaker off. The second bit is the speaker, and turning it on
	 * would make the machine audibly hum during boot. */
	gate = inb(PIT_GATE2);
	outb(PIT_GATE2, (u8)((gate & ~0x02) | 0x01));

	outb(PIT_CMD, 0xB0);			/* channel 2, both bytes, one-shot */
	outb(PIT_CH2, (u8)(count & 0xFF));
	outb(PIT_CH2, (u8)(count >> 8));

	start = rdtsc();

	/* Bit 5 of the gate port goes high when channel 2 finishes counting.
	 *
	 * **Bounded, because a machine is allowed not to have this chip.** The
	 * 8254 has been optional on Intel platforms for years and firmware can
	 * leave it off; port 0x61 then reads back a constant and bit 5 never
	 * rises. This loop had no exit, and on the first real machine this
	 * kernel was ever booted on the report stopped dead one line before
	 * the clock was mentioned.
	 *
	 * The bound is counted in cycles rather than in time, because this is
	 * the function that works out what a cycle is worth -- there is no
	 * clock to time it against yet. Five billion is longer than 50 ms on
	 * any processor that exists: about eight tenths of a second at 6 GHz,
	 * five seconds at 1 GHz. Long enough that a slow machine is never cut
	 * off, short enough that a machine without the chip still boots. */
	while (!(inb(PIT_GATE2) & 0x20)) {
		if (rdtsc() - start > 5000000000ULL) {
			outb(PIT_GATE2, (u8)(gate & ~0x03));
			return 0;	/* every caller already handles this */
		}
	}

	end = rdtsc();

	outb(PIT_GATE2, (u8)(gate & ~0x03));

	return (end - start) / ms;		/* cycles per millisecond */
}

/* --- The real-time clock -------------------------------------------------- */

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static u8 cmos_read(u8 reg)
{
	/* The high bit of the address port is the non-maskable interrupt mask,
	 * and clearing it while reading the clock is the conventional
	 * behaviour -- leaving it set would disable NMIs for good. */
	outb(CMOS_ADDR, (u8)(reg & 0x7F));
	return inb(CMOS_DATA);
}

static u8 from_bcd(u8 v)
{
	return (u8)((v & 0x0F) + ((v >> 4) * 10));
}

/* Days from 1970-01-01 to the first of the given month, without a table and
 * without floating point. The leap year rule is the full one: every fourth
 * year, except centuries, except every fourth century. */
static u64 days_from_civil(unsigned year, unsigned month, unsigned day)
{
	/* Shift the year to start in March, so the leap day becomes the last day
	 * of the year and the month lengths form a repeating pattern. Howard
	 * Hinnant's arrangement, exact for every date the calendar defines.
	 *
	 * Signed throughout, deliberately. The published form handles years
	 * before the epoch, and written with unsigned types the negative branch
	 * is unreachable -- which the compiler correctly points out. Keeping the
	 * signed arithmetic keeps the algorithm the one that was proved rather
	 * than a version of it that happens to work for the years we expect. */
	int y = (int)year - (month <= 2);
	int era = (y >= 0 ? y : y - 399) / 400;
	int yoe = y - era * 400;
	int mp = (int)month + (month > 2 ? -3 : 9);
	int doy = (153 * mp + 2) / 5 + (int)day - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return (u64)((i64)era * 146097 + (i64)doe - 719468);
}

u64 arch_wall_ns(void)
{
	unsigned sec, min, hour, day, month, year;
	u8 status_b;

	/* Bit 7 of register A is set while the clock is mid-update, and reading
	 * during an update gives a time that is partly old and partly new --
	 * which at midnight on the last day of a month is a date that never
	 * existed.
	 *
	 * **This loop could not end on a machine with no CMOS.** An x86 port
	 * with nothing behind it reads as 0xFF, and 0xFF has bit 7 set, so the
	 * condition is permanently true -- not usually true, not true on a
	 * slow machine: true by construction, for ever. Legacy RTCs are
	 * disappearing from laptops and the FADT has a flag for their absence.
	 *
	 * The bound is a count rather than a deadline because the clock this
	 * would consult is the one being read. An update takes under two
	 * milliseconds by specification and each pass here is two port reads,
	 * so a hundred thousand passes is orders of magnitude more than enough
	 * and still finite.
	 *
	 * Zero is returned on giving up, and `time_wall_ns` already knows what
	 * to do with it: fall back to the time the firmware gave us before its
	 * mappings went away. That path was built for exactly this machine and
	 * had no way to be reached. */
	{
		unsigned spins = 0;

		while (cmos_read(0x0A) & 0x80) {
			if (++spins > 100000)
				return 0;
		}
	}

	sec   = cmos_read(0x00);
	min   = cmos_read(0x02);
	hour  = cmos_read(0x04);
	day   = cmos_read(0x07);
	month = cmos_read(0x08);
	year  = cmos_read(0x09);

	status_b = cmos_read(0x0B);

	/* Bit 2 clear means the values are binary-coded decimal: 0x59 means
	 * fifty-nine, not eighty-nine. Machines differ, so it is asked. */
	if (!(status_b & 0x04)) {
		sec   = from_bcd((u8)sec);
		min   = from_bcd((u8)min);
		hour  = from_bcd((u8)(hour & 0x7F));
		day   = from_bcd((u8)day);
		month = from_bcd((u8)month);
		year  = from_bcd((u8)year);
	}

	/* Two digits of year, and no century register that can be relied on.
	 * The convention every PC uses: below 70 is the 2000s. */
	year += (year < 70) ? 2000 : 1900;

	if (month < 1 || month > 12 || day < 1 || day > 31)
		return 0;

	return (days_from_civil(year, month, day) * 86400ULL
		+ hour * 3600ULL + min * 60ULL + sec) * 1000000000ULL;
}

/* --- The contract --------------------------------------------------------- */

u64 arch_monotonic_ns(void)
{
	u64 elapsed = rdtsc() - tsc_at_boot;

	/* **Never a clock that has stopped.**
	 *
	 * This returned zero when the counter was uncalibrated, which is honest
	 * about the rate and catastrophic about everything else: a deadline is
	 * built as `now + n` and tested as `now < deadline`, so a `now` that is
	 * always zero is a wait that never ends. There are fifty-seven such
	 * deadlines in this kernel and every one of them became unbounded on
	 * the first machine that had no PIT.
	 *
	 * So the raw counter is reported as though it ran at one gigahertz.
	 * That rate is correct on no processor and within a small factor on
	 * every one, which is exactly the right trade here: a timeout that
	 * fires at the wrong moment is a driver reporting a fault, and a
	 * timeout that never fires is a machine that stops with nothing on the
	 * screen.
	 *
	 * It is not passed off as a measurement. `time_is_calibrated()` answers
	 * false and the boot report says so on the line above. */
	if (!tsc_khz)
		return elapsed;

	/* Multiply before dividing, and in a widened form, so that a machine
	 * that has been up for an hour does not overflow: at 3GHz an hour is
	 * about 10^13 cycles, and multiplying that by a million would not fit.
	 * Dividing to microseconds first keeps every intermediate in range. */
	return (elapsed / tsc_khz) * 1000000ULL
	     + ((elapsed % tsc_khz) * 1000000ULL) / tsc_khz;
}

/* Called from the interrupt stub for vector 32. */
void x86_timer_interrupt(void)
{
	bool preempt;

	time_tick();
	preempt = sched_tick();

	/* The controller is told *before* the switch, not after. A switch does
	 * not return here -- it returns on another thread's stack, and the
	 * acknowledgement would never happen. The controller would then send no
	 * further interrupts, and the machine would freeze on the first
	 * preemption with everything looking correct. */
	x86_irq_ack(0);

	if (preempt)
		sched_switch();
}

/* The same tick, from this processor's own APIC rather than from the one 8254.
 *
 * Deliberately not x86_timer_interrupt with a flag. Two things differ and both
 * of them are the kind that fail silently: the acknowledgement goes to the APIC
 * and must *not* go to the 8259 -- telling that chip about an interrupt it did
 * not send unbalances it, and it stops delivering the ones it did -- and the
 * global tick counter is advanced by one processor rather than by all of them.
 *
 * The end-of-interrupt is before the switch, for the reason written out in
 * x86_timer_interrupt: a switch does not return here. */
void x86_apic_timer_interrupt(void)
{
	bool preempt;

	time_tick();
	preempt = sched_tick();

	x86_apic_eoi();

	if (preempt)
		sched_switch();
}

void arch_time_init(void)
{
	struct cpu_caps caps;

	arch_cpu_caps(&caps);

	/* **Asked, then measured.** The processor's own answer is exact and
	 * costs one instruction; the measurement against the PIT is what older
	 * parts need and what a machine with no 8254 cannot provide. Doing them
	 * in this order means the machine that has no PIT never depends on one.
	 */
	tsc_khz = tsc_khz_from_cpuid();
	tsc_from_cpuid = tsc_khz != 0;

	if (!tsc_khz)
		tsc_khz = calibrate_tsc();

	tsc_at_boot = rdtsc();

	if (!caps.invariant_timer)
		kputs("  note: this CPU's counter is not invariant, so the "
		      "monotonic clock drifts with clock speed\n");

	pic_remap();
	pit_start_tick();

	__asm__ volatile("sti");
}

void arch_time_print_source(void)
{
	if (!tsc_khz) {
		kprintf("  counter      : UNCALIBRATED -- neither CPUID nor the "
			"PIT would say, so elapsed cycles are reported as "
			"nanoseconds and every timeout here is approximate\n");
		return;
	}

	if (!tsc_from_cpuid) {
		kprintf("  counter      : %lu MHz time stamp counter, measured "
			"against the PIT\n", tsc_khz / 1000);
		return;
	}

	/* **The working, not just the answer**, because nothing has been able
	 * to test this branch: no QEMU processor model exposes leaf 15h with
	 * usable values, so every verification run takes the PIT path instead.
	 * A wrong ratio here produces a clock that is confidently wrong, which
	 * is the one failure a plausible number hides. Printing the inputs
	 * means the first machine to take the branch proves it. */
	if (tsc_ratio_num)
		kprintf("  counter      : %lu MHz time stamp counter, from "
			"CPUID 15h (%lu Hz crystal x %lu / %lu) -- UNTESTED "
			"until a machine reports it\n",
			tsc_khz / 1000, (unsigned long)tsc_crystal_hz,
			(unsigned long)tsc_ratio_num,
			(unsigned long)tsc_ratio_den);
	else
		kprintf("  counter      : %lu MHz time stamp counter, from "
			"CPUID 16h (%lu MHz base) -- UNTESTED until a machine "
			"reports it\n",
			tsc_khz / 1000, (unsigned long)tsc_base_mhz);
}
