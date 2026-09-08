/*
 * Signatures. See include/recon_sign.h for the design and its limits.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>

#include "recon_crypt.h"
#include "recon_fs.h"
#include "recon_sign.h"

/*
 * The one thing this file will not build without.
 *
 * With MBEDTLS_ECDSA_DETERMINISTIC defined, mbedtls_pk_sign over an EC key
 * derives its per-signature secret from the key and the message hash by way of
 * HMAC-DRBG (RFC 6979). Without it, the same call signs with a random `k` --
 * and a `k` that repeats under one key hands over the private key outright.
 *
 * The difference is invisible: the same function, the same arguments, a
 * signature that verifies either way. So it is checked here rather than
 * assumed, because the failure mode of assuming is a build that looks
 * identical and is not.
 */
#if !defined(MBEDTLS_ECDSA_DETERMINISTIC)
#error "mbedTLS must be built with MBEDTLS_ECDSA_DETERMINISTIC. Without it, \
signing uses a random per-signature secret, and one repeat under a key gives \
that key away. See the note at the top of include/recon_sign.h."
#endif

/* NIST P-256, which is what mbedTLS 2.x can do correctly. */
#define CURVE MBEDTLS_ECP_DP_SECP256R1

/* Room for a DER-encoded P-256 signature, which is at most 72 bytes. */
#define SIG_BYTES_MAX 80

/* How many keys the machine may trust. A bound, not a prediction: a trust
 * store nobody can read through is a trust store nobody audits. */
#define TRUSTED_MAX 32

static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_sign_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* --- Randomness --- */

/*
 * A DRBG seeded from the system's entropy, for key generation and for the
 * blinding mbedTLS applies during signing.
 *
 * Set up per call rather than once. This runs when somebody makes a key or
 * signs a package -- not in a loop -- and a long-lived generator is a long-
 * lived piece of state to reason about for no gain.
 */
struct rng {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
};

static bool rng_start(struct rng *r, const char *purpose) {
    mbedtls_entropy_init(&r->entropy);
    mbedtls_ctr_drbg_init(&r->drbg);

    const char *tag = "reconos-sign";
    if (mbedtls_ctr_drbg_seed(&r->drbg, mbedtls_entropy_func, &r->entropy,
            (const unsigned char *)tag, strlen(tag)) != 0) {
        mbedtls_ctr_drbg_free(&r->drbg);
        mbedtls_entropy_free(&r->entropy);
        set_error("there was no randomness to %s with", purpose);
        return false;
    }
    return true;
}

static void rng_end(struct rng *r) {
    mbedtls_ctr_drbg_free(&r->drbg);
    mbedtls_entropy_free(&r->entropy);
}

/* --- Names and paths --- */

/*
 * A key's name is a file name, so it has to be one.
 *
 * Letters, digits, dash and underscore. Not a dot, which would let a name end
 * in something the listing code treats specially; not a slash, which would let
 * "../../System/Config/signing" name the private key.
 */
