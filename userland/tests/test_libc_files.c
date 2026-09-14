/*
 * The file layer, the number parsing and the character classes, all checked
 * against the host's.
 *
 * The `FILE` half is the reason `userland/tests/hostsys.c` exists. Without it
 * the only way to find out whether `fgets` keeps its newline, or whether
 * `fseek` from the current position accounts for what is sitting unread in the
 * buffer, would be to boot a kernel and look at the consequences -- and those
 * are precisely the faults that do not announce themselves. A file read one
 * byte short looks like a file that is one byte short.
 *
 * So here both libraries open the same file, read it the same way, and are
 * required to return the same bytes, the same counts and the same positions.
 *
 * Run with: ./build/recon_libc_file_tests
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ReconOS's, renamed by prefix.h when libc/ was compiled. */
struct recon_stream;

struct recon_stream *recon_fopen(const char *path, const char *mode);
int recon_fclose(struct recon_stream *f);
int recon_fflush(struct recon_stream *f);
unsigned long recon_fread(void *into, unsigned long size, unsigned long count,
			  struct recon_stream *f);
unsigned long recon_fwrite(const void *from, unsigned long size,
			   unsigned long count, struct recon_stream *f);
char *recon_fgets(char *into, int room, struct recon_stream *f);
int recon_fgetc(struct recon_stream *f);
int recon_fseek(struct recon_stream *f, long offset, int from);
long recon_ftell(struct recon_stream *f);
void recon_rewind(struct recon_stream *f);

int recon_isalnum(int c);
int recon_isalpha(int c);
int recon_isdigit(int c);
int recon_isspace(int c);
int recon_isupper(int c);
int recon_islower(int c);
int recon_isxdigit(int c);
int recon_isprint(int c);
int recon_toupper(int c);
int recon_tolower(int c);

int recon_atoi(const char *text);
long recon_strtol(const char *text, char **end, int base);
unsigned long recon_strtoul(const char *text, char **end, int base);
unsigned long long recon_strtoull(const char *text, char **end, int base);
double recon_strtod(const char *text, char **end);
int recon_abs(int value);
void recon_qsort(void *base, size_t count, size_t size,
		 int (*compare)(const void *, const void *));
char *recon_getenv(const char *name);

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

static void same_long(long mine, long theirs, const char *what,
		      const char *on)
{
	g_checks++;
	if (mine != theirs) {
		g_failures++;
		printf("  FAIL: %s%s%s\n    ReconOS: %ld   reference: %ld\n",
		       what, on ? " on " : "", on ? on : "", mine, theirs);
	}
}

/* --- Character classes --- */

static void test_the_character_classes(void)
{
	int c;

	printf("every byte value, through every classifier\n");

	/*
	 * EOF and every byte, which is the whole domain the standard defines
	 * for these: the argument has to be representable as an `unsigned
	 * char` or be EOF, and anything else is undefined. Over that range the
	 * two are held to exact equality, including the return of `toupper`
	 * and `tolower` rather than only their sign.
	 *
	 * The range *below* -1 is a separate question with a separate answer,
	 * immediately after this.
	 */
	for (c = -1; c < 256; c++) {
		char label[32];

		snprintf(label, sizeof(label), "byte %d", c);

		same_long(!!recon_isdigit(c), !!isdigit(c), "isdigit", label);
		same_long(!!recon_isalpha(c), !!isalpha(c), "isalpha", label);
		same_long(!!recon_isalnum(c), !!isalnum(c), "isalnum", label);
		same_long(!!recon_isspace(c), !!isspace(c), "isspace", label);
		same_long(!!recon_isupper(c), !!isupper(c), "isupper", label);
		same_long(!!recon_islower(c), !!islower(c), "islower", label);
		same_long(!!recon_isxdigit(c), !!isxdigit(c), "isxdigit",
			  label);
		same_long(!!recon_isprint(c), !!isprint(c), "isprint", label);
		same_long(recon_toupper(c), toupper(c), "toupper", label);
		same_long(recon_tolower(c), tolower(c), "tolower", label);
	}
}

