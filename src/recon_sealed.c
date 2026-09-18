/*
 * A file only this account can read. See include/recon_sealed.h.
 *
 * --- The format, and why there is no version number in it ---
 *
 * On disk:
 *
 *     "ReconOS sealed 1\n"   the magic, seventeen bytes including the newline
 *     nonce                  12 bytes, fresh for every write
 *     tag                    16 bytes
 *     ciphertext             the rest
 *
 * The magic is the version. A later format gets a different magic and a reader
 * that knows both, rather than a version byte inside a format that claims to
 * describe itself -- which is a thing that can disagree with the bytes around
 * it. `src/recon_package.c` reached the same conclusion about receipts for the
 * same reason, and it is worth the two of them agreeing.
 *
 * It also makes the failure legible. A file that is not this reports that it
 * is not a sealed file, instead of failing to authenticate and sending
 * somebody looking for a tampering that never happened.
 *
 * --- What is authenticated besides the content ---
 *
 * The magic and the name, as associated data. The name matters: without it,
 * somebody who could write the folder could move `browser/cookies` on top of
 * some other sealed file and every byte would still verify. With it, a
 * ciphertext is bound to the one name it was written under and a move is a
 * decryption failure.
 *
 * --- Where they live ---
 *
 * `/Users/<account>/.sealed/<name>`, beside `.keyring`, and written with
 * `recon_fs_write_private` for the same reason that is: it is ciphertext, and
 * a stolen copy is not a stolen secret, but a file of somebody's encrypted
 * things readable by anything on the machine is still a file somebody can take
 * away and work on at leisure.
 *
 * The account is the one **the keyring is unlocked for**, not the one signed
 * in. They are the same in every ordinary case, and where they are not, the
 * key would not open what the path pointed at.
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
#include "recon_sealed.h"

#define MAGIC      "ReconOS sealed 1\n"
#define MAGIC_SIZE (sizeof(MAGIC) - 1)

#define KEY_SIZE   32
#define NONCE_SIZE 12
#define TAG_SIZE   16

/*
 * The purpose handed to `recon_keyring_derive`. Written once, here, because
 * two spellings of it are two different keys and the second one would present
 * as every sealed file having been tampered with.
 */
#define SEALED_PURPOSE "sealed-files"

static char g_error[192];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_sealed_last_error(void) {
    return g_error[0] != 0 ? g_error : "no error";
}

/*
 * Whether a name may be used, and the answer is refuse rather than repair.
 *
 * A name becomes a path, so a name that can climb out of the folder is a name
 * that can write anywhere the desktop can. Sanitizing it would mean a caller
 * asking for one thing and getting another silently; refusing means the
 * program that built a bad name finds out.
 *
 * `/` is allowed and makes a directory, which is what lets "browser/cookies"
 * sit next to "browser/history". A leading or trailing one is not, because
 * neither names a file.
 */
static bool name_is_usable(const char *name) {
    if (name == NULL || *name == 0) {
        set_error("a sealed file needs a name");
        return false;
    }

    size_t n = strlen(name);

    if (n >= RECON_SEALED_NAME_MAX) {
        set_error("'%s' is too long a name for a sealed file", name);
        return false;
    }
    if (name[0] == '/' || name[n - 1] == '/') {
        set_error("'%s' does not name a file", name);
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '/' || c == '-' || c == '_' ||
            c == '.';

        if (!ok) {
            set_error("'%s' has a character a sealed file's name may not", name);
            return false;
        }
        /* `..` cannot appear at all, rather than only as a whole component:
         * there is no legitimate sealed file with two dots in a row in its
         * name, and the narrower rule is the one that has to be got right. */
        if (c == '.' && i + 1 < n && name[i + 1] == '.') {
            set_error("'%s' may not climb out of the folder", name);
            return false;
        }
    }
    return true;
}

static bool sealed_path(const char *name, char *out, size_t size) {
    const char *user = recon_keyring_user();

    if (user == NULL || *user == 0) {
        set_error("nobody is signed in, so there is nowhere to keep it");
        return false;
    }

    int n = snprintf(out, size, "/Users/%s/.sealed/%s", user, name);

    if (n <= 0 || (size_t)n >= size) {
        set_error("'%s' makes too long a path", name);
        return false;
    }
    return true;
}

