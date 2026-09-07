/*
 * Secrets that exist only while somebody is signed in.
 *
 * The Mail window asks for a password every time it connects and holds it only
 * while the window is open, and the note beside that said why: there was
 * nowhere safe to put it. The registry means anything that can read a file can
 * read it, and obfuscation is worse than plain text because it looks like
 * protection.
 *
 * This is the somewhere. It is a subsystem rather than a field, and the whole
 * of it is the answer to one question: what is the key, and when does it exist?
 *
 * --- The key ---
 *
 * Derived from the account password at sign-in, with PBKDF2, and held in
 * memory. It is never written anywhere, so a copy of the disk is a copy of
 * ciphertext and nothing else. Signing out erases it, and everything in here
 * becomes unreadable until somebody signs in again.
 *
 * **The salt is not the login salt.** That is the property everything else
 * rests on. The accounts file stores PBKDF2(password, login_salt) to check a
 * password against; if the keyring used the same salt, that stored value would
 * *be* the key, and anybody who could read the accounts file could read every
 * secret without knowing the password at all. A second salt, stored beside the
 * first, makes the two derivations independent -- and it is written down here
 * because it is the kind of thing that looks like a detail and is the whole
 * design.
 *
 * --- What is stored ---
 *
 * AES-256-GCM, from mbedTLS. Not a cipher written here: the rule in
 * docs/ROADMAP.md is that vetted primitives guard anything that actually
 * protects user data, and this is the first thing in ReconOS that does.
 *
 * A fresh random nonce for every write, stored beside the ciphertext. GCM with
 * a repeated nonce under the same key is not weakened, it is broken -- two
 * messages under one nonce leak their difference and hand over the
 * authentication key -- so the nonce comes from /dev/urandom and is never
 * derived from anything.
 *
 * The entry's **name is authenticated** as associated data, so a ciphertext
 * cannot be moved from one name to another. Without that, somebody who could
 * write the file could swap the mail password onto a name a different program
 * reads, and every byte would still verify.
 *
 * --- What this does not do ---
 *
 * It does not protect against somebody who is signed in. While the session is
 * unlocked the key is in this process's memory and any code in this process
 * can ask for a secret -- which is every module, since a module is loaded into
 * this process. That is a real limit and it is a property of having no kernel:
 * there are no separate address spaces to hide a key in. It protects a stolen
 * disk and a stolen backup, and it says so rather than implying more.
 */

#ifndef RECON_KEYRING_H
#define RECON_KEYRING_H

#include <stdbool.h>
#include <stddef.h>

/* The longest secret this will hold. A password, a token, a key -- not a file.
 * Something larger belongs in a file that is encrypted, which is a different
 * thing and is not this. */
#define RECON_KEYRING_SECRET_MAX 512

/* A name for one secret: "mail/password", "mail/send-password". Shaped like a
 * registry key on purpose -- a person who has read one can read the other. */
#define RECON_KEYRING_NAME_MAX 128

/*
 * Make the keyring readable, using an account's password.
 *
 * Called at sign-in, with the password that was just checked. False when there
 * is no such account or the derivation fails; a wrong password does *not* fail
 * here, because nothing has been decrypted yet -- it fails at the first `get`,
 * which is where a wrong key is actually detectable.
 *
 * That is deliberate rather than lazy. Reporting "wrong password" here would
 * mean decrypting something to check, which means keeping a known plaintext to
 * check against, which is a hint about the key sitting in the file next to the
 * things it protects.
 */
bool recon_keyring_unlock(const char *user, const char *password);

/*
 * Forget the key. Everything stored stays on disk and becomes unreadable.
 *
 * Called at sign-out, at lock, and at shutdown. Safe to call when already
 * locked, so a caller does not have to ask.
 */
void recon_keyring_lock(void);

bool recon_keyring_unlocked(void);

/*
 * Keep a secret under a name, replacing anything there.
 *
 * False when locked, when the name or secret is too long, or when the write
 * fails. A false is a secret that was not kept, and a caller that ignores it
 * has a program which silently forgets passwords.
 */
bool recon_keyring_put(const char *name, const char *secret);

/*
 * Read one back. False when locked, when there is nothing under that name, or
 * when what is there does not authenticate -- which means the file was edited,
 * or the password has changed since it was written.
 *
 * `out` is erased before anything else happens, so a caller that ignores the
 * result is not left holding a previous secret.
 */
bool recon_keyring_get(const char *name, char *out, size_t size);

/* Remove one. True if it was there. */
bool recon_keyring_forget(const char *name);

/* How many secrets are kept, for a page that lists them. Names only -- there
 * is no call here that hands out every secret at once, because nothing has a
 * reason to want that. */
int recon_keyring_count(void);
bool recon_keyring_name_at(int index, char *out, size_t size);

const char *recon_keyring_last_error(void);

#endif /* RECON_KEYRING_H */
