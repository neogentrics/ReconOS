/*
 * Signatures.
 *
 * The claims worth testing here are not "does a signature verify" -- mbedTLS
 * is responsible for that and is better tested than anything written here.
 * They are the ones this file decides:
 *
 *   a changed byte stops verifying
 *   an untrusted key's signature does not verify against a trusted store
 *   distrusting a key stops what it signed from verifying
 *   a private key cannot be added to the trust store
 *   a key name cannot escape the trust directory
 *   signing twice over the same bytes gives the same signature
 *
 * That last one is the deterministic-nonce property, and it is the one that
 * would silently stop holding if mbedTLS were rebuilt without
 * MBEDTLS_ECDSA_DETERMINISTIC. There is a #error for that in recon_sign.c,
 * and this is the check that would notice if the #error were ever removed.
 *
 * Run with: ./build/recon_sign_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_fs.h"
#include "recon_sign.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* --- Tests --- */

static void test_a_signature_covers_the_bytes(void) {
    printf("a signature verifies, and stops verifying when a byte changes\n");

    check(recon_sign_make_key("tester"), "a key can be made");
    check(recon_sign_have_own_key(), "and the machine has one now");

    const char *message = "the quick brown fox";
    char signature[RECON_SIGN_MAX];
    check(recon_sign_data(message, strlen(message), signature,
        sizeof(signature)), "something can be signed");

    char signer[RECON_SIGN_NAME_MAX];
    check(recon_sign_verify(message, strlen(message), signature,
        signer, sizeof(signer)), "and the signature verifies");
    check(strcmp(signer, "tester") == 0, "naming the key that made it");

    /* One byte, in the middle. */
    const char *changed = "the quick brown box";
    check(!recon_sign_verify(changed, strlen(changed), signature,
        signer, sizeof(signer)), "a changed byte stops it verifying");

    /* One byte fewer. */
    check(!recon_sign_verify(message, strlen(message) - 1, signature,
        signer, sizeof(signer)), "so does a shorter message");

    /* And a signature that is not one. */
    check(!recon_sign_verify(message, strlen(message), "00ff", signer,
        sizeof(signer)), "nonsense does not verify");
    check(!recon_sign_verify(message, strlen(message), "", signer,
        sizeof(signer)), "nor does nothing");
    check(!recon_sign_verify(message, strlen(message), "zz", signer,
        sizeof(signer)), "nor does something that is not hex");
}

static void test_signing_is_deterministic(void) {
    printf("the same bytes under the same key give the same signature\n");

    /*
     * Which is the point. ECDSA with a random per-signature secret is one
     * repeat away from handing over the private key, and a repeat is exactly
     * what a failing randomness source produces. RFC 6979 removes the random
     * value entirely: `k` comes from the key and the message.
     *
     * If this ever starts failing, mbedTLS has been built without
     * MBEDTLS_ECDSA_DETERMINISTIC and every signature this machine makes is
     * one bad `k` away from being its last.
     */
    const char *message = "sign me twice";
    char first[RECON_SIGN_MAX];
    char second[RECON_SIGN_MAX];

    check(recon_sign_data(message, strlen(message), first, sizeof(first)),
        "it signs once");
    check(recon_sign_data(message, strlen(message), second, sizeof(second)),
        "and again");
    check(strcmp(first, second) == 0,
        "and both signatures are identical, so there is no random k");

    /* Different message, different signature -- the obvious other half. */
    char other[RECON_SIGN_MAX];
    check(recon_sign_data("something else", 14, other, sizeof(other)),
        "a different message signs");
    check(strcmp(first, other) != 0, "and gives a different signature");
}

