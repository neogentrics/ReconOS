/*
 * A file only this account can read.
 *
 * --- The gap this fills, in the keyring's own words ---
 *
 * `include/recon_keyring.h` says what does not belong in a keyring:
 *
 *   "The longest secret this will hold. A password, a token, a key -- not a
 *    file. Something larger belongs in a file that is encrypted, which is a
 *    different thing and is not this."
 *
 * That different thing did not exist, and the absence had a cost that was
 * easy to miss: **the browser cannot keep a cookie past the window closing.**
 * A stored session cookie is a key to somebody's account, so the registry is
 * out and plain text on disk is out -- and a jar of three hundred cookies is
 * half a megabyte, which is a thousand times what a keyring entry holds. The
 * board row read "blocked on the keyring's size", and this is that block.
 *
 * --- What it is ---
 *
 * Bytes on disk, encrypted with a key derived from the account password at
 * sign-in, authenticated, and unreadable when nobody is signed in. The same
 * protection the keyring gives a password, for something the size of a file.
 *
 * `recon_keyring_derive("sealed-files", ...)` is where the key comes from, so
 * the account key lives in one place and is not copied to grow a second. HMAC
 * is one way, so this key does not lead back to that one.
 *
 * --- What it protects against, stated exactly ---
 *
 * A stolen disk and a stolen backup. That is the whole of it, and it is the
 * same sentence the keyring ends on: while somebody is signed in, the key is
 * in this process's memory and any code in this process can read any sealed
 * file. There are no address spaces yet. When there are, this is one of the
 * things that gets to mean more; until then it says what is true.
 *
 * It is also not a guarantee that the bytes still exist. Deleting a sealed
 * file is deleting a file; nothing here hides one or keeps a copy.
 *
 * --- One blob, not a stream ---
 *
 * A sealed file is encrypted and authenticated as a single piece, so reading
 * one means holding all of it, and **nothing is handed to a caller until the
 * whole file has authenticated.** That is the property worth having: a caller
 * can never act on the first half of a file whose second half was tampered
 * with, which is exactly the fault a streaming reader has to work to avoid.
 *
 * The cost is the size limit below, and it is deliberate rather than an
 * oversight. Something genuinely large -- a film, a disk image -- wants a
 * format with authenticated chunks, and would be a different file with a
 * different header. Getting it by accident here, by raising a constant until
 * it stopped complaining, is how a mechanism ends up used for something it was
 * never reasoned about.
 */

#ifndef RECON_SEALED_H
#define RECON_SEALED_H

#include <stdbool.h>
#include <stddef.h>

/*
 * A name for one sealed file: "browser/cookies".
 *
 * Shaped like a keyring name and a registry key, so a person who has read one
 * can read the others. A `/` in it makes a directory, which is what lets one
 * program's sealed files sit together.
 */
#define RECON_SEALED_NAME_MAX 128

/*
 * The largest sealed file, in bytes.
 *
 * Four mebibytes, and the number is a judgement rather than a limit anything
 * imposes: a cookie jar at its fullest is about half a megabyte, and this
 * leaves room for the things that are like a cookie jar without leaving room
 * for the things that are not. See the note above on why it is not simply
 * large.
 *
 * It is also the cap on what a *read* will allocate, which matters
 * independently: the size read back comes from a file, and a file on a disk
 * somebody else has had is a number nobody should be multiplying by anything.
 */
#define RECON_SEALED_MAX (4 * 1024 * 1024)

/*
 * Seal `size` bytes under `name`, replacing whatever was there.
 *
 * False when the keyring is locked, when the name is unusable, when the bytes
 * are too many, or when the write fails -- and a false is bytes that were not
 * kept, so a caller that ignores it has a program that silently forgets.
 *
 * Writing zero bytes is allowed and is not the same as forgetting: the file
 * exists and reads back empty, which is how "this jar has been emptied" is
 * told apart from "this jar has never existed".
 */
bool recon_sealed_write(const char *name, const void *bytes, size_t size);

/*
 * Read one back. NULL when it is not there, the keyring is locked, or **what
 * is there does not authenticate** -- which means the file was edited, or it
 * belongs to a different account, or the password has changed since it was
 * written.
 *
 * Those are one answer on purpose. Telling a caller *which* is telling
 * somebody holding the disk which of their guesses was closest, and the caller
 * does the same thing in every case: treat it as absent.
 *
 * The caller frees what comes back. `size_out` is set to the length and the
 * bytes are NUL-terminated one past it, so a sealed file holding text can be
 * used as a string without copying -- the terminator is not part of the
 * content and is not sealed.
 */
void *recon_sealed_read(const char *name, size_t *size_out);

/* Whether there is a sealed file under this name. Says nothing about whether
 * it can be read: a file belonging to another account is still a file. */
bool recon_sealed_exists(const char *name);

/* Remove one. True if it was there. */
bool recon_sealed_forget(const char *name);

const char *recon_sealed_last_error(void);

#endif /* RECON_SEALED_H */
