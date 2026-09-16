/*
 * ReconOS's <limits.h>, against the host's.
 *
 * A header of nothing but macros, which makes it look like the one file in
 * this library that could not be wrong. It is the opposite: **a wrong limit is
 * silent**. Code that checks `INT_MAX` before doing arithmetic gets a wrong
 * answer and carries on, and nothing in a test suite that does not look at
 * this header would ever disagree.
 *
 * So every value is compared with the host's, which is the same method the
 * rest of the library is held to.
 *
 * --- how both headers are in one program ---------------------------------
 *
 * The host's arrives first, by `<limits.h>`, and its values are copied into
 * constants. Then ReconOS's is included by relative path, which redefines the
 * macros, and the copies are what the new ones are compared against.
 *
 * `#undef` between the two rather than letting the second win quietly: a
 * redefinition with a different value is a warning that would be lost in a
 * build, and being explicit is what makes the comparison a comparison.
 */

#define _POSIX_C_SOURCE 200809L

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>

/* The host's answers, taken before the other header is allowed near them. */
static const long long HOST_CHAR_BIT   = CHAR_BIT;
static const long long HOST_SCHAR_MAX  = SCHAR_MAX;
static const long long HOST_SCHAR_MIN  = SCHAR_MIN;
static const long long HOST_UCHAR_MAX  = UCHAR_MAX;
static const long long HOST_CHAR_MAX   = CHAR_MAX;
static const long long HOST_CHAR_MIN   = CHAR_MIN;
static const long long HOST_SHRT_MAX   = SHRT_MAX;
static const long long HOST_SHRT_MIN   = SHRT_MIN;
static const long long HOST_USHRT_MAX  = USHRT_MAX;
static const long long HOST_INT_MAX    = INT_MAX;
static const long long HOST_INT_MIN    = INT_MIN;
static const unsigned long long HOST_UINT_MAX  = UINT_MAX;
static const long long HOST_LONG_MAX   = LONG_MAX;
static const long long HOST_LONG_MIN   = LONG_MIN;
static const unsigned long long HOST_ULONG_MAX = ULONG_MAX;
static const long long HOST_LLONG_MAX  = LLONG_MAX;
static const long long HOST_LLONG_MIN  = LLONG_MIN;
static const unsigned long long HOST_ULLONG_MAX = ULLONG_MAX;

/* Out of the way, so ReconOS's are not a redefinition of the host's. */
#undef CHAR_BIT
#undef SCHAR_MAX
#undef SCHAR_MIN
#undef UCHAR_MAX
#undef CHAR_MAX
#undef CHAR_MIN
#undef MB_LEN_MAX
#undef SHRT_MAX
#undef SHRT_MIN
#undef USHRT_MAX
#undef INT_MAX
#undef INT_MIN
#undef UINT_MAX
#undef LONG_MAX
#undef LONG_MIN
#undef ULONG_MAX
#undef LLONG_MAX
#undef LLONG_MIN
#undef ULLONG_MAX
#undef PATH_MAX
#undef NAME_MAX

#include "../include/limits.h"

static int g_failures;
static int g_checks;

static void same(long long ours, long long theirs, const char *what) {
    g_checks++;
    if (ours != theirs) {
        g_failures++;
        printf("  FAIL: %s is %lld here and %lld there\n", what, ours, theirs);
    }
}