static bool usable_name(const char *name) {
    if (name == NULL || *name == '\0' || strlen(name) >= RECON_SIGN_NAME_MAX) {
        return false;
    }
    for (const char *c = name; *c != '\0'; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
            (*c >= '0' && *c <= '9') || *c == '-' || *c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool trusted_path(const char *name, char *out, size_t size) {
    if (!usable_name(name)) {
        set_error("'%s' is not a usable key name -- letters, digits, dash and "
            "underscore", name != NULL ? name : "");
        return false;
    }
    int n = snprintf(out, size, "%s/%s.pub", RECON_SIGN_TRUST_DIR, name);
    return n > 0 && (size_t)n < size;
}

/* --- Making a key --- */

bool recon_sign_make_key(const char *name) {
    char pub_path[RECON_PATH_MAX];
    if (!trusted_path(name, pub_path, sizeof(pub_path))) {
        return false;
    }

    /*
     * Refused rather than replaced.
     *
     * Overwriting a key would leave every package it had signed unverifiable,
     * and would do it without saying so. Somebody who means to replace one can
     * distrust it first, which is a sentence they have to type.
     */
    if (recon_fs_exists("/", pub_path)) {
        set_error("a key called '%s' is already trusted", name);
        return false;
    }
    if (recon_fs_exists("/", RECON_SIGN_OWN_KEY)) {
        set_error("this machine already has a signing key");
        return false;
    }

    struct rng rng;
    if (!rng_start(&rng, "make a key")) {
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    bool ok = false;
    unsigned char private_pem[2048];
    unsigned char public_pem[2048];
    memset(private_pem, 0, sizeof(private_pem));
    memset(public_pem, 0, sizeof(public_pem));

    if (mbedtls_pk_setup(&pk,
            mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) {
        set_error("the key type would not set up");
        goto done;
    }
    if (mbedtls_ecp_gen_key(CURVE, mbedtls_pk_ec(pk),
            mbedtls_ctr_drbg_random, &rng.drbg) != 0) {
        set_error("the key would not generate");
        goto done;
    }
    if (mbedtls_pk_write_key_pem(&pk, private_pem, sizeof(private_pem)) != 0 ||
            mbedtls_pk_write_pubkey_pem(&pk, public_pem,
                sizeof(public_pem)) != 0) {
        set_error("the key would not write out");
        goto done;
    }

    recon_fs_mkdir("/", RECON_SIGN_TRUST_DIR);

    /*
     * The private half first, and private.
     *
     * If the public half landed first and the private write then failed,
     * the machine would trust a key it cannot sign with -- which looks like
     * a working setup and is not.
     */
    if (!recon_fs_write_private("/", RECON_SIGN_OWN_KEY,
            (const char *)private_pem, strlen((const char *)private_pem))) {
        set_error("%s", recon_fs_last_error());
        goto done;
    }
    if (!recon_fs_write("/", pub_path, (const char *)public_pem,
            strlen((const char *)public_pem))) {
        set_error("%s", recon_fs_last_error());
        recon_fs_remove("/", RECON_SIGN_OWN_KEY);
        goto done;
    }

    ok = true;

done:
    mbedtls_pk_free(&pk);
    rng_end(&rng);
    recon_secure_erase(private_pem, sizeof(private_pem));
    return ok;
}

bool recon_sign_have_own_key(void) {
    return recon_fs_exists("/", RECON_SIGN_OWN_KEY);
}

/* --- Signing --- */

bool recon_sign_data(const void *bytes, size_t size, char *out,
        size_t out_size) {
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    if (bytes == NULL) {
        set_error("there is nothing to sign");
        return false;
    }

    size_t key_size = 0;
    char *key_pem = recon_fs_read("/", RECON_SIGN_OWN_KEY, &key_size);
    if (key_pem == NULL) {
        set_error("this machine has no signing key -- make one first");
        return false;
    }

    struct rng rng;
    if (!rng_start(&rng, "sign")) {
        recon_secure_erase(key_pem, key_size);
        free(key_pem);
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = false;

    unsigned char signature[SIG_BYTES_MAX];
    size_t signature_size = 0;
    uint8_t digest[RECON_SHA256_SIZE];

    /*
     * The PEM has to be handed over including its terminating NUL, which is
     * what mbedTLS uses to tell PEM from DER. recon_fs_read always terminates.
     */
    if (mbedtls_pk_parse_key(&pk, (const unsigned char *)key_pem,
            key_size + 1, NULL, 0) != 0) {
        set_error("this machine's signing key would not load");
        goto done;
    }

    recon_sha256(bytes, size, digest);

    /*
     * Deterministic, because of the build-time check at the top of this file:
     * with MBEDTLS_ECDSA_DETERMINISTIC the per-signature secret comes from the
     * key and this digest, and there is no random value that can repeat.
     */
    if (mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, digest, sizeof(digest),
            signature, &signature_size,
            mbedtls_ctr_drbg_random, &rng.drbg) != 0) {
        set_error("that could not be signed");
        goto done;
    }

    if (signature_size * 2 + 1 > out_size) {
        set_error("there is not room for the signature");
        goto done;
    }
    recon_to_hex(signature, signature_size, out);
    ok = true;

done:
    mbedtls_pk_free(&pk);
    rng_end(&rng);
    recon_secure_erase(key_pem, key_size);
    free(key_pem);
    return ok;
}

/* --- Verifying --- */

/* One trusted key against one signature. */
static bool verify_with(const char *pem, size_t pem_size,
        const uint8_t *digest, const unsigned char *signature,
        size_t signature_size) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    bool ok = false;
    if (mbedtls_pk_parse_public_key(&pk, (const unsigned char *)pem,
            pem_size + 1) == 0) {
        ok = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest,
            RECON_SHA256_SIZE, signature, signature_size) == 0;
    }
    mbedtls_pk_free(&pk);
    return ok;
}

bool recon_sign_verify(const void *bytes, size_t size, const char *signature,
        char *signer, size_t signer_size) {
    if (signer != NULL && signer_size > 0) {
        signer[0] = '\0';
    }
    if (bytes == NULL || signature == NULL) {
        set_error("there is nothing to check");
        return false;
    }

    size_t hex_length = strlen(signature);
    if (hex_length == 0 || hex_length % 2 != 0 ||
            hex_length / 2 > SIG_BYTES_MAX) {
        set_error("that is not a signature this wrote");
        return false;
    }

    unsigned char raw[SIG_BYTES_MAX];
    if (!recon_from_hex(signature, raw, hex_length / 2)) {
        set_error("that is not a signature this wrote");
        return false;
    }

    uint8_t digest[RECON_SHA256_SIZE];
    recon_sha256(bytes, size, digest);

    struct recon_dirent entries[TRUSTED_MAX];
    int count = recon_fs_list("/", RECON_SIGN_TRUST_DIR, entries, TRUSTED_MAX);
    if (count > TRUSTED_MAX) {
        count = TRUSTED_MAX;
    }
    if (count <= 0) {
        set_error("this machine trusts no keys, so nothing can be checked "
            "against one");
        return false;
    }

    /*
     * Every key, in whatever order the directory gives them.
     *
     * There is no hint in the signature about which key made it, on purpose:
     * a hint would be a field somebody could set to point at the wrong key,
     * and trying them all costs a few milliseconds against a store bounded at
     * thirty-two.
     */
    for (int i = 0; i < count; i++) {
        const char *dot = strrchr(entries[i].name, '.');
        if (dot == NULL || strcmp(dot, ".pub") != 0) {
            continue;
        }

        char path[RECON_PATH_MAX];
        if (!recon_fs_join(path, sizeof(path), RECON_SIGN_TRUST_DIR,
                entries[i].name)) {
            continue;
        }

        size_t pem_size = 0;
        char *pem = recon_fs_read("/", path, &pem_size);
        if (pem == NULL) {
            continue;
        }

        bool matched = verify_with(pem, pem_size, digest, raw,
            hex_length / 2);
        free(pem);

        if (matched) {
            if (signer != NULL && signer_size > 0) {
                size_t stem = (size_t)(dot - entries[i].name);
                if (stem >= signer_size) {
                    stem = signer_size - 1;
                }
                memcpy(signer, entries[i].name, stem);
                signer[stem] = '\0';
            }
            return true;
        }
    }

    set_error("no key this machine trusts signed that");
    return false;
}

/* --- The trust store --- */

bool recon_sign_trust(const char *path, const char *name) {
    char destination[RECON_PATH_MAX];
    if (!trusted_path(name, destination, sizeof(destination))) {
        return false;
    }
    if (recon_fs_exists("/", destination)) {
        set_error("a key called '%s' is already trusted", name);
        return false;
    }

    size_t size = 0;
    char *pem = recon_fs_read("/", path, &size);
    if (pem == NULL) {
        set_error("%s", recon_fs_last_error());
        return false;
    }

    /*
     * It has to parse as a public key, and it has to *not* parse as a private
     * one.
     *
     * The second check is the one worth having. A private key placed in the
     * trust directory would verify everything it signed and would also be a
     * private key sitting in a directory whose whole purpose is to be readable
     * -- so somebody who mistyped a file name would publish their own key and
     * the machine would report success.
     */
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool is_public = mbedtls_pk_parse_public_key(&pk,
        (const unsigned char *)pem, size + 1) == 0;
    mbedtls_pk_free(&pk);

    mbedtls_pk_init(&pk);
    bool is_private = mbedtls_pk_parse_key(&pk, (const unsigned char *)pem,
        size + 1, NULL, 0) == 0;
    mbedtls_pk_free(&pk);

    bool ok = false;
    if (is_private) {
        set_error("that is a private key. Trusting one would put it somewhere "
            "everything can read");
    } else if (!is_public) {
        set_error("that is not a public key this can read");
    } else {
        recon_fs_mkdir("/", RECON_SIGN_TRUST_DIR);
        ok = recon_fs_write("/", destination, pem, size);
        if (!ok) {
            set_error("%s", recon_fs_last_error());
        }
    }

    recon_secure_erase(pem, size);
    free(pem);
    return ok;
}

bool recon_sign_distrust(const char *name) {
    char path[RECON_PATH_MAX];
    if (!trusted_path(name, path, sizeof(path))) {
        return false;
    }
    if (!recon_fs_exists("/", path)) {
        set_error("no key called '%s' is trusted", name);
        return false;
    }
    /*
     * The key goes; what it signed stays installed. A package that was
     * trusted when it went on is code already on the disk, and removing the
     * key does not remove the code -- saying otherwise would be theatre.
     */
    return recon_fs_remove("/", path);
}

int recon_sign_trusted_count(void) {
    struct recon_dirent entries[TRUSTED_MAX];
    int count = recon_fs_list("/", RECON_SIGN_TRUST_DIR, entries, TRUSTED_MAX);
    if (count > TRUSTED_MAX) {
        count = TRUSTED_MAX;
    }

    int keys = 0;
    for (int i = 0; i < count; i++) {
        const char *dot = strrchr(entries[i].name, '.');
        if (dot != NULL && strcmp(dot, ".pub") == 0) {
            keys++;
        }
    }
    return keys;
}

bool recon_sign_trusted_at(int index, char *out, size_t size) {
    if (out == NULL || size == 0) {
        return false;
    }
    out[0] = '\0';

    struct recon_dirent entries[TRUSTED_MAX];
    int count = recon_fs_list("/", RECON_SIGN_TRUST_DIR, entries, TRUSTED_MAX);
    if (count > TRUSTED_MAX) {
        count = TRUSTED_MAX;
    }

    int at = 0;
    for (int i = 0; i < count; i++) {
        const char *dot = strrchr(entries[i].name, '.');
        if (dot == NULL || strcmp(dot, ".pub") != 0) {
            continue;
        }
        if (at == index) {
            size_t stem = (size_t)(dot - entries[i].name);
            if (stem >= size) {
                stem = size - 1;
            }
            memcpy(out, entries[i].name, stem);
            out[stem] = '\0';
            return true;
        }
        at++;
    }
    return false;
}
