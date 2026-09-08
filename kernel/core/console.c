#include <recon/kernel/console.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/fbcon.h>
#include <recon/kernel/kstring.h>

#include <recon/kernel/lock.h>

#include <stdarg.h>

/* One processor at a time gets to finish a line.
 *
 * Added the moment a second processor existed, because the first thing four
 * processors did was write over each other mid-word:
 *
 *     [[cpu 3:cp online, iu d says 3,2: ir oqs online, n]
 *
 * which is three messages plaited together. Funny once, and then it is the
 * console you have to read a fault report on.
 *
 * Interrupt-safe, because the timer interrupt prints during a fault and would
 * otherwise deadlock against the code it interrupted. The lock is taken by the
 * whole-string functions rather than by kputc, so a line is atomic rather than
 * a character being atomic -- a character-level lock would let four processors
 * interleave and would cost four times as much doing it. */
static struct spinlock console_lock = SPINLOCK_INIT("console");

void kputc(char c)
{
	/* The console speaks lines; the hardware speaks bytes. A serial terminal
	 * needs the carriage return that a C newline does not carry. */
	if (c == '\n')
		arch_console_putc('\r');
	arch_console_putc(c);

	/* And the screen, where there is one.
	 *
	 * Both surfaces get everything, rather than one being chosen: the rig
	 * reads the serial port and a person reads the screen, and a message
	 * that went to only one of them is a message somebody did not get. It
	 * costs a call that returns immediately on a machine with no
	 * framebuffer, which is every aarch64 boot in the matrix. */
	fbcon_putc(c);
}

/* The whole of the output path, without the lock. Everything that already holds
 * the lock uses this; nothing else should.
 *
 * This exists because kprintf held the lock and then called kputs to print the
 * "0x" before a pointer -- and kputs took the lock again. A spinlock taken twice
 * by the same processor is a processor waiting for itself, and it presented as
 * the kernel stopping mid-word, on the exact line it was printing. */
static void raw_puts(const char *s)
{
	if (!s)
		s = "(null)";
	while (*s)
		kputc(*s++);
}

/* For panic and the fault reporter. They are called when something has already
 * gone wrong, possibly while the console lock is held by the code that went
 * wrong -- so taking it would turn a report into a hang, which is the one
 * outcome worse than the fault. Output may interleave. That is the right trade:
 * a garbled report can be read, a missing one cannot. */
void kputs_unlocked(const char *s)
{
	raw_puts(s);
}

void kputs(const char *s)
{
	u64 flags = spin_lock_irq(&console_lock);

	raw_puts(s);
	spin_unlock_irq(&console_lock, flags);
}

/* Unsigned integer in any base from 2 to 16. Written into the caller's buffer
 * back-to-front, which is the only way to do this without division by a
 * variable being needed twice. */
static void put_unsigned(u64 value, unsigned base, bool upper, unsigned pad)
{
	static const char lower_digits[] = "0123456789abcdef";
	static const char upper_digits[] = "0123456789ABCDEF";
	const char *digits = upper ? upper_digits : lower_digits;
	char buf[64];
	unsigned n = 0;

	if (value == 0) {
		buf[n++] = '0';
	} else {
		while (value && n < sizeof(buf)) {
			buf[n++] = digits[value % base];
			value /= base;
		}
	}

	while (n < pad && n < sizeof(buf))
		buf[n++] = '0';

	while (n--)
		kputc(buf[n]);
}

/* A string in a field of `width`, padded with spaces. Truncating would be the
 * other choice and is the wrong one: a name cut down to fit a column is a
 * different name, and this printer's whole job is to be believed. */
static void put_padded(const char *s, unsigned width, bool left)
{
	size_t n = 0;
	const char *q = s;

	if (!s)
		s = q = "(null)";

	while (q[n])
		n++;

	if (!left)
		while (n < width--)
			kputc(' ');

	raw_puts(s);

	if (left)
		while (n < width--)
			kputc(' ');
}

