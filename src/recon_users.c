/*
 * User accounts. See include/recon_users.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

#include "recon_crypt.h"
#include "recon_fs.h"
#include "recon_users.h"

#define SALT_SIZE 16
#define HASH_SIZE RECON_SHA256_SIZE

struct account {
    struct recon_user info;
    uint32_t iterations;
    uint8_t salt[SALT_SIZE];
    uint8_t hash[HASH_SIZE];
    bool used;
};

static struct account g_accounts[RECON_USERS_MAX];
static int g_count;
static char g_current[RECON_USERS_NAME_MAX];
static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_users_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* --- Names --- */

/*
 * An account gets a folder, so a name that cannot be a folder cannot be a
 * name. No slashes, no dots that would mean "here" or "up", nothing that a
 * listing would render as invisible.
 */
static bool name_is_valid(const char *name) {
    if (name == NULL) {
        return false;
    }
    size_t length = strlen(name);
    if (length == 0 || length >= RECON_USERS_NAME_MAX) {
        return false;
    }
    if (name[0] == '.' || name[0] == ' ' || name[length - 1] == ' ') {
        return false;
    }

    for (const char *c = name; *c != '\0'; c++) {
        if (*c == '/' || *c == ':' || *c == '\\' || *c == '\n') {
            return false;
        }
        if ((unsigned char)*c < 0x20) {
            return false;
        }
    }
    return true;
}

static struct account *find(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < RECON_USERS_MAX; i++) {
        if (g_accounts[i].used &&
                strcasecmp(g_accounts[i].info.name, name) == 0) {
            return &g_accounts[i];
        }
    }
    return NULL;
}

/* --- On disk --- */

/*
 * One line per account:
 *
 *   name:role:iterations:salt:hash
 *
 * Text, so it can be read and repaired. The hash is not a secret in the sense
 * the password is -- but it is what an attacker would work from, which is why
 * the file lives in /System where a limited account cannot read it back out
 * through the file explorer.
 *
 * Writing it can be refused: a limited account may not touch this file at all.
 * So every change below is made in memory, written, and *undone* if the write
 * does not happen. Without that, a refused change still took effect as far as
 * the running system was concerned -- a limited account was told it could not
 * create an administrator, and an administrator existed anyway until the next
 * restart. A refusal that only half refuses is worse than none, because nobody
 * thinks to check.
 */
static bool save(void) {
    char text[RECON_USERS_MAX * 256 + 256];
    size_t used = (size_t)snprintf(text, sizeof(text),
        "# ReconOS accounts.\n"
        "# name:role:iterations:salt:hash\n"
        "# Passwords are PBKDF2-HMAC-SHA256. Editing this by hand can only\n"
        "# lock you out; there is no way to write a password in from here.\n\n");

    for (int i = 0; i < RECON_USERS_MAX && used < sizeof(text); i++) {
        if (!g_accounts[i].used) {
            continue;
        }

        char salt[SALT_SIZE * 2 + 1] = "";
        char hash[HASH_SIZE * 2 + 1] = "";
        if (g_accounts[i].info.has_password) {
            recon_to_hex(g_accounts[i].salt, SALT_SIZE, salt);
            recon_to_hex(g_accounts[i].hash, HASH_SIZE, hash);
        }

        int written = snprintf(text + used, sizeof(text) - used,
            "%s:%s:%u:%s:%s\n",
            g_accounts[i].info.name,
            g_accounts[i].info.role == RECON_ROLE_ADMINISTRATOR
                ? "administrator" : "limited",
            g_accounts[i].info.has_password ? g_accounts[i].iterations : 0,
            salt, hash);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }

    if (!recon_fs_write("/", RECON_USERS_FILE, text, used)) {
        set_error("%s", recon_fs_last_error());
        return false;
    }
    return true;
}

