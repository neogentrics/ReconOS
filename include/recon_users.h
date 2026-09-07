/*
 * User accounts.
 *
 * Who exists, who is signed in, and what they are allowed to do.
 *
 * Being straight about what the roles mean today: ReconOS runs as a single
 * process under a single host user, so "administrator" and "limited" are
 * enforced *by ReconOS, inside ReconOS*. The filesystem refuses a limited
 * account writes to /System and into other people's folders, and the settings
 * that belong to the machine cannot be changed. That is real, and it is what
 * stops a limited account breaking the system by accident or by trying.
 *
 * It is not isolation. A native program running on the host underneath is not
 * subject to any of it, and will not be until ReconOS has a kernel of its own.
 * Anyone reading this should size their trust accordingly, and nobody should
 * put a password here that they use anywhere else.
 *
 * Passwords are stored as PBKDF2-HMAC-SHA256 with a random salt per account.
 * That is a published algorithm rather than an invention -- see recon_crypt.h
 * for why.
 */

#ifndef RECON_USERS_H
#define RECON_USERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where the account list lives. Inside /System, so a limited account cannot
 * rewrite it. */
#define RECON_USERS_FILE "/System/Config/users"

#define RECON_USERS_NAME_MAX 32
#define RECON_USERS_MAX 32

/*
 * How much a password guess costs. Raised over time as machines get faster;
 * the number used is stored with each password, so raising it does not
 * invalidate accounts that already exist.
 */
#define RECON_USERS_ITERATIONS 120000

/* How many bytes of salt an account carries. */
#define RECON_USERS_SALT_SIZE 16

/*
 * An account's salt.
 *
 * For deriving a *second* key from the same password without that key being
 * the one already stored -- which is what the keyring does. It hashes this
 * with a tag of its own first, so the two derivations are independent: PBKDF2
 * under two different salts gives two unrelated results, and the value in the
 * accounts file says nothing about the other one.
 *
 * Handing out the salt is safe and is the point of a salt: it is not secret,
 * it exists so that two people with the same password do not get the same
 * hash, and it has been sitting in the accounts file in plain hex since the
 * file existed.
 *
 * False when there is no such account, or it has no password -- in which case
 * there is nothing to derive from and the caller has to say so rather than
 * deriving from an empty string, which would give every passwordless account
 * on every machine the same key.
 */
bool recon_users_salt(const char *name, uint8_t out[RECON_USERS_SALT_SIZE]);

enum recon_user_role {
    /* Can use the system and their own files, and nothing else. */
    RECON_ROLE_LIMITED,
    /* Can manage accounts, system settings and /System. */
    RECON_ROLE_ADMINISTRATOR,
};

struct recon_user {
    char name[RECON_USERS_NAME_MAX];
    enum recon_user_role role;
    bool has_password;
};

/*
 * Read the account list. A missing file is not an error: a system with no
 * accounts yet is a system on its first run, which is a state to handle rather
 * than a problem to report.
 */
bool recon_users_init(void);
void recon_users_finish(void);

/* --- Who exists --- */

int recon_users_count(void);
bool recon_users_at(int index, struct recon_user *out);
bool recon_users_find(const char *name, struct recon_user *out);

/* How many administrators there are. Used to refuse the change that would
 * leave a system nobody can administer. */
int recon_users_admin_count(void);

/* --- Changing them --- */

/*
 * Create an account. `password` may be NULL or empty for one without.
 *
 * Names are checked: something that is not a usable folder name is not a
 * usable account name, since the account gets a folder.
 */
bool recon_users_create(const char *name, const char *password,
    enum recon_user_role role);

/*
 * Delete an account. Refused for the last administrator, and refused for the
 * account currently signed in -- deleting the ground you are standing on is
 * never what was meant.
 *
 * The account's files are left alone. Removing somebody's login is not a
 * decision to destroy their documents.
 */
/*
 * Remove an account, and decide what happens to its folder.
 *
 * `delete_files` is a separate answer from removing the login because they
 * are separate decisions: closing somebody's account is an administrative
 * act, and destroying their documents is not something that should follow
 * from it silently. Whoever asks has to say which they meant.
 *
 * The account goes first and the folder second. If the folder cannot be
 * removed the account is still gone -- the login is what was asked about, and
 * leaving it in place because a file was locked would be answering a
 * different question.
 */
bool recon_users_remove(const char *name, bool delete_files);

bool recon_users_set_role(const char *name, enum recon_user_role role);

/* Change a password. `password` may be NULL or empty to remove it. */
bool recon_users_set_password(const char *name, const char *password);

/* --- Signing in --- */

/*
 * Check a password without signing in. Takes the same time whether the account
 * exists or not, so the answer does not say which accounts are real.
 */
bool recon_users_check(const char *name, const char *password);

bool recon_users_login(const char *name, const char *password);
void recon_users_logout(void);

/* The account signed in, or NULL when nobody is. */
const char *recon_users_current(void);
bool recon_users_current_is_admin(void);

/* Whether the signed-in account may act on another's files or on /System. */
bool recon_users_may_administer(void);

const char *recon_users_last_error(void);

#endif
