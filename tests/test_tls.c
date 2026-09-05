/*
 * Tests for the network port's encryption.
 *
 * Two failures worth catching, and neither is visible from a running system.
 *
 * The first is regeneration. A certificate that is remade every time remote
 * access is turned on has a different fingerprint every time, so every client
 * that pinned the old one refuses to connect and the person is told their
 * machine is being impersonated by itself. Trust on first use only works if
 * the identity survives.
 *
 * The second is the private key's mode. Written world-readable it is still a
 * working certificate and everything appears fine, right up until it is not.
 *
 * Run with: cmake --build build && ./build/recon_tls_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "recon_fs.h"
#include "recon_tls.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* The fingerprint from the first run, to compare the second against. */
static char g_first[RECON_TLS_FINGERPRINT_MAX];

static void test_it_makes_one(void) {
    printf("a certificate is made on first use\n");

    check(!recon_fs_exists("/", RECON_TLS_CERT_FILE),
        "there is none to start with");

    char why[256];
    check(recon_tls_init(why, sizeof(why)), "one is made");
    if (!recon_tls_ready()) {
        printf("  (%s)\n", why);
        return;
    }

    check(recon_tls_ready(), "and it is loaded");
    check(recon_fs_exists("/", RECON_TLS_CERT_FILE),
        "the certificate is on disk");
    check(recon_fs_exists("/", RECON_TLS_KEY_FILE), "and so is the key");
}

static void test_the_fingerprint_reads_right(void) {
    printf("the fingerprint is what a person can compare\n");

    check(recon_tls_fingerprint(g_first, sizeof(g_first)),
        "there is one");

    /* SHA-256 is 32 bytes: 64 hex digits and 31 colons. */
    check(strlen(g_first) == 95, "it is the length a SHA-256 makes");

    int colons = 0;
    bool shape = true;
    for (const char *c = g_first; *c != '\0'; c++) {
        if (*c == ':') {
            colons++;
        } else if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'F'))) {
            shape = false;
        }
    }

    check(colons == 31, "with a colon between every byte");
    check(shape, "and nothing but uppercase hex besides");
}

static void test_it_does_not_make_another(void) {
    printf("the identity survives being turned off and on\n");

    /*
     * The whole of trust-on-first-use rests on this. A second certificate
     * would have a second fingerprint, every client that pinned the first
     * would refuse to connect, and what they would report is that the machine
     * is being impersonated -- by itself.
     */
    recon_tls_finish();
    check(!recon_tls_ready(), "it is shut down");

    char why[256];
    check(recon_tls_init(why, sizeof(why)), "and started again");

    char again[RECON_TLS_FINGERPRINT_MAX];
    check(recon_tls_fingerprint(again, sizeof(again)),
        "there is still a fingerprint");
    check(strcmp(g_first, again) == 0,
        "and it is the same one, so a pinned client still connects");
}

static void test_the_key_is_private(void) {
    printf("the private key is readable only by its owner\n");

    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    check(recon_fs_resolve("/", RECON_TLS_KEY_FILE, host, sizeof(host),
        canonical, sizeof(canonical)), "the key can be located on the host");

    struct stat info;
    check(stat(host, &info) == 0, "and asked about");

    /*
     * Nothing for anybody else at all. A world-readable private key is a
     * working certificate that behaves correctly in every test anybody would
     * think to write, which is exactly why this one is written.
     */
    check((info.st_mode & 0077) == 0,
        "and nobody but the owner can read it");
}

int main(void) {
    char root[] = "/tmp/reconos-tls-XXXXXX";
    if (mkdtemp(root) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    printf("ReconOS TLS tests, root %s\n\n", root);

    if (!recon_fs_init(root)) {
        printf("could not set up the test filesystem: %s\n",
            recon_fs_last_error());
        return 1;
    }

    test_it_makes_one();
    test_the_fingerprint_reads_right();
    test_it_does_not_make_another();
    test_the_key_is_private();

    recon_tls_finish();
    recon_fs_finish();

    char command[512];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    if (system(command) != 0) {
        printf("\nnote: could not remove %s\n", root);
    }

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
