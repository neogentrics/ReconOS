/*
 * Tests for the keyring.
 *
 * What is worth checking here is not "does a secret come back" -- it does, and
 * a test that only checked that would pass against a version that stored
 * everything in plain text. What is worth checking is the things that make it
 * a keyring rather than a file:
 *
 *   - a locked keyring hands out nothing
 *   - the wrong password does not decrypt
 *   - the file on disk does not contain the secret
 *   - an edited file does not decrypt, rather than decrypting to something
 *   - a ciphertext moved to another name does not decrypt
 *   - two writes of the same secret do not produce the same bytes
 *
 * The last one is the nonce. Two identical ciphertexts under one key would
 * mean the nonce is being reused, which does not weaken GCM -- it breaks it --
 * and it is invisible from every other test in this file.
 *
 * Run with: cmake --build build && ./build/recon_keyring_tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_crypt.h"
#include "recon_fs.h"
#include "recon_keyring.h"
#include "recon_users.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    } else {
        printf("  ok    %s\n", what);
    }
}

/* The keyring's file, read straight off disk, so a test can look at what was
 * actually written rather than at what this program believes. */
static char *read_store(const char *user, size_t *size) {
    char path[RECON_PATH_MAX];
    snprintf(path, sizeof(path), "/Users/%s/.keyring", user);
    return recon_fs_read("/", path, size);
}

static void test_locked_gives_nothing(void) {
    printf("A locked keyring\n");

    recon_keyring_lock();
    check(!recon_keyring_unlocked(), "says it is locked");

    char out[64] = "not touched";
    check(!recon_keyring_get("mail/password", out, sizeof(out)),
        "refuses to read");
    check(out[0] == '\0',
        "and clears the buffer, so a caller ignoring the answer holds nothing");
    check(!recon_keyring_put("mail/password", "hunter2"),
        "and refuses to write");
}

static void test_round_trip(void) {
    printf("Keeping and reading back\n");

    check(recon_keyring_unlock("Tester", "correct horse"),
        "unlocks with the account password");
    check(recon_keyring_unlocked(), "and says so");

    check(recon_keyring_put("mail/password", "hunter2"), "a secret is kept");

    char out[64];
    check(recon_keyring_get("mail/password", out, sizeof(out)),
        "and comes back");
    check(strcmp(out, "hunter2") == 0, "unchanged");

    check(!recon_keyring_get("nothing/here", out, sizeof(out)),
        "a name nobody kept anything under is not found");

    check(recon_keyring_put("mail/password", "different"),
        "writing the same name again replaces it");
    check(recon_keyring_get("mail/password", out, sizeof(out)) &&
        strcmp(out, "different") == 0, "with the new value");

    check(recon_keyring_count() == 1, "and there is still one of it");
}

static void test_not_in_the_file(void) {
    printf("What reaches the disk\n");

    check(recon_keyring_put("mail/password", "swordfish"), "a secret is kept");

    size_t size = 0;
    char *text = read_store("Tester", &size);
    check(text != NULL, "the file exists");
    if (text == NULL) {
        return;
    }

    check(strstr(text, "swordfish") == NULL,
        "AND THE SECRET IS NOT IN IT");
    check(strstr(text, "mail/password") != NULL,
        "the name is, because an entry has to be findable");
    check(strstr(text, "correct horse") == NULL,
        "and neither is the account password");

    free(text);
}

static void test_the_nonce_changes(void) {
    printf("The nonce\n");

    /*
     * The same secret, under the same name, written twice. If the bytes on
     * disk are identical the nonce is being reused -- which is the one mistake
     * in this whole file that no other test can see, and the one that turns
     * GCM from strong into broken.
     */
    check(recon_keyring_put("same", "identical secret"), "written once");
    size_t size_a = 0;
    char *first = read_store("Tester", &size_a);

    check(recon_keyring_put("same", "identical secret"), "written again");
    size_t size_b = 0;
    char *second = read_store("Tester", &size_b);

    check(first != NULL && second != NULL, "both were written");
    if (first != NULL && second != NULL) {
        check(strcmp(first, second) != 0,
            "AND THE TWO FILES DIFFER, so the nonce was fresh");
    }

    free(first);
    free(second);
}

static void test_wrong_password(void) {
    printf("The wrong password\n");

    check(recon_keyring_put("mail/password", "swordfish"), "a secret is kept");
    recon_keyring_lock();

    check(recon_keyring_unlock("Tester", "wrong"),
        "unlocking with the wrong password does not fail here");

    char out[64] = "not touched";
    check(!recon_keyring_get("mail/password", out, sizeof(out)),
        "but the secret does not come back");
    check(out[0] == '\0', "and nothing is left in the buffer");

    /* And the right one still works, so the wrong attempt did no damage. */
    recon_keyring_lock();
    check(recon_keyring_unlock("Tester", "correct horse"), "unlock again");
    check(recon_keyring_get("mail/password", out, sizeof(out)) &&
        strcmp(out, "swordfish") == 0,
        "the right password still reads it");
}

