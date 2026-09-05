/*
 * Tests for version numbers.
 *
 * The bug this file exists to prevent is one line long and every project
 * writes it once: comparing versions with strcmp. It is correct for the first
 * nine releases of anything, and on the tenth it starts reporting the newest
 * build as the oldest -- so an update system built on it refuses the update it
 * was written to install, and says the new applet is older than the one it is
 * replacing.
 *
 * The second thing tested here is what happens to a version that will not
 * parse, which matters for the same reason. A typo that silently becomes
 * 0.0.0 makes an applet replaceable by everything and able to replace nothing,
 * and nothing about that looks wrong from the outside.
 *
 * Run with: cmake --build build && ./build/recon_version_tests
 */

#include <stdio.h>
#include <string.h>

#include "recon_version.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void parses(const char *text, int major, int minor, int patch) {
    struct recon_version v;
    char what[128];

    snprintf(what, sizeof(what), "'%s' reads as %d.%d.%d", text, major, minor,
        patch);
    if (!recon_version_parse(text, &v)) {
        g_checks++;
        g_failures++;
        printf("  FAIL: '%s' would not parse at all\n", text);
        return;
    }
    check(v.major == major && v.minor == minor && v.patch == patch, what);
}

static void refuses(const char *text) {
    struct recon_version v;
    char what[128];
    snprintf(what, sizeof(what), "'%s' is refused", text == NULL ? "(null)" : text);
    check(!recon_version_parse(text, &v), what);
}

/* Comparison, by text, expecting a sign. */
static void orders(const char *a, const char *b, int expected) {
    bool bad = false;
    int got = recon_version_compare_text(a, b, &bad);
    char what[160];
    const char *word = expected < 0 ? "before" : (expected > 0 ? "after" : "same as");

    snprintf(what, sizeof(what), "%s sorts %s %s", a, word, b);
    if (bad) {
        g_checks++;
        g_failures++;
        printf("  FAIL: %s -- one of them would not parse\n", what);
        return;
    }
    if (expected == 0) {
        check(got == 0, what);
    } else if (expected < 0) {
        check(got < 0, what);
    } else {
        check(got > 0, what);
    }
}

static void test_parsing(void) {
    printf("Reading a version\n");

    parses("1.2.3", 1, 2, 3);
    parses("0.3.0", 0, 3, 0);

    /* Missing parts are zero, which is the only reading under which "1.2" and
     * "1.2.0" are one release rather than two. */
    parses("1.2", 1, 2, 0);
    parses("2", 2, 0, 0);
    parses("v1.4.0", 1, 4, 0);
    parses("V0.0.1", 0, 0, 1);
    parses("0.0.0", 0, 0, 0);
    parses("1000.2000.3000", 1000, 2000, 3000);

    refuses("");
    refuses(NULL);
    refuses("v");
    refuses("1.2.");
    refuses(".1.2");
    refuses("1..2");
    refuses("1.2.3.4");
    refuses("1.2.3-beta");
    refuses("1.O.0");            /* the letter O, which is the real typo */
    refuses("-1.0.0");
    refuses("1.2.3 ");
    refuses(" 1.2.3");
    refuses("99999.0.0");        /* more digits than a part may hold */
}

static void test_ordering(void) {
    printf("Ordering versions\n");

    /*
     * The one that matters. strcmp puts 1.10 before 1.9, because '1' is less
     * than '9' and it stops looking there. Every other test in this file could
     * pass with strcmp underneath; this one could not.
     */
    orders("1.10.0", "1.9.0", 1);
    orders("1.9.0", "1.10.0", -1);
    orders("0.2.17", "0.2.9", 1);
    orders("2.0.0", "10.0.0", -1);
    orders("1.0.10", "1.0.9", 1);

    orders("1.2.3", "1.2.3", 0);
    orders("1.2", "1.2.0", 0);
    orders("v1.2.0", "1.2.0", 0);

    orders("2.0.0", "1.99.99", 1);
    orders("1.3.0", "1.2.99", 1);
    orders("1.2.4", "1.2.3", 1);
}

static void test_unreadable_is_not_equal(void) {
    printf("An unreadable version is not the same as an equal one\n");

    bool bad = false;
    int got = recon_version_compare_text("1.O.0", "1.2.0", &bad);

    check(bad, "a version that will not parse is reported as such");
    check(got == 0, "and compares as zero, which callers must read as "
        "'cannot say' rather than 'equal'");

    /*
     * Stated as a test because the flag is easy to ignore and the consequence
     * of ignoring it is the wrong applet winning: a caller that treats zero as
     * "equal" and refuses to replace on equal happens to do the right thing,
     * and a caller that treats zero as "equal" and replaces on equal installs
     * an applet whose version it could not read.
     */
    bad = false;
    check(recon_version_compare_text("1.2.0", "1.2.0", &bad) == 0 && !bad,
        "a genuine tie sets no flag, so the two cases are distinguishable");
}

static void test_formatting(void) {
    printf("Writing a version out\n");

    char out[RECON_VERSION_MAX];
    struct recon_version v;

    recon_version_parse("1.2", &v);
    recon_version_format(&v, out, sizeof(out));
    check(strcmp(out, "1.2.0") == 0, "'1.2' writes out as '1.2.0'");

    recon_version_parse("v0.3.0", &v);
    recon_version_format(&v, out, sizeof(out));
    check(strcmp(out, "0.3.0") == 0, "the leading v is not kept");

    recon_version_format(NULL, out, sizeof(out));
    check(strcmp(out, "0.0.0") == 0, "no version at all writes as 0.0.0");
}

int main(void) {
    printf("ReconOS version tests\n\n");

    test_parsing();
    test_ordering();
    test_unreadable_is_not_equal();
    test_formatting();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
