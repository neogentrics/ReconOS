/*
 * SHA-256, HMAC and PBKDF2. See include/recon_crypt.h.
 *
 * Written from the published specifications (FIPS 180-4, RFC 2104, RFC 8018)
 * and checked against their published test vectors. Nothing here is original,
 * and it should stay that way.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "recon_crypt.h"

/* --- SHA-256 --- */

/* The first thirty-two bits of the fractional parts of the cube roots of the
 * first sixty-four primes. From FIPS 180-4. */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotate_right(uint32_t value, int by) {
    return (value >> by) | (value << (32 - by));
}

static void compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];

    /* Big-endian, as the specification says, regardless of this machine. */
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^
            (w[i - 15] >> 3);
        uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^
            (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + choose + K[i] + w[i];
        uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + majority;

        h = g; g = f; f = e;
        e = d + temp1;
        d = c; c = b; b = a;
        a = temp1 + temp2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void recon_sha256_init(struct recon_sha256 *ctx) {
    /* Fractional parts of the square roots of the first eight primes. */
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->length = 0;
    ctx->buffered = 0;
}

void recon_sha256_update(struct recon_sha256 *ctx, const void *data, size_t size) {
    const uint8_t *bytes = data;
    ctx->length += (uint64_t)size * 8;

    while (size > 0) {
        size_t room = RECON_SHA256_BLOCK - ctx->buffered;
        size_t take = size < room ? size : room;

        memcpy(ctx->buffer + ctx->buffered, bytes, take);
        ctx->buffered += take;
        bytes += take;
        size -= take;

        if (ctx->buffered == RECON_SHA256_BLOCK) {
            compress(ctx->state, ctx->buffer);
            ctx->buffered = 0;
        }
    }
}

void recon_sha256_final(struct recon_sha256 *ctx, uint8_t out[RECON_SHA256_SIZE]) {
    uint64_t length = ctx->length;

    /* A single one bit, then zeroes, then the length. */
    uint8_t one = 0x80;
    recon_sha256_update(ctx, &one, 1);
    ctx->length = length; /* padding is not part of the message */

    uint8_t zero = 0;
    while (ctx->buffered != 56) {
        recon_sha256_update(ctx, &zero, 1);
        ctx->length = length;
    }

    uint8_t tail[8];
    for (int i = 0; i < 8; i++) {
        tail[i] = (uint8_t)(length >> (56 - i * 8));
    }
    recon_sha256_update(ctx, tail, 8);

    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void recon_sha256(const void *data, size_t size, uint8_t out[RECON_SHA256_SIZE]) {
    struct recon_sha256 ctx;
    recon_sha256_init(&ctx);
    recon_sha256_update(&ctx, data, size);
    recon_sha256_final(&ctx, out);
}

/* --- HMAC, from RFC 2104 --- */

void recon_hmac_sha256(const void *key, size_t key_size,
        const void *data, size_t data_size, uint8_t out[RECON_SHA256_SIZE]) {
    uint8_t padded[RECON_SHA256_BLOCK];
    memset(padded, 0, sizeof(padded));

    /* A key longer than a block is replaced by its hash; a shorter one is
     * padded with zeroes. */
    if (key_size > RECON_SHA256_BLOCK) {
        recon_sha256(key, key_size, padded);
    } else {
        memcpy(padded, key, key_size);
    }

    uint8_t inner_pad[RECON_SHA256_BLOCK];
    uint8_t outer_pad[RECON_SHA256_BLOCK];
    for (int i = 0; i < RECON_SHA256_BLOCK; i++) {
        inner_pad[i] = padded[i] ^ 0x36;
        outer_pad[i] = padded[i] ^ 0x5c;
    }

    uint8_t inner[RECON_SHA256_SIZE];
    struct recon_sha256 ctx;
    recon_sha256_init(&ctx);
    recon_sha256_update(&ctx, inner_pad, sizeof(inner_pad));
    recon_sha256_update(&ctx, data, data_size);
    recon_sha256_final(&ctx, inner);

    recon_sha256_init(&ctx);
    recon_sha256_update(&ctx, outer_pad, sizeof(outer_pad));
    recon_sha256_update(&ctx, inner, sizeof(inner));
    recon_sha256_final(&ctx, out);
}

/* --- PBKDF2, from RFC 8018 --- */

void recon_pbkdf2_sha256(const char *password, const uint8_t *salt,
        size_t salt_size, uint32_t iterations, uint8_t *out, size_t out_size) {
    size_t password_size = strlen(password);
    uint32_t block_index = 1;
    size_t produced = 0;

    if (iterations == 0) {
        iterations = 1;
    }

    while (produced < out_size) {
        /* Each block hashes the salt followed by the block number, so
         * different blocks of the output are not the same bytes. */
        uint8_t seed[RECON_SHA256_BLOCK * 2 + 4];
        size_t seed_size = salt_size;
        if (seed_size > sizeof(seed) - 4) {
            seed_size = sizeof(seed) - 4;
        }
        memcpy(seed, salt, seed_size);
        seed[seed_size] = (uint8_t)(block_index >> 24);
        seed[seed_size + 1] = (uint8_t)(block_index >> 16);
        seed[seed_size + 2] = (uint8_t)(block_index >> 8);
        seed[seed_size + 3] = (uint8_t)(block_index);

        uint8_t current[RECON_SHA256_SIZE];
        uint8_t accumulated[RECON_SHA256_SIZE];

        recon_hmac_sha256(password, password_size, seed, seed_size + 4, current);
        memcpy(accumulated, current, sizeof(current));

        /* Every round folds into the last. This is the cost. */
        for (uint32_t i = 1; i < iterations; i++) {
            recon_hmac_sha256(password, password_size, current,
                sizeof(current), current);
            for (size_t j = 0; j < sizeof(accumulated); j++) {
                accumulated[j] ^= current[j];
            }
        }

        size_t take = out_size - produced;
        if (take > sizeof(accumulated)) {
            take = sizeof(accumulated);
        }
        memcpy(out + produced, accumulated, take);
        produced += take;
        block_index++;
    }
}

/* --- Odds and ends --- */

bool recon_random_bytes(void *out, size_t size) {
    /*
     * From the kernel's pool. Read rather than generated here on purpose:
     * there is no way to write a good random number generator from inside a
     * program, and a predictable salt is barely a salt at all.
     */
    FILE *source = fopen("/dev/urandom", "rb");
    if (source == NULL) {
        return false;
    }

    size_t got = fread(out, 1, size, source);
    fclose(source);
    return got == size;
}

bool recon_equal_constant_time(const void *a, const void *b, size_t size) {
    const uint8_t *x = a;
    const uint8_t *y = b;
    uint8_t differences = 0;

    /* Every byte is looked at, whatever the first one said. Stopping early
     * would make the time taken report how much of a guess was right. */
    for (size_t i = 0; i < size; i++) {
        differences |= (uint8_t)(x[i] ^ y[i]);
    }
    return differences == 0;
}

void recon_secure_erase(void *data, size_t size) {
    if (data == NULL || size == 0) {
        return;
    }

    /*
     * Written through a volatile pointer, one byte at a time.
     *
     * `volatile` tells the compiler that these stores have an effect it cannot
     * see, so it may not decide they are dead and remove them -- which is
     * exactly what it is entitled to do to a plain memset whose result nothing
     * reads. See the note in recon_crypt.h.
     *
     * A byte at a time, and slowly, and that is fine: this runs when a window
     * closes or an account is forgotten, not in a loop.
     */
    volatile unsigned char *at = data;
    while (size-- > 0) {
        *at++ = 0;
    }
}

void recon_to_hex(const uint8_t *bytes, size_t size, char *out) {
    static const char DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < size; i++) {
        out[i * 2] = DIGITS[bytes[i] >> 4];
        out[i * 2 + 1] = DIGITS[bytes[i] & 0x0F];
    }
    out[size * 2] = '\0';
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool recon_from_hex(const char *text, uint8_t *out, size_t size) {
    if (text == NULL || strlen(text) != size * 2) {
        return false;
    }
    for (size_t i = 0; i < size; i++) {
        int high = hex_value(text[i * 2]);
        int low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}
