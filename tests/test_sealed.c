/*
 * Sealed files: what they refuse.
 *
 * --- What a round-trip test would not catch ---
 *
 * `tests/test_keyring.c` opens with the argument this file inherits: a suite
 * that only proves a secret comes back would pass against a version storing
 * everything in plain text. Sealing a file has the same shape and one more
 * thing that can go quietly wrong, because a sealed file is a *file* and
 * somebody who has the disk can move one.
 *
 * So the checks that matter here are:
 *
 *   - a locked keyring gives nothing, and takes nothing
 *   - an edited file does not open to *something* -- not in the ciphertext,
 *     not in the tag, not in the nonce
 *   - **a sealed file moved to another name is not that name's file**, which
 *     is the whole reason the name is authenticated as associated data and the
 *     one check a careful round-trip test still misses
 *   - a file belonging to a different account does not open under this one
 *   - a name that could climb out of the folder is refused rather than
 *     repaired
 *
 * --- And one thing that is not a refusal ---
 *
 * An empty sealed file is a real thing and reads back empty, which is how "the
 * jar has been emptied" is told apart from "the jar has never existed". A
 * mechanism that could not say the difference would make the browser forget
 * that somebody had signed out.
 *
 * Run with: ./build/recon_sealed_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_crypt.h"
#include "recon_fs.h"
#include "recon_keyring.h"
#include "recon_sealed.h"
#include "recon_users.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void same_int(int got, int wanted, const char *what) {
    g_checks++;
    if (got != wanted) {
        g_failures++;
        printf("  FAIL: %s -- got %d, wanted %d\n", what, got, wanted);
    }
}

/* A message is allowed to change; what it must keep doing is tell the two
 * kinds of failure apart, so this asks for a phrase rather than a sentence. */
static void says(const char *got, const char *phrase, const char *what) {
    g_checks++;
    if (got == NULL || strstr(got, phrase) == NULL) {
        g_failures++;
        printf("  FAIL: %s -- '%s' does not mention '%s'\n", what,
            got != NULL ? got : "(none)", phrase);
    }
}

#define NAME  "browser/cookies"
#define OTHER "browser/history"
#define PATH  "/Users/Tester/.sealed/" NAME
#define OTHER_PATH "/Users/Tester/.sealed/" OTHER

/* Bigger than one AES block and not a multiple of it, so a fault that only
 * shows at a boundary has somewhere to show. */
static const char JAR[] =
    "session=6f2a91; host=example.com; path=/; secure\n"
    "prefs=dark; host=example.com; path=/\n";

static void unlock(void) {
    recon_keyring_lock();
    check(recon_keyring_unlock("Tester", "correct horse"),
        "the keyring unlocks");
}

/*
 * Whether a run of bytes appears anywhere in another.
 *
 * `memmem` would do it and is a GNU extension, which would mean a feature-test
 * macro on a file that already names _POSIX_C_SOURCE. Four lines is cheaper
 * than that argument.
 */
