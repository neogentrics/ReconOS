/*
 * The ReconOS C library, checked against the host's.
 *
 * These functions are called about two and a half thousand times across the
 * desktop, and every one of those calls currently reaches glibc. When the
 * desktop is built for this kernel they will reach the code in
 * `userland/libc/` instead, and **any difference in behaviour becomes a
 * difference in what ReconOS does** -- not a crash, in most cases, but a path
 * compared wrong, a size displayed wrong, a header matched wrong.
 *
 * So this does not test that they work. It tests that they answer *the same
 * thing the reference answers*, over a corpus that includes the cases nobody
 * writing a test from memory would think of: empty strings, overlapping
 * ranges, embedded NULs, lengths of zero, bytes above 127, the most negative
 * integer, a precision longer than the string, a width shorter than the
 * number.
 *
 * Where ReconOS deliberately differs from the reference, the difference is
 * asserted rather than skipped -- see `test_where_it_differs_on_purpose`.
 *
 * Run with: ./build/recon_libc_tests
 */

#define _GNU_SOURCE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The ReconOS versions, renamed by prefix.h when libc/ was compiled. */
void *recon_memcpy(void *to, const void *from, size_t length);
void *recon_memmove(void *to, const void *from, size_t length);
void *recon_memset(void *to, int value, size_t length);
int recon_memcmp(const void *a, const void *b, size_t length);
void *recon_memchr(const void *in, int value, size_t length);

size_t recon_strlen(const char *text);
size_t recon_strnlen(const char *text, size_t most);
int recon_strcmp(const char *a, const char *b);
int recon_strncmp(const char *a, const char *b, size_t length);
int recon_strcasecmp(const char *a, const char *b);
int recon_strncasecmp(const char *a, const char *b, size_t length);
char *recon_strcpy(char *to, const char *from);
char *recon_strncpy(char *to, const char *from, size_t length);
char *recon_strcat(char *to, const char *from);
char *recon_strchr(const char *text, int value);
char *recon_strrchr(const char *text, int value);
char *recon_strstr(const char *haystack, const char *needle);
size_t recon_strspn(const char *text, const char *of);
size_t recon_strcspn(const char *text, const char *stop);

int recon_snprintf(char *to, size_t room, const char *format, ...);

static int g_failures;
static int g_checks;

static void check(int condition, const char *what)
{
	g_checks++;
	if (!condition) {
		g_failures++;
		printf("  FAIL: %s\n", what);
	}
}

/* Both answers, and the inputs, when they differ -- because "strcmp differed"
 * without saying on what is a failure nobody can act on. */
static void same_int(long mine, long theirs, const char *what,
		     const char *a, const char *b)
{
	g_checks++;
	if (mine != theirs) {
		g_failures++;
		printf("  FAIL: %s\n    on: \"%s\" / \"%s\"\n"
		       "    ReconOS: %ld   reference: %ld\n",
		       what, a ? a : "(none)", b ? b : "(none)", mine, theirs);
	}
}

/*
 * The sign, not the value.
 *
 * The standard promises only that a comparison is negative, zero or positive
 * -- glibc returns the byte difference and other libraries return -1/0/1, and
 * a test that demanded the exact number would be testing glibc's choice rather
 * than correctness. **Every caller in ReconOS uses the sign**, so the sign is
 * what is checked.
 */
static int sign_of(int v)
{
	return (v > 0) - (v < 0);
}

/* --- The corpus --- */

static const char *const WORDS[] = {
	"",
	"a",
	"A",
	"ab",
	"abc",
	"ABC",
	"abC",
	"abcd",
	"hello",
	"Hello",
	"HELLO",
	"hello world",
	"hello,world",
	" leading",
	"trailing ",
	"/dev/fb0",
	"Content-Type",
	"content-type",
	"CONTENT-TYPE",
	"text/html; charset=utf-8",
	/* Above 127, which is where a signed `char` comparison goes backwards
	 * -- and every non-English character in this system is such a byte. */
	"caf\xC3\xA9",
	"caf\xC3\xA8",
	"\xFF",
	"\x80",
	"\x7F",
	"a\xFF" "b",
	"~",
	"0",
	"00",
	"9",
};

#define WORD_COUNT (sizeof(WORDS) / sizeof(WORDS[0]))