/*
 * A caller that passes a plain `char` hands over a negative number for every
 * byte above 127 -- which is every non-English character in a UTF-8 system.
 * The standard says that is undefined, and it means it: glibc reaches into a
 * table at a negative index, which it arranges to be valid, and answers as
 * though the byte were Latin-1, so `toupper(-128)` there is 128.
 *
 * ReconOS answers differently on purpose, and the difference is asserted here
 * rather than skipped, because it is the only kind of difference this whole
 * arrangement exists to make visible.
 *
 * The reasoning is in `libc/ctype.c` and is about UTF-8: above 127 there is no
 * "the character" to be the case of, because the character is two or more
 * bytes. So `toupper` returns what it was given, unchanged, which is the only
 * answer that cannot corrupt text. Mapping -128 to 128 -- as the reference
 * does -- would rewrite the first byte of a multi-byte character into the
 * second half of a different one.
 *
 * Note what is *not* asserted as a difference: every `is...` function already
 * agrees with the reference across this range, both saying no. Only the two
 * case conversions differ.
 */
static void test_the_bytes_the_standard_does_not_define(void)
{
	int c;

	printf("bytes below -1, where ReconOS answers differently on"
	       " purpose\n");

	for (c = -128; c < -1; c++) {
		char label[32];

		snprintf(label, sizeof(label), "byte %d", c);

		/* Unchanged, rather than mapped into Latin-1. */
		same_long(recon_toupper(c), c, "toupper leaves it alone",
			  label);
		same_long(recon_tolower(c), c, "tolower leaves it alone",
			  label);

		/* And these agree with the reference anyway. */
		same_long(!!recon_isalpha(c), !!isalpha(c), "isalpha", label);
		same_long(!!recon_isdigit(c), !!isdigit(c), "isdigit", label);
		same_long(!!recon_isspace(c), !!isspace(c), "isspace", label);
		same_long(!!recon_isprint(c), !!isprint(c), "isprint", label);
	}
}

/* --- Numbers out of text --- */

static const char *const NUMBERS[] = {
	"", " ", "0", "1", "-1", "+1", "  42", "42  ", "007",
	"2147483647", "2147483648", "-2147483648", "-2147483649",
	"9223372036854775807", "9223372036854775808",
	"-9223372036854775808", "-9223372036854775809",
	"18446744073709551615", "18446744073709551616",
	"0x1F", "0X1f", "0x", "0xg", "0b101", "0777", "08",
	"abc", "12abc", "  -  7", "--7", "1 2", "+-1",
	"1e3", "1.5", ".5", "-.5", "1.", "e5", "1e", "1e+", "1e-2",
	"3.14159", "0.1", "100.001", "  2.5e2xyz",

	/*
	 * Added after the first run. The binary prefix and the hexadecimal
	 * float were both accepted by the reference and refused here, and a
	 * refusal in these functions is not a failure -- it is a different
	 * number, returned without complaint.
	 */
	"0b101", "0B101", "0b", "0b2", "-0b11", "0b1010101010101010",
	"0x1F", "0X1f", "0x.8", "0x1.8", "0x1p4", "0x1P-2", "0x1p",
	"0x1.8p1", "0xABCDEF", "0x10000000000000001",

	/* And the two words a file of measurements can contain. */
	"inf", "INF", "Inf", "infinity", "INFINITY", "-inf", "+infinity",
	"info", "infinit", "nan", "NAN", "-nan", "nan(1)", "nan(", "nan(x)",
	"nand",
};

#define NUMBER_COUNT (sizeof(NUMBERS) / sizeof(NUMBERS[0]))