static bool contains(const char *hay, size_t hay_size, const char *needle) {
    size_t n = strlen(needle);

    if (n > hay_size) {
        return false;
    }
    for (size_t i = 0; i + n <= hay_size; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* The sealed file's bytes, for the tests that have to damage one. */
static char *raw(size_t *size) {
    return recon_fs_read("/", PATH, size);
}

static void put_back(const char *bytes, size_t size) {
    recon_fs_write("/", PATH, bytes, size);
}


/* --- The tests --------------------------------------------------------- */

static void test_it_comes_back(void) {
    printf("what is sealed comes back\n");

    unlock();
    check(recon_sealed_write(NAME, JAR, strlen(JAR)), "it seals");
    check(recon_sealed_exists(NAME), "and there is a file");

    size_t size = 0;
    char *back = recon_sealed_read(NAME, &size);

    check(back != NULL, "it opens");
    same_int((int)size, (int)strlen(JAR), "with the length it was given");
    check(back != NULL && memcmp(back, JAR, strlen(JAR)) == 0,
        "and the bytes that went in");
    check(back != NULL && back[size] == 0,
        "terminated one past the end, so text can be used as a string");
    free(back);

    /*
     * Not a test of the cipher -- a test that the *file* is not the content.
     * If this ever passes, something has stopped encrypting and every other
     * check in this file would still be green.
     */
    size_t file_size = 0;
    char *file = raw(&file_size);

    check(file != NULL, "the file is on the disk");
    check(file != NULL && !contains(file, file_size, "example.com"),
        "and the content is nowhere in it");
    free(file);
}

static void test_an_empty_one_is_a_real_thing(void) {
    printf("an empty sealed file is not a missing one\n");

    unlock();
    check(recon_sealed_write("browser/empty", "", 0), "zero bytes seal");
    check(recon_sealed_exists("browser/empty"), "and make a file");

    size_t size = 999;
    char *back = recon_sealed_read("browser/empty", &size);

    check(back != NULL, "which opens");
    same_int((int)size, 0, "and is empty");
    free(back);

    check(!recon_sealed_exists("browser/never-written"),
        "while a name nothing wrote has no file");
    check(recon_sealed_read("browser/never-written", NULL) == NULL,
        "and opens to nothing");
}

static void test_locked_gives_nothing(void) {
    printf("a locked keyring takes nothing and gives nothing\n");

    unlock();
    check(recon_sealed_write(NAME, JAR, strlen(JAR)), "sealed while unlocked");

    recon_keyring_lock();
    check(recon_sealed_read(NAME, NULL) == NULL, "locked, it does not open");
    check(!recon_sealed_write(NAME, JAR, strlen(JAR)),
        "and nothing new can be sealed");

    /*
     * The file is still there, which is the point of saying so: locking does
     * not delete anything, it stops it being readable.
     */
    size_t size = 0;
    char *file = raw(&size);

    check(file != NULL, "the file is still on the disk");
    free(file);

    unlock();

    char *again = recon_sealed_read(NAME, NULL);

    check(again != NULL, "and opens again after");
    free(again);
}

static void test_an_edited_file_does_not_open(void) {
    printf("a file somebody has edited\n");

    unlock();
    check(recon_sealed_write(NAME, JAR, strlen(JAR)), "sealed");

    size_t size = 0;
    char *good = raw(&size);

    check(good != NULL, "and read back as bytes");
    if (good == NULL) {
        return;
    }

    /*
     * One byte at a time, in each of the three parts. A cipher wired up
     * carelessly can authenticate the ciphertext and ignore the nonce, and
     * that version passes a test that only damages the ciphertext.
     */
    struct { const char *what; size_t at; } spots[] = {
        { "the magic",     2 },
        { "the nonce",     17 + 3 },
        { "the tag",       17 + 12 + 3 },
        { "the ciphertext", 17 + 12 + 16 + 3 },
    };

    for (size_t i = 0; i < sizeof(spots) / sizeof(spots[0]); i++) {
        char *bad = malloc(size);

        memcpy(bad, good, size);
        bad[spots[i].at] = (char)(bad[spots[i].at] ^ 0x40);
        put_back(bad, size);
        free(bad);

        char *out = recon_sealed_read(NAME, NULL);

        check(out == NULL, spots[i].what);
        free(out);
    }

    /* The magic is checked before the cipher, so its failure says something
     * different -- somebody looking at this should not be sent hunting for a
     * tampering when they handed over the wrong file. */
    char *bad = malloc(size);

    memcpy(bad, good, size);
    bad[2] = 'X';
    put_back(bad, size);
    free(bad);
    recon_sealed_read(NAME, NULL);
    says(recon_sealed_last_error(), "not a sealed file",
        "a wrong magic is reported as a wrong file, not as tampering");

    /* Truncated to less than its own header: not a sealed file, and nothing
     * subtracts past zero on the way to saying so. */
    put_back(good, 20);
    check(recon_sealed_read(NAME, NULL) == NULL, "a truncated file");

    put_back(good, size);

    char *whole = recon_sealed_read(NAME, NULL);

    check(whole != NULL, "and the real one still opens");
    free(whole);
    free(good);
}

static void test_moved_between_names(void) {
    printf("a sealed file moved to another name\n");

    unlock();
    check(recon_sealed_write(NAME, JAR, strlen(JAR)), "one is sealed");
    check(recon_sealed_write(OTHER, "not the jar", 11), "and so is another");

    /*
     * The check a round-trip test cannot make.
     *
     * Somebody who can write the folder copies the cookie jar over the history
     * file. Every byte of it is a byte this account sealed, so the key is
     * right and the tag is right -- and without the name in the associated
     * data it would open, and the browser would read its cookies as its
     * history, or the other way about.
     */
    size_t size = 0;
    char *jar = raw(&size);

    check(jar != NULL, "the jar's bytes are taken");
    if (jar == NULL) {
        return;
    }
    recon_fs_write("/", OTHER_PATH, jar, size);
    free(jar);

    check(recon_sealed_read(OTHER, NULL) == NULL,
        "and under the other name it does not open");

    char *own = recon_sealed_read(NAME, NULL);

    check(own != NULL, "while under its own it still does");
    free(own);
}

static void test_another_account_cannot_open_it(void) {
    printf("a file sealed by one account, under another\n");

    unlock();
    check(recon_sealed_write(NAME, JAR, strlen(JAR)), "Tester seals one");

    size_t size = 0;
    char *bytes = raw(&size);

    check(bytes != NULL, "its bytes are taken");
    if (bytes == NULL) {
        return;
    }

    recon_keyring_lock();
    check(recon_keyring_unlock("Other", "different horse"),
        "another account signs in");

    /* Put where the other account would look for it, so that what is being
     * tested is the key rather than the path. */
    recon_fs_mkdir("/", "/Users/Other/.sealed");
    recon_fs_mkdir("/", "/Users/Other/.sealed/browser");
    recon_fs_write("/", "/Users/Other/.sealed/" NAME, bytes, size);
    free(bytes);

    check(recon_sealed_exists(NAME), "the file is there for them");
    check(recon_sealed_read(NAME, NULL) == NULL, "and does not open");
}

static void test_a_name_that_could_climb_out(void) {
    printf("names that are refused rather than repaired\n");

    unlock();

    const char *bad[] = {
        "../../System/Config/users",
        "browser/../../../etc/passwd",
        "..",
        "/browser/cookies",
        "browser/",
        "",
        "browser/cookies;rm",
        "browser cookies",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        check(!recon_sealed_write(bad[i], "x", 1), bad[i][0] != 0
            ? bad[i] : "(an empty name)");
        check(recon_sealed_read(bad[i], NULL) == NULL, "and reads nothing");
    }

    char too_long[RECON_SEALED_NAME_MAX + 8];

    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = 0;
    check(!recon_sealed_write(too_long, "x", 1), "a name past the limit");

    /* And the shapes that are fine, so the rule is not simply "refuse". */
    check(recon_sealed_write("plain", "x", 1), "a bare name works");
    check(recon_sealed_write("a/b/c", "x", 1), "and so does a nested one");
    check(recon_sealed_write("with.dot", "x", 1),
        "and one dot is not two");
}

static void test_too_much_is_refused(void) {
    printf("more than a sealed file holds\n");

    unlock();

    /*
     * One byte over. Allocated rather than declared, because four megabytes on
     * the stack is a crash rather than a failed check -- and the failure being
     * looked for is the refusal.
     */
    size_t over = RECON_SEALED_MAX + 1;
    char *big = calloc(1, over);

    if (big == NULL) {
        printf("  (skipped: no memory for the oversized case)\n");
        return;
    }
    check(!recon_sealed_write("big", big, over), "is refused");
    says(recon_sealed_last_error(), "more than a sealed file holds",
        "and says so with the size");
    check(!recon_sealed_exists("big"), "and nothing was written");
    free(big);
}

static void test_the_nonce_changes(void) {
    printf("the same bytes sealed twice\n");

    unlock();
    check(recon_sealed_write(NAME, JAR, strlen(JAR)), "sealed once");

    size_t first_size = 0;
    char *first = raw(&first_size);

    check(recon_sealed_write(NAME, JAR, strlen(JAR)), "and again");

    size_t second_size = 0;
    char *second = raw(&second_size);

    check(first != NULL && second != NULL, "both are on the disk");
    if (first == NULL || second == NULL) {
        free(first);
        free(second);
        return;
    }

    same_int((int)first_size, (int)second_size, "the same length");

    /*
     * A repeated nonce under one key does not weaken GCM, it breaks it: two
     * messages under one nonce leak their difference and hand over the
     * authentication key. So identical plaintext must not give identical
     * ciphertext, and this is the only way to see that from outside.
     */
    check(memcmp(first, second, first_size) != 0,
        "and not one byte the same, because the nonce is fresh");
    free(first);
    free(second);
}

static void test_forget(void) {
    printf("forgetting one\n");

    unlock();
    check(recon_sealed_write("to-forget", "x", 1), "sealed");
    check(recon_sealed_forget("to-forget"), "and forgotten");
    check(!recon_sealed_exists("to-forget"), "the file is gone");
    check(!recon_sealed_forget("to-forget"),
        "and forgetting it again says there was nothing");
}

static void test_derived_keys(void) {
    printf("the keys sealed files are built on\n");

    unlock();

    uint8_t a[32], b[32], c[32];

    check(recon_keyring_derive("sealed-files", a, sizeof(a)), "a key derives");
    check(recon_keyring_derive("sealed-files", b, sizeof(b)), "twice");
    check(memcmp(a, b, sizeof(a)) == 0,
        "and the same purpose gives the same key, which is what makes a file "
        "written yesterday readable today");

    check(recon_keyring_derive("something-else", c, sizeof(c)),
        "another purpose derives");
    check(memcmp(a, c, sizeof(a)) != 0,
        "and is a different key, so a fault in one thing is not a fault in "
        "every thing");

    /*
     * A shorter key is the first bytes of the same HMAC, which is a
     * legitimate thing to want and is checked so that it stays true rather
     * than quietly becoming a second derivation.
     *
     * Before the refusals below, and that ordering is the point: the first
     * attempt at this compared against `a` afterwards and failed, because a
     * refused derivation **erases the buffer it was given** -- so `a` was
     * thirty-two zeroes by then. The promise doing its job, and a test written
     * as though it were not there.
     */
    uint8_t half[16];

    check(recon_keyring_derive("sealed-files", half, sizeof(half)),
        "a shorter key derives");
    check(memcmp(half, a, sizeof(half)) == 0, "and is the front of the longer");

    uint8_t big[64];

    check(!recon_keyring_derive("sealed-files", big, sizeof(big)),
        "more than one HMAC block is refused rather than invented");
    check(!recon_keyring_derive(NULL, a, sizeof(a)), "and a purpose is needed");
    check(!recon_keyring_derive("", a, sizeof(a)), "a real one");

    /* Erased on failure, so a caller that ignores the answer is not left
     * holding a key-shaped thing that is not a key. */
    uint8_t leftover[32];

    memset(leftover, 0xAB, sizeof(leftover));
    recon_keyring_derive(NULL, leftover, sizeof(leftover));

    bool cleared = true;

    for (size_t i = 0; i < sizeof(leftover); i++) {
        if (leftover[i] != 0) {
            cleared = false;
        }
    }
    check(cleared, "a refused derivation clears what it was given");

    recon_keyring_lock();
    check(!recon_keyring_derive("sealed-files", a, sizeof(a)),
        "and a locked keyring derives nothing");
    check(recon_keyring_user()[0] == 0, "nor says who it was for");
}


int main(void) {
    printf("Sealed files\n\n");

    char root[] = "/tmp/recon-sealed-test-XXXXXX";

    if (mkdtemp(root) == NULL) {
        printf("could not make a temporary root\n");
        return 1;
    }
    setenv("RECONOS_ROOT", root, 1);

    if (!recon_fs_init(NULL)) {
        printf("could not start the filesystem: %s\n", recon_fs_last_error());
        return 1;
    }
    recon_fs_mkdir("/", "/Users");
    recon_users_init();

    if (!recon_users_create("Tester", "correct horse",
            RECON_ROLE_ADMINISTRATOR)) {
        printf("could not make the test account: %s\n",
            recon_users_last_error());
        return 1;
    }
    if (!recon_users_create("Other", "different horse", RECON_ROLE_LIMITED)) {
        printf("could not make the second account: %s\n",
            recon_users_last_error());
        return 1;
    }
    recon_fs_mkdir("/", "/Users/Tester");
    recon_fs_mkdir("/", "/Users/Other");

    test_it_comes_back();
    test_an_empty_one_is_a_real_thing();
    test_locked_gives_nothing();
    test_an_edited_file_does_not_open();
    test_moved_between_names();
    test_another_account_cannot_open_it();
    test_a_name_that_could_climb_out();
    test_too_much_is_refused();
    test_the_nonce_changes();
    test_forget();
    test_derived_keys();

    recon_keyring_lock();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
