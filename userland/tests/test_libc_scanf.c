/*
 * `sscanf`, against the host's.
 *
 * The failure that matters for a scanner is not returning the wrong count. It
 * is **assigning the right value to the wrong variable** -- reading the second
 * field into the first, or writing through an argument the format said to
 * suppress. A test that checks only the variables it expected to be filled
 * cannot see either.
 *
 * So every case here declares a block of variables, fills it with a sentinel,
 * runs both libraries on identical copies, and compares **the whole block** as
 * well as the return value. A variable that should not have been touched is
 * checked to be untouched.
 *
 * The corpus is in three parts:
 *
 * - **the desktop's own fourteen formats**, verbatim, on realistic input --
 *   because those are the ones that have to work,
 * - the conversions and modifiers this implements, each on input that
 *   exercises its edges: widths that cut a number short, signs, hexadecimal
 *   with and without a prefix, whitespace where the format has none,
 * - and the failures: input that runs out, literals that do not match, a
 *   conversion this does not implement.
 *
 * Run with: ./build/recon_libc_scanf_tests
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int recon_sscanf(const char *text, const char *format, ...);

static unsigned long checks;
static unsigned long failures;

/*
 * A block of somewhere to put things.
 *
 * One struct rather than separate variables so that "was anything else
 * touched" is a single memcmp. The sentinel is a pattern no conversion here
 * would produce.
 */
struct slots {
	long long a, b, c, d, e, f;
	unsigned long long ua, ub, uc;
	char sa[128], sb[128];
	double da;
	int na, nb;
};

static void wipe(struct slots *s)
{
	memset(s, 0xA5, sizeof(*s));
}

static void report(const char *format, const char *input,
		   int mine, int theirs,
		   const struct slots *a, const struct slots *b)
{
	checks++;

	if (mine == theirs && memcmp(a, b, sizeof(*a)) == 0)
		return;

	failures++;
	if (failures > 25)
		return;

	printf("  FAIL  sscanf(%-32s, %-28s)\n", input, format);

	if (mine != theirs)
		printf("        returned %d, the host returned %d\n",
		       mine, theirs);
	else
		printf("        returned %d, but wrote different values\n",
		       mine);

	if (a->a != b->a)
		printf("          a: %lld against %lld\n", a->a, b->a);
	if (a->b != b->b)
		printf("          b: %lld against %lld\n", a->b, b->b);
	if (a->c != b->c)
		printf("          c: %lld against %lld\n", a->c, b->c);
	if (a->ua != b->ua)
		printf("          ua: %llu against %llu\n", a->ua, b->ua);
	if (memcmp(a->sa, b->sa, sizeof(a->sa)) != 0)
		printf("          sa: %.40s against %.40s\n", a->sa, b->sa);
	if (a->na != b->na)
		printf("          n: %d against %d\n", a->na, b->na);
}

/* --- the cases ------------------------------------------------------------
 *
 * Each is a small function so that the argument lists -- which are the whole
 * point -- are written once per format and used by both libraries.
 */

#define BOTH(call_mine, call_theirs)				\
	do {							\
		struct slots mine, theirs;			\
		int a, b;					\
								\
		wipe(&mine);					\
		wipe(&theirs);					\
		a = (call_mine);				\
		b = (call_theirs);				\
		report(format, input, a, b, &mine, &theirs);	\
	} while (0)

static void three_ints(const char *input, const char *format)
{
	BOTH(recon_sscanf(input, format, &mine.a, &mine.b, &mine.c),
	     sscanf(input, format, &theirs.a, &theirs.b, &theirs.c));
}

static void three_ints_narrow(const char *input, const char *format)
{
	struct slots mine, theirs;
	int a, b;
	int m1 = -1, m2 = -1, m3 = -1;
	int t1 = -1, t2 = -1, t3 = -1;

	wipe(&mine);
	wipe(&theirs);

	a = recon_sscanf(input, format, &m1, &m2, &m3);
	b = sscanf(input, format, &t1, &t2, &t3);

	checks++;
	if (a != b || m1 != t1 || m2 != t2 || m3 != t3) {
		failures++;
		if (failures <= 25)
			printf("  FAIL  sscanf(%s, %s) -> %d (%d %d %d) "
			       "against %d (%d %d %d)\n",
			       input, format, a, m1, m2, m3, b, t1, t2, t3);
	}
}

static void one_string(const char *input, const char *format)
{
	BOTH(recon_sscanf(input, format, mine.sa),
	     sscanf(input, format, theirs.sa));
}

