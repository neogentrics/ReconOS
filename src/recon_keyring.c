/*
 * Secrets that exist only while somebody is signed in.
 * See include/recon_keyring.h for the design and its limits.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/gcm.h>

#include "recon_crypt.h"
#include "recon_fs.h"
#include "recon_keyring.h"
#include "recon_users.h"

#define KEY_SIZE 32     /* AES-256 */
#define NONCE_SIZE 12   /* what GCM is specified for; any other size is a
                         * different construction wearing the same name */
#define TAG_SIZE 16
#define ENTRIES_MAX 64

/*
 * The tag that separates this derivation from the login one.
 *
 * The accounts file stores PBKDF2(password, login_salt) to check a password
 * against. Deriving the keyring key from the same salt would make that stored
 * value the key: reading the accounts file would be reading every secret. So
 * the keyring's salt is a hash of the login salt with this tag in front of it,
 * which makes the two derivations independent -- PBKDF2 under two different
 * salts gives two unrelated results, and knowing one says nothing about the
 * other.
 *
 * The version in the tag is not decoration. If the derivation ever changes,
 * the tag changes with it, and old entries stop decrypting rather than
 * decrypting to something else.
 */
#define KEYRING_SALT_TAG "reconos-keyring-v1"

struct entry {
    char name[RECON_KEYRING_NAME_MAX];
    uint8_t nonce[NONCE_SIZE];
    uint8_t tag[TAG_SIZE];
    uint8_t cipher[RECON_KEYRING_SECRET_MAX];
    size_t cipher_size;
    bool used;
};

/*
 * The key, and nothing else about it.
 *
 * Static because there is one signed-in person at a time, which is the same
 * assumption recon_users makes. `g_unlocked` rather than testing the key for
 * zeroes: a key of all zeroes is a valid key and a test that treats it as
 * "absent" is a test that will one day be wrong about a real one.
 */
static uint8_t g_key[KEY_SIZE];
static bool g_unlocked;
static char g_user[64];

static struct entry g_entries[ENTRIES_MAX];
static char g_error[192];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_keyring_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* --- Where it lives --- */

static bool store_path(const char *user, char *out, size_t size) {
    if (user == NULL || *user == '\0') {
        return false;
    }
    int n = snprintf(out, size, "/Users/%s/.keyring", user);
    return n > 0 && (size_t)n < size;
}

/* --- On disk --- */

/*
 * One entry per line: name, nonce, tag, ciphertext, tab-separated, hex.
 *
 * Hex rather than base64 because recon_to_hex and recon_from_hex already exist
 * and round-trip exactly, and these are short. A format that needed a new
 * encoder would be a new encoder to get wrong.
 */
static void load(void) {
    memset(g_entries, 0, sizeof(g_entries));

    char path[RECON_PATH_MAX];
    if (!store_path(g_user, path, sizeof(path))) {
        return;
    }

    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        /* No keyring yet is the ordinary case, not an error. */
        return;
    }

    int at = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(text, "\n", &saveptr);
            line != NULL && at < ENTRIES_MAX;
            line = strtok_r(NULL, "\n", &saveptr)) {

        if (*line == '\0' || *line == '#') {
            continue;
        }

        char *fields[4] = { line, NULL, NULL, NULL };
        int found = 1;
        for (char *p = line; *p != '\0' && found < 4; p++) {
            if (*p == '\t') {
                *p = '\0';
                fields[found++] = p + 1;
            }
        }
        if (found != 4) {
            continue;
        }

        struct entry *e = &g_entries[at];
        size_t hex_len = strlen(fields[3]);
        if (hex_len % 2 != 0 || hex_len / 2 > sizeof(e->cipher)) {
            continue;
        }

        if (!recon_from_hex(fields[1], e->nonce, NONCE_SIZE) ||
                !recon_from_hex(fields[2], e->tag, TAG_SIZE) ||
                !recon_from_hex(fields[3], e->cipher, hex_len / 2)) {
            continue;
        }

        snprintf(e->name, sizeof(e->name), "%s", fields[0]);
        e->cipher_size = hex_len / 2;
        e->used = true;
        at++;
    }

    free(text);
}