/*
 * The folder, and the folders inside it a name with a slash asks for.
 *
 * Made a component at a time, because recon_fs_mkdir makes one directory and a
 * name like "browser/cookies" wants ".sealed" and ".sealed/browser" both. An
 * existing directory is not an error here -- the second write of the day would
 * fail if it were.
 */
static bool make_folders_for(const char *path) {
    char work[RECON_PATH_MAX];

    if (snprintf(work, sizeof(work), "%s", path) < 0) {
        return false;
    }

    /* Every slash except the first, which is the root, and not the last
     * component, which is the file. */
    for (char *p = work + 1; *p != 0; p++) {
        if (*p != '/') {
            continue;
        }
        *p = 0;
        if (!recon_fs_exists("/", work)) {
            recon_fs_mkdir("/", work);
        }
        *p = '/';
    }
    return true;
}

/* The key for sealed files, or false when the keyring is locked. */
static bool sealed_key(uint8_t out[KEY_SIZE]) {
    if (!recon_keyring_derive(SEALED_PURPOSE, out, KEY_SIZE)) {
        set_error("%s", recon_keyring_last_error());
        return false;
    }
    return true;
}

bool recon_sealed_write(const char *name, const void *bytes, size_t size) {
    if (!name_is_usable(name)) {
        return false;
    }
    if (size > 0 && bytes == NULL) {
        set_error("asked to seal %zu bytes and given none", size);
        return false;
    }
    if (size > RECON_SEALED_MAX) {
        set_error("%zu bytes is more than a sealed file holds (%d)", size,
            RECON_SEALED_MAX);
        return false;
    }

    char path[RECON_PATH_MAX];

    if (!sealed_path(name, path, sizeof(path))) {
        return false;
    }

    uint8_t key[KEY_SIZE];

    if (!sealed_key(key)) {
        return false;
    }

    size_t total = MAGIC_SIZE + NONCE_SIZE + TAG_SIZE + size;
    uint8_t *file = malloc(total);

    if (file == NULL) {
        recon_secure_erase(key, sizeof(key));
        set_error("there was not enough memory to seal '%s'", name);
        return false;
    }

    memcpy(file, MAGIC, MAGIC_SIZE);

    uint8_t *nonce = file + MAGIC_SIZE;
    uint8_t *tag = nonce + NONCE_SIZE;
    uint8_t *cipher = tag + TAG_SIZE;

    /*
     * A fresh nonce every write, from the system's randomness and never
     * derived from anything. GCM under a repeated nonce with the same key is
     * not weakened, it is broken: two messages under one nonce leak their
     * difference and hand over the authentication key.
     */
    if (!recon_random_bytes(nonce, NONCE_SIZE)) {
        recon_secure_erase(key, sizeof(key));
        free(file);
        set_error("there was no randomness for a nonce");
        return false;
    }

    mbedtls_gcm_context gcm;

    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_SIZE * 8)
            != 0) {
        mbedtls_gcm_free(&gcm);
        recon_secure_erase(key, sizeof(key));
        free(file);
        set_error("the cipher would not take the key");
        return false;
    }

    /*
     * The magic and the name are authenticated but not encrypted. The name is
     * what binds this ciphertext to this path; see the header of this file.
     */
    uint8_t aad[MAGIC_SIZE + RECON_SEALED_NAME_MAX];
    size_t aad_size = MAGIC_SIZE + strlen(name);

    memcpy(aad, MAGIC, MAGIC_SIZE);
    memcpy(aad + MAGIC_SIZE, name, strlen(name));

    int rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, size,
        nonce, NONCE_SIZE, aad, aad_size, bytes, cipher, TAG_SIZE, tag);

    mbedtls_gcm_free(&gcm);
    recon_secure_erase(key, sizeof(key));

    if (rc != 0) {
        free(file);
        set_error("'%s' could not be sealed", name);
        return false;
    }

    make_folders_for(path);

    bool ok = recon_fs_write_private("/", path, (const char *)file, total);

    if (!ok) {
        set_error("%s", recon_fs_last_error());
    }
    free(file);
    return ok;
}

