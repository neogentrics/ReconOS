/*
 * Tests for the ReconOS registry.
 *
 * A settings store is worth testing on its own because its failures are quiet:
 * a value that comes back wrong does not crash anything, it just makes the
 * desktop behave slightly oddly forever, and nobody connects the two.
 *
 * Run with: ninja -C build && ./build/recon_registry_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "recon_fs.h"
#include "recon_registry.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_text(const char *got, const char *expected, const char *what) {
    g_checks++;
    if (got == NULL || strcmp(got, expected) != 0) {
        g_failures++;
        printf("  FAIL: %s\n", what);
        printf("        wanted '%s', got '%s'\n", expected,
            got != NULL ? got : "(null)");
    }
}

/* --- Tests --- */

static void test_basics(void) {
    printf("reading and writing\n");

    check(recon_registry_set(RECON_REG_USER, "desktop/icon-size", "48"),
        "set a value");
    check_text(recon_registry_get(RECON_REG_USER, "desktop/icon-size", "?"),
        "48", "read it back");

    check(recon_registry_has(RECON_REG_USER, "desktop/icon-size"),
        "the key exists");
    check(!recon_registry_has(RECON_REG_USER, "desktop/nothing"),
        "a key that was never set does not exist");

    check_text(recon_registry_get(RECON_REG_USER, "desktop/nothing", "default"),
        "default", "a missing key gives the fallback");

    check(recon_registry_set(RECON_REG_USER, "desktop/icon-size", "32"),
        "overwrite it");
    check_text(recon_registry_get(RECON_REG_USER, "desktop/icon-size", "?"),
        "32", "the new value is there");
}

static void test_types(void) {
    printf("numbers and flags\n");

    check(recon_registry_set_int(RECON_REG_USER, "windows/Notepad/x", 420),
        "set a number");
    check(recon_registry_get_int(RECON_REG_USER, "windows/Notepad/x", -1) == 420,
        "read it back");
    check(recon_registry_get_int(RECON_REG_USER, "windows/Notepad/y", -1) == -1,
        "a missing number gives the fallback");

    check(recon_registry_set_int(RECON_REG_USER, "windows/Notepad/y", -30),
        "negative numbers work");
    check(recon_registry_get_int(RECON_REG_USER, "windows/Notepad/y", 0) == -30,
        "and read back");

    /*
     * The one that matters. A value that will not parse must give the
     * fallback, not zero: a damaged file should look like missing settings,
     * not like a real setting of 0 that puts every window in the corner.
     */
    check(recon_registry_set(RECON_REG_USER, "windows/Notepad/w", "not a number"),
        "store something unparseable");
    check(recon_registry_get_int(RECON_REG_USER, "windows/Notepad/w", 640) == 640,
        "an unparseable number gives the fallback, not 0");

    check(recon_registry_set(RECON_REG_USER, "windows/Notepad/h", "12abc"),
        "store a number with rubbish after it");
    check(recon_registry_get_int(RECON_REG_USER, "windows/Notepad/h", 480) == 480,
        "a partly-numeric value is refused rather than truncated");

    check(recon_registry_set_bool(RECON_REG_USER, "desktop/show-hidden", true),
        "set a flag");
    check(recon_registry_get_bool(RECON_REG_USER, "desktop/show-hidden", false),
        "read it back");
    check(!recon_registry_get_bool(RECON_REG_USER, "desktop/missing", false),
        "a missing flag gives the fallback");

    check(recon_registry_set(RECON_REG_USER, "desktop/spelled", "yes"), "yes");
    check(recon_registry_get_bool(RECON_REG_USER, "desktop/spelled", false),
        "'yes' reads as true");
    check(recon_registry_set(RECON_REG_USER, "desktop/spelled", "0"), "0");
    check(!recon_registry_get_bool(RECON_REG_USER, "desktop/spelled", true),
        "'0' reads as false");
}

static void test_keys(void) {
    printf("what counts as a key\n");

    check(!recon_registry_set(RECON_REG_USER, "", "x"), "an empty key is refused");
    check(!recon_registry_set(RECON_REG_USER, "/leading", "x"),
        "a leading slash is refused");
    check(!recon_registry_set(RECON_REG_USER, "trailing/", "x"),
        "a trailing slash is refused");
    check(!recon_registry_set(RECON_REG_USER, "double//slash", "x"),
        "an empty segment is refused");
    check(!recon_registry_set(RECON_REG_USER, "has space", "x"),
        "a space is refused");
    check(!recon_registry_set(RECON_REG_USER, "has=equals", "x"),
        "an equals sign is refused, since it separates key from value");

    check(recon_registry_set(RECON_REG_USER, "a/b/c/d/e", "deep"),
        "a deep path is fine");
    check(recon_registry_set(RECON_REG_USER, "with-dash_and.dot", "fine"),
        "dashes, underscores and dots are fine");
}

static void test_scopes(void) {
    printf("the two hives\n");

    check(recon_registry_set(RECON_REG_SYSTEM, "theme", "recon"), "set a system value");
    check(recon_registry_set(RECON_REG_USER, "theme", "midnight"), "set a user value");

    /* The whole point of two hives: one account's preference is not
     * everyone's. */
    check_text(recon_registry_get(RECON_REG_SYSTEM, "theme", "?"),
        "recon", "the system value is unchanged");
    check_text(recon_registry_get(RECON_REG_USER, "theme", "?"),
        "midnight", "the user value is its own");

    check(!recon_registry_has(RECON_REG_SYSTEM, "desktop/icon-size"),
        "a user key is not in the system hive");
}

