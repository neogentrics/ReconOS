/*
 * Numbers out of text, sorting, and the two that have no answer here.
 *
 * Measured across `src/`: `atoi` 33, `getenv` 20, `strtoul` 8, `qsort` 6,
 * `strtol` 5, `strtod` 3, `exit` 2, and one each of `atof`, `atoll` and `abs`.
 *
 * `malloc` and `free` are **not** here, and their absence is the subject of
 * the first entry in `docs/KERNEL-WANTS.md`: there is no way for a program on
 * this kernel to ask for memory, so there is nothing for an allocator to be
 * built on. Everything in this file is written to need none.
 */

#include "internal.h"

/* --- Text to number --- */

/*
 * One digit in a base, or -1.
 *
 * Letters continue past 'f' so that base 36 works, which costs nothing and
 * means the base argument is honest rather than "2, 8, 10 or 16".
 */
static int digit_value(int c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'Z') {
		return c - 'A' + 10;
	}
	return -1;
}

/*
 * The shared body of `strtol` and `strtoul`.
 *
 * `saturate` is what the signed version needs: the standard says an overflowing
 * conversion returns the limit rather than wrapping, and a caller reading a
 * size out of a file is far better served by a number that is too big than by
 * one that has wrapped to negative.
 */
static unsigned long long parse(const char *text, char **end, int base,
				int *negative, unsigned long long limit,
				int *overflowed)
{
	const char *at = text;
	unsigned long long value = 0;
	int any = 0;

	*negative = 0;
	*overflowed = 0;

	while (isspace((unsigned char)*at)) {
		at++;
	}

	if (*at == '+' || *at == '-') {
		*negative = (*at == '-');
		at++;
	}

	/*
	 * A base of zero means "work it out", which is what makes `strtol`
	 * read both 0x1F and 31. The prefix is only consumed when it is
	 * actually followed by a digit in that base -- "0x" alone is the
	 * number zero followed by an 'x', and a parser that swallowed the
	 * prefix would report a success that consumed two characters of
	 * something else.
	 */
	if ((base == 0 || base == 16) && at[0] == '0' &&
	    (at[1] == 'x' || at[1] == 'X') && digit_value(at[2]) >= 0 &&
	    digit_value(at[2]) < 16) {
		at += 2;
		base = 16;
	} else if ((base == 0 || base == 2) && at[0] == '0' &&
		   (at[1] == 'b' || at[1] == 'B') && digit_value(at[2]) >= 0 &&
		   digit_value(at[2]) < 2) {
		/*
		 * The binary prefix, under the same rule as the hexadecimal
		 * one above and for the same reason: it is only a prefix when
		 * a digit follows it, so "0b" alone stays the number zero
		 * followed by a 'b'.
		 *
		 * It is here because the reference accepts it. C23 added it
		 * and glibc has shipped it since 2.38, so "0b101" is five on
		 * every machine the desktop is built on today. Reading it as
		 * zero and stopping after one character would not have failed
		 * anywhere -- it would have quietly returned a different
		 * number.
		 */
		at += 2;
		base = 2;
	} else if (base == 0 && at[0] == '0') {
		base = 8;
	} else if (base == 0) {
		base = 10;
	}

	for (;;) {
		int d = digit_value((unsigned char)*at);

		if (d < 0 || d >= base) {
			break;
		}
		any = 1;

		/* Checked before it happens rather than detected after: once
		 * it has wrapped the value is gone and there is nothing left
		 * to notice. */
		if (value > (limit - (unsigned long long)d) /
		    (unsigned long long)base) {
			*overflowed = 1;
		} else {
			value = value * (unsigned long long)base +
				(unsigned long long)d;
		}
		at++;
	}

	/*
	 * With no digits at all, nothing is consumed -- `end` goes back to
	 * where the caller started, not to where the sign was. That is how a
	 * caller tells "this was not a number" from "this was zero".
	 */
	if (end != NULL) {
		*end = (char *)(any ? at : text);
	}
	return any ? value : 0;
}

