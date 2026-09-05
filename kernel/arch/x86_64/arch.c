#include <recon/kernel/arch.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/kstring.h>

#include "x86_64.h"

/* --- Port I/O ---------------------------------------------------------- */

static inline void outb(u16 port, u8 value)
{
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port)
{
	u8 value;
	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

/* --- 16550 UART -------------------------------------------------------- */

/* COM1. Fixed at this port since the IBM PC, and still where QEMU puts it,
 * which is why serial is the one output that works before anything else does. */
#define COM1 0x3F8

#define UART_DATA        0
#define UART_INT_ENABLE  1
#define UART_FIFO_CTRL   2
#define UART_LINE_CTRL   3
#define UART_MODEM_CTRL  4
#define UART_LINE_STATUS 5

#define UART_LSR_THR_EMPTY (1u << 5)

static void uart_init(void)
{
	outb(COM1 + UART_INT_ENABLE, 0x00); /* no interrupts: nothing services them yet */
	outb(COM1 + UART_LINE_CTRL, 0x80);  /* DLAB: the next two ports are the divisor */
	outb(COM1 + UART_DATA, 0x01);       /* divisor 1 -> 115200 baud */
	outb(COM1 + UART_INT_ENABLE, 0x00);
	outb(COM1 + UART_LINE_CTRL, 0x03);  /* 8 bits, no parity, one stop bit */
	outb(COM1 + UART_FIFO_CTRL, 0xC7);  /* enable and clear FIFOs, 14-byte trigger */
	outb(COM1 + UART_MODEM_CTRL, 0x0B); /* DTR, RTS, OUT2 */
}

/* --- CPUID ------------------------------------------------------------- */

static void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
	__asm__ volatile("cpuid"
			 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			 : "a"(leaf), "c"(0));
}

/* --- Contract ---------------------------------------------------------- */

const char *arch_name(void)
{
	return "x86_64";
}

/* E820's type numbers, which both Multiboot2 and PVH pass through unchanged.
 * Anything not listed is reserved: an unknown type is memory whose owner we do
 * not know, and the safe reading of that is "not ours". */
void x86_add_e820_region(u64 base, u64 len, u32 e820_type)
{
	enum mem_kind kind;

	switch (e820_type) {
	case 1:  kind = MEM_USABLE;       break;
	case 2:  kind = MEM_RESERVED;     break;
	case 3:  kind = MEM_ACPI_RECLAIM; break;
	case 4:  kind = MEM_ACPI_NVS;     break;
	case 5:  kind = MEM_BAD;          break;
	default: kind = MEM_RESERVED;     break;
	}

	boot_add_region((paddr_t)base, len, kind);
}

void arch_early_init(void)
{
	bool ok = false;

	/* The console first, so that a failure to understand the boot protocol
	 * can be reported rather than merely happening. */
	uart_init();

	switch (boot_protocol) {
	case BOOT_PROTOCOL_MULTIBOOT2:
		/* The magic number is the loader's assertion that it really did
		 * follow the specification. Checked, because a wrong value means
		 * the pointer in the other register is not what we think. */
		if (boot_magic == MULTIBOOT2_BOOTLOADER_MAGIC)
			ok = mb2_parse(boot_info_phys);
		break;
	case BOOT_PROTOCOL_PVH:
		ok = pvh_parse(boot_info_phys);
		break;
	default:
		break;
	}

	if (!ok) {
		/* No memory map. Say so, and record nothing rather than invent
		 * something -- a kernel that guesses at what memory exists is a
		 * kernel that will corrupt whatever it guessed wrong about. */
		boot_info_reset("unrecognised", BOOT_FIRMWARE_UNKNOWN);
	}

	boot_finish_regions();
}

void arch_console_putc(char c)
{
	while (!(inb(COM1 + UART_LINE_STATUS) & UART_LSR_THR_EMPTY))
		;
	outb(COM1 + UART_DATA, (u8)c);
}

void arch_cpu_identify(char *buf, size_t len)
{
	u32 a, b, c, d;
	u32 words[12];
	size_t n;

	/* The 48-byte brand string, if the CPU has one: leaves 0x80000002
	 * through 0x80000004, four registers each, already in order. */
	cpuid(0x80000000, &a, &b, &c, &d);
	if (a >= 0x80000004) {
		for (unsigned i = 0; i < 3; i++)
			cpuid(0x80000002 + i, &words[i * 4 + 0], &words[i * 4 + 1],
			      &words[i * 4 + 2], &words[i * 4 + 3]);

		const char *brand = (const char *)words;
		/* Intel pads the brand string with leading spaces. */
		while (*brand == ' ')
			brand++;

		n = 0;
		while (n + 1 < len && n < sizeof(words) && brand[n])
			n++;
		kmemcpy(buf, brand, n);
		buf[n] = '\0';
		return;
	}

	/* No brand string: fall back to the 12-byte vendor id from leaf 0. */
	cpuid(0, &a, &b, &c, &d);
	words[0] = b;
	words[1] = d;
	words[2] = c;

	n = len - 1 < 12 ? len - 1 : 12;
	kmemcpy(buf, words, n);
	buf[n] = '\0';
}

void arch_halt(void)
{
	for (;;)
		__asm__ volatile("cli; hlt");
}

void arch_wait_for_interrupt(void)
{
	__asm__ volatile("hlt");
}
