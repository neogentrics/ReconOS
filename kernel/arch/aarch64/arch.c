#include <recon/kernel/console.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/smbios.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/kstring.h>

#include "aarch64.h"

#include <recon/kernel/smp.h>

/* Set by boot.S from x0. The device tree is the only description of this
 * machine that exists before any driver runs. */
u64 arch_dtb_pointer;

/* --- PL011 UART -------------------------------------------------------- */

#define PL011_DR    0x00 /* data */
#define PL011_FR    0x18 /* flags */
#define PL011_FR_TXFF (1u << 5) /* transmit FIFO full */

/* Zero until vm_init replaces the map boot.S built. See aarch64.h. */
u64 aarch64_device_offset;

/* --- Contract ---------------------------------------------------------- */

const char *arch_name(void)
{
	return "aarch64";
}

void arch_early_init(void)
{
	/* The UART is already configured by the firmware at the baud rate the
	 * user's terminal is watching, and reprogramming it before we can print
	 * means a mistake here is silent. Left alone deliberately until there is
	 * a reason to own it. */

	/* Our own loader, if that is what started us. It has already translated
	 * the firmware memory map and found the framebuffer, so there is
	 * nothing left here to read out of the device tree. */
	if (reconboot_parse((paddr_t)reconboot_handoff)) {
		boot_finish_regions();
		return;
	}

	if (!fdt_parse(arch_dtb_pointer)) {
		/* Without a device tree this architecture has no other way to
		 * learn what memory exists -- there is no equivalent of E820 to
		 * fall back on. Report the absence rather than assume a size. */
		boot_info_reset("none", BOOT_FIRMWARE_UNKNOWN);

		/* Record whatever we were handed even though it did not parse.
		 * A zero here means the firmware passed no device tree at all;
		 * a non-zero one means it passed something that was not a blob,
		 * and those are different faults with different fixes. */
		boot_info()->dtb = (paddr_t)arch_dtb_pointer;
	}

	boot_finish_regions();
}

void arch_console_putc(char c)
{
	while (mmio_r32(PL011_BASE, PL011_FR) & PL011_FR_TXFF)
		;
	mmio_w32(PL011_BASE, PL011_DR, (u32)(unsigned char)c);
}


void arch_halt(void)
{
	__asm__ volatile("msr daifset, #0xf"); /* mask every interrupt class */
	for (;;)
		__asm__ volatile("wfi");
}

void arch_wait_for_interrupt(void)
{
	__asm__ volatile("wfi");
}

/* --- Processors and interrupts -------------------------------------------- */

void arch_irq_route_init(void)
{
	/* Nothing to move.
	  *
	  * The GIC is not two chips with one of them cascaded into the other and
	  * a single wire out. Its distributor already decides which processor
	  * each interrupt goes to, and it has done since the controller was
	  * brought up -- so the thing arch_irq_route_init exists to arrange on
	  * x86_64 is arranged here by having used the right controller from the
	  * start. */
}

bool arch_irq_self_test(void)
{
	unsigned gen = aarch64_gic_generation();

	/* There is no message-signalled interrupt to check here, and saying so
	  * by returning true would be a test that cannot fail -- which this
	  * project has been bitten by twice and does not do.
	  *
	  * x86_64 needs a check because an MSI is an *encoding*: an address and
	  * a data word this kernel composes, which can be composed wrongly and
	  * then silently delivers nothing. The GIC has no equivalent -- an
	  * interrupt reaches a processor because the distributor was told to
	  * send it there, not because a message was built correctly.
	  *
	  * So what is asserted is the thing that stands in the same place: that
	  * a controller was actually identified. Zero here is a machine with no
	  * interrupt controller at all, which would take no timer interrupt and
	  * schedule nothing -- and gic_detect can return it, so this is a
	  * question with two possible answers. */
	if (gen != 2 && gen != 3) {
		kprintf("  gic: no interrupt controller was identified (generation %u)\n", gen);
		return false;
	}

	return true;
}

void arch_irq_print_summary(void)
{
	kputs("  routing      : GIC distributor, which can send an interrupt "
	      "to any processor\n");
}

unsigned arch_cpu_id(void)
{
	u64 id;

	/* The kernel's own index, put here by boot.S before this processor ran
	 * any C. It is deliberately NOT derived from MPIDR.
	 *
	 * This used to return `mpidr & 0xFF`, which is affinity level 0 -- the
	 * processor within its cluster. Every machine this kernel had run on had
	 * one cluster, so that number was unique; on a two-socket board it is
	 * not, and processor 0 of cluster 0 and processor 0 of cluster 1 would
	 * index the same entry of every per-processor array there is: the same
	 * task state, the same current thread, the same active address space,
	 * the same idle thread. Nothing would fail until they touched it at the
	 * same moment.
	 *
	 * Folding the higher affinity fields in would have made the value unique
	 * and would not have made it an index -- MPIDR values are sparse, and a
	 * second socket may start at a large affinity number. So the identity is
	 * assigned by discovery, dense by construction, and each processor
	 * carries its own. */
	__asm__ volatile("mrs %0, tpidr_el1" : "=r"(id));

	/* Clamped rather than trusted, because the caller is about to index an
	 * array with it. smp_init() reports the processors it dropped. */
	return (id < MAX_CPUS) ? (unsigned)id : 0;
}

u64 arch_irq_save(void)
{
	u64 flags;

	/* DAIF holds the four exception masks. Saving all of them and putting
	 * them back is what makes nesting safe. */
	__asm__ volatile("mrs %0, daif; msr daifset, #2" : "=r"(flags) : : "memory");
	return flags;
}

