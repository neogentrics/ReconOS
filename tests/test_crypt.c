/*
 * Checks the hashing against published test vectors.
 *
 * The point is that these answers come from outside this project. A hash that
 * agrees with itself is a hash that is wrong in a way nobody notices, and the
 * failure mode is silent: passwords still "work", they are just protected by
 * something that is not SHA-256.
 *
 * Vectors: FIPS 180-4 and RFC 6234 for SHA-256, RFC 4231 for HMAC, RFC 6070
 * for PBKDF2 (which specifies SHA-1; the SHA-256 values here are the widely
 * published equivalents for the same inputs).
 *
 * Run with: ninja -C build && ./build/recon_crypt_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_crypt.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_hex(const uint8_t *got, size_t size, const char *expected,
        const char *what) {
    char text[256];
    recon_to_hex(got, size, text);

    g_checks++;
    if (strcmp(text, expected) != 0) {
        g_failures++;
        printf("  FAIL: %s\n", what);
        printf("        wanted %s\n", expected);
        printf("        got    %s\n", text);
    }
}

/* --- SHA-256 --- */

static void test_sha256(void) {
    printf("SHA-256, against FIPS 180-4 and RFC 6234\n");

    uint8_t out[RECON_SHA256_SIZE];

    /* The empty string. Worth having because a length of zero is exactly
     * where padding code tends to go wrong. */
    recon_sha256("", 0, out);
    check_hex(out, sizeof(out),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "the empty string");

    recon_sha256("abc", 3, out);
    check_hex(out, sizeof(out),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "\"abc\"");

    /* 56 bytes: one byte short of needing a second block, which is the case
     * that catches an off-by-one in the padding. */
    const char *fifty_six =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    recon_sha256(fifty_six, strlen(fifty_six), out);
    check_hex(out, sizeof(out),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "a message that just fills a block");

    /* Longer than one block, so the streaming path runs more than once. */
    const char *long_message =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    recon_sha256(long_message, strlen(long_message), out);
    check_hex(out, sizeof(out),
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
        "a message spanning two blocks");

    /* A million 'a's, from FIPS 180-4. Slow-ish, and the only vector that
     * exercises the length counter past a small value. */
    struct recon_sha256 ctx;
    recon_sha256_init(&ctx);
    for (int i = 0; i < 1000; i++) {
        char chunk[1000];
        memset(chunk, 'a', sizeof(chunk));
        recon_sha256_update(&ctx, chunk, sizeof(chunk));
    }
    recon_sha256_final(&ctx, out);
    check_hex(out, sizeof(out),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "a million letters, fed in pieces");
}

/* --- HMAC --- */

static void test_hmac(void) {
    printf("HMAC-SHA-256, against RFC 4231\n");

    uint8_t out[RECON_SHA256_SIZE];

    uint8_t key1[20];
    memset(key1, 0x0b, sizeof(key1));
    recon_hmac_sha256(key1, sizeof(key1), "Hi There", 8, out);
    check_hex(out, sizeof(out),
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
        "case 1");

    recon_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
    check_hex(out, sizeof(out),
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
        "case 2");

    /* A key longer than a block, which must be hashed down first. */
    uint8_t key5[131];
    memset(key5, 0xaa, sizeof(key5));
    const char *data5 = "Test Using Larger Than Block-Size Key - Hash Key First";
    recon_hmac_sha256(key5, sizeof(key5), data5, strlen(data5), out);
    check_hex(out, sizeof(out),
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
        "case 6, a key longer than a block");
}

/* --- PBKDF2 --- */

static void test_pbkdf2(void) {
    printf("PBKDF2-HMAC-SHA256, against the published vectors\n");

    uint8_t out[40];

    recon_pbkdf2_sha256("password", (const uint8_t *)"salt", 4, 1, out, 32);
    check_hex(out, 32,
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b",
        "one iteration");

    recon_pbkdf2_sha256("password", (const uint8_t *)"salt", 4, 2, out, 32);
    check_hex(out, 32,
        "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43",
        "two iterations");

    recon_pbkdf2_sha256("password", (const uint8_t *)"salt", 4, 4096, out, 32);
    check_hex(out, 32,
        "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a",
        "four thousand iterations");

    /*
     * Output longer than one hash, so the block-index path runs more than
     * once. Getting this wrong repeats the same 32 bytes, which looks fine
     * until someone asks for a long key.
     */
    recon_pbkdf2_sha256("passwordPASSWORDpassword",
        (const uint8_t *)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096,
        out, 40);
    check_hex(out, 40,
        "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1"
        "c635518c7dac47e9",
        "output longer than one hash");
}

/* --- The pieces around them --- */

static void test_helpers(void) {
    printf("the pieces around them\n");

    uint8_t bytes[4] = { 0x00, 0x7f, 0x80, 0xff };
    char text[16];
    recon_to_hex(bytes, sizeof(bytes), text);
    check(strcmp(text, "007f80ff") == 0, "bytes become hex");

    uint8_t back[4];
    check(recon_from_hex("007f80ff", back, sizeof(back)), "hex becomes bytes");
    check(memcmp(bytes, back, sizeof(bytes)) == 0, "and survives the trip");

    check(!recon_from_hex("007f80f", back, sizeof(back)),
        "an odd number of digits is refused");
    check(!recon_from_hex("007f80fg", back, sizeof(back)),
        "a non-hex character is refused");
    check(!recon_from_hex("007f80ffff", back, sizeof(back)),
        "the wrong length is refused");

    check(recon_equal_constant_time("secret", "secret", 6), "equal is equal");
    check(!recon_equal_constant_time("secret", "secrat", 6),
        "a difference in the middle is caught");
    check(!recon_equal_constant_time("secret", "secreT", 6),
        "a difference at the end is caught");
    check(!recon_equal_constant_time("aecret", "secret", 6),
        "a difference at the start is caught");

    /* Not a test of timing -- that cannot be asserted reliably -- but of the
     * thing that makes timing constant: every byte is examined. */
    uint8_t a[64], b[64];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    b[63] = 1;
    check(!recon_equal_constant_time(a, b, sizeof(a)),
        "a difference in the very last byte is still caught");

    uint8_t random1[32], random2[32];
    check(recon_random_bytes(random1, sizeof(random1)), "random bytes arrive");
    check(recon_random_bytes(random2, sizeof(random2)), "and again");
    check(memcmp(random1, random2, sizeof(random1)) != 0,
        "two draws differ, so they are not a constant");
}

int main(void) {
    printf("ReconOS hashing tests\n\n");

    test_sha256();
    test_hmac();
    test_pbkdf2();
    test_helpers();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