static void test_the_number_parsing(void)
{
	size_t i;
	int base;

	printf("numbers out of text, including the ones that are not\n");

	for (i = 0; i < NUMBER_COUNT; i++) {
		const char *text = NUMBERS[i];

		same_long(recon_atoi(text), atoi(text), "atoi", text);

		/*
		 * Base 0 is the interesting one -- it decides for itself
		 * between decimal, octal and hexadecimal, and the rule for
		 * when a "0x" prefix counts is the part implementations get
		 * wrong. "0x" alone is the number zero followed by an 'x'.
		 */
		for (base = 0; base <= 16; base++) {
			char *mine_end = NULL;
			char *their_end = NULL;
			long mine, theirs;
			unsigned long umine, utheirs;

			if (base == 1) {
				continue;	/* not a base */
			}

			mine = recon_strtol(text, &mine_end, base);
			theirs = strtol(text, &their_end, base);
			same_long(mine, theirs, "strtol", text);
			same_long(mine_end - text, their_end - text,
				  "strtol stopped in the same place", text);

			umine = recon_strtoul(text, &mine_end, base);
			utheirs = strtoul(text, &their_end, base);
			same_long((long)umine, (long)utheirs, "strtoul", text);
			same_long(mine_end - text, their_end - text,
				  "strtoul stopped in the same place", text);

			/*
			 * `strtoull` saturates at the 64-bit limit whatever a
			 * `long` happens to be, which on this host is the same
			 * number -- so the corpus that matters here is the one
			 * straddling it: 18446744073709551615 and the value
			 * one past it are both in the list above.
			 */
			{
				unsigned long long wmine =
					recon_strtoull(text, &mine_end, base);
				unsigned long long wtheirs =
					strtoull(text, &their_end, base);

				g_checks++;
				if (wmine != wtheirs) {
					g_failures++;
					printf("  FAIL: strtoull on \"%s\""
					       " base %d\n    ReconOS: %llu"
					       "   reference: %llu\n",
					       text, base, wmine, wtheirs);
				}
				same_long(mine_end - text, their_end - text,
					  "strtoull stopped in the same place",
					  text);
			}
		}
	}

	same_long(recon_abs(-7), abs(-7), "abs", NULL);
	same_long(recon_abs(7), abs(7), "abs", NULL);
	check(recon_getenv("HOME") == NULL,
	      "getenv answers nothing, because there is no environment");
}

static void test_the_fraction_parsing(void)
{
	size_t i;

	printf("strtod, held to the bound printf.c states\n");

	for (i = 0; i < NUMBER_COUNT; i++) {
		const char *text = NUMBERS[i];
		char *mine_end = NULL;
		char *their_end = NULL;
		double mine = recon_strtod(text, &mine_end);
		double theirs = strtod(text, &their_end);
		double gap;

		/*
		 * Where it stopped must match exactly -- that is a decision
		 * about the text, not about arithmetic, and a parser that
		 * consumed a different number of characters would leave the
		 * caller reading from the wrong place.
		 */
		same_long(mine_end - text, their_end - text,
			  "strtod stopped in the same place", text);

		/*
		 * Infinity and not-a-number are compared first and by hand,
		 * because a tolerance cannot see them: infinity minus infinity
		 * is not-a-number, not-a-number compares false against
		 * everything including itself, and `gap > bound` is therefore
		 * false in exactly the cases where the two disagree most. A
		 * tolerance check alone would pass silently on a function that
		 * returned not-a-number for every input.
		 */
		g_checks++;
		if (mine != mine || theirs != theirs) {
			if ((mine != mine) != (theirs != theirs)) {
				g_failures++;
				printf("  FAIL: strtod of \"%s\": one of them"
				       " is not a number and the other is"
				       " %.17g\n", text,
				       mine != mine ? theirs : mine);
			}
			continue;
		}
		if (mine == theirs) {
			continue;	/* including both infinite, same sign */
		}

		/*
		 * Otherwise the value is held to a relative tolerance rather
		 * than to equality, for the reason `printf.c` gives at length:
		 * the decimal path accumulates and scales in floating point
		 * where the reference converts exactly, so the two can differ
		 * in the last bit. The bound is checked rather than the
		 * difference being waved through.
		 */
		gap = mine - theirs;
		if (gap < 0) {
			gap = -gap;
		}
		{
			double scale = theirs < 0 ? -theirs : theirs;

			if (scale < 1.0) {
				scale = 1.0;
			}
			if (gap > scale * 1e-12) {
				g_failures++;
				printf("  FAIL: strtod of \"%s\"\n"
				       "    ReconOS: %.17g   reference:"
				       " %.17g\n", text, mine, theirs);
			}
		}
	}
}

