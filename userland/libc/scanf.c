/*
 * `sscanf` -- reading values back out of text.
 *
 * Fourteen call sites in the desktop, and it was invisible to
 * `scripts/measure-libc.py` until v0.4.30: glibc emits a call to `sscanf` as
 * `__isoc99_sscanf`, which sat behind a filter meant for compiler internals.
 * It is the one function that was in neither total, and BG-197 is the entry.
 *
 * --- What the desktop actually asks for ---
 *
 * Measured before any of this was written, by reading all fourteen calls:
 *
 *   %d %2d %4d     a signed number, sometimes with a width
 *   %u %lu %llu    unsigned, with length modifiers
 *   %lx            hexadecimal into an unsigned long
 *   %s %63s        a run of non-space characters
 *   %n             how far the scan has got, which two date parsers rely on
 *
 * and literal text around them -- `MemTotal: %lu kB`, `* %d EXISTS`,
 * `%4d-%2d-%2d %2d:%2d %n`.
 *
 * What is implemented is that set plus the rest of the integer and floating
 * conversions, which cost almost nothing on top: `%i` `%o` `%X` `%c` `%f`
 * `%e` `%g` `%%`, assignment suppression with `*`, and the `hh h l ll z j`
 * length modifiers.
 *
 * **Scansets -- `%[a-z]` -- are not implemented, and neither is `%p`.**
 * Nothing in the desktop uses either, and a scanset is a small parser of its
 * own with its own edge cases around `]` first and `-` last. A format
 * containing one **stops the scan** and returns what matched before it, which
 * is what C says happens at any matching failure -- so a caller gets a short
 * count rather than a wrong value. Said here because the alternative, quietly
 * skipping the conversion, is how a program comes to read the second field
 * into the first variable.
 *
 * --- How it is checked ---
 *
 * Against the host's, with the same input and the same format, comparing both
 * the return value *and* every variable, including the ones that should not
 * have been touched. A `sscanf` that returns the right count and assigns the
 * right numbers to the wrong arguments is the failure that matters, and only
 * checking the untouched ones finds it.
 */

#include "internal.h"

#include "../include/errno.h"

/* Enough for the longest number any of these conversions can take: 64 bits of
 * binary is 20 digits, hexadecimal with a 0x prefix and a sign is shorter, and
 * a double's text form is bounded by its exponent. Anything longer than this
 * is not a number a caller can hold. */
#define NUMBER_MAX 96

struct scan {
	const char *at;
	const char *start;
	int assigned;
	int converted;		/* conversions attempted, for the EOF rule */
	int ran_out;		/* a conversion found no input left */
};

static int is_space(int c)
{
	return c == ' ' || c == '\t' || c == '\n' ||
	       c == '\v' || c == '\f' || c == '\r';
}

static void skip_space(struct scan *s)
{
	while (*s->at && is_space((unsigned char)*s->at))
		s->at++;
}

/* --- reading a number ------------------------------------------------------
 *
 * The digits are copied into a buffer bounded by the field width and then
 * handed to `strtoll` or `strtoull`, which are already written and already
 * held against the host's. Scanning the number a second time here would be a
 * second implementation of the same thing, and the two would eventually
 * disagree about something like a leading `+` on a hexadecimal.
 *
 * What this has to get right is only *how much text to offer*, and then how
 * far the conversion actually got, so the cursor lands in the right place.
 */
static int gather_number(struct scan *s, char *into, size_t room,
			 unsigned width, int base, int allow_sign)
{
	size_t n = 0;
	const char *p = s->at;
	unsigned taken = 0;

	if (allow_sign && (*p == '+' || *p == '-')) {
		if (!width || taken < width) {
			into[n++] = *p++;
			taken++;
		}
	}

	/* `0x` is part of the number for base 16 and for base 0, and is two
	 * characters the width has to account for. */
	if ((base == 16 || base == 0) && p[0] == '0' &&
	    (p[1] == 'x' || p[1] == 'X') &&
	    (!width || taken + 2 <= width) && n + 2 < room) {
		into[n++] = *p++;
		into[n++] = *p++;
		taken += 2;
	}

	while (*p && n + 1 < room && (!width || taken < width)) {
		int c = (unsigned char)*p;
		int value;

		if (c >= '0' && c <= '9')
			value = c - '0';
		else if (c >= 'a' && c <= 'z')
			value = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z')
			value = c - 'A' + 10;
		else
			break;

		/* Base 0 decides itself from the prefix, so everything up to
		 * a hex digit is offered and strtoll sorts it out. */
		if (base && value >= base)
			break;
		if (!base && value > 15)
			break;

		into[n++] = *p++;
		taken++;
	}

	into[n] = '\0';
	return (int)n;
}

