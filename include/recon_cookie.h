/*
 * Cookies: what a server asked this machine to remember, and when it is
 * handed back.
 *
 * A cookie is a small piece of somebody else's state kept on your computer and
 * returned to them on every visit. That is what makes a session survive a page,
 * and it is also the whole mechanism by which browsing is followed around --
 * the two are the same feature and there is no version of it that is only the
 * first.
 *
 * So the interesting content of this file is what it *refuses*. Nothing here
 * is a switch: every rule below is unconditional, because a safety check with
 * a way round it is a check that is off on somebody's machine.
 *
 * --- The four refusals ---
 *
 * **1. A cookie belongs to the host that set it, and to no other.** The
 * `Domain` attribute -- which asks for a cookie to be sent to a whole family of
 * hosts -- is read, and is honoured only when it names the host itself.
 * Anything wider is *narrowed* to the host rather than refused outright,
 * because narrowing is always safe and refusing the cookie would break a site
 * that had asked for something reasonable.
 *
 * The reason is not squeamishness. Honouring `Domain` safely means knowing
 * where the registrable part of a name ends -- that `example.co.uk` is a site
 * and `co.uk` is not -- and that is not derivable from the name. It is a list,
 * the Public Suffix List, and a *copy* of that list goes stale in the one
 * direction that matters: a suffix registered after the copy was taken is one
 * this system would treat as an ordinary domain, so `Domain=.something.new`
 * would be accepted and every site under it would share one cookie. A check
 * that quietly weakens as the file ages is worse than one that was never
 * there, because nobody is watching it.
 *
 * What that costs is real and bounded: a cookie set on `example.com` is not
 * sent to `www.example.com`. It is not lost -- it is kept where it came from.
 *
 * **2. Only the document carries them.** A page's pictures and stylesheets are
 * fetched without cookies, always. Nothing needs a session to serve a logo,
 * and a subresource request that carries one is precisely the mechanism that
 * follows somebody between sites. This is enforced by the shape of the call
 * rather than by a rule: `recon_http_get` takes the jar as an argument, so
 * every fetch in this system says at its call site whether it is carrying the
 * session, and two of the four say no.
 *
 * **3. A `Secure` cookie is never set over an unencrypted connection**, and
 * never sent over one. A server that asks for both has asked for a
 * contradiction; the encrypted half wins.
 *
 * **4. What is written to disk is sealed, and it is not everything.**
 *
 * This used to say *"nothing is written to disk"*, and gave the reason: a
 * stored session cookie is a key to somebody's account sitting in a file, the
 * place for a key is the keyring, and the keyring holds 512 bytes -- about a
 * thousandth of a full jar. `include/recon_sealed.h` is what was missing. A
 * saved jar is encrypted under a key derived from the account password at
 * sign-in, so a stolen disk is ciphertext and a locked machine is a jar
 * nothing can read.
 *
 * **A cookie with no expiry is still never written.** That is not caution, it
 * is what the word means: a session cookie is defined as lasting until the
 * browser closes, and keeping one would be storing something the server asked
 * not to be stored. So what survives a restart is exactly what a server asked
 * to survive one, and nothing else.
 *
 * A cookie whose expiry passed while the browser was shut is dropped as the
 * jar is read, not when it is next looked at. The difference matters: it is
 * never in the jar at all, so nothing can send it in the window between
 * loading and the first sweep.
 *
 * And the limit is worth stating rather than implying. This protects a disk
 * somebody has taken away. It does **not** protect against code running in
 * this process while somebody is signed in -- there are no address spaces yet,
 * and `include/recon_sealed.h` says the same thing about everything it
 * holds.
 *
 * --- What is not refused ---
 *
 * `HttpOnly` is recorded and does nothing, because it exists to hide a cookie
 * from script and there is no script. `SameSite` is recorded and does nothing
 * for the same kind of reason: this sends cookies on no cross-site request of
 * any kind, which is stricter than `SameSite=Strict` and is not a setting.
 */

#ifndef RECON_COOKIE_H
#define RECON_COOKIE_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/*
 * Ceilings, all of them deliberate. A server chooses these numbers, so every
 * one is a bound on what somebody else can make this machine hold.
 *
 * The value is large because real session tokens are: a signed JSON token runs
 * to several hundred bytes and is entirely ordinary.
 */
#define RECON_COOKIE_NAME_MAX 128
#define RECON_COOKIE_VALUE_MAX 1024
#define RECON_COOKIE_HOST_MAX 256
#define RECON_COOKIE_PATH_MAX 256

/* Per host, and in total. A site that sets more than fifty is a site that has
 * lost track; past that the soonest to expire is dropped to make room. */
#define RECON_COOKIE_PER_HOST 50
#define RECON_COOKIE_MAX 300

struct recon_cookie_jar;

/*
 * What one cookie is, for anything that wants to show them.
 *
 * The value is deliberately included. A viewer that offers to show somebody
 * what is being sent on their behalf and then hides the part that matters is
 * not showing them anything.
 */
struct recon_cookie_view {
    char host[RECON_COOKIE_HOST_MAX];
    char path[RECON_COOKIE_PATH_MAX];
    char name[RECON_COOKIE_NAME_MAX];
    char value[RECON_COOKIE_VALUE_MAX];
    bool secure;
    bool http_only;

    /* 0 for a cookie that lasts as long as the window. */
    time_t expires;

    /* True when the server asked for a wider scope than the host and was
     * narrowed to it. Kept so a listing can say so rather than leaving
     * somebody to wonder why a cookie is not being sent to a subdomain. */
    bool was_narrowed;
};

struct recon_cookie_jar *recon_cookie_jar_new(void);
void recon_cookie_jar_free(struct recon_cookie_jar *jar);