static bool save(void) {
    char path[RECON_PATH_MAX];
    if (!store_path(g_user, path, sizeof(path))) {
        set_error("there is nowhere to keep it");
        return false;
    }

    /* Sized for the worst case rather than grown, because the worst case is
     * small and known: sixty-four entries of a name and three hex fields. */
    static char text[ENTRIES_MAX *
        (RECON_KEYRING_NAME_MAX + (NONCE_SIZE + TAG_SIZE +
            RECON_KEYRING_SECRET_MAX) * 2 + 8) + 128];
    size_t used = 0;

    int n = snprintf(text, sizeof(text),
        "# ReconOS keyring. Encrypted with a key derived from the account\n"
        "# password at sign-in and never written down. Editing this by hand\n"
        "# can only lose a secret; there is no way to read one from here.\n");
    if (n < 0) {
        return false;
    }
    used = (size_t)n;

    for (int i = 0; i < ENTRIES_MAX; i++) {
        if (!g_entries[i].used) {
            continue;
        }
        struct entry *e = &g_entries[i];

        char nonce[NONCE_SIZE * 2 + 1];
        char tag[TAG_SIZE * 2 + 1];
        char cipher[RECON_KEYRING_SECRET_MAX * 2 + 1];
        recon_to_hex(e->nonce, NONCE_SIZE, nonce);
        recon_to_hex(e->tag, TAG_SIZE, tag);
        recon_to_hex(e->cipher, e->cipher_size, cipher);

        n = snprintf(text + used, sizeof(text) - used, "%s\t%s\t%s\t%s\n",
            e->name, nonce, tag, cipher);
        if (n < 0 || (size_t)n >= sizeof(text) - used) {
            set_error("the keyring is too full to write");
            return false;
        }
        used += (size_t)n;
    }

    /*
     * Written private from the moment it exists.
     *
     * recon_fs_write_private, not recon_fs_write: this is ciphertext and a
     * stolen copy is not a stolen secret, but a file of somebody's encrypted
     * passwords readable by anything on the machine is still a file somebody
     * can take away and work on at leisure.
     */
    bool ok = recon_fs_write_private("/", path, text, used);
    if (!ok) {
        set_error("%s", recon_fs_last_error());
    }
    recon_secure_erase(text, sizeof(text));
    return ok;
}

/* --- Locking and unlocking --- */

bool recon_keyring_unlock(const char *user, const char *password) {
    recon_keyring_lock();

    if (user == NULL || *user == '\0' || password == NULL) {
        set_error("there is no account to unlock");
        return false;
    }

    uint8_t login_salt[RECON_USERS_SALT_SIZE];
    if (!recon_users_salt(user, login_salt)) {
        set_error("'%s' has no account here", user);
        return false;
    }

    /*
     * The keyring's own salt: the login salt hashed with a tag in front of it.
     *
     * See the note on KEYRING_SALT_TAG. The short version is that the accounts
     * file already holds PBKDF2 of this password under the login salt, and
     * using the same salt here would make that stored value the key.
     */
    struct recon_sha256 ctx;
    uint8_t salt[RECON_SHA256_SIZE];
    recon_sha256_init(&ctx);
    recon_sha256_update(&ctx, KEYRING_SALT_TAG, strlen(KEYRING_SALT_TAG));
    recon_sha256_update(&ctx, login_salt, sizeof(login_salt));
    recon_sha256_final(&ctx, salt);

    recon_pbkdf2_sha256(password, salt, sizeof(salt), RECON_USERS_ITERATIONS,
        g_key, KEY_SIZE);
    recon_secure_erase(salt, sizeof(salt));
    recon_secure_erase(login_salt, sizeof(login_salt));

    snprintf(g_user, sizeof(g_user), "%s", user);
    g_unlocked = true;
    load();
    return true;
}

void recon_keyring_lock(void) {
    recon_secure_erase(g_key, sizeof(g_key));
    /* The ciphertext goes too. It is safe to keep and there is no reason to:
     * a locked keyring that still held its entries would be a thing to walk
     * over looking for a bug. */
    recon_secure_erase(g_entries, sizeof(g_entries));
    recon_secure_erase(g_user, sizeof(g_user));
    g_unlocked = false;
}

bool recon_keyring_unlocked(void) {
    return g_unlocked;
}

/* --- Secrets --- */

static struct entry *find(const char *name) {
    for (int i = 0; i < ENTRIES_MAX; i++) {
        if (g_entries[i].used && strcmp(g_entries[i].name, name) == 0) {
            return &g_entries[i];
        }
    }
    return NULL;
}

