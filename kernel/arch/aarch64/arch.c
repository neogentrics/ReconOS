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

static inline void mmio_write32(u64 base, u64 off, u32 value)
{
	*(volatile u32 *)(base + off) = value;
}

static inline u32 mmio_read32(u64 base, u64 off)
{
	return *(volatile u32 *)(base + off);
}

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
	while (mmio_read32(PL011_BASE, PL011_FR) & PL011_FR_TXFF)
		;
	mmio_write32(PL011_BASE, PL011_DR, (u32)(unsigned char)c);
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

unsigned arch_cpu_id(void)
{
	unsigned id = arch_cpu_id_real();

	/* Clamped rather than trusted. A machine whose processors are numbered
	 * beyond what this kernel can hold would otherwise index past the
	 * per-processor array -- and the failure would be a corrupted neighbour
	 * rather than an error. smp_init() reports the ones it dropped. */
	return (id < MAX_CPUS) ? id : 0;
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