long strtol(const char *text, char **end, int base)
{
	int negative;
	int overflowed;
	unsigned long long limit = 0x7FFFFFFFFFFFFFFFULL;
	unsigned long long v;

	v = parse(text, end, base, &negative, limit + 1, &overflowed);

	if (negative) {
		if (overflowed || v > limit + 1) {
			return (long)(-limit - 1);
		}
		return (long)(0 - v);
	}
	if (overflowed || v > limit) {
		return (long)limit;
	}
	return (long)v;
}

unsigned long strtoul(const char *text, char **end, int base)
{
	int negative;
	int overflowed;
	unsigned long long v;

	v = parse(text, end, base, &negative, ~0ULL, &overflowed);

	if (overflowed) {
		return ~0UL;
	}
	/* A leading minus on an unsigned conversion negates, which is what the
	 * standard says and is startling enough to be worth a line. */
	return negative ? (unsigned long)(0 - v) : (unsigned long)v;
}

int atoi(const char *text)
{
	return (int)strtol(text, (char **)0, 10);
}

long long atoll(const char *text)
{
	return (long long)strtol(text, (char **)0, 10);
}

/*
 * --- Text to double ---
 *
 * Three call sites, all of them reading a file of numbers somebody wrote.
 *
 * **The same caveat as `%f` in printf.c, from the same cause.** This
 * accumulates the digits as an integer and then scales, so the result is the
 * nearest double to the *scaled* value rather than to the decimal as written,
 * and for a long fraction the two can differ in the last bit. Reading back a
 * number this library printed is exact for anything with a short decimal
 * form -- which is what the grapher's files contain -- and is not guaranteed
 * for seventeen significant digits.
 *
 * Doing better means arbitrary-precision decimal, the same several hundred
 * lines `%f` would need, and it would be the same algorithm. If either ever
 * needs to be exact, both should be, and they should share it.
 */
/*
 * Scaling by a power of two, which is the one operation in this file that is
 * exact: doubling and halving a finite double change only the exponent until
 * the result runs out of range, and then give infinity or zero, which is what
 * the reference gives too.
 *
 * The count is clamped first. A mantissa here is at most 2^64, so anything
 * past a couple of thousand doublings is infinity and anything past a couple
 * of thousand halvings is zero -- the clamp changes no answer and stops
 * "0x1p99999999" from spending a second in this loop.
 */
static double scale_by_two(double value, int power)
{
	if (power > 2100) {
		power = 2100;
	}
	if (power < -2100) {
		power = -2100;
	}
	while (power > 0) {
		value *= 2.0;
		power--;
	}
	while (power < 0) {
		value *= 0.5;
		power++;
	}
	return value;
}

