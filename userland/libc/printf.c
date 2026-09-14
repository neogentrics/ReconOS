/*
 * `snprintf`, which is the single most-called library function in ReconOS.
 *
 * Measured across `src/`: **2,022 `%s`, 389 `%d`**, and around nine hundred
 * and fifty calls in total. Everything else in this file exists because
 * something in the desktop uses it, and the list of conversions was taken from
 * the tree rather than from the standard:
 *
 *     %s  2022   %-Ns 63   %zu 46   %llu 16   %c   9   %%  7
 *     %d   389   %.*s 12   %u  16   %lld  4   %NX  8   %.Nf 11
 *
 * So: flags, width, precision, the length modifiers that appear, and `%f`.
 * Not `%e`, not `%g`, not `%a`, not `%n` -- nothing in the tree uses them, and
 * a conversion nobody calls is a conversion nobody has checked.
 *
 * --- What it does when asked for something it does not have ---
 *
 * It writes the specifier back out verbatim, exactly as it appeared. Not
 * nothing, and not a guess: a format string that comes out with `%q` still in
 * it is a bug somebody can see and find, while one that silently drops the
 * argument produces a sentence with a word missing and no clue where it went.
 *
 * --- Truncation ---
 *
 * The return value is **what would have been written**, not what was, which is
 * what the standard says and what every caller in this system relies on to
 * detect a cut. A great deal of ReconOS is built on noticing that number
 * exceeding the buffer -- it is how a path that would not fit is refused
 * rather than shortened.
 */

#include <stdarg.h>
#include <stddef.h>

/*
 * Where the output is going, and how much of it has been thrown away.
 *
 * `wanted` counts every byte the format asked for, whether or not there was
 * room, because that is the return value. `used` counts what actually landed.
 * Keeping them apart is the whole of truncation handling: there is no test for
 * "is it full" anywhere else in this file.
 */
struct sink {
	char *to;
	size_t room;		/* bytes the buffer holds, including the NUL */
	size_t used;
	size_t wanted;
};

static void put(struct sink *s, char c)
{
	if (s->used + 1 < s->room) {
		s->to[s->used++] = c;
	}
	s->wanted++;
}

static void put_bytes(struct sink *s, const char *from, size_t length)
{
	size_t i;

	for (i = 0; i < length; i++) {
		put(s, from[i]);
	}
}

static void put_repeat(struct sink *s, char c, int times)
{
	while (times-- > 0) {
		put(s, c);
	}
}

/* --- Numbers --- */

#define DIGITS_MAX 24		/* 64 bits in octal is 22, plus a sign */

/*
 * An unsigned value, written backwards into `out` and returned as a length.
 *
 * Backwards because the digits come out least-significant first and reversing
 * a short buffer at the end is cheaper and clearer than working out how many
 * digits there will be in order to write forwards.
 */
static int digits_of(unsigned long long value, unsigned base, int upper,
		     char *out)
{
	static const char LOWER[] = "0123456789abcdef";
	static const char UPPER[] = "0123456789ABCDEF";
	const char *set = upper ? UPPER : LOWER;
	int n = 0;

	if (value == 0) {
		out[n++] = '0';
		return n;
	}
	while (value != 0 && n < DIGITS_MAX) {
		out[n++] = set[value % base];
		value /= base;
	}
	return n;
}

struct spec {
	int left;		/* '-' */
	int zero;		/* '0' */
	int plus;		/* '+' */
	int space;		/* ' ' */
	int alt;		/* '#' */
	int width;
	int precision;		/* -1 when none was given */
};

/*
 * Lay out one already-converted value: sign, prefix, zero padding, digits, and
 * whatever space padding the width asks for on whichever side.
 *
 * One function for every numeric conversion, because the ordering rules are
 * the part that is easy to get subtly wrong -- zero padding goes *after* the
 * sign and the `0x`, space padding goes outside both, and a precision turns
 * the zero flag off. Written once, it is wrong once or right once.
 */
