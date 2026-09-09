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
 * **4. Nothing is written to disk.** A cookie with an expiry a year away is
 * kept until the browser closes and no longer. A stored session cookie is a
 * key to somebody's account sitting in a file, and the place for a key in this
 * system is the keyring -- which holds 512 bytes per secret and has no consent
 * question in front of it yet. Both of those are on the list. Until then this
 * says what is true: signing in lasts as long as the window.
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

#endif /* RECON_COOKIE_H */