bool recon_users_init(void) {
    memset(g_accounts, 0, sizeof(g_accounts));
    g_count = 0;
    g_current[0] = '\0';

    size_t size = 0;
    char *text = recon_fs_read("/", RECON_USERS_FILE, &size);
    if (text == NULL) {
        /* No accounts yet. A first run, not a failure. */
        return true;
    }

    char *saveptr = NULL;
    for (char *line = strtok_r(text, "\n", &saveptr);
            line != NULL;
            line = strtok_r(NULL, "\n", &saveptr)) {

        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '\0' || *line == '#') {
            continue;
        }

        /* Split on colons, in place. */
        char *fields[5] = { line, NULL, NULL, NULL, NULL };
        int found = 1;
        for (char *c = line; *c != '\0' && found < 5; c++) {
            if (*c == ':') {
                *c = '\0';
                fields[found++] = c + 1;
            }
        }
        if (found < 3 || !name_is_valid(fields[0]) || find(fields[0]) != NULL) {
            continue; /* Unusable line, skipped rather than guessed at. */
        }

        struct account *slot = NULL;
        for (int i = 0; i < RECON_USERS_MAX && slot == NULL; i++) {
            if (!g_accounts[i].used) {
                slot = &g_accounts[i];
            }
        }
        if (slot == NULL) {
            break;
        }

        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        snprintf(slot->info.name, sizeof(slot->info.name), "%s", fields[0]);
        slot->info.role = (strcasecmp(fields[1], "administrator") == 0)
            ? RECON_ROLE_ADMINISTRATOR : RECON_ROLE_LIMITED;
        slot->iterations = (uint32_t)strtoul(fields[2], NULL, 10);

        /*
         * A password needs all of its parts. A line with an iteration count
         * but no hash is treated as having no password rather than as having
         * one that can never be matched -- the second locks somebody out of
         * their own machine over a damaged file.
         */
        if (slot->iterations > 0 && fields[3] != NULL && fields[4] != NULL &&
                recon_from_hex(fields[3], slot->salt, SALT_SIZE) &&
                recon_from_hex(fields[4], slot->hash, HASH_SIZE)) {
            slot->info.has_password = true;
        } else {
            slot->info.has_password = false;
            slot->iterations = 0;
        }

        g_count++;
    }

    free(text);
    return true;
}

void recon_users_finish(void) {
    /* The hashes go out of memory with everything else. */
    memset(g_accounts, 0, sizeof(g_accounts));
    g_count = 0;
    g_current[0] = '\0';
}

/* --- Who exists --- */

int recon_users_count(void) {
    return g_count;
}

bool recon_users_at(int index, struct recon_user *out) {
    int seen = 0;
    for (int i = 0; i < RECON_USERS_MAX; i++) {
        if (!g_accounts[i].used) {
            continue;
        }
        if (seen == index) {
            if (out != NULL) {
                *out = g_accounts[i].info;
            }
            return true;
        }
        seen++;
    }
    return false;
}

bool recon_users_find(const char *name, struct recon_user *out) {
    struct account *account = find(name);
    if (account == NULL) {
        return false;
    }
    if (out != NULL) {
        *out = account->info;
    }
    return true;
}

int recon_users_admin_count(void) {
    int count = 0;
    for (int i = 0; i < RECON_USERS_MAX; i++) {
        if (g_accounts[i].used &&
                g_accounts[i].info.role == RECON_ROLE_ADMINISTRATOR) {
            count++;
        }
    }
    return count;
}

/* --- Passwords --- */

/* Work out the hash for a password against a given salt. */
static void derive(const char *password, const uint8_t *salt,
        uint32_t iterations, uint8_t out[HASH_SIZE]) {
    recon_pbkdf2_sha256(password, salt, SALT_SIZE, iterations, out, HASH_SIZE);
}

static bool set_password(struct account *account, const char *password) {
    if (password == NULL || *password == '\0') {
        account->info.has_password = false;
        account->iterations = 0;
        memset(account->salt, 0, sizeof(account->salt));
        memset(account->hash, 0, sizeof(account->hash));
        return true;
    }

    /* A fresh salt every time, so two accounts with the same password do not
     * have the same hash, and so changing a password does not reuse the old
     * one's work. */
    if (!recon_random_bytes(account->salt, sizeof(account->salt))) {
        set_error("no source of randomness, so no password can be set safely");
        return false;
    }

    account->iterations = RECON_USERS_ITERATIONS;
    derive(password, account->salt, account->iterations, account->hash);
    account->info.has_password = true;
    return true;
}

/* --- Changing them --- */