static void emit_number(struct sink *s, const struct spec *f,
			const char *rev, int n, const char *sign,
			const char *prefix)
{
	int sign_len = 0;
	int prefix_len = 0;
	int zeros = 0;
	int body;
	int pad;
	int i;

	while (sign[sign_len] != '\0') {
		sign_len++;
	}
	while (prefix[prefix_len] != '\0') {
		prefix_len++;
	}

	if (f->precision > n) {
		zeros = f->precision - n;
	}

	body = sign_len + prefix_len + zeros + n;

	/* A precision on an integer turns off zero padding -- the standard
	 * says so, and it is what makes "%08.3d" pad with spaces. */
	if (f->zero && !f->left && f->precision < 0 && f->width > body) {
		zeros += f->width - body;
		body = f->width;
	}

	pad = f->width > body ? f->width - body : 0;

	if (!f->left) {
		put_repeat(s, ' ', pad);
	}
	put_bytes(s, sign, (size_t)sign_len);
	put_bytes(s, prefix, (size_t)prefix_len);
	put_repeat(s, '0', zeros);
	for (i = n; i > 0; i--) {
		put(s, rev[i - 1]);
	}
	if (f->left) {
		put_repeat(s, ' ', pad);
	}
}

/*
 * --- %f ---
 *
 * Eleven call sites, every one of them `%.Nf` showing a quantity to somebody:
 * a size in megabytes, a percentage, a duration. So this is written for that
 * and says so, rather than pretending to be a general float formatter.
 *
 * **What it does not do:** it does not round-trip, it does not use the
 * shortest representation that reads back exactly, and above about 2^63 it
 * gives up and writes `huge`. Those are the properties a serialiser needs and
 * none of the eleven callers is one -- they are all putting a number in front
 * of a person, where "1.9 GB" is the right answer and the seventeen digits
 * that would reproduce the double exactly are not.
 *
 * --- Rounding, and why the first choice here was wrong ---
 *
 * This rounds **half to even**, which is what the reference does, and the
 * first version did not: it rounded away from zero on the reasoning that 0.125
 * displayed as 0.13 is what somebody reading a size expects.
 *
 * That reasoning was answering the wrong question. The point of this file is
 * that the desktop prints **the same thing on this kernel that it printed on
 * Linux yesterday** -- a port that quietly changed a number anywhere in ninety
 * thousand lines would be a port nobody could trust. Preferring a rounding
 * somebody might find more intuitive, at the cost of that, is a trade with
 * nothing on the other side: the difference only shows at an exact half, which
 * for a binary float means 0.5, 0.25, 0.125 and their like, and no reader has
 * ever been served by 0.13 over 0.12.
 *
 * Caught by the differential suite on 8 checks out of 73,748.
 *
 * --- Where this still differs from the reference, measured ---
 *
 * A decimal like 0.05 is not 0.05 as a double; it is very slightly more. The
 * reference knows that, because it converts exactly, and prints 0.1 at one
 * place. This scales in floating point, where that difference is lost, sees
 * exactly a half, and applies the even rule -- giving 0.0.
 *
 * **So: a value whose decimal expansion lands within rounding distance of a
 * half at the requested precision may differ by one in the last digit.** Exact
 * binary fractions -- 0.5, 0.25, 0.125, 1.5, 1023.5 -- are not affected and
 * are checked against the reference directly.
 *
 * Getting the rest exact needs arbitrary-precision decimal conversion, which
 * is several hundred lines of a well-known algorithm. It is not written
 * because all eleven callers are displaying a size or a percentage to a
 * person, and it is written down here rather than left to be discovered by
 * whoever first needs `%f` to round-trip.
 */