static void put_signed(i64 value)
{
	u64 magnitude;

	if (value < 0) {
		kputc('-');
		/* Negating the most negative value overflows, so widen through
		 * unsigned rather than negating in signed arithmetic. */
		magnitude = (u64)(-(value + 1)) + 1;
	} else {
		magnitude = (u64)value;
	}
	put_unsigned(magnitude, 10, false, 0);
}

/* The formatter itself, holding no lock.
 *
 * Split out from kprintf when the fault reporter needed to print without
 * locking -- which is exactly the condition the original version of this file
 * named for splitting it: "when a second caller wants vkprintf, that is the
 * moment". */
static void kvprintf_raw(const char *fmt, va_list ap)
{
	for (const char *p = fmt; *p; p++) {
		unsigned longness = 0;

		unsigned width = 0;
		bool left = false;

		if (*p != '%') {
			kputc(*p);
			continue;
		}

		p++;

		/* An optional `-` and a decimal width, which is the whole of
		 * the formatting this kernel needs: every table it prints is
		 * columns of names and numbers, and the alternative is padding
		 * them by hand with spaces in the format string, which is what
		 * was being done and is why %-30s got reached for. */
		if (*p == '-') {
			left = true;
			p++;
		}
		while (*p >= '0' && *p <= '9') {
			width = width * 10 + (unsigned)(*p - '0');
			p++;
		}

		while (*p == 'l') {
			longness++;
			p++;
		}

		switch (*p) {
		case 's':
			put_padded(va_arg(ap, const char *), width, left);
			break;
		case 'c':
			kputc((char)va_arg(ap, int));
			break;
		case 'd':
		case 'i':
			if (longness)
				put_signed(va_arg(ap, i64));
			else
				put_signed(va_arg(ap, int));
			break;
		case 'u':
			if (longness)
				put_unsigned(va_arg(ap, u64), 10, false, 0);
			else
				put_unsigned(va_arg(ap, unsigned), 10, false, 0);
			break;
		case 'x':
			if (longness)
				put_unsigned(va_arg(ap, u64), 16, false, 0);
			else
				put_unsigned(va_arg(ap, unsigned), 16, false, 0);
			break;
		case 'p':
			raw_puts("0x");
			put_unsigned((u64)(uintptr_t)va_arg(ap, void *), 16, false,
				     sizeof(void *) * 2);
			break;
		case '%':
			kputc('%');
			break;
		case '\0':
			/* Trailing '%' with nothing after it: say so rather than
			 * reading past the end of the format string. */
			kputc('%');
			return;
		default:
			/* An unsupported conversion cannot be recovered from, and
			 * this used to try.
			 *
			 * It printed the two characters and carried on -- which
			 * looks like the careful thing and is not, because the
			 * argument was never consumed. Every conversion after it
			 * then read the *previous* caller's argument: a pointer
			 * printed as a number, a length printed as an address.
			 * The output stays perfectly well formed and every value
			 * in it is wrong, which is the worst way for a printer to
			 * fail. It was found reading a real filesystem, where a
			 * directory listing reported a file of 2148777108 bytes
			 * that was a pointer.
			 *
			 * There is no way to skip the argument, because its width
			 * is exactly what is not understood. So: say so, and stop.
			 * Missing output is a bug someone fixes; wrong output is a
			 * bug someone believes. (BG-129) */
			raw_puts("%<unsupported conversion '");
			kputc(*p);
			raw_puts("'; the rest of this line is not printed>\n");
			return;
		}
	}
}

void kprintf(const char *fmt, ...)
{
	va_list ap;
	u64 flags = spin_lock_irq(&console_lock);

	va_start(ap, fmt);
	kvprintf_raw(fmt, ap);
	va_end(ap);

	spin_unlock_irq(&console_lock, flags);
}

/* For the fault reporter. Same reasoning as kputs_unlocked(): a fault can be
 * taken by code that is holding the console lock, and a reporter that waits for
 * that lock turns a fault into a hang. Output may interleave with another
 * processor's -- a garbled report can be read, a missing one cannot. */
void kprintf_unlocked(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	kvprintf_raw(fmt, ap);
	va_end(ap);
}