/* --- the scan -------------------------------------------------------------- */

enum length {
	LEN_INT, LEN_CHAR, LEN_SHORT, LEN_LONG, LEN_LONGLONG, LEN_SIZE
};

static void store_signed(va_list *ap, enum length length, long long value)
{
	switch (length) {
	case LEN_CHAR:
		*va_arg(*ap, signed char *) = (signed char)value;
		break;
	case LEN_SHORT:
		*va_arg(*ap, short *) = (short)value;
		break;
	case LEN_LONG:
		*va_arg(*ap, long *) = (long)value;
		break;
	case LEN_LONGLONG:
		*va_arg(*ap, long long *) = value;
		break;
	case LEN_SIZE:
		*va_arg(*ap, size_t *) = (size_t)value;
		break;
	default:
		*va_arg(*ap, int *) = (int)value;
		break;
	}
}

static void store_unsigned(va_list *ap, enum length length,
			   unsigned long long value)
{
	switch (length) {
	case LEN_CHAR:
		*va_arg(*ap, unsigned char *) = (unsigned char)value;
		break;
	case LEN_SHORT:
		*va_arg(*ap, unsigned short *) = (unsigned short)value;
		break;
	case LEN_LONG:
		*va_arg(*ap, unsigned long *) = (unsigned long)value;
		break;
	case LEN_LONGLONG:
		*va_arg(*ap, unsigned long long *) = value;
		break;
	case LEN_SIZE:
		*va_arg(*ap, size_t *) = (size_t)value;
		break;
	default:
		*va_arg(*ap, unsigned int *) = (unsigned int)value;
		break;
	}
}