static void test_the_comparisons(void)
{
	size_t i, j;

	printf("every pair of the corpus, compared both ways\n");

	for (i = 0; i < WORD_COUNT; i++) {
		for (j = 0; j < WORD_COUNT; j++) {
			const char *a = WORDS[i];
			const char *b = WORDS[j];
			size_t n;

			same_int(sign_of(recon_strcmp(a, b)),
				 sign_of(strcmp(a, b)), "strcmp", a, b);
			same_int(sign_of(recon_strcasecmp(a, b)),
				 sign_of(strcasecmp(a, b)), "strcasecmp",
				 a, b);

			/* Every length from none to past both ends: the
			 * boundary cases are 0, exactly the shorter string,
			 * and one past the longer. */
			for (n = 0; n <= 12; n++) {
				same_int(sign_of(recon_strncmp(a, b, n)),
					 sign_of(strncmp(a, b, n)),
					 "strncmp", a, b);
				same_int(sign_of(recon_strncasecmp(a, b, n)),
					 sign_of(strncasecmp(a, b, n)),
					 "strncasecmp", a, b);
			}
		}
	}
}

static void test_the_lengths_and_searches(void)
{
	size_t i;

	printf("lengths, and finding a byte in a string\n");

	for (i = 0; i < WORD_COUNT; i++) {
		const char *a = WORDS[i];
		size_t n;
		int c;

		same_int((long)recon_strlen(a), (long)strlen(a), "strlen",
			 a, NULL);

		for (n = 0; n <= 12; n++) {
			same_int((long)recon_strnlen(a, n),
				 (long)strnlen(a, n), "strnlen", a, NULL);
		}

		/* Including 0, which finds the terminator rather than
		 * nothing -- callers use that to find the end in one pass. */
		for (c = 0; c < 256; c += 7) {
			const char *mine = recon_strchr(a, c);
			const char *theirs = strchr(a, c);

			same_int(mine == NULL, theirs == NULL,
				 "strchr found-or-not", a, NULL);
			if (mine && theirs) {
				same_int(mine - a, theirs - a,
					 "strchr position", a, NULL);
			}

			mine = recon_strrchr(a, c);
			theirs = strrchr(a, c);
			same_int(mine == NULL, theirs == NULL,
				 "strrchr found-or-not", a, NULL);
			if (mine && theirs) {
				same_int(mine - a, theirs - a,
					 "strrchr position", a, NULL);
			}
		}
	}

	/* strchr for 0 specifically, on every word. */
	for (i = 0; i < WORD_COUNT; i++) {
		const char *a = WORDS[i];

		same_int(recon_strchr(a, 0) - a, strchr(a, 0) - a,
			 "strchr of the terminator", a, NULL);
	}
}

static void test_strstr(void)
{
	size_t i, j;

	printf("finding one string inside another\n");

	for (i = 0; i < WORD_COUNT; i++) {
		for (j = 0; j < WORD_COUNT; j++) {
			const char *h = WORDS[i];
			const char *n = WORDS[j];
			const char *mine = recon_strstr(h, n);
			const char *theirs = strstr(h, n);

			same_int(mine == NULL, theirs == NULL,
				 "strstr found-or-not", h, n);
			if (mine && theirs) {
				same_int(mine - h, theirs - h,
					 "strstr position", h, n);
			}
		}
	}
}

static void test_the_memory_functions(void)
{
	unsigned char mine[64];
	unsigned char theirs[64];
	size_t n, at;

	printf("memory, including ranges that overlap\n");

	for (n = 0; n <= 32; n++) {
		size_t i;

		for (i = 0; i < sizeof(mine); i++) {
			mine[i] = (unsigned char)(i * 7 + 1);
			theirs[i] = mine[i];
		}
		recon_memset(mine, 0xA5, n);
		memset(theirs, 0xA5, n);
		check(memcmp(mine, theirs, sizeof(mine)) == 0,
		      "memset wrote the same bytes and no others");

		same_int(sign_of(recon_memcmp(mine, theirs, n)),
			 sign_of(memcmp(mine, theirs, n)), "memcmp",
			 NULL, NULL);
	}

	/*
	 * The one that matters: an overlap where the destination is *after*
	 * the source. Copying forwards there overwrites bytes that have not
	 * been read, and the result is the first few bytes correct and the
	 * rest a repeating pattern -- which looks almost right.
	 */
	for (at = 0; at <= 8; at++) {
		for (n = 0; n <= 16; n++) {
			size_t i;

			for (i = 0; i < sizeof(mine); i++) {
				mine[i] = (unsigned char)(i + 1);
				theirs[i] = mine[i];
			}
			recon_memmove(mine + at, mine, n);
			memmove(theirs + at, theirs, n);
			check(memcmp(mine, theirs, sizeof(mine)) == 0,
			      "memmove forwards over an overlap");

			for (i = 0; i < sizeof(mine); i++) {
				mine[i] = (unsigned char)(i + 1);
				theirs[i] = mine[i];
			}
			recon_memmove(mine, mine + at, n);
			memmove(theirs, theirs + at, n);
			check(memcmp(mine, theirs, sizeof(mine)) == 0,
			      "memmove backwards over an overlap");
		}
	}

	/* memchr, including the byte that is not there and a length of 0. */
	{
		static const unsigned char hay[] = { 1, 2, 3, 0, 4, 5, 0xFF };
		int want;

		for (want = 0; want < 256; want++) {
			for (n = 0; n <= sizeof(hay); n++) {
				const void *a = recon_memchr(hay, want, n);
				const void *b = memchr(hay, want, n);

				same_int(a == NULL, b == NULL,
					 "memchr found-or-not", NULL, NULL);
				if (a && b) {
					same_int((const char *)a -
						 (const char *)hay,
						 (const char *)b -
						 (const char *)hay,
						 "memchr position", NULL, NULL);
				}
			}
		}
	}
}