static int hex_value(int c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/*
 * Case-insensitive, and only over the length given -- `word_is` answers about
 * a prefix, because "infinity" has to beat "inf" and both have to lose to a
 * caller who wrote "info".
 */
static int word_is(const char *at, const char *word)
{
	size_t i;

	for (i = 0; word[i] != '\0'; i++) {
		if (tolower((unsigned char)at[i]) != word[i]) {
			return 0;
		}
	}
	return 1;
}

double strtod(const char *text, char **end)
{
	const char *at = text;
	double value = 0.0;
	int negative = 0;
	int any = 0;
	int exponent = 0;

	while (isspace((unsigned char)*at)) {
		at++;
	}
	if (*at == '+' || *at == '-') {
		negative = (*at == '-');
		at++;
	}

	/*
	 * --- The two words ---
	 *
	 * The reference reads "inf", "infinity" and "nan" as numbers, in any
	 * mixture of cases, and a file of measurements written by anything
	 * that can produce them will contain them. Reading "inf" as zero and
	 * consuming nothing is the failure that looks like data.
	 *
	 * Built with the compiler's own constants rather than by overflowing
	 * an expression, which a compiler is entitled to fold and warn about.
	 */
	if (word_is(at, "infinity")) {
		at += 8;
		if (end != NULL) {
			*end = (char *)at;
		}
		return negative ? -__builtin_inf() : __builtin_inf();
	}
	if (word_is(at, "inf")) {
		at += 3;
		if (end != NULL) {
			*end = (char *)at;
		}
		return negative ? -__builtin_inf() : __builtin_inf();
	}
	if (word_is(at, "nan")) {
		at += 3;
		/*
		 * The parenthesised tag is part of the number when it closes.
		 * An unclosed one is not, and the 'n' of "nan" is where the
		 * number ends -- so the scan is undone rather than left where
		 * it stopped.
		 */
		if (*at == '(') {
			const char *tag = at + 1;

			while (*tag == '_' || isalnum((unsigned char)*tag)) {
				tag++;
			}
			if (*tag == ')') {
				at = tag + 1;
			}
		}
		if (end != NULL) {
			*end = (char *)at;
		}
		return negative ? -__builtin_nan("") : __builtin_nan("");
	}

	/*
	 * --- Hexadecimal ---
	 *
	 * The standard requires this of `strtod` and the reference does it, so
	 * "0x1F" is thirty-one and not zero-followed-by-an-x. It also happens
	 * to be the only exact path in this function: hexadecimal digits are
	 * four bits each, so the mantissa is accumulated as an integer and the
	 * scaling is by a power of two, and neither step loses anything the
	 * decimal path below cannot help losing.
	 *
	 * The prefix is only a prefix when a hexadecimal digit follows it,
	 * with or without a point in between -- the same rule `parse` uses,
	 * for the same reason.
	 */
	if (at[0] == '0' && (at[1] == 'x' || at[1] == 'X') &&
	    (hex_value((unsigned char)at[2]) >= 0 ||
	     (at[2] == '.' && hex_value((unsigned char)at[3]) >= 0))) {
		unsigned long long mantissa = 0;
		int binary = 0;

		at += 2;
		while (hex_value((unsigned char)*at) >= 0) {
			if (mantissa <= (~0ULL >> 4)) {
				mantissa = mantissa * 16 +
					(unsigned long long)
					hex_value((unsigned char)*at);
			} else {
				/* Past what the accumulator holds. The digit
				 * is dropped and its place is kept, which is
				 * a rounding error and never a magnitude
				 * error. */
				binary += 4;
			}
			at++;
		}
		if (*at == '.') {
			at++;
			while (hex_value((unsigned char)*at) >= 0) {
				if (mantissa <= (~0ULL >> 4)) {
					mantissa = mantissa * 16 +
						(unsigned long long)
						hex_value((unsigned char)*at);
					binary -= 4;
				}
				at++;
			}
		}

		/* The binary exponent, which `strtod` makes optional even
		 * though the language requires it of a constant in source. */
		if (*at == 'p' || *at == 'P') {
			const char *mark = at;
			int esign = 0;
			int evalue = 0;
			int edigits = 0;

			at++;
			if (*at == '+' || *at == '-') {
				esign = (*at == '-');
				at++;
			}
			while (isdigit((unsigned char)*at)) {
				if (evalue < 100000) {
					evalue = evalue * 10 + (*at - '0');
				}
				at++;
				edigits = 1;
			}
			if (edigits) {
				binary += esign ? -evalue : evalue;
			} else {
				at = mark;
			}
		}

		value = scale_by_two((double)mantissa, binary);
		if (end != NULL) {
			*end = (char *)at;
		}
		return negative ? -value : value;
	}

	while (isdigit((unsigned char)*at)) {
		value = value * 10.0 + (double)(*at - '0');
		at++;
		any = 1;
	}

	if (*at == '.') {
		at++;
		while (isdigit((unsigned char)*at)) {
			value = value * 10.0 + (double)(*at - '0');
			exponent--;
			at++;
			any = 1;
		}
	}

	if (!any) {
		/* Nothing was read. Back to the start, including over the
		 * sign, so the caller can tell this from a genuine zero. */
		if (end != NULL) {
			*end = (char *)text;
		}
		return 0.0;
	}

	if (*at == 'e' || *at == 'E') {
		const char *mark = at;
		int esign = 0;
		int evalue = 0;
		int edigits = 0;

		at++;
		if (*at == '+' || *at == '-') {
			esign = (*at == '-');
			at++;
		}
		while (isdigit((unsigned char)*at)) {
			if (evalue < 100000) {
				evalue = evalue * 10 + (*at - '0');
			}
			at++;
			edigits = 1;
		}
		if (edigits) {
			exponent += esign ? -evalue : evalue;
		} else {
			/* "1e" is the number one followed by an 'e'. The
			 * exponent is only consumed when it has digits. */
			at = mark;
		}
	}

	/* Scaled by repeated multiplication rather than by a power table,
	 * which would be one more thing to keep correct for no measurable
	 * gain at three call sites. */
	while (exponent > 0) {
		value *= 10.0;
		exponent--;
	}
	while (exponent < 0) {
		value /= 10.0;
		exponent++;
	}

	if (end != NULL) {
		*end = (char *)at;
	}
	return negative ? -value : value;
}

double atof(const char *text)
{
	return strtod(text, (char **)0);
}

int abs(int value)
{
	return value < 0 ? -value : value;
}

/* --- Sorting --- */

static void swap_bytes(unsigned char *a, unsigned char *b, size_t size)
{
	size_t i;

	for (i = 0; i < size; i++) {
		unsigned char t = a[i];

		a[i] = b[i];
		b[i] = t;
	}
}

/*
 * An insertion sort, and that is a decision rather than a shortcut.
 *
 * `qsort` is called six times in this system, on a skin list, a font list, a
 * table of columns and the like -- tens of items, never thousands. Insertion
 * sort is **stable**, needs no stack and no scratch memory, and cannot be made
 * quadratic by an adversarial input the way a naive quicksort pivot can.
 *
 * Stability is the property that actually matters here: a list of skins sorted
 * by group and then by name comes out grouped, and an unstable sort would
 * reorder equal keys differently on different runs — which reads as the list
 * shuffling itself for no reason.
 *
 * If something ever sorts a large array this should be revisited, and the
 * comment is here so that whoever does knows it was a choice.
 */
void qsort(void *base, size_t count, size_t size,
	   int (*compare)(const void *, const void *))
{
	unsigned char *at = base;
	size_t i, j;

	if (count < 2 || size == 0 || compare == NULL) {
		return;
	}

	for (i = 1; i < count; i++) {
		for (j = i; j > 0; j--) {
			unsigned char *left = at + (j - 1) * size;
			unsigned char *right = at + j * size;

			if (compare(left, right) <= 0) {
				break;
			}
			swap_bytes(left, right, size);
		}
	}
}

/* --- The two with no answer on this kernel --- */

/*
 * There is no environment.
 *
 * Twenty call sites read one, and every one of them is asking the *host* --
 * `HOME`, `XDG_RUNTIME_DIR`, `WAYLAND_DISPLAY`, `RECONOS_ROOT` -- which are
 * questions about being a Linux program. On ReconOS's own kernel a process is
 * started by the kernel with no environment at all, so the honest answer is
 * that there is nothing of that name.
 *
 * Returning NULL rather than an empty string is deliberate: the two mean
 * different things, every caller already handles a missing variable, and an
 * empty string would be read as "it is set, to nothing" and used as a path.
 */
char *getenv(const char *name)
{
	(void)name;
	return (char *)0;
}

/*
 * `exit` ends the process and does not return.
 *
 * No atexit handlers and no stream flushing, because there is no atexit and
 * the FILE layer flushes on close. A program that wants its output on the
 * screen closes its files, which is what the two callers do.
 */
void exit(int code)
{
	extern void recon_sys_exit(int code);

	recon_sys_exit(code);
	for (;;) {
	}
}