static void test_only_a_trusted_key_counts(void) {
    printf("a signature from a key nobody trusts does not verify\n");

    const char *message = "who signed this";
    char signature[RECON_SIGN_MAX];
    char signer[RECON_SIGN_NAME_MAX];

    if (!recon_sign_data(message, strlen(message), signature,
            sizeof(signature))) {
        check(false, "it signs");
        return;
    }
    check(recon_sign_verify(message, strlen(message), signature, signer,
        sizeof(signer)), "it verifies while the key is trusted");

    /*
     * Distrusting is what makes the store mean anything. A signature that
     * still verified after its key was removed would make the trust store a
     * label rather than a decision.
     */
    check(recon_sign_distrust("tester"), "the key can be distrusted");
    check(!recon_sign_verify(message, strlen(message), signature, signer,
        sizeof(signer)), "and then the same signature does not verify");
    check(recon_sign_trusted_count() == 0, "nothing is trusted now");

    check(!recon_sign_distrust("tester"),
        "distrusting it twice says no the second time");
}

static void test_the_trust_store_refuses_a_private_key(void) {
    printf("a private key cannot be put where everything can read it\n");

    /*
     * The mistake this catches is one keystroke: trusting `signing.key`
     * instead of `signing.pub`. Without the check the machine would report
     * success and would have published the key it signs with.
     */
    check(!recon_sign_trust(RECON_SIGN_OWN_KEY, "oops"),
        "the machine's own private key is refused");
    check(recon_sign_trusted_count() == 0, "and nothing was added");

    /* Something that is not a key at all. */
    recon_fs_write("/", "/Temp/notakey.txt", "hello", 5);
    check(!recon_sign_trust("/Temp/notakey.txt", "nope"),
        "a file that is not a key is refused");

    check(!recon_sign_trust("/Temp/does-not-exist", "gone"),
        "a file that is not there is refused");
}

static void test_a_key_name_cannot_escape(void) {
    printf("a key name is a name, not a path\n");

    /*
     * `recon_sign_trust(x, "../../System/Config/signing")` would write into
     * the directory holding the private key. The name is checked rather than
     * the resulting path, because a check on the path is a check somebody has
     * to keep correct as the path-joining changes.
     */
    check(!recon_sign_make_key("../escape"), "a name with a slash is refused");
    check(!recon_sign_make_key(".."), "so is a bare parent");
    check(!recon_sign_make_key("has space"), "so is a space");
    check(!recon_sign_make_key("with.dot"), "so is a dot");
    check(!recon_sign_make_key(""), "so is nothing");
    check(!recon_sign_make_key(NULL), "so is no name at all");
}

static void test_a_key_is_not_replaced_quietly(void) {
    printf("making a key over one that exists is refused\n");

    check(recon_sign_make_key("first"), "a key is made");

    /*
     * Overwriting would leave everything the old key signed unverifiable,
     * silently. Somebody who means to replace one distrusts it first, which
     * is a sentence they have to type.
     */
    check(!recon_sign_make_key("first"), "the same name again is refused");
    check(!recon_sign_make_key("second"),
        "and so is a second key, because the machine signs with one");

    check(recon_sign_trusted_count() == 1, "there is one trusted key");

    char name[RECON_SIGN_NAME_MAX];
    check(recon_sign_trusted_at(0, name, sizeof(name)) &&
        strcmp(name, "first") == 0, "and it can be listed by name");
    check(!recon_sign_trusted_at(1, name, sizeof(name)),
        "and there is no second one");
}

int main(void) {
    printf("ReconOS signature tests\n\n");

    /*
     * A filesystem of its own, thrown away after. These tests make keys and
     * change what the machine trusts, which is not something to do to whatever
     * root happens to be configured.
     */
    char root[] = "/tmp/reconos-sign-tests-XXXXXX";
    if (mkdtemp(root) == NULL) {
        printf("  FAIL: no temporary directory to work in\n");
        return 1;
    }
    if (!recon_fs_init(root)) {
        printf("  FAIL: %s\n", recon_fs_last_error());
        return 1;
    }

    test_a_signature_covers_the_bytes();
    test_signing_is_deterministic();
    test_only_a_trusted_key_counts();
    test_the_trust_store_refuses_a_private_key();
    test_a_key_name_cannot_escape();

    /* The machine's own key was made by the first test; clear it so the last
     * test starts from nothing, which is what it is about. */
    recon_fs_remove("/", RECON_SIGN_OWN_KEY);
    test_a_key_is_not_replaced_quietly();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