static void test_the_copies(void)
{
	size_t i, n;

	printf("copying, including strncpy's two surprises\n");

	for (i = 0; i < WORD_COUNT; i++) {
		char mine[64];
		char theirs[64];

		memset(mine, 0x5A, sizeof(mine));
		memset(theirs, 0x5A, sizeof(theirs));
		recon_strcpy(mine, WORDS[i]);
		strcpy(theirs, WORDS[i]);
		check(memcmp(mine, theirs, sizeof(mine)) == 0,
		      "strcpy wrote the same bytes and no others");

		/*
		 * strncpy does not terminate when the source fills the buffer,
		 * and pads the whole remainder with NULs when it does not.
		 * Both are surprising, both are required, and an implementation
		 * that "fixed" either would break a caller relying on it.
		 */
		for (n = 0; n <= 12; n++) {
			memset(mine, 0x5A, sizeof(mine));
			memset(theirs, 0x5A, sizeof(theirs));
			recon_strncpy(mine, WORDS[i], n);
			strncpy(theirs, WORDS[i], n);
			check(memcmp(mine, theirs, sizeof(mine)) == 0,
			      "strncpy, including its padding and its"
			      " missing terminator");
		}

		/* strcat onto something already there. */
		memset(mine, 0, sizeof(mine));
		memset(theirs, 0, sizeof(theirs));
		strcpy(mine, "start:");
		strcpy(theirs, "start:");
		recon_strcat(mine, WORDS[i]);
		strcat(theirs, WORDS[i]);
		check(memcmp(mine, theirs, sizeof(mine)) == 0, "strcat");
	}
}

/* --- snprintf --- */

/*
 * One format, both implementations, byte for byte -- and the return value,
 * which is the part ReconOS depends on most.
 *
 * The return is *what would have been written*, and a great deal of this
 * system is built on noticing it exceed the buffer: that is how a path that
 * would not fit is refused rather than shortened. An implementation that
 * returned the truncated length would make every one of those checks pass.
 */
#define SAME_FORMAT(fmt, ...)                                                 \
	do {                                                                  \
		char mine[128];                                               \
		char theirs[128];                                             \
		int a, b;                                                     \
		size_t room;                                                  \
									      \
		for (room = 0; room <= 40; room++) {                          \
			memset(mine, 0x5A, sizeof(mine));                     \
			memset(theirs, 0x5A, sizeof(theirs));                 \
			a = recon_snprintf(mine, room, fmt, __VA_ARGS__);      \
			b = snprintf(theirs, room, fmt, __VA_ARGS__);          \
			g_checks++;                                           \
			if (a != b ||                                         \
			    memcmp(mine, theirs, sizeof(mine)) != 0) {        \
				g_failures++;                                 \
				printf("  FAIL: snprintf(\"%s\") at room"     \
				       " %zu\n    ReconOS: %d \"%s\"\n"       \
				       "    reference: %d \"%s\"\n",          \
				       fmt, room, a, room ? mine : "",        \
				       b, room ? theirs : "");                \
				break;                                        \
			}                                                     \
		}                                                             \
	} while (0)