bool recon_users_create(const char *name, const char *password,
        enum recon_user_role role) {
    if (!name_is_valid(name)) {
        set_error("'%s' is not a usable account name",
            name != NULL ? name : "");
        return false;
    }
    if (find(name) != NULL) {
        set_error("there is already an account called '%s'", name);
        return false;
    }
    if (g_count >= RECON_USERS_MAX) {
        set_error("there is no room for another account");
        return false;
    }

    struct account *slot = NULL;
    for (int i = 0; i < RECON_USERS_MAX && slot == NULL; i++) {
        if (!g_accounts[i].used) {
            slot = &g_accounts[i];
        }
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    snprintf(slot->info.name, sizeof(slot->info.name), "%s", name);
    slot->info.role = role;

    if (!set_password(slot, password)) {
        memset(slot, 0, sizeof(*slot));
        return false;
    }

    /* The folders come with the account: an account whose Documents folder
     * appears only later is one that half exists. */
    if (!recon_fs_create_user(name)) {
        memset(slot, 0, sizeof(*slot));
        set_error("%s", recon_fs_last_error());
        return false;
    }

    g_count++;
    if (!save()) {
        /* The write was refused, so this account does not exist. Its folder
         * is left behind, which is untidy but harmless -- and far better than
         * an account that exists until the next restart and then does not. */
        memset(slot, 0, sizeof(*slot));
        g_count--;
        return false;
    }
    return true;
}

bool recon_users_remove(const char *name, bool delete_files) {
    struct account *account = find(name);
    if (account == NULL) {
        set_error("there is no account called '%s'", name != NULL ? name : "");
        return false;
    }

    if (g_current[0] != '\0' && strcasecmp(g_current, name) == 0) {
        set_error("'%s' is signed in", name);
        return false;
    }

    /* A system nobody can administer is a system nobody can repair. */
    if (account->info.role == RECON_ROLE_ADMINISTRATOR &&
            recon_users_admin_count() <= 1) {
        set_error("'%s' is the only administrator", name);
        return false;
    }

    struct account removed = *account;
    memset(account, 0, sizeof(*account));
    g_count--;

    if (!save()) {
        *account = removed;
        g_count++;
        return false;
    }

    if (delete_files) {
        /*
         * Asked for, so done -- and reported through the error if it fails,
         * without putting the account back. The login is what was removed;
         * whether the folder went with it is a second fact, and an
         * administrator who is told "removed, but the files are still there"
         * knows exactly where they stand.
         */
        char home[RECON_PATH_MAX];
        snprintf(home, sizeof(home), "%s/%s", RECON_DIR_USERS, removed.info.name);

        if (recon_fs_exists("/", home) && !recon_fs_remove_tree("/", home)) {
            set_error("'%s' is gone, but its files could not be removed: %s",
                removed.info.name, recon_fs_last_error());
            return false;
        }
    }

    return true;
}

bool recon_users_set_role(const char *name, enum recon_user_role role) {
    struct account *account = find(name);
    if (account == NULL) {
        set_error("there is no account called '%s'", name != NULL ? name : "");
        return false;
    }

    if (account->info.role == RECON_ROLE_ADMINISTRATOR &&
            role != RECON_ROLE_ADMINISTRATOR &&
            recon_users_admin_count() <= 1) {
        set_error("'%s' is the only administrator", name);
        return false;
    }

    enum recon_user_role previous = account->info.role;
    account->info.role = role;

    if (!save()) {
        account->info.role = previous;
        return false;
    }
    return true;
}

bool recon_users_set_password(const char *name, const char *password) {
    struct account *account = find(name);
    if (account == NULL) {
        set_error("there is no account called '%s'", name != NULL ? name : "");
        return false;
    }
    /* Kept whole rather than field by field: the salt, the hash, the count
     * and the flag are one thing, and restoring three of four would leave an
     * account whose password can never be matched. */
    struct account previous = *account;

    if (!set_password(account, password)) {
        *account = previous;
        return false;
    }
    if (!save()) {
        *account = previous;
        return false;
    }
    return true;
}

/* --- Signing in --- */

bool recon_users_check(const char *name, const char *password) {
    struct account *account = find(name);

    /*
     * An account that does not exist is checked against a made-up salt anyway,
     * so answering takes the same time either way. Returning early would let
     * anyone learn which accounts are real by timing the reply.
     */
    static const uint8_t DECOY_SALT[SALT_SIZE] = {
        0x9d, 0x2a, 0x41, 0x77, 0xc3, 0x0e, 0x5b, 0xf1,
        0x68, 0x34, 0xab, 0x12, 0x7e, 0xd0, 0x95, 0x63,
    };

    const uint8_t *salt = account != NULL ? account->salt : DECOY_SALT;
    uint32_t iterations = (account != NULL && account->iterations > 0)
        ? account->iterations : RECON_USERS_ITERATIONS;

    uint8_t attempt[HASH_SIZE];
    derive(password != NULL ? password : "", salt, iterations, attempt);

    if (account == NULL) {
        return false;
    }
    if (!account->info.has_password) {
        /* No password means anyone at the keyboard can sign in, which is a
         * choice the account made. An empty attempt is the right one. */
        return password == NULL || *password == '\0';
    }
    return recon_equal_constant_time(attempt, account->hash, HASH_SIZE);
}

bool recon_users_login(const char *name, const char *password) {
    if (!recon_users_check(name, password)) {
        set_error("that name and password do not match an account");
        return false;
    }

    struct account *account = find(name);
    snprintf(g_current, sizeof(g_current), "%s", account->info.name);

    /* The filesystem follows: from here on, "the user's folder" means this
     * one, and what /System will allow depends on the role. */
    recon_fs_set_user(account->info.name,
        account->info.role == RECON_ROLE_ADMINISTRATOR);
    return true;
}

void recon_users_logout(void) {
    g_current[0] = '\0';
    recon_fs_set_user(NULL, false);
}

const char *recon_users_current(void) {
    return g_current[0] != '\0' ? g_current : NULL;
}

bool recon_users_current_is_admin(void) {
    struct account *account = find(g_current);
    return account != NULL &&
        account->info.role == RECON_ROLE_ADMINISTRATOR;
}

bool recon_users_may_administer(void) {
    /* Nobody signed in yet means setup, which has to be able to do things. */
    return g_current[0] == '\0' || recon_users_current_is_admin();
}
