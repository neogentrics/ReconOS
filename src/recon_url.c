/*
 * Reading an address. See include/recon_http.h.
 *
 * Split out of recon_http.c, which fetches pages. The two were one file, and
 * the consequence was that testing the address parser meant linking the
 * fetching -- which pulls in sockets, TLS, the registry and the Wayland event
 * loop. So it was not tested, and the fragment bug lived here: `#main`
 * resolved to no named place at all, because the code that pulls the fragment
 * out searched the *path*, and the path that branch writes is the base's,
 * whose own hash was stripped when the base was parsed.
 *
 * Nothing in this file opens anything. It is string handling on somebody
 * else's bytes, which is exactly the sort of thing that wants a suite.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "recon_http.h"
#include "recon_http_error.h"

static char g_error[256];

void recon_http_set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_http_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* --- Addresses --- */

void recon_http_format_url(const struct recon_http_url *url, char *out,
        size_t size) {
    if (url == NULL || out == NULL || size == 0) {
        return;
    }

    /* The port is shown only when it is not the usual one for the scheme.
     * "https://example.com:443/" is the same address written less clearly. */
    int usual = url->secure ? 443 : 80;
    if (url->port == usual) {
        snprintf(out, size, "%s://%s%s", url->secure ? "https" : "http",
            url->host, url->path);
    } else {
        snprintf(out, size, "%s://%s:%d%s", url->secure ? "https" : "http",
            url->host, url->port, url->path);
    }
}

/* Everything up to the last "/" of a path, which is what a relative link is
 * relative to. "/a/b/c" gives "/a/b/", and "/a" gives "/". */
static void directory_of(const char *path, char *out, size_t size) {
    const char *last = strrchr(path, '/');
    size_t keep = (last != NULL) ? (size_t)(last - path) + 1 : 1;
    if (keep >= size) {
        keep = size - 1;
    }
    memcpy(out, path, keep);
    out[keep] = '\0';
    if (out[0] == '\0') {
        snprintf(out, size, "/");
    }
}

/*
 * Flatten "." and ".." out of a path.
 *
 * Done here rather than left to the server, because a link written as
 * "../index.html" three directories deep produces a path a server may or may
 * not understand, and because ".." is how a path escapes upward -- the same
 * reason recon_fs resolves rather than concatenates.
 */
static void tidy_path(char *path) {
    char out[RECON_HTTP_URL_MAX];
    size_t used = 0;
    out[used++] = '/';

    const char *at = path;
    while (*at == '/') {
        at++;
    }

    while (*at != '\0') {
        const char *end = strchr(at, '/');
        size_t length = (end != NULL) ? (size_t)(end - at) : strlen(at);

        if (length == 1 && at[0] == '.') {
            /* "." is where we already are. */
        } else if (length == 2 && at[0] == '.' && at[1] == '.') {
            /* Back up one segment, never past the root. */
            if (used > 1) {
                used--;                         /* the trailing slash */
                while (used > 1 && out[used - 1] != '/') {
                    used--;
                }
            }
        } else if (length > 0) {
            if (used + length + 1 < sizeof(out)) {
                memcpy(out + used, at, length);
                used += length;
                if (end != NULL) {
                    out[used++] = '/';
                }
            }
        }

        if (end == NULL) {
            break;
        }
        at = end + 1;
    }

    out[used] = '\0';
    snprintf(path, RECON_HTTP_URL_MAX, "%s", out);
}

bool recon_http_parse_url(const char *text, const struct recon_http_url *base,
        struct recon_http_url *out) {
    if (text == NULL || out == NULL) {
        return false;
    }

    while (*text == ' ' || *text == '\t') {
        text++;
    }
    if (*text == '\0') {
        recon_http_set_error("there is no address there");
        return false;
    }

    memset(out, 0, sizeof(*out));

    /*
     * https by default, including for a bare host name.
     *
     * Guessing http would be guessing the one that costs somebody their
     * privacy, and every site worth reading answers on 443. A site that only
     * speaks http can be reached by typing http:// deliberately.
     */
    bool have_scheme = false;
    if (strncasecmp(text, "https://", 8) == 0) {
        out->secure = true;
        text += 8;
        have_scheme = true;
    } else if (strncasecmp(text, "http://", 7) == 0) {
        out->secure = false;
        text += 7;
        have_scheme = true;
    } else if (strstr(text, "://") != NULL) {
        recon_http_set_error("only http and https addresses can be opened");
        return false;
    }

    if (!have_scheme && base != NULL) {
        /* Relative to the page it came from. */
        out->secure = base->secure;
        snprintf(out->host, sizeof(out->host), "%s", base->host);
        out->port = base->port;

        if (text[0] == '/') {
            snprintf(out->path, sizeof(out->path), "%s", text);
        } else if (text[0] == '#') {
            /*
             * A fragment on the same page: the same document, at a place in
             * it. Fetching it again to end up where you already are would
             * throw away the scroll position to arrive at the same bytes.
             *
             * The fragment is taken from `text` here and not by the hash
             * search below, because that search looks in `out->path` -- and
             * the path this branch writes is the *base's*, which has already
             * had its own hash stripped. So the search found nothing, every
             * "#main" parsed to an empty fragment, and the viewer correctly
             * treated a link with no named place as "go to the top". Which is
             * what it did: click the skip link, arrive at the top of the page
             * you were already at the top of.
             */
            snprintf(out->path, sizeof(out->path), "%s", base->path);
            snprintf(out->fragment, sizeof(out->fragment), "%s", text + 1);
        } else {
            char directory[RECON_HTTP_URL_MAX];
            directory_of(base->path, directory, sizeof(directory));
            snprintf(out->path, sizeof(out->path), "%s%s", directory, text);
        }

        char *hash = strchr(out->path, '#');
        if (hash != NULL) {
            snprintf(out->fragment, sizeof(out->fragment), "%s", hash + 1);
            *hash = '\0';
        }
        tidy_path(out->path);
        return true;
    }

    if (!have_scheme) {
        /* A bare address typed into the bar. */
        out->secure = true;
    }

    /* The host, up to the port, the path, or the end. */
    size_t host_length = strcspn(text, ":/?#");
    if (host_length == 0 || host_length >= sizeof(out->host)) {
        recon_http_set_error("that address has no server name in it");
        return false;
    }
    memcpy(out->host, text, host_length);
    out->host[host_length] = '\0';
    text += host_length;

    out->port = out->secure ? 443 : 80;
    if (*text == ':') {
        text++;
        int port = atoi(text);
        if (port <= 0 || port > 65535) {
            recon_http_set_error("'%.16s' is not a port", text);
            return false;
        }
        out->port = port;
        text += strspn(text, "0123456789");
    }

    if (*text == '\0' || *text == '#') {
        /* "example.com#top" -- the whole path is the root, and the fragment
         * is everything that follows the hash. Handled here as well as below
         * because this branch never looks at `text` again. */
        snprintf(out->path, sizeof(out->path), "/");
        if (*text == '#') {
            snprintf(out->fragment, sizeof(out->fragment), "%s", text + 1);
        }
    } else {
        snprintf(out->path, sizeof(out->path), "%s", text);
        char *hash = strchr(out->path, '#');
        if (hash != NULL) {
            snprintf(out->fragment, sizeof(out->fragment), "%s", hash + 1);
            *hash = '\0';
        }
    }
    tidy_path(out->path);
    return true;
}

