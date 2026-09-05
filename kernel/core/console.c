#include <recon/kernel/console.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/kstring.h>

#include <stdarg.h>

void kputc(char c)
{
	/* The console speaks lines; the hardware speaks bytes. A serial terminal
	 * needs the carriage return that a C newline does not carry. */
	if (c == '\n')
		arch_console_putc('\r');
	arch_console_putc(c);
}

void kputs(const char *s)
{
	if (!s)
		s = "(null)";
	while (*s)
		kputc(*s++);
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

void kprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	for (const char *p = fmt; *p; p++) {
		unsigned longness = 0;

		if (*p != '%') {
			kputc(*p);
			continue;
		}

		p++;
		while (*p == 'l') {
			longness++;
			p++;
		}

		switch (*p) {
		case 's':
			kputs(va_arg(ap, const char *));
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
			kputs("0x");
			put_unsigned((u64)(uintptr_t)va_arg(ap, void *), 16, false,
				     sizeof(void *) * 2);
			break;
		case '%':
			kputc('%');
			break;
		case '\0':
			/* Trailing '%' with nothing after it: say so rather than
			 * reading past the end of the format string. */
			kputs("%<truncated>");
			va_end(ap);
			return;
		default:
			/* An unsupported conversion is a bug in the caller. Print it
			 * visibly instead of silently dropping it. */
			kputc('%');
			kputc(*p);
			break;
		}
	}

	va_end(ap);
}
