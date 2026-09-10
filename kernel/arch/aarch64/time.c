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
#include <recon/kernel/boot.h>
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

/* --- Two generations of the same controller ------------------------------
 *
 * Everything above is GICv2, and GICv2 stops at eight processors. Above that a
 * machine has a GICv3, whose distributor looks similar and whose CPU interface
 * is somewhere else entirely: not memory at all, but system registers.
 *
 * This kernel spoke only v2, at a hardcoded address, and on a machine with nine
 * or more processors that address is not a CPU interface. The first write to it
 * took an external abort and the machine panicked before finishing boot. It was
 * never seen because the verification rig stopped at eight -- the largest
 * machine QEMU's `virt` board still gives a GICv2 for. (BG-124.)
 *
 * --- What changes, and what does not ---
 *
 *   the distributor    same registers, plus a bit that must be set for the
 *                      newer routing model
 *   the CPU interface  system registers instead of MMIO
 *   private interrupts the timer's interrupt is per-processor, and on v3 those
 *                      live in a REDISTRIBUTOR frame of their own rather than
 *                      in the distributor
 *
 * The last is the one that catches people: a v3 timer configured in the
 * distributor is configured perfectly and never fires, because the distributor
 * does not own interrupt 30 any more.
 */
#define GICD_PIDR2	  0xFFE8	/* [7:4] is the architecture revision */

#define GICD_CTLR_ARE_NS  (1u << 4)	/* affinity routing, which v3 requires */
#define GICD_CTLR_GRP1NS  (1u << 1)

/* A redistributor is two 64KiB frames: the control frame, then the one holding
 * the per-processor interrupts. One pair per processor, laid out end to end. */
#define GICR_STRIDE	  0x20000
#define GICR_CTLR	  0x0000
#define GICR_TYPER	  0x0008	/* [63:32] affinity, [4] last frame */
#define GICR_WAKER	  0x0014
#define GICR_SGI_BASE	  0x10000
#define GICR_IGROUPR0	  (GICR_SGI_BASE + 0x0080)
#define GICR_ISENABLER0	  (GICR_SGI_BASE + 0x0100)
#define GICR_IPRIORITYR	  (GICR_SGI_BASE + 0x0400)

#define GICR_WAKER_SLEEP  (1u << 1)	/* this processor is asleep */
#define GICR_WAKER_ASLEEP (1u << 2)	/* the redistributor agrees */

/* Which one this is. Read once, because the answer cannot change while the
 * machine is running and every processor needs it. */
static unsigned gic_version;

/* Which generation the interrupt controller turned out to be, for the portable
 * side to assert on. Zero means none was found -- which is not a machine that
 * boots, and is exactly the thing worth checking rather than assuming. */
unsigned aarch64_gic_generation(void)
{
	return gic_version;
}

/* The CPU interface, on v3, is reached through system registers rather than
 * through memory. Named by their encodings because the assembler in use does
 * not know the newer mnemonics on every target. */
#define ICC_SRE_EL1	"S3_0_C12_C12_5"
#define ICC_PMR_EL1	"S3_0_C4_C6_0"
#define ICC_IGRPEN1_EL1	"S3_0_C12_C12_7"
#define ICC_IAR1_EL1	"S3_0_C12_C12_0"
#define ICC_EOIR1_EL1	"S3_0_C12_C12_1"

#define sysreg_write(name, v) \
	__asm__ volatile("msr " name ", %0" :: "r" ((u64)(v)) : "memory")

#define sysreg_read(name) ({ \
	u64 _v; \
	__asm__ volatile("mrs %0, " name : "=r" (_v) :: "memory"); \
	_v; })

/* --- How the generation is discovered, and how it is not -----------------
 *
 * The obvious way is to read the distributor's peripheral identification
 * register and look at the revision field. That was the first attempt here, and
 * it does not work, for a reason that only appears on the machine it breaks:
 *
 *   GICv3 puts that register at offset 0xFFE8.
 *   GICv2 puts it at 0xFE8.
 *
 * So a kernel that reads 0xFFE8 to find out which one it has is reading an
 * offset that does not exist on half the machines it is asking about. On QEMU's
 * virt board with eight processors or fewer, that read took an external abort
 * and the kernel panicked -- while machines with *nine or more* worked
 * perfectly. Fixing the larger machines had broken every smaller one, and the
 * verification rig would have caught it in the same run.
 *
 * The device tree already says. `arm,gic-v3` on the interrupt controller node
 * is the machine describing itself, it cannot fault, and it is the same source
 * the memory map and the PCI window already come from.
 *
 * A machine with no device tree -- the UEFI path, which describes itself with
 * ACPI -- falls back to v2, which is what every such machine here has. Written
 * down because it is an assumption rather than a discovery, and the day it is
 * wrong the symptom will be this exact fault at this exact address.
 *
 * The ACPI MADT says too, in its GICD structure, and the kernel already walks
 * ACPI tables. Filed as issue #282 rather than left here, because a note about
 * a wrong assumption is least useful in the file that holds it.
 */