static void test_printf_strings(void)
{
	size_t i;

	printf("%%s, which is two thousand of the calls in this system\n");

	for (i = 0; i < WORD_COUNT; i++) {
		SAME_FORMAT("%s", WORDS[i]);
		SAME_FORMAT("[%s]", WORDS[i]);
		SAME_FORMAT("%10s", WORDS[i]);
		SAME_FORMAT("%-10s", WORDS[i]);
		SAME_FORMAT("%.3s", WORDS[i]);
		SAME_FORMAT("%.0s", WORDS[i]);
		SAME_FORMAT("%12.4s", WORDS[i]);
		SAME_FORMAT("%-12.4s", WORDS[i]);
		SAME_FORMAT("%.*s", 4, WORDS[i]);
		SAME_FORMAT("%*s", 9, WORDS[i]);
		SAME_FORMAT("%*s", -9, WORDS[i]);
		SAME_FORMAT("a%sb%sc", WORDS[i], WORDS[i]);
	}
}

/*
 * The compiler warns about `%08.3d` -- the zero flag is ignored when a
 * precision is given. That is the rule being checked, so the warning is
 * turned off here rather than the check being dropped: an implementation that
 * got it wrong would pad with zeros, and nothing else in this suite would
 * notice.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"

static void test_printf_integers(void)
{
	static const long long VALUES[] = {
		0, 1, -1, 7, -7, 9, 10, 99, 100, 255, 256, 1000, 65535,
		65536, 123456789, -123456789, 2147483647, -2147483647,
		(long long)-2147483647 - 1,
		4294967295LL, 4294967296LL,
		9223372036854775807LL,
		/* The most negative value, whose positive counterpart does not
		 * exist -- negating it is undefined, and an implementation
		 * that does prints something wrong or crashes. */
		-9223372036854775807LL - 1,
	};
	size_t i;

	printf("integers, including the one with no positive counterpart\n");

	for (i = 0; i < sizeof(VALUES) / sizeof(VALUES[0]); i++) {
		long long v = VALUES[i];
		int as_int = (int)v;
		unsigned long long u = (unsigned long long)v;

		SAME_FORMAT("%lld", v);
		SAME_FORMAT("%20lld", v);
		SAME_FORMAT("%-20lld", v);
		SAME_FORMAT("%020lld", v);
		SAME_FORMAT("%+lld", v);
		SAME_FORMAT("% lld", v);
		SAME_FORMAT("%llu", u);
		SAME_FORMAT("%llx", u);
		SAME_FORMAT("%llX", u);
		SAME_FORMAT("%#llx", u);
		SAME_FORMAT("%llo", u);

		SAME_FORMAT("%d", as_int);
		SAME_FORMAT("%8d", as_int);
		SAME_FORMAT("%-8d", as_int);
		SAME_FORMAT("%08d", as_int);
		SAME_FORMAT("%.5d", as_int);
		SAME_FORMAT("%8.5d", as_int);
		/* A precision turns zero padding off, which is the rule most
		 * implementations written from memory get wrong. */
		SAME_FORMAT("%08.3d", as_int);
		SAME_FORMAT("%*d", 7, as_int);

		SAME_FORMAT("%u", (unsigned)as_int);
		SAME_FORMAT("%x", (unsigned)as_int);
		SAME_FORMAT("%zu", (size_t)u);
	}
}

#pragma GCC diagnostic pop

static void test_printf_the_rest(void)
{
	printf("characters, percent signs, and the awkward formats\n");

	SAME_FORMAT("%c", 'x');
	SAME_FORMAT("%5c", 'x');
	SAME_FORMAT("%-5c", 'x');
	SAME_FORMAT("100%% of %s", "it");
	SAME_FORMAT("%s%%%s", "a", "b");
	SAME_FORMAT("no conversions at all%s", "");
	SAME_FORMAT("%s and %d and %s", "one", 2, "three");

	/* A null string. glibc prints "(null)" and so does this -- and the
	 * reason is not politeness: faulting here takes down whatever was
	 * trying to report something, which is usually an error path. */
	{
		const char *nothing = NULL;

		SAME_FORMAT("%s", nothing);
		SAME_FORMAT("[%10s]", nothing);
	}
}