/* --- Sorting --- */

static int by_int(const void *a, const void *b)
{
	int x = *(const int *)a;
	int y = *(const int *)b;

	return (x > y) - (x < y);
}

static void test_sorting(void)
{
	int n;

	printf("qsort, against the host's, on arrays that are awkward\n");

	for (n = 0; n <= 40; n++) {
		int mine[40];
		int theirs[40];
		int i;

		/* Already sorted, reversed, all equal, and a scramble -- the
		 * four shapes that break a sort in different ways. */
		for (i = 0; i < n; i++) {
			mine[i] = i;
			theirs[i] = i;
		}
		recon_qsort(mine, (size_t)n, sizeof(int), by_int);
		qsort(theirs, (size_t)n, sizeof(int), by_int);
		check(memcmp(mine, theirs, sizeof(int) * (size_t)n) == 0,
		      "qsort of an already-sorted array");

		for (i = 0; i < n; i++) {
			mine[i] = n - i;
			theirs[i] = n - i;
		}
		recon_qsort(mine, (size_t)n, sizeof(int), by_int);
		qsort(theirs, (size_t)n, sizeof(int), by_int);
		check(memcmp(mine, theirs, sizeof(int) * (size_t)n) == 0,
		      "qsort of a reversed array");

		for (i = 0; i < n; i++) {
			mine[i] = 5;
			theirs[i] = 5;
		}
		recon_qsort(mine, (size_t)n, sizeof(int), by_int);
		qsort(theirs, (size_t)n, sizeof(int), by_int);
		check(memcmp(mine, theirs, sizeof(int) * (size_t)n) == 0,
		      "qsort where every element is equal");

		for (i = 0; i < n; i++) {
			mine[i] = (i * 7919) % 101;
			theirs[i] = mine[i];
		}
		recon_qsort(mine, (size_t)n, sizeof(int), by_int);
		qsort(theirs, (size_t)n, sizeof(int), by_int);
		check(memcmp(mine, theirs, sizeof(int) * (size_t)n) == 0,
		      "qsort of a scrambled array");
	}
}

/* --- Files --- */

#define FIXTURE "/tmp/recon-libc-fixture.txt"

/*
 * A file with the awkward parts in it: a line with no newline at the end, an
 * empty line, a line longer than any buffer a caller is likely to offer, and
 * bytes above 127.
 */
static void write_the_fixture(void)
{
	FILE *f = fopen(FIXTURE, "wb");
	int i;

	if (f == NULL) {
		printf("  could not write the fixture\n");
		return;
	}
	fputs("first line\n", f);
	fputs("\n", f);
	fputs("third, with a caf\xC3\xA9 in it\n", f);
	for (i = 0; i < 300; i++) {
		fputc('x', f);
	}
	fputs("\n", f);
	fputs("last line with no newline", f);
	fclose(f);
}