static bool gic_saw_v3;

static void gic_note_v3(u64 base, u64 size)
{
	(void)base;
	(void)size;
	gic_saw_v3 = true;
}

static unsigned gic_detect(void)
{
	/* What boot.S recorded, which is a pointer to a blob whether or not that
	 * blob turned out to parse. `boot_info()->dtb` would do equally well;
	 * this is one step closer to the firmware. */
	u64 dtb = arch_dtb_pointer;

	if (!dtb)
		return 2;

	gic_saw_v3 = false;
	fdt_each_compatible(dtb, "arm,gic-v3", gic_note_v3);

	return gic_saw_v3 ? 3 : 2;
}

/* Printed at boot, because this decision is made once, silently, and being
 * wrong about it does not produce a message saying so -- it produces an
 * external abort at an address that means nothing without knowing which
 * generation the kernel thought it had. It cost a full debugging session to
 * learn that. The line is cheap. */
static void gic_announce(unsigned gen)
{
	kprintf("  interrupt   : GICv%u\n", gen);
}

/* This processor's redistributor frame.
 *
 * Found by affinity rather than by index: nothing guarantees that the frames
 * are in the same order as the processors, and using the wrong frame would
 * enable the timer on somebody else's processor -- which looks like a machine
 * where one processor never gets a tick and another gets two.
 */
static u64 gicr_for_this_cpu(void)
{
	u64 mpidr;
	u32 want;
	u64 frame = GICR_BASE;
	unsigned guard;

	__asm__ volatile("mrs %0, mpidr_el1" : "=r" (mpidr));

	/* Aff3..Aff0 packed the way GICR_TYPER packs them. */
	want = (u32)((mpidr & 0xFFFFFF) | ((mpidr >> 32) & 0xFF) << 24);

	/* Bounded by what is mapped, not by an arbitrary number. A frame whose
	 * "last" bit never arrives would otherwise walk off the end of the
	 * mapping and fault while looking for the thing that reports faults. */
	for (guard = 0; guard < GICR_FRAMES_MAPPED; guard++) {
		u32 lo = mmio_r32(frame, GICR_TYPER);
		u32 hi = mmio_r32(frame, GICR_TYPER + 4);

		if (hi == want)
			return frame;

		if (lo & (1u << 4))
			break;			/* that was the last one */

		frame += GICR_STRIDE;
	}

	/* No frame claims this processor. Returning the first is wrong; saying
	 * so and returning zero lets the caller decline to arm a timer that
	 * would belong to somebody else. */
	return 0;
}

/* Brings this processor's redistributor out of sleep, which it powers up in.
 * A redistributor left asleep forwards nothing, and the timer that depends on
 * it is configured perfectly and silent. */
static bool gicr_wake(u64 frame)
{
	u32 w = mmio_r32(frame, GICR_WAKER);
	unsigned guard;

	mmio_w32(frame, GICR_WAKER, w & ~GICR_WAKER_SLEEP);

	for (guard = 0; guard < 100000; guard++)
		if (!(mmio_r32(frame, GICR_WAKER) & GICR_WAKER_ASLEEP))
			return true;

	return false;
}

/* The EL1 physical timer's interrupt. Private to each CPU -- a "private
 * peripheral interrupt" -- and 30 by architectural convention rather than by
 * discovery, which is one of the few ARM numbers that genuinely is fixed. */
#define TIMER_IRQ 30

static void arm_timer(void);

/* Finished with an interrupt, on whichever generation this is. */
static void gic_eoi(u32 iar)
{
	if (gic_version >= 3)
		sysreg_write(ICC_EOIR1_EL1, iar);
	else
		mmio_w32(GICC_BASE, GICC_EOIR, iar);
}