static void test_printf_fractions(void)
{
	/*
	 * Exact binary fractions only, and that restriction is the honest half
	 * of this test rather than a way of avoiding a failure.
	 *
	 * A value like 0.05 is not 0.05 as a double. The reference converts
	 * exactly and knows it is a hair above a half; this scales in floating
	 * point, where that is lost. Those cases are checked separately, below,
	 * against the bound that is documented in printf.c -- and they are
	 * checked, not skipped.
	 */
	static const double VALUES[] = {
		0.0, 1.0, -1.0, 0.5, -0.5, 0.25, 0.125, 0.375,
		1.5, 2.5, 3.5, 4.5, -1.5, -2.5,
		1023.5, 1024.0, 1048576.0, 1073741824.0,
		255.75, 0.0625,
	};
	size_t i;

	printf("%%f on exact binary fractions, against the reference\n");

	for (i = 0; i < sizeof(VALUES) / sizeof(VALUES[0]); i++) {
		double v = VALUES[i];

		SAME_FORMAT("%.0f", v);
		SAME_FORMAT("%.1f", v);
		SAME_FORMAT("%.2f", v);
		SAME_FORMAT("%.3f", v);
		SAME_FORMAT("%8.2f", v);
		SAME_FORMAT("%-8.2f", v);
		SAME_FORMAT("%+.2f", v);
	}
}

/*
 * The values that are not exactly representable, held to a stated bound.
 *
 * Not skipped. The claim in printf.c is that such a value may differ by one in
 * the last printed digit and by no more than that, and a claim nobody checks
 * is a comment. This checks it -- and it would fail if the conversion were
 * wrong by two, or wrong in the integer part, or wrong in a way that is not
 * about the final rounding at all.
 */
static void test_the_inexact_decimals(void)
{
	static const double VALUES[] = {
		0.05, 0.1, 0.2, 0.3, 0.7, 99.994, 99.995, 99.999,
		0.999, 0.9999, 3.14159265358979, 123456.789, 2.675,
	};
	static const int PLACES[] = { 0, 1, 2, 3 };
	size_t i, p;

	printf("values with no exact binary form, held to one in the last"
	       " place\n");

	for (i = 0; i < sizeof(VALUES) / sizeof(VALUES[0]); i++) {
		for (p = 0; p < sizeof(PLACES) / sizeof(PLACES[0]); p++) {
			char mine[64];
			char theirs[64];
			char format[8];
			long a, b;
			long scale = 1;
			int k;

			snprintf(format, sizeof(format), "%%.%df", PLACES[p]);
			recon_snprintf(mine, sizeof(mine), format, VALUES[i]);
			snprintf(theirs, sizeof(theirs), format, VALUES[i]);

			/* Compare as scaled integers, so "one in the last
			 * place" is a number rather than a string difference. */
			for (k = 0; k < PLACES[p]; k++) {
				scale *= 10;
			}
			a = (long)(atof(mine) * scale + 0.5);
			b = (long)(atof(theirs) * scale + 0.5);

			g_checks++;
			if (a > b + 1 || b > a + 1) {
				g_failures++;
				printf("  FAIL: %s of %g differs by more than"
				       " one in the last place\n"
				       "    ReconOS: %s   reference: %s\n",
				       format, VALUES[i], mine, theirs);
			}
		}
	}
}

/*
 * Where ReconOS differs on purpose.
 *
 * Asserted rather than skipped. A difference that is a decision should fail
 * the day somebody changes it without meaning to, exactly like a difference
 * that is a bug.
 */
static void test_where_it_differs_on_purpose(void)
{
	char mine[64];

	printf("the two places this deliberately does not match\n");

	/*
	 * A conversion this does not have comes back verbatim rather than
	 * being swallowed, and the argument is not consumed -- because this
	 * does not know what type it was, and guessing would put every
	 * argument after it one place out.
	 */
	recon_snprintf(mine, sizeof(mine), "a%qb", 1);
	check(strcmp(mine, "a%qb") == 0,
	      "an unknown conversion is written back out, so it can be found");

	/*
	 * A double too large for the fixed-point conversion says so rather
	 * than printing something wrong. Every caller of %f in this system is
	 * putting a quantity in front of a person; none is a serialiser, and
	 * "huge" is a better answer to a person than a wrong number.
	 */
	recon_snprintf(mine, sizeof(mine), "%.2f", 1e30);
	check(strcmp(mine, "huge") == 0,
	      "a value past the fixed-point range says so");
}

int main(void)
{
	printf("ReconOS C library, against the host's\n\n");

	test_the_comparisons();
	test_the_lengths_and_searches();
	test_strstr();
	test_the_memory_functions();
	test_the_copies();
	test_printf_strings();
	test_printf_integers();
	test_printf_the_rest();
	test_printf_fractions();
	test_the_inexact_decimals();
	test_where_it_differs_on_purpose();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