static void string_then_two_hex(const char *input, const char *format)
{
	BOTH(recon_sscanf(input, format, mine.sa, &mine.ua, &mine.ub),
	     sscanf(input, format, theirs.sa, &theirs.ua, &theirs.ub));
}

static void one_unsigned_long(const char *input, const char *format)
{
	struct slots mine, theirs;
	unsigned long m = 0xA5A5A5A5UL, t = 0xA5A5A5A5UL;
	int a, b;

	wipe(&mine);
	wipe(&theirs);

	a = recon_sscanf(input, format, &m);
	b = sscanf(input, format, &t);

	checks++;
	if (a != b || m != t) {
		failures++;
		if (failures <= 25)
			printf("  FAIL  sscanf(%s, %s) -> %d (%lu) against "
			       "%d (%lu)\n", input, format, a, m, b, t);
	}
}

/* The date parsers, which are the two that use %n and the reason it is here. */
static void date_with_offset(const char *input, const char *format)
{
	struct slots mine, theirs;
	int my[5] = { -1, -1, -1, -1, -1 }, my_n = -1;
	int th[5] = { -1, -1, -1, -1, -1 }, th_n = -1;
	int a, b, i;
	int same = 1;

	wipe(&mine);
	wipe(&theirs);

	a = recon_sscanf(input, format, &my[0], &my[1], &my[2], &my[3],
			 &my[4], &my_n);
	b = sscanf(input, format, &th[0], &th[1], &th[2], &th[3],
		   &th[4], &th_n);

	for (i = 0; i < 5; i++) {
		if (my[i] != th[i])
			same = 0;
	}

	checks++;
	if (a != b || !same || my_n != th_n) {
		failures++;
		if (failures <= 25)
			printf("  FAIL  sscanf(%s, %s) -> %d n=%d against "
			       "%d n=%d\n", input, format, a, my_n, b, th_n);
	}
}

static void ten_unsigned_long_long(const char *input, const char *format)
{
	unsigned long long m[10], t[10];
	int a, b, i;
	int same = 1;

	for (i = 0; i < 10; i++)
		m[i] = t[i] = 0xA5A5A5A5A5A5A5A5ULL;

	a = recon_sscanf(input, format, &m[0], &m[1], &m[2], &m[3], &m[4],
			 &m[5], &m[6], &m[7], &m[8], &m[9]);
	b = sscanf(input, format, &t[0], &t[1], &t[2], &t[3], &t[4],
		   &t[5], &t[6], &t[7], &t[8], &t[9]);

	for (i = 0; i < 10; i++) {
		if (m[i] != t[i])
			same = 0;
	}

	checks++;
	if (a != b || !same) {
		failures++;
		if (failures <= 25)
			printf("  FAIL  ten values from %s -> %d against %d\n",
			       input, a, b);
	}
}

