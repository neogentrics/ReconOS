/*
 * The file layer, the number parsing, the character classes and the calendar,
 * all checked against the host's.
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
#include <locale.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
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

/*
 * The time half, and note what is *not* declared here: a struct.
 *
 * These take the **host's** `struct tm`. ReconOS's has the same nine fields in
 * the same order and nothing more, and glibc's carries two GNU extensions
 * after them which nothing here writes -- so handing one to the other is safe,
 * and it turns "do the layouts agree" from a thing to be asserted into a thing
 * the suite finds out. A field in the wrong order does not read as a style
 * difference; it reads as a wrong date on the first run.
 */
int recon_puts(const char *text);

long long recon_libc_time(long long *out);
int recon_clock_gettime(int which, struct timespec *into);
double recon_difftime(long long later, long long earlier);
struct tm *recon_gmtime_r(const long long *when, struct tm *into);
struct tm *recon_localtime_r(const long long *when, struct tm *into);
size_t recon_strftime(char *into, size_t room, const char *format,
		      const struct tm *parts);

#define RECON_CLOCK_REALTIME  0
#define RECON_CLOCK_MONOTONIC 1

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

/*
 * `puts` writes to standard output, so it cannot be compared against the
 * host's the way everything else here is -- both would write to the same
 * place and neither could see what the other put there.
 *
 * What can be checked is what actually matters about it: that it appends the
 * newline, which is the entire difference between it and a write, and that it
 * refuses a null pointer rather than faulting on one. The bytes are read back
 * off the descriptor by pointing standard output at a file for the duration.
 */
