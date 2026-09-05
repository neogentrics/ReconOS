/* aarch64 time: a counter, a tick, and the date.
 *
 * ARM is kinder than x86 here in one respect and harsher in another.
 *
 * KINDER: the generic timer is architectural. Every aarch64 CPU has it, it runs
 * at a fixed frequency the CPU will tell you (CNTFRQ_EL0), and it does not
 * change rate with clock speed. There is nothing to calibrate and no
 * "invariant" bit to check -- the equivalent of x86's entire TSC calibration
 * dance is one register read.
 *
 * HARSHER: an interrupt does not arrive because a device raised it. It arrives
 * because the *interrupt controller* was configured to route it, and on ARM the
 * controller is a separate piece of hardware at an address the kernel has to
 * know. So the tick needs a GIC driver before it needs a timer.
 */
#include "aarch64.h"

#include <recon/kernel/arch.h>
#include <recon/kernel/time.h>
#include <recon/kernel/console.h>
#include <recon/kernel/sched.h>

/* --- The generic interrupt controller ------------------------------------
 *
 * GICv2, at the addresses QEMU's `virt` machine uses -- which are also the
 * addresses of the ARM reference design, which is why so many boards agree.
 * Hardcoded for the same reason the UART is: reading them from the device tree
 * needs a parser that can report its own failures, and this is the layer that
 * makes reporting possible.
 *
 * Two halves, and the split is the point. The DISTRIBUTOR is shared by every
 * CPU and decides which interrupts exist and where they go. The CPU INTERFACE
 * is per-CPU and is what a processor reads to find out what it must handle.
 */
#define GICD_CTLR         0x000
#define GICD_ISENABLER    0x100
#define GICD_IPRIORITYR   0x400

#define GICC_CTLR 0x0000
#define GICC_PMR  0x0004
#define GICC_IAR  0x000C	/* read: which interrupt, and acknowledge it */
#define GICC_EOIR 0x0010	/* write: finished with it */

/* The EL1 physical timer's interrupt. Private to each CPU -- a "private
 * peripheral interrupt" -- and 30 by architectural convention rather than by
 * discovery, which is one of the few ARM numbers that genuinely is fixed. */
#define TIMER_IRQ 30

static inline void mmio_w32(u64 base, u64 off, u32 v)
{
	*(volatile u32 *)(base + off) = v;
}

static inline u32 mmio_r32(u64 base, u64 off)
{
	return *(volatile u32 *)(base + off);
}

static void gic_init(void)
{
	/* Priority mask: only interrupts of higher priority than this reach the
	 * CPU. 0xF0 is the lowest useful threshold -- it lets everything
	 * through. A mask of zero, which is the reset value, lets nothing
	 * through, and produces a timer that is configured perfectly and never
	 * fires. */
	mmio_w32(GICC_BASE, GICC_PMR, 0xF0);
	mmio_w32(GICC_BASE, GICC_CTLR, 1);

	mmio_w32(GICD_BASE, GICD_CTLR, 1);

	/* Priority for our interrupt, then enable it. Priorities are one byte
	 * each, so the register index is the interrupt number over four. */
	{
		u64 off = GICD_IPRIORITYR + (TIMER_IRQ & ~3u);
		u32 v = mmio_r32(GICD_BASE, off);
		unsigned shift = (TIMER_IRQ & 3) * 8;

		v &= ~(0xFFu << shift);
		v |= (0xA0u << shift);
		mmio_w32(GICD_BASE, off, v);
	}

	mmio_w32(GICD_BASE, GICD_ISENABLER + (TIMER_IRQ / 32) * 4,
		 1u << (TIMER_IRQ % 32));
}

/* --- The generic timer ---------------------------------------------------- */

static u64 timer_hz;
static u64 count_at_boot;
static u64 tick_interval;

static inline u64 read_count(void)
{
	u64 v;

	/* The barrier matters: without it the counter read can be reordered
	 * against surrounding work, which is a fine way to measure a negative
	 * interval. */
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void arm_timer(void)
{
	/* TVAL is a countdown: writing it says "interrupt me this many ticks
	 * from now". Rearming from TVAL rather than from an absolute compare
	 * value drifts slightly, and that is deliberate for now -- an absolute
	 * deadline needs CVAL and a policy about what to do when the handler was
	 * late, which belongs with the scheduler. */
	__asm__ volatile("msr cntp_tval_el0, %0" : : "r"(tick_interval));
	__asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));	/* enable, unmasked */
}

/* Called from the IRQ path in trap.c. */
void aarch64_irq(void)
{
	u32 iar = mmio_r32(GICC_BASE, GICC_IAR);
	u32 id = iar & 0x3FF;

	/* 1023 means "spurious": the interrupt went away before it was
	 * acknowledged. Not an error, and must not be acknowledged. */
	if (id == 1023)
		return;

	if (id == TIMER_IRQ) {
		bool preempt;

		time_tick();
		arm_timer();
		preempt = sched_tick();

		/* End-of-interrupt before the switch, never after: a switch does
		 * not return here, it returns on another thread's stack, so the
		 * acknowledgement would never happen and the controller would
		 * send nothing further. The machine would freeze on the first
		 * preemption with every part of it looking correct. */
		mmio_w32(GICC_BASE, GICC_EOIR, iar);

		if (preempt)
			sched_switch();
		return;
	}

	mmio_w32(GICC_BASE, GICC_EOIR, iar);
}

u64 arch_monotonic_ns(void)
{
	u64 elapsed = read_count() - count_at_boot;

	if (!timer_hz)
		return 0;

	/* Whole seconds first, then the remainder scaled -- so that neither the
	 * multiplication nor the division can overflow, however long the machine
	 * has been up. The obvious `elapsed * 1000000000 / hz` overflows after
	 * about eighteen seconds at 62.5MHz, which is exactly long enough for
	 * everything to look fine during testing. */
	return (elapsed / timer_hz) * 1000000000ULL
	     + ((elapsed % timer_hz) * 1000000000ULL) / timer_hz;
}

/* --- The date ------------------------------------------------------------
 *
 * A PL031, which is about as simple as hardware gets: one register holding
 * seconds since 1970. No BCD, no update-in-progress flag, no century
 * ambiguity -- everything x86's CMOS clock makes you think about.
 */
#define PL031_DR 0x000

u64 arch_wall_ns(void)
{
	u32 secs = mmio_r32(PL031_BASE, PL031_DR);

	/* Zero means the machine has no clock the kernel can read, which is a
	 * real answer on a board with no battery. */
	if (!secs)
		return 0;

	return (u64)secs * 1000000000ULL;
}

/* --- The contract --------------------------------------------------------- */

void arch_time_init(void)
{
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timer_hz));

	if (!timer_hz) {
		kputs("  note: this CPU does not report its timer frequency, "
		      "so there is no clock\n");
		return;
	}

	count_at_boot = read_count();
	tick_interval = timer_hz / TIME_TICK_HZ;

	gic_init();
	arm_timer();

	/* Unmask IRQs. Everything up to here ran with them masked, which is why
	 * the order matters: an interrupt arriving before the controller is
	 * configured is an interrupt nothing can identify. */
	__asm__ volatile("msr daifclr, #2");
}

void aarch64_time_print_source(void)
{
	kprintf("  counter      : %lu MHz generic timer, fixed by the architecture\n",
		timer_hz / 1000000);
}