static void test_listing(void) {
    printf("listing\n");

    recon_registry_remove_all(RECON_REG_USER, "listtest");
    check(recon_registry_set(RECON_REG_USER, "listtest/b", "2"), "set b");
    check(recon_registry_set(RECON_REG_USER, "listtest/a", "1"), "set a");
    check(recon_registry_set(RECON_REG_USER, "listtest/c", "3"), "set c");

    check(recon_registry_count(RECON_REG_USER, "listtest") == 3,
        "three keys under the prefix");

    /* Sorted, so a listing is stable between runs rather than depending on
     * the order things happened to be written. */
    const char *key = NULL;
    check(recon_registry_at(RECON_REG_USER, "listtest", 0, &key, NULL) &&
        strcmp(key, "listtest/a") == 0, "listed in order, first");
    check(recon_registry_at(RECON_REG_USER, "listtest", 2, &key, NULL) &&
        strcmp(key, "listtest/c") == 0, "listed in order, last");
    check(!recon_registry_at(RECON_REG_USER, "listtest", 3, NULL, NULL),
        "past the end is refused");

    /*
     * A prefix matches whole segments. Without this "listtest" would also
     * catch "listtestother", and removing a prefix would take unrelated
     * settings with it.
     */
    check(recon_registry_set(RECON_REG_USER, "listtestother/x", "9"),
        "set something with a similar name");
    check(recon_registry_count(RECON_REG_USER, "listtest") == 3,
        "a similar name is not under the prefix");

    check(recon_registry_remove_all(RECON_REG_USER, "listtest") == 3,
        "removing a prefix removes exactly those three");
    check(recon_registry_has(RECON_REG_USER, "listtestother/x"),
        "and leaves the similar one alone");
}

static void test_removal(void) {
    printf("removing\n");

    check(recon_registry_set(RECON_REG_USER, "temporary", "x"), "set it");
    check(recon_registry_remove(RECON_REG_USER, "temporary"), "remove it");
    check(!recon_registry_has(RECON_REG_USER, "temporary"), "it is gone");
    check(!recon_registry_remove(RECON_REG_USER, "temporary"),
        "removing it again reports that it was not there");
}

/* Everything above only proves the in-memory copy works. This is the test that
 * proves settings actually survive a restart, which is the entire point. */
static void test_persistence(void) {
    printf("surviving a restart\n");

    check(recon_registry_set(RECON_REG_USER, "persist/text", "hello world"),
        "set a value with a space in it");
    check(recon_registry_set(RECON_REG_USER, "persist/awkward",
        "one\ntwo = three\\four"),
        "set a value with a newline, an equals and a backslash");
    check(recon_registry_set_int(RECON_REG_SYSTEM, "persist/number", 1234),
        "set a system number");

    /* Close and open again: exactly what a restart does. */
    recon_registry_finish();
    check(recon_registry_init(), "load again");

    check_text(recon_registry_get(RECON_REG_USER, "persist/text", "?"),
        "hello world", "the spaced value survived");
    check_text(recon_registry_get(RECON_REG_USER, "persist/awkward", "?"),
        "one\ntwo = three\\four",
        "the newline, equals and backslash all survived");
    check(recon_registry_get_int(RECON_REG_SYSTEM, "persist/number", -1) == 1234,
        "the system number survived");

    check(recon_registry_get_int(RECON_REG_USER, "windows/Notepad/x", -1) == 420,
        "and so did everything set earlier");
}

/* A file somebody has edited by hand, or that got damaged. */
static void test_damaged_file(void) {
    printf("a file that is not quite right\n");

    const char *text =
        "# a comment\n"
        "\n"
        "   spaced/key   =   spaced value   \n"
        "this line has no equals sign\n"
        "/bad-key = ignored\n"
        "good/key = kept\n";

    check(recon_fs_write("/", RECON_REGISTRY_SYSTEM_FILE, text, strlen(text)),
        "write a hand-edited file");

    recon_registry_finish();
    check(recon_registry_init(), "load it");

    check_text(recon_registry_get(RECON_REG_SYSTEM, "spaced/key", "?"),
        "spaced value   ", "whitespace around the separator is trimmed");
    check_text(recon_registry_get(RECON_REG_SYSTEM, "good/key", "?"),
        "kept", "a good line after bad ones is still read");
    check(!recon_registry_has(RECON_REG_SYSTEM, "/bad-key"),
        "an invalid key is skipped rather than stored");

    /* The point: one bad line costs that line, not the file. */
    check(recon_registry_count(RECON_REG_SYSTEM, "") == 2,
        "exactly the two usable lines were kept");
}

int main(void) {
    char root[] = "/tmp/reconos-reg-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS registry tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n", recon_fs_last_error());
        return 1;
    }
    if (!recon_registry_init()) {
        printf("could not start the registry: %s\n", recon_registry_last_error());
        return 1;
    }

    test_basics();
    test_types();
    test_keys();
    test_scopes();
    test_listing();
    test_removal();
    test_persistence();
    test_damaged_file();

    recon_registry_finish();
    recon_fs_finish();

    char command[512];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command) != 0) {
        printf("\nnote: could not remove %s\n", root);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