static void test_reading_a_file(void)
{
	struct recon_stream *mine;
	FILE *theirs;
	int line;

	printf("reading the same file, both ways, byte for byte\n");

	write_the_fixture();

	/* --- fgets, at several buffer sizes --- */
	{
		int room;

		for (room = 2; room <= 64; room += 7) {
			char a[128];
			char b[128];

			mine = recon_fopen(FIXTURE, "r");
			theirs = fopen(FIXTURE, "r");
			check(mine != NULL && theirs != NULL,
			      "both opened the fixture");
			if (mine == NULL || theirs == NULL) {
				return;
			}

			for (line = 0; line < 40; line++) {
				char *ra = recon_fgets(a, room, mine);
				char *rb = fgets(b, room, theirs);

				g_checks++;
				if ((ra == NULL) != (rb == NULL)) {
					g_failures++;
					printf("  FAIL: fgets disagreed about"
					       " the end at room %d, line"
					       " %d\n", room, line);
					break;
				}
				if (ra == NULL) {
					break;
				}
				g_checks++;
				if (strcmp(a, b) != 0) {
					g_failures++;
					printf("  FAIL: fgets at room %d line"
					       " %d\n    ReconOS: \"%s\"\n"
					       "    reference: \"%s\"\n",
					       room, line, a, b);
					break;
				}
				same_long(recon_ftell(mine), ftell(theirs),
					  "ftell after fgets", NULL);
			}

			recon_fclose(mine);
			fclose(theirs);
		}
	}

	/* --- fread, at several item sizes --- */
	{
		unsigned long size;

		for (size = 1; size <= 7; size++) {
			unsigned char a[512];
			unsigned char b[512];
			unsigned long ra, rb;

			mine = recon_fopen(FIXTURE, "rb");
			theirs = fopen(FIXTURE, "rb");
			if (mine == NULL || theirs == NULL) {
				return;
			}

			memset(a, 0, sizeof(a));
			memset(b, 0, sizeof(b));
			ra = recon_fread(a, size, 40, mine);
			rb = fread(b, size, 40, theirs);

			same_long((long)ra, (long)rb,
				  "fread returned the same item count", NULL);
			check(memcmp(a, b, sizeof(a)) == 0,
			      "fread put the same bytes in the buffer");
			same_long(recon_ftell(mine), ftell(theirs),
				  "ftell after fread", NULL);

			recon_fclose(mine);
			fclose(theirs);
		}
	}

	/* --- seeking, which is where the buffer makes it interesting --- */
	{
		long where;

		mine = recon_fopen(FIXTURE, "rb");
		theirs = fopen(FIXTURE, "rb");
		if (mine == NULL || theirs == NULL) {
			return;
		}

		for (where = 0; where < 60; where += 3) {
			char a[16];
			char b[16];

			check(recon_fseek(mine, where, SEEK_SET) ==
			      fseek(theirs, where, SEEK_SET),
			      "fseek from the start agreed");
			same_long(recon_ftell(mine), ftell(theirs),
				  "ftell after seeking from the start", NULL);

			recon_fread(a, 1, sizeof(a), mine);
			fread(b, 1, sizeof(b), theirs);
			check(memcmp(a, b, sizeof(a)) == 0,
			      "the same bytes after a seek");

			/*
			 * From the current position, which is the one that
			 * catches a layer that forgot its read-ahead: the
			 * descriptor is further on than the caller, so handing
			 * the offset straight to the kernel lands in the wrong
			 * place.
			 */
			recon_fseek(mine, -4, SEEK_CUR);
			fseek(theirs, -4, SEEK_CUR);
			same_long(recon_ftell(mine), ftell(theirs),
				  "ftell after seeking from where it was",
				  NULL);

			recon_fread(a, 1, 4, mine);
			fread(b, 1, 4, theirs);
			check(memcmp(a, b, 4) == 0,
			      "the same bytes after a relative seek");
		}

		recon_rewind(mine);
		rewind(theirs);
		same_long(recon_ftell(mine), ftell(theirs),
			  "ftell after rewind", NULL);

		recon_fclose(mine);
		fclose(theirs);
	}

	/* --- fgetc to the very end --- */
	{
		int n;

		mine = recon_fopen(FIXTURE, "rb");
		theirs = fopen(FIXTURE, "rb");
		if (mine == NULL || theirs == NULL) {
			return;
		}
		for (n = 0; n < 4096; n++) {
			int a = recon_fgetc(mine);
			int b = fgetc(theirs);

			same_long(a, b, "fgetc", NULL);
			if (a < 0 || b < 0) {
				break;
			}
		}
		recon_fclose(mine);
		fclose(theirs);
	}
}