static void test_puts(void)
{
	static const char PATH[] = "/tmp/recon-libc-puts.txt";
	int saved;
	int redirected;
	FILE *back;
	char got[128];
	size_t n;

	printf("puts, which nothing in the desktop calls by name\n");

	check(recon_puts(NULL) < 0, "a null pointer is refused, not followed");

	fflush(stdout);
	saved = dup(1);
	redirected = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (saved < 0 || redirected < 0) {
		check(0, "could not redirect standard output");
		return;
	}
	dup2(redirected, 1);

	recon_puts("first");
	recon_puts("");
	recon_puts("third with a caf\xC3\xA9");

	fflush(stdout);
	dup2(saved, 1);
	close(saved);
	close(redirected);

	back = fopen(PATH, "rb");
	if (back == NULL) {
		check(0, "could not read back what puts wrote");
		return;
	}
	n = fread(got, 1, sizeof(got) - 1, back);
	got[n] = '\0';
	fclose(back);
	unlink(PATH);

	check(strcmp(got, "first\n\nthird with a caf\xC3\xA9\n") == 0,
	      "puts wrote each line with a newline after it");
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

/* --- Time --- */

/*
 * The corpus is the awkward dates rather than a range of plausible ones.
 *
 * A calendar is wrong at boundaries or nowhere: the day a leap second is not,
 * the century that is divisible by four and is not a leap year, the second
 * before an epoch, the day a year ends on a Sunday. A sweep of ordinary
 * timestamps agrees with anything.
 */
static const long long MOMENTS[] = {
	0LL,			/* the epoch, a Thursday */
	1LL, -1LL, 59LL, 60LL, 61LL,
	86399LL, 86400LL, 86401LL,
	-86399LL, -86400LL, -86401LL,

	/* 1970 and the years around it, from both sides. */
	31535999LL, 31536000LL,		/* 1970-12-31 / 1971-01-01 */
	-2208988800LL,			/* 1900-01-01, not a leap year */
	-2203891200LL,			/* 1900-03-01 */

	/* The leap day that is, and the century leap that is not. */
	951782400LL,			/* 2000-02-29 */
	951868800LL,			/* 2000-03-01 */
	4107542400LL,			/* 2100-02-28 */
	4107628800LL,			/* 2100-03-01 -- 2100 is not a leap */

	/* Where a 32-bit time_t stops, and one second either side. */
	2147483647LL, 2147483648LL, -2147483648LL, -2147483649LL,

	/* Far out, but inside what the reference will answer for. */
	253402300799LL,			/* 9999-12-31 23:59:59 */
	-62135596800LL,			/* 0001-01-01 00:00:00 */

	/*
	 * Years with fewer than four digits, which is where %Y, %C and %F
	 * stop agreeing with what the standard's wording suggests. The
	 * corpus reached year 1 by accident and that accident found two
	 * faults; these are here so it is no longer an accident.
	 */
	-62104233600LL,		/* 0001-12-31 */
	-62009884800LL,		/* 0005-01-01 */
	-60589296000LL,		/* 0050-01-01 */
	-59011459200LL,		/* 0100-01-01 */
	-52540358400LL,		/* 0305-01-01 */
	-30641760000LL,		/* 0999-01-01 */
	-30610224000LL,		/* 1000-01-01 */

	/* And a few ordinary ones, because the ordinary case has to work too. */
	1757808000LL, 1000000000LL, 1234567890LL, -1234567890LL,
};

#define MOMENT_COUNT (sizeof(MOMENTS) / sizeof(MOMENTS[0]))

static void same_field(int mine, int theirs, const char *field, long long when)
{
	g_checks++;
	if (mine != theirs) {
		g_failures++;
		printf("  FAIL: %s at %lld\n    ReconOS: %d   reference: %d\n",
		       field, when, mine, theirs);
	}
}

static void check_one_moment(long long when)
{
	struct tm mine;
	struct tm theirs;
	time_t host_when = (time_t)when;

	memset(&mine, 0, sizeof(mine));
	memset(&theirs, 0, sizeof(theirs));

	if (gmtime_r(&host_when, &theirs) == NULL) {
		return;		/* outside what the reference answers for */
	}
	g_checks++;
	if (recon_gmtime_r(&when, &mine) == NULL) {
		g_failures++;
		printf("  FAIL: gmtime_r refused %lld and the reference did"
		       " not\n", when);
		return;
	}

	same_field(mine.tm_sec, theirs.tm_sec, "tm_sec", when);
	same_field(mine.tm_min, theirs.tm_min, "tm_min", when);
	same_field(mine.tm_hour, theirs.tm_hour, "tm_hour", when);
	same_field(mine.tm_mday, theirs.tm_mday, "tm_mday", when);
	same_field(mine.tm_mon, theirs.tm_mon, "tm_mon", when);
	same_field(mine.tm_year, theirs.tm_year, "tm_year", when);
	same_field(mine.tm_wday, theirs.tm_wday, "tm_wday", when);
	same_field(mine.tm_yday, theirs.tm_yday, "tm_yday", when);
	same_field(mine.tm_isdst, theirs.tm_isdst, "tm_isdst", when);
}

static void test_the_calendar(void)
{
	size_t i;
	long long step;

	printf("gmtime_r, on the dates a calendar is wrong on\n");

	for (i = 0; i < MOMENT_COUNT; i++) {
		check_one_moment(MOMENTS[i]);
		/* And a second either side of each, because an off-by-one in
		 * the floored division only shows on a boundary. */
		check_one_moment(MOMENTS[i] - 1);
		check_one_moment(MOMENTS[i] + 1);
	}

	/*
	 * Then a sweep: every 7 hours 13 minutes and 11 seconds across two
	 * and a half centuries, forwards and backwards from the epoch. The
	 * odd stride is so the sample does not land on the same hour or the
	 * same weekday twice in a row -- a stride of a day would agree with a
	 * calendar that had the week wrong.
	 */
	printf("gmtime_r, swept across two and a half centuries\n");
	step = 7LL * 3600 + 13 * 60 + 11;
	for (i = 0; i < 20000; i++) {
		check_one_moment((long long)i * step);
		check_one_moment(-(long long)i * step);
	}
}

/*
 * Every conversion this library claims, one per entry, plus the shapes that
 * are about the format string rather than about the date.
 */
static const char *const FORMATS[] = {
	"%Y", "%y", "%C", "%m", "%d", "%e", "%H", "%I", "%M", "%S", "%j",
	"%p", "%a", "%A", "%b", "%h", "%B", "%F", "%D", "%T", "%R", "%z",
	"%n", "%t", "%%",

	/* The three the desktop actually uses. */
	"%Y-%m-%d %H%M%S",
	"%Y-%m-%d %H:%M:%S",
	"%e %B %Y at %H:%M",

	/* And the awkward ones. */
	"", "no conversions at all", "%", "%%%", "a%Yb",
	"%Y%m%d%H%M%S", "%A, %e %B %Y at %I:%M %p",
};

#define FORMAT_COUNT (sizeof(FORMATS) / sizeof(FORMATS[0]))

static void test_formatting_a_date(void)
{
	size_t i;
	size_t j;

	/*
	 * Explicitly the C locale. It is already the default at program start,
	 * and saying so removes the one way this suite could pass or fail
	 * because of something outside it: `%A` and `%B` are a locale's names,
	 * and this library has only the C locale's.
	 */
	setlocale(LC_ALL, "C");

	printf("strftime, every conversion against the host's\n");

	for (i = 0; i < MOMENT_COUNT; i++) {
		struct tm parts;
		time_t host_when = (time_t)MOMENTS[i];

		memset(&parts, 0, sizeof(parts));
		if (gmtime_r(&host_when, &parts) == NULL) {
			continue;
		}

		for (j = 0; j < FORMAT_COUNT; j++) {
			char mine[256];
			char theirs[256];
			size_t a;
			size_t b;

			memset(mine, '#', sizeof(mine));
			memset(theirs, '#', sizeof(theirs));

			a = recon_strftime(mine, sizeof(mine), FORMATS[j],
					   &parts);
			b = strftime(theirs, sizeof(theirs), FORMATS[j],
				     &parts);

			same_long((long)a, (long)b,
				  "strftime returned the same length",
				  FORMATS[j]);
			g_checks++;
			if (strcmp(mine, theirs) != 0) {
				g_failures++;
				printf("  FAIL: strftime \"%s\" at %lld\n"
				       "    ReconOS: \"%s\"\n"
				       "    reference: \"%s\"\n",
				       FORMATS[j], MOMENTS[i], mine, theirs);
			}
		}
	}

	/*
	 * And what it does when it does not fit, which is the part callers
	 * get wrong: zero, and the contents are unspecified. Both libraries
	 * are held to returning zero at the same room, which is the only part
	 * of that a caller can rely on.
	 */
	printf("strftime, at every buffer size around the answer\n");
	{
		struct tm parts;
		time_t host_when = 1757808000;
		size_t room;

		memset(&parts, 0, sizeof(parts));
		gmtime_r(&host_when, &parts);

		for (room = 1; room < 40; room++) {
			char mine[64];
			char theirs[64];
			size_t a;
			size_t b;

			memset(mine, '#', sizeof(mine));
			memset(theirs, '#', sizeof(theirs));

			a = recon_strftime(mine, room, "%Y-%m-%d %H:%M:%S",
					   &parts);
			b = strftime(theirs, room, "%Y-%m-%d %H:%M:%S",
				     &parts);

			same_long((long)a, (long)b,
				  "strftime agreed about whether it fitted",
				  NULL);
			if (a != 0 && b != 0) {
				check(strcmp(mine, theirs) == 0,
				      "and wrote the same thing when it did");
			}
			/* Whatever happened, it did not write past the end. */
			check(mine[room] == '#',
			      "strftime wrote nothing past the room it was"
			      " given");
		}
	}
}

/*
 * Two places this deliberately does not match, both asserted rather than
 * skipped -- the same rule `test_libc.c` follows.
 */
static void test_where_time_differs_on_purpose(void)
{
	struct tm parts;
	time_t host_when = 1757808000;
	long long when = 1757808000LL;
	char mine[64];
	char theirs[64];
	struct tm local;
	struct tm utc;

	printf("the two places time deliberately does not match\n");

	memset(&parts, 0, sizeof(parts));
	gmtime_r(&host_when, &parts);

	/*
	 * One: %Z is "UTC", where the host's C locale says "GMT" for a time
	 * produced by gmtime. GMT is a zone with a history and this system is
	 * not in it -- what it has is a count of seconds since 1970, and the
	 * name for that is UTC.
	 */
	recon_strftime(mine, sizeof(mine), "%Z", &parts);
	strftime(theirs, sizeof(theirs), "%Z", &parts);
	check(strcmp(mine, "UTC") == 0, "%Z is UTC here");
	check(strcmp(mine, theirs) != 0,
	      "and that is a difference from the reference, not a match");

	/*
	 * Two: localtime_r is gmtime_r. On this kernel there is no host to ask
	 * and no zone database to read, so UTC is the only true answer. The
	 * desktop is unaffected -- recon_clock.c applies ReconOS's own zone to
	 * its own arithmetic and never comes through here.
	 */
	memset(&local, 0, sizeof(local));
	memset(&utc, 0, sizeof(utc));
	recon_localtime_r(&when, &local);
	recon_gmtime_r(&when, &utc);
	check(memcmp(&local, &utc, sizeof(local)) == 0,
	      "localtime_r is gmtime_r, deliberately");
}

/*
 * The two clocks cannot be compared against a reference -- they read a clock,
 * and the answer is different every time. What can be checked is the property
 * each one exists to have.
 */
static void test_the_two_clocks(void)
{
	struct timespec a;
	struct timespec b;
	long long mine;
	time_t theirs;
	int i;

	printf("the two clocks, held to the property each one is for\n");

	/* The wall clock agrees with the host's, because on this build it is
	 * the host's -- within a second, because two calls are two moments. */
	mine = recon_libc_time(NULL);
	theirs = time(NULL);
	check(mine >= (long long)theirs - 1 && mine <= (long long)theirs + 1,
	      "time() is the same second the reference reports");

	/* And writing through the pointer agrees with returning it. */
	{
		long long through = 0;
		long long returned = recon_libc_time(&through);

		check(through == returned,
		      "time() writes what it returns");
	}

	/* The monotonic clock does not go backwards, which is the whole of
	 * what it is for. Checked across enough calls that a wrap or a
	 * sign error would have to show. */
	check(recon_clock_gettime(RECON_CLOCK_MONOTONIC, &a) == 0,
	      "the monotonic clock answers");
	for (i = 0; i < 20000; i++) {
		check(recon_clock_gettime(RECON_CLOCK_MONOTONIC, &b) == 0,
		      "the monotonic clock answers every time");
		if (b.tv_sec < a.tv_sec ||
		    (b.tv_sec == a.tv_sec && b.tv_nsec < a.tv_nsec)) {
			g_checks++;
			g_failures++;
			printf("  FAIL: the monotonic clock went backwards\n");
			break;
		}
		a = b;
	}

	/* Nanoseconds stay inside a second. A split that let them out would
	 * make every duration computed from them wrong by a second, sometimes. */
	check(a.tv_nsec >= 0 && a.tv_nsec < 1000000000L,
	      "the nanosecond part is inside a second");

	/* An unknown clock is refused rather than answered from the nearer
	 * one, which is the failure the kernel offers two calls to prevent. */
	check(recon_clock_gettime(99, &a) != 0, "an unknown clock is refused");
	check(recon_clock_gettime(RECON_CLOCK_MONOTONIC, NULL) != 0,
	      "and so is nowhere to put the answer");

	/* difftime, which is arithmetic and can be checked exactly. */
	check(recon_difftime(100, 40) == difftime(100, 40), "difftime");
	check(recon_difftime(40, 100) == difftime(40, 100),
	      "difftime, backwards");
	check(recon_difftime(-2208988800LL, 253402300799LL) ==
	      difftime((time_t)-2208988800LL, (time_t)253402300799LL),
	      "difftime across the whole range");
}

int main(void)
{
	printf("ReconOS C library: files, numbers, characters and dates\n\n");

	test_the_character_classes();
	test_the_bytes_the_standard_does_not_define();
	test_the_number_parsing();
	test_the_fraction_parsing();
	test_sorting();
	test_reading_a_file();
	test_writing_a_file();
	test_what_it_refuses();
	test_puts();
	test_the_calendar();
	test_formatting_a_date();
	test_where_time_differs_on_purpose();
	test_the_two_clocks();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