bool recon_keyring_put(const char *name, const char *secret) {
    if (!g_unlocked) {
        set_error("nobody is signed in, so there is nowhere to keep it");
        return false;
    }
    if (name == NULL || *name == '\0' || secret == NULL) {
        set_error("a secret needs a name");
        return false;
    }
    if (strlen(name) >= RECON_KEYRING_NAME_MAX ||
            strchr(name, '\t') != NULL || strchr(name, '\n') != NULL) {
        set_error("that is not a usable name");
        return false;
    }

    size_t length = strlen(secret);
    if (length >= RECON_KEYRING_SECRET_MAX) {
        set_error("that is longer than this keeps");
        return false;
    }

    struct entry *e = find(name);
    if (e == NULL) {
        for (int i = 0; i < ENTRIES_MAX && e == NULL; i++) {
            if (!g_entries[i].used) {
                e = &g_entries[i];
            }
        }
    }
    if (e == NULL) {
        set_error("the keyring is full");
        return false;
    }

    /*
     * A fresh nonce, from the system's source, every single time.
     *
     * Not a counter, not derived from the name, not reused when replacing an
     * entry. GCM under a repeated nonce is broken rather than weakened: two
     * messages under one nonce leak their difference and give away the
     * authentication key. A failure to get randomness is a refusal to write.
     */
    uint8_t nonce[NONCE_SIZE];
    if (!recon_random_bytes(nonce, sizeof(nonce))) {
        set_error("there was no randomness to encrypt with");
        return false;
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, g_key,
            KEY_SIZE * 8) != 0) {
        mbedtls_gcm_free(&gcm);
        set_error("the key would not load");
        return false;
    }

    uint8_t cipher[RECON_KEYRING_SECRET_MAX];
    uint8_t tag[TAG_SIZE];

    /*
     * The name is authenticated, not encrypted.
     *
     * It is in the file in the clear either way -- an entry has to be findable
     * by name -- and passing it as associated data binds the ciphertext to it.
     * Without that, somebody able to write the file could move the mail
     * password onto a name something else reads, and every byte would still
     * verify.
     */
    int rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, length,
        nonce, sizeof(nonce),
        (const unsigned char *)name, strlen(name),
        (const unsigned char *)secret, cipher, sizeof(tag), tag);
    mbedtls_gcm_free(&gcm);

    if (rc != 0) {
        set_error("that could not be encrypted");
        return false;
    }

    snprintf(e->name, sizeof(e->name), "%s", name);
    memcpy(e->nonce, nonce, sizeof(nonce));
    memcpy(e->tag, tag, sizeof(tag));
    memcpy(e->cipher, cipher, length);
    e->cipher_size = length;
    e->used = true;

    recon_secure_erase(cipher, sizeof(cipher));
    return save();
}

bool recon_keyring_get(const char *name, char *out, size_t size) {
    /* Erased first, so a caller that ignores the answer is not left holding
     * whatever was in the buffer before. */
    if (out != NULL && size > 0) {
        recon_secure_erase(out, size);
    }

    if (!g_unlocked) {
        set_error("nobody is signed in");
        return false;
    }
    if (name == NULL || out == NULL || size == 0) {
        return false;
    }

    struct entry *e = find(name);
    if (e == NULL) {
        set_error("there is nothing kept under that name");
        return false;
    }
    if (e->cipher_size >= size) {
        set_error("there is not room for that secret");
        return false;
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, g_key,
            KEY_SIZE * 8) != 0) {
        mbedtls_gcm_free(&gcm);
        set_error("the key would not load");
        return false;
    }

    /*
     * auth_decrypt, which checks the tag before handing anything back.
     *
     * The distinction matters: a version that decrypted first and checked
     * afterwards would hand the caller plaintext derived from a wrong key or
     * an edited file, and the caller would have used it before the check
     * failed. mbedTLS does it in the right order and returns nothing when the
     * tag is wrong; this returns false, which is the same answer for a wrong
     * password, an edited file and a changed account password -- and it is the
     * same answer because they are indistinguishable and pretending otherwise
     * would be a guess.
     */
    int rc = mbedtls_gcm_auth_decrypt(&gcm, e->cipher_size,
        e->nonce, NONCE_SIZE,
        (const unsigned char *)name, strlen(name),
        e->tag, TAG_SIZE,
        e->cipher, (unsigned char *)out);
    mbedtls_gcm_free(&gcm);

    if (rc != 0) {
        recon_secure_erase(out, size);
        set_error("that secret could not be read -- the account password may "
            "have changed since it was kept");
        return false;
    }

    out[e->cipher_size] = '\0';
    return true;
}

bool recon_keyring_forget(const char *name) {
    if (!g_unlocked || name == NULL) {
        return false;
    }
    struct entry *e = find(name);
    if (e == NULL) {
        return false;
    }
    recon_secure_erase(e, sizeof(*e));
    return save();
}

int recon_keyring_count(void) {
    int count = 0;
    for (int i = 0; i < ENTRIES_MAX; i++) {
        if (g_entries[i].used) {
            count++;
        }
    }
    return count;
}

bool recon_keyring_name_at(int index, char *out, size_t size) {
    int at = 0;
    for (int i = 0; i < ENTRIES_MAX; i++) {
        if (!g_entries[i].used) {
            continue;
        }
        if (at == index) {
            snprintf(out, size, "%s", g_entries[i].name);
            return true;
        }
        at++;
    }
    return false;
}