static void emit_double(struct sink *s, const struct spec *f, double value)
{
	/*
	 * Laid out whole, then padded -- rather than handing the integer part
	 * to `emit_number` and writing the point afterwards.
	 *
	 * That was the first version and it was wrong for exactly one case:
	 * a left-justified conversion had its trailing spaces written before
	 * the point, so `%-8.2f` of 100 came out `100  .00`. The differential
	 * suite found it on 26 checks out of 73,094, all of them that format.
	 *
	 * The lesson is not "handle left-justification specially". It is that
	 * a number with a fractional part is **one string**, and padding
	 * belongs outside it rather than inside one of its halves.
	 */
	char body[72];
	int n = 0;
	int precision = f->precision < 0 ? 6 : f->precision;
	const char *sign = "";
	int pad;
	int i;

	/* The scale below is a 64-bit integer, so beyond eighteen digits it
	 * would overflow rather than gain precision. Said here rather than
	 * discovered: no caller in this system asks for more than three. */
	if (precision > 18) {
		precision = 18;
	}

	if (value != value) {			/* NaN: the only self-inequality */
		body[n++] = 'n';
		body[n++] = 'a';
		body[n++] = 'n';
		goto pad_it;
	}

	if (value < 0) {
		sign = "-";
		value = -value;
	} else if (f->plus) {
		sign = "+";
	} else if (f->space) {
		sign = " ";
	}

	body[n++] = sign[0];
	if (sign[0] == '\0') {
		n = 0;
	}

	/*
	 * Past what a 64-bit integer holds, this says so rather than printing
	 * something wrong. Every `%f` in ReconOS is putting a quantity in
	 * front of a person -- a size, a percentage, a duration -- and none is
	 * a serialiser. "huge" is a better answer to a person than a number
	 * that is not the number.
	 */
	if (value >= 18446744073709551615.0) {
		body[n++] = 'h';
		body[n++] = 'u';
		body[n++] = 'g';
		body[n++] = 'e';
		goto pad_it;
	}

	{
		unsigned long long whole = (unsigned long long)value;
		double fraction = value - (double)whole;
		unsigned long long scale = 1;
		unsigned long long scaled;
		char rev[DIGITS_MAX];
		int digits;

		for (i = 0; i < precision; i++) {
			scale *= 10;
		}

		/*
		 * Half to even, matching the reference. Floor first, then
		 * decide on the remainder, rather than adding a half and
		 * truncating -- that is the away-from-zero rule, and it is
		 * what this used to do.
		 *
		 * The carry still matters either way: 0.999 at two places is
		 * 1.00, and the integer part changes.
		 */
		{
			double target = fraction * (double)scale;
			unsigned long long down = (unsigned long long)target;
			double rest = target - (double)down;

			if (rest > 0.5) {
				scaled = down + 1;
			} else if (rest < 0.5) {
				scaled = down;
			} else {
				/*
				 * Exactly half: round so the *kept* digit is
				 * even -- and the kept digit is the last one
				 * of the whole number, not of the fraction.
				 *
				 * Testing the fraction alone is wrong at
				 * precision 0, where it is always zero and
				 * therefore always even: 1.5 came out 1 and
				 * 2.5 came out 2, so half the answers were
				 * right and the rule was not being applied at
				 * all. The value that has to be even is
				 * `whole * scale + down`, which at precision 0
				 * is just `whole`.
				 */
				unsigned long long kept = whole * scale + down;

				scaled = (kept & 1ULL) ? down + 1 : down;
			}
		}
		if (scaled >= scale) {
			whole++;
			scaled -= scale;
		}

		digits = digits_of(whole, 10, 0, rev);
		for (i = digits; i > 0; i--) {
			body[n++] = rev[i - 1];
		}

		if (precision > 0) {
			char frev[DIGITS_MAX];
			int fn = digits_of(scaled, 10, 0, frev);

			body[n++] = '.';
			/* The leading zeros the digit conversion dropped:
			 * 0.05 at two places is "05", and digits_of said
			 * "5". */
			for (i = 0; i < precision - fn; i++) {
				body[n++] = '0';
			}
			for (i = fn; i > 0; i--) {
				body[n++] = frev[i - 1];
			}
		}
	}

pad_it:
	pad = f->width > n ? f->width - n : 0;

	if (f->left) {
		for (i = 0; i < n; i++) {
			put(s, body[i]);
		}
		put_repeat(s, ' ', pad);
		return;
	}

	/*
	 * Zero padding goes *after* the sign, not before it -- `%08.2f` of
	 * -1.5 is "-0001.50" and never "0000-1.5". So the sign is written
	 * first and the zeros counted against the rest.
	 */
	if (f->zero) {
		int from = 0;

		if (n > 0 && (body[0] == '-' || body[0] == '+' ||
			      body[0] == ' ')) {
			put(s, body[0]);
			from = 1;
		}
		put_repeat(s, '0', pad);
		for (i = from; i < n; i++) {
			put(s, body[i]);
		}
		return;
	}

	put_repeat(s, ' ', pad);
	for (i = 0; i < n; i++) {
		put(s, body[i]);
	}
}

