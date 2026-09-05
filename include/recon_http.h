/*
 * Fetching a page over HTTP.
 *
 * Enough of HTTP/1.1 to ask a server for a document and read what comes back:
 * GET, headers, redirects, chunked bodies, and the content type. Not a
 * library -- a client, shaped for the one thing above it that wants one.
 *
 * --- What it does not do, and why the list is short on purpose ---
 *
 * No keep-alive. Every request says `Connection: close` and the connection
 * ends with the document. Keep-alive saves a handshake and costs a pool of
 * idle sockets, a timeout policy, and a class of bug where the second request
 * on a reused connection reads the tail of the first. A viewer that fetches a
 * page every few seconds does not need it.
 *
 * No POST, no cookies, no authentication. This reads documents. Anything that
 * changes state on somebody else's server is a decision, and a viewer should
 * not be able to make one by accident.
 *
 * No compression. `Accept-Encoding` is not sent, so servers send plain text.
 * gzip would be a decompressor to write and an attack surface to own for a
 * saving nobody here is measuring.
 *
 * --- The one security rule ---
 *
 * A redirect may go from http to https. It may not go the other way. A server
 * that answers an encrypted request with "now ask me again in the clear" is
 * either broken or hostile, and following it would silently undo the thing the
 * encryption was for. That is refused with a message saying so, rather than
 * followed quietly.
 */

#ifndef RECON_HTTP_H
#define RECON_HTTP_H

#include <stdbool.h>
#include <stddef.h>

/*
 * How much of a document is read.
 *
 * A page of text is tens of kilobytes. This is a bound rather than a
 * prediction: without one, a server that streams forever decides how much
 * memory the desktop uses. Hitting it is reported rather than hidden, because
 * a document silently cut in half looks like a document that ends there.
 */
#define RECON_HTTP_BODY_MAX (2 * 1024 * 1024)

/* Enough for a long URL, and bounded so nothing here has to grow one. */
#define RECON_HTTP_URL_MAX 2048

/* How many redirects to follow before giving up. Five is more than any honest
 * site needs and short enough that a loop is reported rather than chased. */
#define RECON_HTTP_REDIRECTS_MAX 5

/* --- Addresses --- */

struct recon_http_url {
    bool secure;                       /* https rather than http */
    char host[256];
    int port;
    char path[RECON_HTTP_URL_MAX];     /* always begins with "/" */
};

/*
 * Read a URL, filling in what was left out.
 *
 * A bare "example.com" becomes https://example.com/ -- https rather than http,
 * because guessing the insecure one is a guess that costs somebody their
 * privacy, and every site worth reading answers on 443.
 *
 * `base` may be NULL. When it is not, a relative address is resolved against
 * it, which is what makes a link on a page work.
 *
 * False for something that is not an address at all.
 */
bool recon_http_parse_url(const char *text, const struct recon_http_url *base,
    struct recon_http_url *out);

/* Write a URL back out, for an address bar and for the history. */
void recon_http_format_url(const struct recon_http_url *url, char *out,
    size_t size);

/* --- Fetching --- */

struct recon_http_request;

/*
 * What a fetch reports. All optional.
 *
 * `progress` fires as the body grows, so a window can say something is
 * happening during a slow one. `done` fires exactly once and is the last
 * thing: after it the request is finished with and the caller owns `body`.
 */
struct recon_http_handlers {
    void (*progress)(void *user, size_t received);

    /*
     * `body` is the document, NUL-terminated, and becomes the caller's to
     * free. NULL when the fetch failed, in which case `error` says why in a
     * sentence rather than a status code -- "that page was not found" is
     * something to act on and "404" is something to look up.
     *
     * `final_url` is where the document actually came from, which is not
     * always where it was asked for: a redirect changes it, and an address bar
     * that keeps showing the old one is lying about what is on screen.
     */
    void (*done)(void *user, bool ok, char *body, size_t length,
        const char *content_type, const struct recon_http_url *final_url,
        const char *error);
};

/*
 * Ask for a document. Returns immediately; nothing has happened yet.
 *
 * NULL when it could not be started at all -- a bad address, no network
 * permission -- and then no handler is ever called.
 */
struct recon_http_request *recon_http_get(const char *application,
    const struct recon_http_url *url,
    const struct recon_http_handlers *handlers, void *user);

/*
 * Give up on one. The `done` handler is not called: the caller asked, so the
 * caller knows.
 */
void recon_http_cancel(struct recon_http_request *request);

const char *recon_http_last_error(void);

#endif /* RECON_HTTP_H */
