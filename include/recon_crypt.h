/*
 * The small amount of cryptography ReconOS needs to store a password.
 *
 * SHA-256, HMAC-SHA-256 and PBKDF2, implemented here because ReconOS has no
 * dependencies to borrow them from and will one day have no host to borrow
 * them from either.
 *
 * These are deliberately *not* an invention. They are published algorithms
 * with published test vectors, and tests/test_crypt.c checks this code against
 * the vectors from RFC 6234 and RFC 6070 rather than against itself. A hash
 * that is merely self-consistent is a hash that is wrong in a way nobody
 * notices.
 *
 * The project intends a cryptography scheme of its own eventually. That will
 * be a thing to study and break; it will not be what guards anybody's
 * password. This is.
 */

#ifndef RECON_CRYPT_H
#define RECON_CRYPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RECON_SHA256_SIZE 32
#define RECON_SHA256_BLOCK 64

struct recon_sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t buffer[RECON_SHA256_BLOCK];
    size_t buffered;
};

void recon_sha256_init(struct recon_sha256 *ctx);
void recon_sha256_update(struct recon_sha256 *ctx, const void *data, size_t size);
void recon_sha256_final(struct recon_sha256 *ctx, uint8_t out[RECON_SHA256_SIZE]);

/* The whole thing at once, for when there is nothing to stream. */
void recon_sha256(const void *data, size_t size, uint8_t out[RECON_SHA256_SIZE]);

void recon_hmac_sha256(const void *key, size_t key_size,
    const void *data, size_t data_size, uint8_t out[RECON_SHA256_SIZE]);

/*
 * PBKDF2-HMAC-SHA256.
 *
 * The iteration count is the whole point: it makes each guess cost something,
 * so a stolen file is expensive to attack rather than free. Higher is safer
 * and slower, and the number used is stored alongside each password so it can
 * be raised later without invalidating what is already there.
 */
void recon_pbkdf2_sha256(const char *password, const uint8_t *salt,
    size_t salt_size, uint32_t iterations, uint8_t *out, size_t out_size);

/* --- Odds and ends --- */

/*
 * Random bytes, from the operating system. False if none could be had, which
 * must be treated as a failure rather than quietly filled with zeroes -- a
 * predictable salt is barely a salt.
 */
bool recon_random_bytes(void *out, size_t size);

/*
 * Compare without leaking where the difference was.
 *
 * An ordinary comparison stops at the first byte that differs, and how long it
 * took says how much of the guess was right. Everything here takes the same
 * time whatever it is given.
 */
bool recon_equal_constant_time(const void *a, const void *b, size_t size);

/*
 * Erase memory that held a secret, in a way the compiler may not remove.
 *
 * `memset(p, 0, n); free(p);` is the obvious way to write this and it is not
 * reliable. Those stores are never read afterwards, so a compiler is entitled
 * to delete the whole call as dead -- and at higher optimisation levels they
 * do. The result is a program whose source says the password was erased and
 * whose binary leaves it in the heap for whatever gets that memory next.
 *
 * It is a known enough mistake to have a number: CWE-14, "compiler removal of
 * code to clear buffers".
 *
 * Checked rather than assumed, because the interesting part is that it is not
 * always wrong: the calls in recon_mail.c survived at this project's current
 * optimisation level, verified with objdump. Correct today and not guaranteed
 * tomorrow is not the same as correct, and a security property that depends on
 * which flags somebody built with is not a property.
 *
 * This writes through a volatile pointer, which the standard does not allow to
 * be elided. It is slower than memset by a good deal and is used only where
 * something secret was.
 */
void recon_secure_erase(void *data, size_t size);

/* Hex, for writing a hash into a text file and reading it back. */
void recon_to_hex(const uint8_t *bytes, size_t size, char *out);
bool recon_from_hex(const char *text, uint8_t *out, size_t size);

#endif