static void test_writing_a_file(void)
{
	static const char PAYLOAD[] =
		"a line\nand another\n\nand one with a caf\xC3\xA9\n";
	struct recon_stream *mine;
	FILE *theirs;
	char a[4096];
	char b[4096];
	unsigned long na, nb;
	int i;

	printf("writing the same bytes, both ways\n");

	mine = recon_fopen("/tmp/recon-libc-mine.txt", "wb");
	theirs = fopen("/tmp/recon-libc-theirs.txt", "wb");
	check(mine != NULL && theirs != NULL, "both opened for writing");
	if (mine == NULL || theirs == NULL) {
		return;
	}

	/* Several small writes, then one larger than the buffer -- which is
	 * the case that takes the straight-to-the-kernel path. */
	for (i = 0; i < 20; i++) {
		recon_fwrite(PAYLOAD, 1, sizeof(PAYLOAD) - 1, mine);
		fwrite(PAYLOAD, 1, sizeof(PAYLOAD) - 1, theirs);
	}
	{
		static char big[8192];

		memset(big, 'Z', sizeof(big));
		recon_fwrite(big, 1, sizeof(big), mine);
		fwrite(big, 1, sizeof(big), theirs);
	}

	check(recon_fclose(mine) == 0, "closing flushed what was buffered");
	fclose(theirs);

	/* And the two files are the same file. */
	{
		FILE *ra = fopen("/tmp/recon-libc-mine.txt", "rb");
		FILE *rb = fopen("/tmp/recon-libc-theirs.txt", "rb");

		if (ra == NULL || rb == NULL) {
			check(0, "could not read back what was written");
			return;
		}
		for (;;) {
			na = fread(a, 1, sizeof(a), ra);
			nb = fread(b, 1, sizeof(b), rb);
			same_long((long)na, (long)nb,
				  "the two files are the same length", NULL);
			if (na == 0 || na != nb) {
				break;
			}
			check(memcmp(a, b, na) == 0,
			      "the two files hold the same bytes");
		}
		fclose(ra);
		fclose(rb);
	}

	unlink("/tmp/recon-libc-mine.txt");
	unlink("/tmp/recon-libc-theirs.txt");
}

static void test_what_it_refuses(void)
{
	struct recon_stream *f;

	printf("the modes it will not open, and the limit it has\n");

	/*
	 * Append is refused rather than being treated as the nearest thing it
	 * has. A program that asked to append and was given truncate destroys
	 * the file it meant to add to.
	 */
	f = recon_fopen(FIXTURE, "a");
	check(f == NULL, "append is refused, not silently turned into write");

	f = recon_fopen(FIXTURE, "r+");
	check(f == NULL, "read-and-write is refused rather than half-done");

	f = recon_fopen("/tmp/recon-no-such-file-at-all", "r");
	check(f == NULL, "a file that is not there does not open");

	/*
	 * Sixteen streams, and the seventeenth is refused. The limit exists
	 * because there is no allocator; the point of testing it is that the
	 * refusal is clean rather than a table overrun.
	 */
	{
		struct recon_stream *held[24];
		int i;
		int opened = 0;

		for (i = 0; i < 24; i++) {
			held[i] = recon_fopen(FIXTURE, "rb");
			if (held[i] != NULL) {
				opened++;
			}
		}
		check(opened == 16,
		      "sixteen streams open, and the rest are refused");
		for (i = 0; i < 24; i++) {
			if (held[i] != NULL) {
				recon_fclose(held[i]);
			}
		}

		/* And closing them gives them back, rather than leaking the
		 * table one run at a time. */
		f = recon_fopen(FIXTURE, "rb");
		check(f != NULL, "and closing them gives the table back");
		if (f != NULL) {
			recon_fclose(f);
		}
	}

	unlink(FIXTURE);
}

int main(void)
{
	printf("ReconOS C library: files, numbers and characters\n\n");

	test_the_character_classes();
	test_the_bytes_the_standard_does_not_define();
	test_the_number_parsing();
	test_the_fraction_parsing();
	test_sorting();
	test_reading_a_file();
	test_writing_a_file();
	test_what_it_refuses();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