static void same_u(unsigned long long ours, unsigned long long theirs,
        const char *what) {
    g_checks++;
    if (ours != theirs) {
        g_failures++;
        printf("  FAIL: %s is %llu here and %llu there\n", what, ours, theirs);
    }
}

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void test_every_limit(void) {
    printf("every limit the standard names, against the host's\n");

    same(CHAR_BIT, HOST_CHAR_BIT, "CHAR_BIT");
    same(SCHAR_MAX, HOST_SCHAR_MAX, "SCHAR_MAX");
    same(SCHAR_MIN, HOST_SCHAR_MIN, "SCHAR_MIN");
    same(UCHAR_MAX, HOST_UCHAR_MAX, "UCHAR_MAX");
    same(CHAR_MAX, HOST_CHAR_MAX, "CHAR_MAX");
    same(CHAR_MIN, HOST_CHAR_MIN, "CHAR_MIN");
    same(SHRT_MAX, HOST_SHRT_MAX, "SHRT_MAX");
    same(SHRT_MIN, HOST_SHRT_MIN, "SHRT_MIN");
    same(USHRT_MAX, HOST_USHRT_MAX, "USHRT_MAX");
    same(INT_MAX, HOST_INT_MAX, "INT_MAX");
    same(INT_MIN, HOST_INT_MIN, "INT_MIN");
    same_u(UINT_MAX, HOST_UINT_MAX, "UINT_MAX");
    same(LONG_MAX, HOST_LONG_MAX, "LONG_MAX");
    same(LONG_MIN, HOST_LONG_MIN, "LONG_MIN");
    same_u(ULONG_MAX, HOST_ULONG_MAX, "ULONG_MAX");
    same(LLONG_MAX, HOST_LLONG_MAX, "LLONG_MAX");
    same(LLONG_MIN, HOST_LLONG_MIN, "LLONG_MIN");
    same_u(ULLONG_MAX, HOST_ULLONG_MAX, "ULLONG_MAX");
}

static void test_the_minimums_are_the_real_ones(void) {
    printf("a minimum has no positive counterpart, and that is the trap\n");

    /*
     * **`-2147483648` is not an `int`.** It is `2147483648` negated, which
     * does not fit in an `int`, so it promotes -- and a limits.h that writes
     * its minimums as literals hands back values of the wrong *type*, which
     * changes the result of every comparison they take part in without
     * changing any of the numbers a person would print.
     *
     * The tree has the same trap written down about `(int)~0U >> 1`, which is
     * -1 rather than INT_MAX and would have made every row of a table measure
     * as zero lines.
     *
     * So: the minimums are one below the negated maximum, and adding the
     * maximum back gets to -1. Arithmetic a wrong type does not survive.
     */
    check(INT_MIN + INT_MAX == -1, "INT_MIN and INT_MAX are a pair");
    check(LONG_MIN + LONG_MAX == -1L, "and so are the long ones");
    check(LLONG_MIN + LLONG_MAX == -1LL, "and the long long ones");
    check(SCHAR_MIN + SCHAR_MAX == -1, "and the signed char ones");
    check(SHRT_MIN + SHRT_MAX == -1, "and the short ones");

    /* And the unsigned maxima are what the signed ones say they should be. */
    check(UINT_MAX == (unsigned)INT_MAX * 2u + 1u,
        "UINT_MAX is twice INT_MAX plus one");
    check(USHRT_MAX == (unsigned)SHRT_MAX * 2u + 1u,
        "and USHRT_MAX likewise");
}

static void test_this_systems_own(void) {
    printf("the two that are not the compiler's business\n");

    /*
     * PATH_MAX is ReconOS's and is deliberately **not** Linux's 4096. The
     * difference has bitten this project: src/recon_fs.c has a comment about
     * realpath requiring a buffer of at least the *host's* PATH_MAX, and an
     * optimised glibc aborting the process when handed a smaller one.
     */
    check(PATH_MAX == 1024, "PATH_MAX is this system's 1024");
    check(PATH_MAX != HOST_INT_MAX, "and is a real number, not a limit");

    /*
     * NAME_MAX is 255 and `d_name` is 256 bytes, because one of them is the
     * terminator. Off by one here is a name that fits being reported as one
     * that does not.
     */
    check(NAME_MAX == 255, "NAME_MAX leaves room for the terminator");

    check(MB_LEN_MAX == 1,
        "and there is no multibyte state in this library");
}

int main(void) {
    printf("ReconOS limits tests\n\n");

    test_every_limit();
    test_the_minimums_are_the_real_ones();
    test_this_systems_own();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