static void test_edited_file(void) {
    printf("An edited file\n");

    check(recon_keyring_put("mail/password", "swordfish"), "a secret is kept");

    size_t size = 0;
    char *text = read_store("Tester", &size);
    if (text == NULL) {
        check(false, "the file could be read");
        return;
    }

    /*
     * One byte of this entry's ciphertext, changed.
     *
     * The last hex digit on the line the secret is on, flipped to something
     * else. GCM's tag covers the ciphertext, so this must be refused rather
     * than decrypted into a secret that is one character different -- which is
     * what a cipher without authentication would hand back, and what somebody
     * would then use as a password.
     *
     * It has to be *this* entry's line rather than the last line in the file,
     * which is a mistake this test made first: the file holds other entries by
     * now, corrupting one of those proves nothing about reading this one, and
     * the test passed while checking the wrong thing.
     */
    char *line = strstr(text, "mail/password");
    check(line != NULL, "the entry is in the file");
    if (line == NULL) {
        free(text);
        return;
    }
    char *end = strchr(line, '\n');
    if (end == NULL) {
        end = text + strlen(text);
    }
    if (end > line) {
        end[-1] = (end[-1] == 'a') ? 'b' : 'a';
    }

    char path[RECON_PATH_MAX];
    snprintf(path, sizeof(path), "/Users/%s/.keyring", "Tester");
    check(recon_fs_write("/", path, text, strlen(text)), "the file is edited");
    free(text);

    recon_keyring_lock();
    check(recon_keyring_unlock("Tester", "correct horse"), "unlock again");

    char out[64] = "not touched";
    check(!recon_keyring_get("mail/password", out, sizeof(out)),
        "AND THE EDITED SECRET IS REFUSED, not decrypted to something else");
    check(out[0] == '\0', "with nothing left in the buffer");
}

static void test_moved_between_names(void) {
    printf("A ciphertext moved to another name\n");

    recon_keyring_lock();
    check(recon_keyring_unlock("Tester", "correct horse"), "unlock");
    check(recon_keyring_forget("mail/password") ||
        recon_keyring_count() >= 0, "start from a known state");

    check(recon_keyring_put("mail/password", "the mail one"), "one secret");
    check(recon_keyring_put("other/password", "the other one"), "and another");

    size_t size = 0;
    char *text = read_store("Tester", &size);
    if (text == NULL) {
        check(false, "the file could be read");
        return;
    }

    /*
     * Swap the two names, leaving the ciphertexts where they are.
     *
     * The name is passed to GCM as associated data, so each ciphertext is
     * bound to the name it was written under. Without that, somebody who could
     * write this file could move a password from one entry to another and
     * every byte would still verify -- and a program reading "other/password"
     * would be handed the mail password.
     */
    char *a = strstr(text, "mail/password");
    char *b = strstr(text, "other/password");
    check(a != NULL && b != NULL, "both names are in the file");
    if (a != NULL && b != NULL) {
        /* "mail/" and "other" are both five characters; swapping those five
         * turns each name into the other's without changing any length. */
        char keep[6];
        memcpy(keep, a, 5);
        memcpy(a, b, 5);
        memcpy(b, keep, 5);
    }

    char path[RECON_PATH_MAX];
    snprintf(path, sizeof(path), "/Users/%s/.keyring", "Tester");
    check(recon_fs_write("/", path, text, strlen(text)), "the file is edited");
    free(text);

    recon_keyring_lock();
    check(recon_keyring_unlock("Tester", "correct horse"), "unlock again");

    char out[64] = "not touched";
    bool got = recon_keyring_get("mail/password", out, sizeof(out));
    check(!got || strcmp(out, "the other one") != 0,
        "A SWAPPED CIPHERTEXT DOES NOT BECOME THE OTHER SECRET");
}

static void test_no_password_no_keyring(void) {
    printf("An account with no password\n");

    recon_keyring_lock();
    check(!recon_keyring_unlock("Nobody", ""),
        "an account with no password has no keyring");
    check(!recon_keyring_unlocked(), "and stays locked");
}

int main(void) {
    printf("Keyring\n\n");

    /* A filesystem of its own, so this never touches a real one. */
    char root[] = "/tmp/recon-keyring-test-XXXXXX";
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
    recon_users_create("Nobody", NULL, RECON_ROLE_LIMITED);
    recon_fs_mkdir("/", "/Users/Tester");

    test_locked_gives_nothing();
    test_round_trip();
    test_not_in_the_file();
    test_the_nonce_changes();
    test_wrong_password();
    test_edited_file();
    test_moved_between_names();
    test_no_password_no_keyring();

    recon_keyring_lock();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