void arch_irq_restore(u64 flags)
{
	__asm__ volatile("msr daif, %0" : : "r"(flags) : "memory");
}

bool arch_irqs_enabled(void)
{
	u64 flags;

	__asm__ volatile("mrs %0, daif" : "=r"(flags));
	/* Bit 7 is the IRQ mask, and it is a *mask*: set means disabled. */
	return (flags & (1ULL << 7)) == 0;
}

void arch_cpu_relax(void)
{
	__asm__ volatile("yield");
}

/* --- Finding the SMBIOS anchor -------------------------------------------
 *
 * There is nowhere to look. The scan the other architecture uses is of a
 * region that exists because the IBM PC put firmware there in 1981, and an ARM
 * machine has no equivalent -- the tables come from a UEFI configuration table
 * or they do not exist.
 *
 * Reported as absent rather than guessed at. A machine that genuinely has no
 * SMBIOS and a machine whose tables this kernel cannot reach both say "no
 * tables", which is the honest answer for both until the loader carries the
 * address through the handoff. */
paddr_t arch_smbios_anchor(void)
{
	return 0;
}

/* --- The vector unit ------------------------------------------------------
 *
 * On this architecture floating point and NEON are the same unit and are
 * turned on by one field: CPACR_EL1.FPEN. Firmware usually leaves it enabled
 * for EL1 and *not* for EL0, and "usually" is not a thing to rely on -- a
 * machine where it is off makes every floating-point instruction an
 * undefined-instruction trap, which reads as a corrupt binary rather than as a
 * disabled unit.
 *
 * Both bits are set, so the unit works at EL1 and at EL0. The state is saved
 * per thread below, which is the promise that has to accompany turning it on.
 *
 * SVE is deliberately not enabled: its register width is a property of the
 * machine, so its state does not fit a fixed area, and enabling it without
 * saving it is the corruption this pairing exists to prevent. */
/* EM_AARCH64, fixed by the ABI. A number rather than a header constant because
 * there is no ELF header to include here and inventing one for a single value
 * is more code than the value. */
unsigned arch_elf_machine(void)
{
	return 183;
}

void arch_vector_enable(void)
{
	u64 cpacr;

	__asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
	cpacr |= (3ull << 20);		/* FPEN: no trapping at either level */
	__asm__ volatile("msr cpacr_el1, %0" : : "r"(cpacr));

	/* The write has to be in effect before the next instruction that might
	 * use the unit; a system register write is not otherwise ordered
	 * against instruction fetch. */
	__asm__ volatile("isb");
}

/* Thirty-two 128-bit registers, then the two status words. Written out rather
 * than looped because the register number in these instructions is part of the
 * encoding and cannot come from a variable. */
void arch_vector_save(void *area)
{
	__asm__ volatile(
		"stp q0,  q1,  [%0, #0]\n\t"
		"stp q2,  q3,  [%0, #32]\n\t"
		"stp q4,  q5,  [%0, #64]\n\t"
		"stp q6,  q7,  [%0, #96]\n\t"
		"stp q8,  q9,  [%0, #128]\n\t"
		"stp q10,  q11,  [%0, #160]\n\t"
		"stp q12,  q13,  [%0, #192]\n\t"
		"stp q14,  q15,  [%0, #224]\n\t"
		"stp q16,  q17,  [%0, #256]\n\t"
		"stp q18,  q19,  [%0, #288]\n\t"
		"stp q20,  q21,  [%0, #320]\n\t"
		"stp q22,  q23,  [%0, #352]\n\t"
		"stp q24,  q25,  [%0, #384]\n\t"
		"stp q26,  q27,  [%0, #416]\n\t"
		"stp q28,  q29,  [%0, #448]\n\t"
		"stp q30,  q31,  [%0, #480]\n\t"
		: : "r"(area) : "memory");

	{
		u64 fpsr, fpcr;

		__asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
		__asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));

		((u64 *)area)[64] = fpsr;
		((u64 *)area)[65] = fpcr;
	}
}

void arch_vector_restore(const void *area)
{
	{
		u64 fpsr = ((const u64 *)area)[64];
		u64 fpcr = ((const u64 *)area)[65];

		__asm__ volatile("msr fpsr, %0" : : "r"(fpsr));
		__asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
	}

	__asm__ volatile(
		"ldp q0,  q1,  [%0, #0]\n\t"
		"ldp q2,  q3,  [%0, #32]\n\t"
		"ldp q4,  q5,  [%0, #64]\n\t"
		"ldp q6,  q7,  [%0, #96]\n\t"
		"ldp q8,  q9,  [%0, #128]\n\t"
		"ldp q10,  q11,  [%0, #160]\n\t"
		"ldp q12,  q13,  [%0, #192]\n\t"
		"ldp q14,  q15,  [%0, #224]\n\t"
		"ldp q16,  q17,  [%0, #256]\n\t"
		"ldp q18,  q19,  [%0, #288]\n\t"
		"ldp q20,  q21,  [%0, #320]\n\t"
		"ldp q22,  q23,  [%0, #352]\n\t"
		"ldp q24,  q25,  [%0, #384]\n\t"
		"ldp q26,  q27,  [%0, #416]\n\t"
		"ldp q28,  q29,  [%0, #448]\n\t"
		"ldp q30,  q31,  [%0, #480]\n\t"
		: : "r"(area) : "memory");
}

/* There are no I/O ports on this architecture. An ARM machine is powered off
 * through PSCI, which is a firmware call and a different mechanism entirely --
 * so this refuses rather than writing somewhere that looks like a port. */
bool arch_acpi_write_control(u64 address, u16 value)
{
	(void)address;
	(void)value;
	return false;
}