int main(void)
{
	printf("sscanf\n\n");

	/* --- the desktop's own fourteen, verbatim ---------------------- */

	date_with_offset("2026-09-14 17:30 rest of the line",
			 "%4d-%2d-%2d %2d:%2d %n");
	date_with_offset("2026-09-14 -- a note", "%4d-%2d-%2d -- %n");
	date_with_offset("not a date at all", "%4d-%2d-%2d %2d:%2d %n");

	three_ints_narrow("12-40", "%d-%d");
	three_ints_narrow("12", "%d-%d");
	three_ints_narrow("17:30:45", "%d:%d:%d");
	three_ints_narrow("17:30", "%d:%d:%d");
	three_ints_narrow("3,7", "%d,%d");

	three_ints_narrow("+OK 42", "+OK %d");
	three_ints_narrow("-ERR nope", "+OK %d");
	three_ints_narrow("* 17 EXISTS", "* %d EXISTS");
	three_ints_narrow("* 17 FETCH", "* %d EXISTS");
	three_ints_narrow("* 903 FETCH", "* %d FETCH");

	string_then_two_hex("eth0 0100007F 00000000", "%63s %lx %lx");
	string_then_two_hex("eth0 0100007F", "%63s %lx %lx");

	one_string(" nameserver 192.168.1.1", " nameserver %63s");
	one_string("nameserver 192.168.1.1", " nameserver %63s");
	one_string("   nameserver 10.0.0.1", " nameserver %63s");

	one_unsigned_long("MemTotal:       16384000 kB", "MemTotal: %lu kB");
	one_unsigned_long("MemAvailable:    8192000 kB",
			  "MemAvailable: %lu kB");
	one_unsigned_long("MemTotal:  not a number", "MemTotal: %lu kB");

	ten_unsigned_long_long("cpu  1 2 3 4 5 6 7 8 9 10",
			       "%llu %llu %llu %llu %llu %llu %llu %llu "
			       "%llu %llu");

	/* --- widths, signs, bases -------------------------------------- */

	three_ints("1 2 3", "%lld %lld %lld");
	three_ints("-1 -2 -3", "%lld %lld %lld");
	three_ints("+1 +2 +3", "%lld %lld %lld");
	three_ints("  1   2   3  ", "%lld %lld %lld");
	three_ints("1 2", "%lld %lld %lld");
	three_ints("", "%lld %lld %lld");
	three_ints("   ", "%lld %lld %lld");
	three_ints("abc", "%lld %lld %lld");
	three_ints("9223372036854775807 -9223372036854775808 0",
		   "%lld %lld %lld");

	three_ints_narrow("123456", "%2d%2d%2d");
	three_ints_narrow("123456", "%3d%3d");
	three_ints_narrow("1234", "%2d%2d%2d");
	three_ints_narrow("-12-34", "%3d%3d");

	three_ints("ff 0xFF 10", "%llx %llx %llo");
	three_ints("0x10 010 10", "%lli %lli %lli");
	three_ints("z", "%llx %llx %llx");

	/* --- strings and characters ------------------------------------ */

	one_string("hello world", "%s");
	one_string("   hello", "%s");
	one_string("", "%s");
	one_string("   ", "%s");
	one_string("abcdefghij", "%4s");

	/* --- suppression ------------------------------------------------ */

	three_ints_narrow("1 2 3", "%*d %d %d");
	three_ints_narrow("1 2 3", "%d %*d %d");

	/* --- literal percent -------------------------------------------- */

	three_ints_narrow("50% done 7", "%d%% done %d");
	three_ints_narrow("50 done 7", "%d%% done %d");

	/* --- a conversion this does not implement ------------------------
	 *
	 * A scanset. Both must stop -- ours because it is not implemented,
	 * and this records what a caller sees when it meets one. The host
	 * *does* implement it, so this is the one case where a difference is
	 * expected, and it is asserted rather than compared.
	 */
	{
		char mine[64], theirs[64];
		int a, b;

		memset(mine, 0xA5, sizeof(mine));
		memset(theirs, 0xA5, sizeof(theirs));

		a = recon_sscanf("abc123", "%[a-z]", mine);
		b = sscanf("abc123", "%[a-z]", theirs);

		checks++;
		if (a != 0) {
			failures++;
			printf("  FAIL  a scanset should stop the scan and "
			       "return 0, not %d\n", a);
		}

		checks++;
		if (b != 1) {
			failures++;
			printf("  FAIL  the host was expected to implement "
			       "scansets; it returned %d\n", b);
		}
	}

	/* --- stopping is not the same as skipping ------------------------
	 *
	 * The case above cannot tell them apart: with `%[a-z]`, a scan that
	 * skipped the conversion would then match `a-z]` as literals and fail
	 * at the `-`, returning the same 0. A mutation that made unimplemented
	 * conversions skip went undetected by this file until this case
	 * existed.
	 *
	 * What separates them is *which variable the next conversion writes
	 * to* -- which is the failure this suite exists for. An unknown
	 * conversion between two numbers: stopping returns 1 and never touches
	 * the second variable; skipping returns 2 and puts the second field in
	 * it.
	 *
	 * Not compared against the host, because `%q` is undefined there and
	 * an undefined behaviour is not a reference.
	 */
	{
		int first = -1, second = -1, third = -1;
		int n = recon_sscanf("7 8 9", "%d %q %d",
				     &first, &second, &third);

		checks++;
		if (n != 1) {
			failures++;
			printf("  FAIL  an unimplemented conversion should "
			       "stop the scan at 1, not %d\n", n);
		}

		checks++;
		if (first != 7) {
			failures++;
			printf("  FAIL  the conversion before it should still "
			       "have run: %d\n", first);
		}

		checks++;
		if (second != -1 || third != -1) {
			failures++;
			printf("  FAIL  nothing after it may be written: "
			       "%d %d\n", second, third);
		}
	}

	printf("\n%lu checks, %lu failures\n", checks, failures);
	return failures ? 1 : 0;
}
