#include <recon/kernel/arch.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/kstring.h>

#include "aarch64.h"

/* Set by boot.S from x0. The device tree is the only description of this
 * machine that exists before any driver runs. */
u64 arch_dtb_pointer;

/* --- PL011 UART -------------------------------------------------------- */

/* QEMU's `virt` machine puts the first PL011 here, and so do a great many
 * ARM boards, because the address came from the same reference design. It is
 * hardcoded for now on purpose: reading it out of the device tree requires a
 * device tree parser, and a device tree parser that cannot print why it failed
 * is a bad first thing to write. This address is the bootstrap that makes
 * writing the parser possible. */
#define PL011_BASE 0x09000000UL

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

void arch_cpu_identify(char *buf, size_t len)
{
	u64 midr;
	unsigned implementer, part;
	const char *vendor;

	__asm__ volatile("mrs %0, midr_el1" : "=r"(midr));

	implementer = (unsigned)((midr >> 24) & 0xff);
	part        = (unsigned)((midr >> 4) & 0xfff);

	switch (implementer) {
	case 0x41: vendor = "ARM";       break;
	case 0x42: vendor = "Broadcom";  break;
	case 0x43: vendor = "Cavium";    break;
	case 0x46: vendor = "Fujitsu";   break;
	case 0x48: vendor = "HiSilicon"; break;
	case 0x4e: vendor = "NVIDIA";    break;
	case 0x51: vendor = "Qualcomm";  break;
	case 0x61: vendor = "Apple";     break;
	case 0xc0: vendor = "Ampere";    break;
	default:   vendor = "unknown";   break;
	}

	size_t n = kstrlcpy(buf, vendor, len);

	/* Part number in hex, appended by hand -- kprintf writes to the console
	 * and this function has to fill a buffer. */
	static const char hex[] = "0123456789abcdef";
	const char *tail = " part 0x";
	for (const char *p = tail; *p && n + 1 < len; p++)
		buf[n++] = *p;
	for (int shift = 8; shift >= 0 && n + 1 < len; shift -= 4)
		buf[n++] = hex[(part >> shift) & 0xf];
	if (n < len)
		buf[n] = '\0';
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