/*
 * Take one `Set-Cookie` header.
 *
 * `host` and `path` are the request's -- the ones the server is answering, not
 * the ones it asks for. `secure` says the connection was encrypted.
 *
 * Returns NULL when the cookie was taken, and otherwise a sentence saying why
 * it was not. A sentence rather than a code because there is exactly one
 * caller and what it does with a refusal is put it in front of somebody.
 *
 * A `Max-Age` of zero or less, or an `Expires` in the past, deletes a cookie
 * of that name rather than storing one. That is how a server signs somebody
 * out, and treating it as an ordinary cookie would leave them signed in.
 */
const char *recon_cookie_set(struct recon_cookie_jar *jar, const char *host,
    const char *path, bool secure, const char *set_cookie, time_t now);

/*
 * Build the `Cookie:` header value for a request, without the header name.
 *
 * Returns the number of bytes written. Zero -- an empty string -- means send
 * no header at all rather than an empty one.
 *
 * Longer paths first, then oldest first, which is what RFC 6265 asks for and
 * what servers that read only the first of a repeated name depend on.
 */
size_t recon_cookie_header(struct recon_cookie_jar *jar, const char *host,
    const char *path, bool secure, time_t now, char *out, size_t size);

/*
 * Everything in the jar, for showing and for forgetting.
 *
 * `recon_cookie_sweep` drops what has expired and returns how many went. It is
 * called by the two above, so a caller never sees an expired cookie; it is
 * public so a listing can be honest about the count without waiting for a
 * fetch to tidy up first.
 */
int recon_cookie_count(const struct recon_cookie_jar *jar);
bool recon_cookie_at(const struct recon_cookie_jar *jar, int index,
    struct recon_cookie_view *out);
int recon_cookie_sweep(struct recon_cookie_jar *jar, time_t now);

/* Returns how many were dropped, so the answer can say so. */
int recon_cookie_forget_host(struct recon_cookie_jar *jar, const char *host);
int recon_cookie_forget_all(struct recon_cookie_jar *jar);

/*
 * Drop what was only for this window, and keep the rest.
 *
 * A cookie with no expiry is a **session** cookie, and the word is the
 * specification: it lasts until the browser closes. So something has to end
 * it -- and until v0.4.74 nothing did. Closing a built-in application hides
 * its window rather than destroying it, so the destructor that seals the jar
 * ran at shutdown and not on close, and a session cookie outlived the browser
 * closing by the whole of the rest of the login.
 *
 * Which is the failure hardest to see from outside: everything looks right,
 * because the cookie that *should* come back does. The one that should not is
 * indistinguishable from it unless somebody is looking for it.
 *
 * Returns how many were dropped.
 */
int recon_cookie_forget_session(struct recon_cookie_jar *jar);

/* --- Across a restart ---------------------------------------------------
 *
 * Kept in a sealed file, which is a file only the signed-in account can read.
 * See refusal 4 at the top of this header for what is kept and what is not.
 *
 * These are called by the browser rather than by the jar, deliberately: a jar
 * that wrote to disk on its own would be a jar that decided when somebody's
 * cookies were worth keeping, and that is the browser's decision to make and
 * the browser's to stop making when somebody clears them.
 */

/*
 * Write the jar, keeping only what has an expiry in the future.
 *
 * True when it was written -- including when there was nothing to write, which
 * saves an **empty** jar rather than leaving the old one. That is the whole
 * difference between "I signed out" and "I signed out and it came back": a
 * save that skipped an empty jar would leave yesterday's cookies sealed on the
 * disk for the next start to find.
 *
 * False when the keyring is locked or the write fails, and a false is cookies
 * that were not kept.
 */
bool recon_cookie_jar_save(const struct recon_cookie_jar *jar);

/*
 * Read it back into `jar`, dropping anything that expired while it was shut.
 *
 * Returns how many cookies were restored; zero for an empty jar, a locked
 * keyring, nothing saved, or a file that does not open -- which are one answer
 * for the reason `recon_sealed_read` gives, and because the browser does the
 * same thing in every case.
 *
 * Added to whatever is already in the jar rather than replacing it, because
 * the only caller loads into a jar it has just made.
 */
int recon_cookie_jar_load(struct recon_cookie_jar *jar, time_t now);

/* Remove what was saved. For "clear cookies", which has to reach the disk as
 * well as the jar or it clears them until the next start. */
bool recon_cookie_jar_forget_saved(void);

/*
 * Put a cookie back into a jar as it was.
 *
 * --- Why this exists, and why it is not a way in ---
 *
 * `src/recon_cookie_store.c` reads a saved jar, and it is a *different file*
 * from `src/recon_cookie.c` on purpose: sealing pulls in a cipher, a keyring
 * and a filesystem, and the jar is 850 lines of policy that compiles with no
 * libc under it. Putting the two together would have taken the policy off that
 * list to get a file format.
 *
 * So the store needs a way to hand a cookie back, and this is it. **It is not
 * a way round `recon_cookie_set`.** Every refusal that applies to a cookie
 * arriving from a server applies here -- the lengths, the control characters,
 * the room in the jar -- because a saved file is a file somebody with the disk
 * can write, and a cookie that could not have arrived must not be able to be
 * restored either.
 *
 * What it does *not* re-derive is the narrowing and the flags, which are
 * decisions already made when the cookie arrived and are carried across as
 * recorded.
 *
 * `expires` must be in the future; a cookie with none, or one already past, is
 * refused rather than swept later -- a cookie waiting to be tidied up is a
 * cookie that can be sent first.
 *
 * False when the cookie was not put back, for any of those reasons.
 */
bool recon_cookie_restore(struct recon_cookie_jar *jar,
    const struct recon_cookie_view *cookie, time_t now);

#endif /* RECON_COOKIE_H */