/* The parts of the interrupt controller that are per-processor.
 *
 * The distributor is shared and is configured once. The CPU interface and the
 * timer's private interrupt are *banked* -- each processor has its own copy at
 * the same address -- so every processor must enable its own. A secondary that
 * skips this is online, healthy, and permanently uninterruptible. */
void aarch64_gic_cpu_init(void)
{
	if (!gic_version)
		gic_version = gic_detect();

	if (gic_version >= 3) {
		u64 frame = gicr_for_this_cpu();

		if (!frame) {
			kputs("  gic: no redistributor claims this processor; "
			      "its timer is left disarmed\n");
			return;
		}

		if (!gicr_wake(frame)) {
			kputs("  gic: this processor's redistributor would not "
			      "wake; its timer is left disarmed\n");
			return;
		}

		/* The timer's interrupt lives here on v3, not in the
		 * distributor. Group 1 non-secure, which is the group the
		 * interface below enables. */
		{
			u32 g = mmio_r32(frame, GICR_IGROUPR0);
			u64 off = GICR_IPRIORITYR + (TIMER_IRQ & ~3u);
			u32 v = mmio_r32(frame, off);
			unsigned shift = (TIMER_IRQ & 3) * 8;

			mmio_w32(frame, GICR_IGROUPR0, g | (1u << TIMER_IRQ));

			v &= ~(0xFFu << shift);
			v |= (0xA0u << shift);
			mmio_w32(frame, off, v);

			mmio_w32(frame, GICR_ISENABLER0, 1u << TIMER_IRQ);
		}

		/* And the CPU interface, which is registers. SRE first: until
		 * it is set the others do not exist. */
		sysreg_write(ICC_SRE_EL1, sysreg_read(ICC_SRE_EL1) | 1u);
		__asm__ volatile("isb");

		sysreg_write(ICC_PMR_EL1, 0xF0);
		sysreg_write(ICC_IGRPEN1_EL1, 1);
		__asm__ volatile("isb");
		return;
	}

	mmio_w32(GICC_BASE, GICC_PMR, 0xF0);
	mmio_w32(GICC_BASE, GICC_CTLR, 1);

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

/* This processor's own timer, armed and unmasked. Also per-processor: the
 * generic timer's registers are banked exactly like the controller's. */
void aarch64_timer_cpu_init(void)
{
	arm_timer();
	__asm__ volatile("msr daifclr, #2");
}

static void gic_init(void)
{
	gic_version = gic_detect();
	gic_announce(gic_version);

	if (gic_version >= 3) {
		/* Affinity routing on, and group 1 enabled. Without the routing
		 * bit a v3 distributor behaves as though it were a v2 and the
		 * redistributors below are never consulted. */
		u32 c = mmio_r32(GICD_BASE, GICD_CTLR);

		mmio_w32(GICD_BASE, GICD_CTLR,
			 c | GICD_CTLR_ARE_NS | GICD_CTLR_GRP1NS);

		/* Everything else about the timer is per-processor on v3, and
		 * aarch64_gic_cpu_init does it -- including for this one. */
		aarch64_gic_cpu_init();
		return;
	}

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
	/* Where the interrupt number comes from depends on the generation. On
	 * v3 it is a system register, and the number is wider -- 24 bits rather
	 * than 10 -- though nothing this kernel handles needs the extra bits.
	 */
	u32 iar = (gic_version >= 3) ? (u32)sysreg_read(ICC_IAR1_EL1)
				     : mmio_r32(GICC_BASE, GICC_IAR);
	u32 id = iar & 0xFFFFFF;

	/* 1023 means "spurious": the interrupt went away before it was
	 * acknowledged. Not an error, and must not be acknowledged. */
	/* 1023 means "spurious" on both generations: the interrupt went away
	 * before it was acknowledged. Not an error, and must not be
	 * acknowledged. */
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
		gic_eoi(iar);

		if (preempt)
			sched_switch();
		return;
	}

	gic_eoi(iar);
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
	aarch64_gic_cpu_init();

	/* Unmask IRQs last. Everything up to here ran with them masked, which is
	 * why the order matters: an interrupt arriving before the controller is
	 * configured is an interrupt nothing can identify. */
	aarch64_timer_cpu_init();
}

void aarch64_time_print_source(void)
{
	kprintf("  counter      : %lu MHz generic timer, fixed by the architecture\n",
		timer_hz / 1000000);
}