int vsscanf(const char *text, const char *format, va_list args)
{
	struct scan s;
	va_list ap;
	const char *f = format;

	if (!text || !format)
		return -1;

	s.at = text;
	s.start = text;
	s.assigned = 0;
	s.converted = 0;
	s.ran_out = 0;

	va_copy(ap, args);

	while (*f) {
		int suppress = 0;
		unsigned width = 0;
		enum length length = LEN_INT;

		/* Whitespace in the format matches any run of whitespace in
		 * the input, **including none at all**. That is the rule that
		 * makes `" nameserver %63s"` work on a line with no leading
		 * space, and getting it wrong the other way -- requiring at
		 * least one -- is a parser that fails on half its input. */
		if (is_space((unsigned char)*f)) {
			while (is_space((unsigned char)*f))
				f++;
			skip_space(&s);
			continue;
		}

		if (*f != '%') {
			/* A literal. It has to match, and if it does not the
			 * scan is over -- the count so far is the answer. */
			if (*s.at != *f) {
				if (!*s.at)
					s.ran_out = 1;
				goto done;
			}
			s.at++;
			f++;
			continue;
		}

		f++;			/* past the % */

		if (*f == '%') {
			if (*s.at != '%')
				goto done;
			s.at++;
			f++;
			continue;
		}

		if (*f == '*') {
			suppress = 1;
			f++;
		}

		while (*f >= '0' && *f <= '9') {
			width = width * 10 + (unsigned)(*f - '0');
			f++;
		}

		if (*f == 'h') {
			f++;
			length = LEN_SHORT;
			if (*f == 'h') {
				f++;
				length = LEN_CHAR;
			}
		} else if (*f == 'l') {
			f++;
			length = LEN_LONG;
			if (*f == 'l') {
				f++;
				length = LEN_LONGLONG;
			}
		} else if (*f == 'z') {
			f++;
			length = LEN_SIZE;
		} else if (*f == 'j') {
			f++;
			length = LEN_LONGLONG;
		}

		switch (*f) {
		case 'd':
		case 'i':
		case 'u':
		case 'o':
		case 'x':
		case 'X':
		{
			char buffer[NUMBER_MAX];
			char *end = 0;
			int base;
			int got;
			int is_signed = (*f == 'd' || *f == 'i');

			base = (*f == 'i') ? 0
			     : (*f == 'o') ? 8
			     : (*f == 'x' || *f == 'X') ? 16 : 10;

			skip_space(&s);
			s.converted++;

			if (!*s.at) {
				s.ran_out = 1;
				goto done;
			}

			got = gather_number(&s, buffer, sizeof(buffer), width,
					    base, 1);
			if (!got)
				goto done;

			if (is_signed) {
				/* `strtol` rather than a `strtoll` this
				 * library does not have. `long` is 64 bits on
				 * both architectures ReconOS runs on, so the
				 * range is the same -- said here rather than
				 * assumed, because the day there is a 32-bit
				 * one this line is wrong and nothing else
				 * would say so. */
				long long value = strtol(buffer, &end, base);

				if (end == buffer)
					goto done;

				s.at += (end - buffer);
				if (!suppress) {
					store_signed(&ap, length, value);
					s.assigned++;
				}
			} else {
				unsigned long long value =
					strtoull(buffer, &end, base);

				if (end == buffer)
					goto done;

				s.at += (end - buffer);
				if (!suppress) {
					store_unsigned(&ap, length, value);
					s.assigned++;
				}
			}

			f++;
			break;
		}

		case 'f':
		case 'e':
		case 'E':
		case 'g':
		case 'G':
		case 'a':
		{
			char buffer[NUMBER_MAX];
			char *end = 0;
			size_t n = 0;
			double value;

			skip_space(&s);
			s.converted++;

			if (!*s.at) {
				s.ran_out = 1;
				goto done;
			}

			/* A float's text is not a run of digits -- it has a
			 * point, an exponent and a sign inside it -- so the
			 * candidate is copied generously and `strtod` decides
			 * where it really ends. */
			while (s.at[n] && n + 1 < sizeof(buffer) &&
			       (!width || n < width) &&
			       !is_space((unsigned char)s.at[n])) {
				buffer[n] = s.at[n];
				n++;
			}
			buffer[n] = '\0';

			value = strtod(buffer, &end);

			if (end == buffer)
				goto done;

			s.at += (end - buffer);

			if (!suppress) {
				if (length == LEN_LONG ||
				    length == LEN_LONGLONG)
					*va_arg(ap, double *) = value;
				else
					*va_arg(ap, float *) = (float)value;
				s.assigned++;
			}

			f++;
			break;
		}

		case 's':
		{
			char *out = suppress ? 0 : va_arg(ap, char *);
			unsigned n = 0;

			skip_space(&s);
			s.converted++;

			if (!*s.at) {
				s.ran_out = 1;
				goto done;
			}

			while (*s.at && !is_space((unsigned char)*s.at) &&
			       (!width || n < width)) {
				if (out)
					out[n] = *s.at;
				n++;
				s.at++;
			}

			if (!n)
				goto done;

			if (out) {
				out[n] = '\0';
				s.assigned++;
			}

			f++;
			break;
		}

		case 'c':
		{
			char *out = suppress ? 0 : va_arg(ap, char *);
			unsigned wanted = width ? width : 1;
			unsigned n = 0;

			/* **No leading whitespace is skipped**, which is the
			 * one conversion where that is true and the one place
			 * a reader of this file is likely to expect the
			 * opposite. */
			s.converted++;

			if (!*s.at) {
				s.ran_out = 1;
				goto done;
			}

			while (*s.at && n < wanted) {
				if (out)
					out[n] = *s.at;
				n++;
				s.at++;
			}

			if (n < wanted)
				goto done;

			if (out)
				s.assigned++;

			f++;
			break;
		}

		case 'n':
			/* How far the scan has got. **Not an assignment**, so
			 * it does not count towards the return -- which two of
			 * the desktop's date parsers depend on, since they
			 * test the count and then use the offset. */
			if (!suppress)
				store_signed(&ap, length,
					     (long long)(s.at - s.start));
			f++;
			break;

		default:
			/* A conversion this does not implement -- a scanset,
			 * `%p`, or a typo. The scan stops and the count so far
			 * is the answer, which is what C does for a matching
			 * failure. Carrying on past it would put the next
			 * field into this field's variable. */
			goto done;
		}
	}

done:
	va_end(ap);

	/*
	 * EOF when the input ran out before anything could be converted, which
	 * is different from converting nothing: a caller that tests `== 1`
	 * treats both as failure, but one that tests `!= EOF` is asking
	 * whether there was any input at all.
	 */
	if (!s.assigned && s.ran_out)
		return -1;

	return s.assigned;
}

int sscanf(const char *text, const char *format, ...)
{
	va_list args;
	int n;

	va_start(args, format);
	n = vsscanf(text, format, args);
	va_end(args);

	return n;
}