/* --- The engine --- */

enum length { LEN_INT, LEN_CHAR, LEN_SHORT, LEN_LONG, LEN_LLONG, LEN_SIZE };

int vsnprintf(char *to, size_t room, const char *format, va_list args)
{
	struct sink s;
	const char *at = format;

	s.to = to;
	s.room = room;
	s.used = 0;
	s.wanted = 0;

	while (*at != '\0') {
		const char *start = at;
		struct spec f;
		enum length len = LEN_INT;
		char rev[DIGITS_MAX];
		int n;

		if (*at != '%') {
			put(&s, *at++);
			continue;
		}
		at++;

		if (*at == '%') {
			put(&s, '%');
			at++;
			continue;
		}

		f.left = 0;
		f.zero = 0;
		f.plus = 0;
		f.space = 0;
		f.alt = 0;
		f.width = 0;
		f.precision = -1;

		for (;;) {
			if (*at == '-') {
				f.left = 1;
			} else if (*at == '0') {
				f.zero = 1;
			} else if (*at == '+') {
				f.plus = 1;
			} else if (*at == ' ') {
				f.space = 1;
			} else if (*at == '#') {
				f.alt = 1;
			} else {
				break;
			}
			at++;
		}

		if (*at == '*') {
			f.width = va_arg(args, int);
			if (f.width < 0) {	/* a negative width is '-' */
				f.left = 1;
				f.width = -f.width;
			}
			at++;
		} else {
			while (*at >= '0' && *at <= '9') {
				f.width = f.width * 10 + (*at - '0');
				at++;
			}
		}

		if (*at == '.') {
			at++;
			f.precision = 0;
			if (*at == '*') {
				f.precision = va_arg(args, int);
				at++;
			} else {
				while (*at >= '0' && *at <= '9') {
					f.precision =
						f.precision * 10 + (*at - '0');
					at++;
				}
			}
			/* A negative precision is as if none were given. */
			if (f.precision < 0) {
				f.precision = -1;
			}
		}

		if (*at == 'h') {
			at++;
			len = LEN_SHORT;
			if (*at == 'h') {
				at++;
				len = LEN_CHAR;
			}
		} else if (*at == 'l') {
			at++;
			len = LEN_LONG;
			if (*at == 'l') {
				at++;
				len = LEN_LLONG;
			}
		} else if (*at == 'z') {
			at++;
			len = LEN_SIZE;
		} else if (*at == 'j' || *at == 't' || *at == 'L') {
			at++;
			len = LEN_LLONG;
		}

		switch (*at) {
		case 'd':
		case 'i': {
			long long v;
			unsigned long long m;
			const char *sign = "";

			switch (len) {
			case LEN_LONG:  v = va_arg(args, long); break;
			case LEN_LLONG: v = va_arg(args, long long); break;
			case LEN_SIZE:  v = (long long)va_arg(args, size_t);
					break;
			case LEN_SHORT: v = (short)va_arg(args, int); break;
			case LEN_CHAR:  v = (signed char)va_arg(args, int);
					break;
			default:        v = va_arg(args, int); break;
			}

			/*
			 * Negated as unsigned, because the most negative value
			 * of a type has no positive counterpart and `-v` on it
			 * is undefined. The cast happens first and the
			 * negation second, in unsigned arithmetic, where it is
			 * defined and gives the right magnitude.
			 */
			if (v < 0) {
				sign = "-";
				m = ~(unsigned long long)v + 1ULL;
			} else {
				m = (unsigned long long)v;
				if (f.plus) {
					sign = "+";
				} else if (f.space) {
					sign = " ";
				}
			}
			n = digits_of(m, 10, 0, rev);
			emit_number(&s, &f, rev, n, sign, "");
			at++;
			continue;
		}
		case 'u':
		case 'o':
		case 'x':
		case 'X': {
			unsigned long long v;
			unsigned base = 10;
			int upper = (*at == 'X');
			const char *prefix = "";

			switch (len) {
			case LEN_LONG:  v = va_arg(args, unsigned long); break;
			case LEN_LLONG: v = va_arg(args, unsigned long long);
					break;
			case LEN_SIZE:  v = va_arg(args, size_t); break;
			case LEN_SHORT: v = (unsigned short)
						va_arg(args, unsigned int);
					break;
			case LEN_CHAR:  v = (unsigned char)
						va_arg(args, unsigned int);
					break;
			default:        v = va_arg(args, unsigned int); break;
			}

			if (*at == 'o') {
				base = 8;
			} else if (*at == 'x' || *at == 'X') {
				base = 16;
				if (f.alt && v != 0) {
					prefix = upper ? "0X" : "0x";
				}
			}
			n = digits_of(v, base, upper, rev);
			emit_number(&s, &f, rev, n, "", prefix);
			at++;
			continue;
		}
		case 'c': {
			char c = (char)va_arg(args, int);
			int pad = f.width > 1 ? f.width - 1 : 0;

			if (!f.left) {
				put_repeat(&s, ' ', pad);
			}
			put(&s, c);
			if (f.left) {
				put_repeat(&s, ' ', pad);
			}
			at++;
			continue;
		}
		case 's': {
			const char *text = va_arg(args, const char *);
			size_t length = 0;
			int pad;

			/*
			 * A null pointer prints "(null)" rather than faulting.
			 * That is not politeness: a fault here takes down
			 * whatever was trying to *report* something, which is
			 * usually an error path, and the message that would
			 * have named the real problem is lost.
			 */
			if (text == NULL) {
				text = "(null)";
			}

			/* A precision on a string is a maximum, and the string
			 * need not be terminated within it -- which is exactly
			 * why `%.*s` is used to print a run of a page's text
			 * that points into a larger buffer. */
			if (f.precision >= 0) {
				while (length < (size_t)f.precision &&
				       text[length] != '\0') {
					length++;
				}
			} else {
				while (text[length] != '\0') {
					length++;
				}
			}

			pad = f.width > (int)length ? f.width - (int)length : 0;
			if (!f.left) {
				put_repeat(&s, ' ', pad);
			}
			put_bytes(&s, text, length);
			if (f.left) {
				put_repeat(&s, ' ', pad);
			}
			at++;
			continue;
		}
		case 'p': {
			void *p = va_arg(args, void *);

			f.precision = -1;
			n = digits_of((unsigned long long)(size_t)p, 16, 0,
				      rev);
			emit_number(&s, &f, rev, n, "", "0x");
			at++;
			continue;
		}
		case 'f':
		case 'F':
			emit_double(&s, &f, va_arg(args, double));
			at++;
			continue;
		default:
			/*
			 * Something this does not have. The specifier goes out
			 * exactly as it came in, so it is visible in the
			 * output and can be found -- rather than swallowed,
			 * which leaves a sentence with a word missing and
			 * nothing to search for.
			 *
			 * The argument is *not* consumed, because this does
			 * not know what type it was, and guessing would put
			 * every argument after it one place out.
			 */
			put_bytes(&s, start, (size_t)(at - start) + 1);
			if (*at != '\0') {
				at++;
			}
			continue;
		}
	}

	if (s.room > 0) {
		s.to[s.used < s.room ? s.used : s.room - 1] = '\0';
	}
	return (int)s.wanted;
}

int snprintf(char *to, size_t room, const char *format, ...)
{
	va_list args;
	int n;

	va_start(args, format);
	n = vsnprintf(to, room, format, args);
	va_end(args);
	return n;
}
