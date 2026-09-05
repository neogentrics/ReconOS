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

static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline u8  inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

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

/* --- The programmable interval timer ------------------------------------- */

#define PIT_CH0   0x40
#define PIT_CH2   0x42
#define PIT_CMD   0x43
#define PIT_GATE2 0x61

#define PIT_HZ 1193182u

static void pit_start_tick(void)
{
	unsigned divisor = PIT_HZ / TIME_TICK_HZ;

	outb(PIT_CMD, 0x36);			/* channel 0, both bytes, square wave */
	outb(PIT_CH0, (u8)(divisor & 0xFF));
	outb(PIT_CH0, (u8)(divisor >> 8));
}

/* Measures how many cycles the counter advances in a known interval.
 *
 * Channel 2 is used rather than channel 0 because channel 2's gate can be
 * driven directly from a port, so the measurement needs no interrupt -- which
 * matters, because this runs before any interrupt handler exists. */
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

	/* Bit 5 of the gate port goes high when channel 2 finishes counting. */
	while (!(inb(PIT_GATE2) & 0x20))
		;

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
	 * existed. */
	while (cmos_read(0x0A) & 0x80)
		;

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

	if (!tsc_khz)
		return 0;

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
	x86_pic_end_of_interrupt(0);

	if (preempt)
		sched_switch();
}

void arch_time_init(void)
{
	struct cpu_caps caps;

	arch_cpu_caps(&caps);

	tsc_khz = calibrate_tsc();
	tsc_at_boot = rdtsc();

	if (!caps.invariant_timer)
		kputs("  note: this CPU's counter is not invariant, so the "
		      "monotonic clock drifts with clock speed\n");

	pic_remap();
	pit_start_tick();

	__asm__ volatile("sti");
}

void x86_time_print_source(void)
{
	kprintf("  counter      : %lu MHz time stamp counter, %s\n",
		tsc_khz / 1000, tsc_khz ? "calibrated against the PIT" : "not calibrated");
}