void *recon_sealed_read(const char *name, size_t *size_out) {
    if (size_out != NULL) {
        *size_out = 0;
    }
    if (!name_is_usable(name)) {
        return NULL;
    }

    char path[RECON_PATH_MAX];

    if (!sealed_path(name, path, sizeof(path))) {
        return NULL;
    }

    size_t file_size = 0;
    char *file = recon_fs_read("/", path, &file_size);

    if (file == NULL) {
        set_error("there is nothing sealed under '%s'", name);
        return NULL;
    }

    /*
     * The shape, before the cipher is asked anything.
     *
     * A file shorter than its own header is not a tampered sealed file, it is
     * not one -- and handing a negative length to the cipher by subtracting
     * past zero is how that becomes something worse than a wrong answer.
     */
    if (file_size < MAGIC_SIZE + NONCE_SIZE + TAG_SIZE ||
            memcmp(file, MAGIC, MAGIC_SIZE) != 0) {
        free(file);
        set_error("'%s' is not a sealed file", name);
        return NULL;
    }

    size_t size = file_size - MAGIC_SIZE - NONCE_SIZE - TAG_SIZE;

    if (size > RECON_SEALED_MAX) {
        free(file);
        set_error("'%s' claims to hold more than a sealed file may", name);
        return NULL;
    }

    uint8_t key[KEY_SIZE];

    if (!sealed_key(key)) {
        free(file);
        return NULL;
    }

    /* One past the end, so text read out of a sealed file can be used as a
     * string. The terminator is written here and is not part of what was
     * sealed. */
    uint8_t *plain = malloc(size + 1);

    if (plain == NULL) {
        recon_secure_erase(key, sizeof(key));
        free(file);
        set_error("there was not enough memory to open '%s'", name);
        return NULL;
    }

    const uint8_t *nonce = (const uint8_t *)file + MAGIC_SIZE;
    const uint8_t *tag = nonce + NONCE_SIZE;
    const uint8_t *cipher = tag + TAG_SIZE;

    uint8_t aad[MAGIC_SIZE + RECON_SEALED_NAME_MAX];
    size_t aad_size = MAGIC_SIZE + strlen(name);

    memcpy(aad, MAGIC, MAGIC_SIZE);
    memcpy(aad + MAGIC_SIZE, name, strlen(name));

    mbedtls_gcm_context gcm;

    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, KEY_SIZE * 8)
            != 0) {
        mbedtls_gcm_free(&gcm);
        recon_secure_erase(key, sizeof(key));
        free(plain);
        free(file);
        set_error("the cipher would not take the key");
        return NULL;
    }

    int rc = mbedtls_gcm_auth_decrypt(&gcm, size, nonce, NONCE_SIZE,
        aad, aad_size, tag, TAG_SIZE, cipher, plain);

    mbedtls_gcm_free(&gcm);
    recon_secure_erase(key, sizeof(key));
    free(file);

    if (rc != 0) {
        /*
         * Erased rather than freed straight away: mbedTLS writes plaintext as
         * it goes and only then finds the tag wrong, so this buffer holds
         * whatever the wrong key produced. Handing that to the allocator to
         * give to somebody else is how a failed read becomes a leak.
         *
         * One message for every reason it can fail. Saying which -- edited,
         * wrong account, password changed -- tells somebody holding the disk
         * which of their guesses was closest, and every caller does the same
         * thing regardless.
         */
        recon_secure_erase(plain, size);
        free(plain);
        set_error("'%s' could not be opened: it is not what was sealed, or it "
            "belongs to a different sign-in", name);
        return NULL;
    }

    plain[size] = 0;
    if (size_out != NULL) {
        *size_out = size;
    }
    return plain;
}

bool recon_sealed_exists(const char *name) {
    char path[RECON_PATH_MAX];

    if (!name_is_usable(name) || !sealed_path(name, path, sizeof(path))) {
        return false;
    }
    return recon_fs_exists("/", path);
}

bool recon_sealed_forget(const char *name) {
    char path[RECON_PATH_MAX];

    if (!name_is_usable(name) || !sealed_path(name, path, sizeof(path))) {
        return false;
    }
    if (!recon_fs_exists("/", path)) {
        return false;
    }
    if (!recon_fs_remove("/", path)) {
        set_error("%s", recon_fs_last_error());
        return false;
    }
    return true;
}
