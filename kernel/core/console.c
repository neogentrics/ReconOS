#include <recon/kernel/console.h>
#include <recon/kernel/klog.h>
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

/* Whether the panel is being spared the detail. Never affects the serial port
 * or the ring -- see kputc. Not locked: it is written twice in a boot, by the
 * boot processor, outside anything that could race it, and a torn read of a
 * bool would at worst print one extra line. */
static bool screen_quiet;

void console_screen_quiet(bool quiet)
{
	screen_quiet = quiet;
}

void kputc(char c)
{
	/* The console speaks lines; the hardware speaks bytes. A serial terminal
	 * needs the carriage return that a C newline does not carry. */
	if (c == '\n')
		arch_console_putc('\r');
	arch_console_putc(c);

	/* And the ring, which is why this is the only hook it needs:
	 * everything printed anywhere in this kernel arrives here.
	 *
	 * The carriage return above is deliberately not logged. It is
	 * something a serial terminal needs and not something the kernel
	 * said, and a log full of them is a log somebody has to strip
	 * before reading. */
	klog_putc(c);

	/* And the screen, where there is one and where it is wanted.
	 *
	 * This used to read: *both surfaces get everything, rather than one
	 * being chosen -- the rig reads the serial port and a person reads the
	 * screen, and a message that went to only one of them is a message
	 * somebody did not get.* That was right while the screen was the only
	 * way a person could read the report. It is not any more: the report
	 * writes itself to the medium now, and on a machine that boots all the
	 * way a user program paints over the panel before anybody can look.
	 *
	 * **Only the panel is gated.** The serial port above keeps everything,
	 * because the verification rig reads it and a rig that cannot see is a
	 * rig that cannot fail. The ring keeps everything, because the file is
	 * the instrument. And the gate is closed only around the closing
	 * summaries -- everything a driver says while starting, which is where
	 * every refusal is printed, reaches the panel regardless. */
	if (!screen_quiet)
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
static void put_unsigned(u64 value, unsigned base, bool upper,
			 unsigned width, bool zero, bool left)
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

	/* Zero padding goes inside the number, which is why it is done here
	 * and not by the field logic below: `%04x` of 0x0781 is `0781` and not
	 * ` 781`, and the difference is whether somebody can look the thing
	 * up. */
	if (zero)
		while (n < width && n < sizeof(buf))
			buf[n++] = '0';

	/* And a field wider than the number, padded with spaces on whichever
	 * side was asked for. Never truncated: a number cut down to fit a
	 * column is a different number, which is the same rule put_padded
	 * states about names. */
	if (!zero && !left)
		while (n < width--)
			kputc(' ');

	{
		unsigned i = n;

		while (i--)
			kputc(buf[i]);
	}

	if (!zero && left)
		while (n < width--)
			kputc(' ');
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

static void put_signed(i64 value, unsigned width, bool zero, bool left)
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
	/* The sign has already been written, so the field the digits go in is
	 * one narrower than the one asked for. Getting this wrong makes `%04d`
	 * of -7 five characters wide, which is the kind of thing that only
	 * shows up in a column somebody is reading. */
	put_unsigned(magnitude, 10, false,
		     width > 1 ? width - 1 : 0, zero, left);
}

/* --- does the formatter do what the format says? --------------------------
 *
 * There was no way to ask until now. kprintf has no buffer form, so nothing
 * could compare what it produced against what it should have produced -- and
 * a width that was parsed and then discarded survived twenty-three call sites
 * because of it (KF-226).
 *
 * The log ring is the way in. `klog_read` copies the most recent bytes out and
 * everything kprintf writes passes through it, so a line printed between two
 * markers can be read back and compared **character for character**.
 *
 * That last part is the point. The bug this exists for produced
 * `4c3d311-1343-49a6` where `04c3d311-1343-49a6` was meant, which satisfies
 * any check asking whether the output looks like a GUID.
 */
bool console_format_self_test(void)
{
	static const char want[] =
		"[fmt 04 0781 00000ABC    42 42    -007 0]";
	char got[sizeof(want) + 64];
	u32 n;
	const char *at;
	unsigned i;

	kprintf("[fmt %02x %04x %08X %5u %-5u %04d %x]\n",
		0x04u, 0x0781u, 0xABCu, 42u, 42u, -7, 0u);

	n = klog_read(got, (u32)sizeof(got) - 1);
	got[n] = '\0';

	/* The marker is looked for rather than assuming the line is last: the
	 * ring is shared and anything may have printed after it. */
	at = 0;
	for (i = 0; i + sizeof(want) - 1 <= n; i++)
		if (got[i] == '[' && kmemcmp(got + i, "[fmt ", 5) == 0)
			at = got + i;

	if (!at) {
		kputs("  console: the formatted line did not reach the log\n");
		return false;
	}

	if (kmemcmp(at, want, sizeof(want) - 1) != 0) {
		char saw[sizeof(want)];

		kmemcpy(saw, at, sizeof(want) - 1);
		saw[sizeof(want) - 1] = '\0';
		kprintf("  console: formatted %s\n", saw);
		kprintf("  console: wanted    %s\n", want);
		return false;
	}

	return true;
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
		bool zero = false;

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

		/* A leading zero is a flag, not the first digit of the width.
		 * The old loop read it as a digit, which gave the same width by
		 * arithmetic -- `%02x` is width 2 either way -- and lost the
		 * one bit of information that says how to fill the space. */
		if (*p == '0') {
			zero = true;
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
				put_signed(va_arg(ap, i64), width, zero, left);
			else
				put_signed(va_arg(ap, int), width, zero, left);
			break;
		case 'u':
			if (longness)
				put_unsigned(va_arg(ap, u64), 10, false,
					     width, zero, left);
			else
				put_unsigned(va_arg(ap, unsigned), 10, false,
					     width, zero, left);
			break;
		/* Both cases of hexadecimal.
		 *
		 * `put_unsigned` has taken an `upper` argument since it was
		 * written and every caller passed false, so `%X` was an
		 * unsupported conversion -- and `core/user.c` uses `%08X` to
		 * report a framebuffer that did not arrive where it was asked
		 * for. That line has been printing
		 *
		 *   0x%<unsupported conversion 'X'; the rest of this line is
		 *   not printed>
		 *
		 * instead of the address, on the one path a person reads when
		 * something has already gone wrong. Found by the format
		 * self-test on its first run. */
		case 'x':
		case 'X':
			if (longness)
				put_unsigned(va_arg(ap, u64), 16, *p == 'X',
					     width, zero, left);
			else
				put_unsigned(va_arg(ap, unsigned), 16,
					     *p == 'X', width, zero, left);
			break;
		/* Octal, which exists here for exactly one reason: a file's
		 * permission bits. They are grouped in threes and every person
		 * who has ever read one reads them in octal, so printing 0640
		 * as 416 turns a number somebody can check at a glance into one
		 * they have to convert first. */
		case 'o':
			if (longness)
				put_unsigned(va_arg(ap, u64), 8, false,
					     width, zero, left);
			else
				put_unsigned(va_arg(ap, unsigned), 8, false,
					     width, zero, left);
			break;
		case 'p':
			raw_puts("0x");
			/* Zero-padded to the width of a pointer on this
			 * machine, which is what the fourth argument used to
			 * mean on its own and now needs saying. */
			put_unsigned((u64)(uintptr_t)va_arg(ap, void *), 16,
				     false, sizeof(void *) * 2, true, false);
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
			 * bug someone believes. (KF-129) */
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
