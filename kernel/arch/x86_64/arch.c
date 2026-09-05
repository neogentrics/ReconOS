#include <recon/kernel/arch.h>
#include <recon/kernel/kstring.h>

/* Filled in by boot.S from whichever boot protocol was used. */
extern u32 boot_protocol;
extern u32 boot_info_phys;
extern u32 boot_magic;

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

void arch_early_init(void)
{
	uart_init();
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
