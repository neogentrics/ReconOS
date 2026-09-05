/*
 * Fetching a page over HTTP. See include/recon_http.h.
 *
 * One request per struct, driven by the stream's callbacks. The body arrives
 * in whatever sizes the network hands over, so everything here is written to
 * cope with a header split across two reads and a chunk length split across
 * three.
 */

#define _POSIX_C_SOURCE 200809L
/* strcasestr is a GNU extension, and the one place here that wants it is
 * looking for "chunked" inside a Transfer-Encoding that may list more than
 * one encoding in any case. */
#define _GNU_SOURCE

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "recon_http.h"
#include "recon_net.h"

/* The response head, before the body starts. Bounded because a server that
 * sends a megabyte of headers is not one to keep reading from. */
#define HEAD_MAX 16384

static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
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
        set_error("there is no address there");
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
        set_error("only http and https addresses can be opened");
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
            /* A fragment on the same page. There is no anchor navigation
             * here, so it is the same document -- which is better than
             * fetching it again to end up in the same place. */
            snprintf(out->path, sizeof(out->path), "%s", base->path);
        } else {
            char directory[RECON_HTTP_URL_MAX];
            directory_of(base->path, directory, sizeof(directory));
            snprintf(out->path, sizeof(out->path), "%s%s", directory, text);
        }

        char *hash = strchr(out->path, '#');
        if (hash != NULL) {
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
        set_error("that address has no server name in it");
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
            set_error("'%.16s' is not a port", text);
            return false;
        }
        out->port = port;
        text += strspn(text, "0123456789");
    }

    if (*text == '\0' || *text == '#') {
        snprintf(out->path, sizeof(out->path), "/");
    } else {
        snprintf(out->path, sizeof(out->path), "%s", text);
        char *hash = strchr(out->path, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
    }
    tidy_path(out->path);
    return true;
}

/* --- A request --- */

/* How the body is delimited, which the headers decide. */
enum body_kind {
    BODY_UNTIL_CLOSE,     /* no length given; it ends when the socket does */
    BODY_LENGTH,
    BODY_CHUNKED,
};

struct recon_http_request {
    char application[64];
    struct recon_http_url url;

    struct recon_net_stream *stream;
    struct recon_http_handlers handlers;
    void *user;

    /* The head, gathered until the blank line that ends it. */
    char head[HEAD_MAX];
    size_t head_used;
    bool head_done;

    int status;
    char content_type[128];
    char location[RECON_HTTP_URL_MAX];

    enum body_kind kind;
    size_t announced;              /* Content-Length, when there is one */

    char *body;
    size_t body_used;
    bool body_full;                /* hit the ceiling */

    /* Chunked decoding, which arrives in pieces that do not line up with
     * anything. */
    size_t chunk_left;
    bool chunk_in_size;
    char chunk_size[32];
    size_t chunk_size_used;
    bool chunked_done;

    int redirects;
    bool finished;
};

static void finish(struct recon_http_request *r, bool ok, const char *error);

static void deliver(struct recon_http_request *r, const char *bytes,
        size_t length);

/* --- Reading the head --- */

/* Case-insensitive "does this line start with this header name". */
static const char *header_value(const char *line, const char *name) {
    size_t length = strlen(name);
    if (strncasecmp(line, name, length) != 0 || line[length] != ':') {
        return NULL;
    }
    const char *value = line + length + 1;
    while (*value == ' ' || *value == '\t') {
        value++;
    }
    return value;
}

static void read_head(struct recon_http_request *r) {
    char line[1024];
    const char *at = r->head;
    bool first = true;

    while (*at != '\0') {
        const char *end = strstr(at, "\r\n");
        if (end == NULL) {
            break;
        }
        size_t length = (size_t)(end - at);
        if (length == 0) {
            break;                             /* the blank line: head over */
        }
        if (length >= sizeof(line)) {
            length = sizeof(line) - 1;
        }
        memcpy(line, at, length);
        line[length] = '\0';
        at = end + 2;

        if (first) {
            /* "HTTP/1.1 200 OK" */
            const char *space = strchr(line, ' ');
            r->status = (space != NULL) ? atoi(space + 1) : 0;
            first = false;
            continue;
        }

        const char *value = header_value(line, "Content-Type");
        if (value != NULL) {
            snprintf(r->content_type, sizeof(r->content_type), "%s", value);
            continue;
        }

        value = header_value(line, "Location");
        if (value != NULL) {
            snprintf(r->location, sizeof(r->location), "%s", value);
            continue;
        }

        value = header_value(line, "Content-Length");
        if (value != NULL && r->kind != BODY_CHUNKED) {
            r->announced = (size_t)strtoull(value, NULL, 10);
            r->kind = BODY_LENGTH;
            continue;
        }

        value = header_value(line, "Transfer-Encoding");
        if (value != NULL && strcasestr(value, "chunked") != NULL) {
            /*
             * Chunked wins over Content-Length when a server sends both,
             * which the specification requires and which servers do. Taking
             * the length instead would read the chunk headers as document.
             */
            r->kind = BODY_CHUNKED;
            r->chunk_in_size = true;
            continue;
        }
    }
}

/* --- The body --- */

static void append(struct recon_http_request *r, const char *bytes,
        size_t length) {
    if (r->body == NULL || length == 0) {
        return;
    }
    if (r->body_used + length >= RECON_HTTP_BODY_MAX) {
        length = RECON_HTTP_BODY_MAX - r->body_used - 1;
        r->body_full = true;
    }
    if (length == 0) {
        return;
    }

    memcpy(r->body + r->body_used, bytes, length);
    r->body_used += length;
    r->body[r->body_used] = '\0';

    if (r->handlers.progress != NULL) {
        r->handlers.progress(r->user, r->body_used);
    }
}

/*
 * Chunked transfer, decoded a byte at a time.
 *
 * A chunk is a hexadecimal length, CRLF, that many bytes, CRLF, and a zero
 * length ends it. None of those boundaries line up with the sizes the network
 * hands over, so the state lives in the request and every byte is looked at
 * once. Slower than it could be and correct at every split, which is the trade
 * worth making for something a server can divide up any way it likes.
 */
static void feed_chunked(struct recon_http_request *r, const char *bytes,
        size_t length) {
    for (size_t i = 0; i < length && !r->chunked_done; i++) {
        char c = bytes[i];

        if (r->chunk_in_size) {
            if (c == '\n') {
                r->chunk_size[r->chunk_size_used] = '\0';
                /* A chunk header may carry extensions after a semicolon.
                 * strtoull stops at the first character that is not a
                 * hexadecimal digit, which handles them by ignoring them. */
                r->chunk_left = (size_t)strtoull(r->chunk_size, NULL, 16);
                r->chunk_size_used = 0;
                r->chunk_in_size = false;

                if (r->chunk_left == 0) {
                    /* The last chunk. Trailers may follow and are not read;
                     * nothing here wants one. */
                    r->chunked_done = true;
                }
                continue;
            }
            if (c != '\r' && r->chunk_size_used < sizeof(r->chunk_size) - 1) {
                r->chunk_size[r->chunk_size_used++] = c;
            }
            continue;
        }

        if (r->chunk_left > 0) {
            /* Take as much of this chunk as is in front of us, rather than
             * one byte at a time -- the common case is a whole chunk. */
            size_t take = length - i;
            if (take > r->chunk_left) {
                take = r->chunk_left;
            }
            append(r, bytes + i, take);
            r->chunk_left -= take;
            i += take - 1;
            continue;
        }

        /* The CRLF after a chunk's data. */
        if (c == '\n') {
            r->chunk_in_size = true;
        }
    }
}

static void deliver(struct recon_http_request *r, const char *bytes,
        size_t length) {
    if (r->kind == BODY_CHUNKED) {
        feed_chunked(r, bytes, length);
        if (r->chunked_done) {
            finish(r, true, "");
        }
        return;
    }

    append(r, bytes, length);

    if (r->kind == BODY_LENGTH && r->body_used >= r->announced) {
        finish(r, true, "");
    }
}

/* --- Redirects --- */

static bool follow(struct recon_http_request *r);

/* --- Finishing --- */

/* Turn a status code into something worth reading. */
static const char *status_sentence(int status) {
    switch (status) {
    case 400: return "the server said that request was malformed";
    case 401: return "that page needs a sign-in, which this cannot do";
    case 403: return "the server refused to show that page";
    case 404: return "there is no page at that address";
    case 408: return "the server gave up waiting";
    case 410: return "that page is gone, and the server says deliberately so";
    case 429: return "the server is asking to be left alone for a while";
    case 500: return "something went wrong at the server's end";
    case 502: return "the server in front could not reach the one behind";
    case 503: return "the server is not taking requests right now";
    case 504: return "the server in front waited too long for the one behind";
    default:  return NULL;
    }
}

static void finish(struct recon_http_request *r, bool ok, const char *error) {
    if (r->finished) {
        return;
    }
    r->finished = true;

    if (r->stream != NULL) {
        recon_net_stream_close(r->stream);
        r->stream = NULL;
    }

    /*
     * A redirect is not an answer, so it is followed rather than reported --
     * unless it is one of the two kinds that should not be.
     */
    if (ok && r->status >= 300 && r->status < 400 && r->location[0] != '\0') {
        if (follow(r)) {
            return;
        }
        ok = false;
        error = g_error;
    }

    if (ok && (r->status < 200 || r->status >= 300)) {
        const char *said = status_sentence(r->status);
        if (said != NULL) {
            set_error("%s", said);
        } else {
            set_error("the server answered %d, which this does not understand",
                r->status);
        }
        ok = false;
        error = g_error;
    }

    char *body = r->body;
    size_t length = r->body_used;
    r->body = NULL;

    if (r->handlers.done != NULL) {
        r->handlers.done(r->user, ok, ok ? body : NULL, ok ? length : 0,
            r->content_type, &r->url, error);
    }
    if (!ok) {
        free(body);
    }

    free(r);
}

/* --- The stream --- */

static void on_opened(void *user, struct recon_net_stream *stream) {
    struct recon_http_request *r = user;

    char request[RECON_HTTP_URL_MAX + 512];
    int written = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: ReconOS\r\n"
        "Accept: text/html, text/plain\r\n"
        /*
         * No keep-alive. The body then ends when the connection does, which
         * removes a pool of idle sockets, a timeout policy, and the class of
         * bug where the second request on a reused connection reads the tail
         * of the first.
         */
        "Connection: close\r\n"
        "\r\n",
        r->url.path, r->url.host);

    if (written < 0 || !recon_net_stream_send(stream, request,
            (size_t)written)) {
        set_error("%s", recon_net_last_error());
        finish(r, false, g_error);
    }
}

static void on_received(void *user, struct recon_net_stream *stream,
        const char *bytes, size_t length) {
    struct recon_http_request *r = user;
    (void)stream;

    if (r->head_done) {
        deliver(r, bytes, length);
        return;
    }

    /* Gather until the blank line, which may arrive split across reads. */
    size_t room = sizeof(r->head) - r->head_used - 1;
    size_t take = length < room ? length : room;
    memcpy(r->head + r->head_used, bytes, take);
    r->head_used += take;
    r->head[r->head_used] = '\0';

    char *split = strstr(r->head, "\r\n\r\n");
    if (split == NULL) {
        if (room == 0) {
            set_error("the server's reply had no end to its headers");
            finish(r, false, g_error);
        }
        return;
    }

    r->head_done = true;
    read_head(r);

    r->body = calloc(1, RECON_HTTP_BODY_MAX);
    if (r->body == NULL) {
        set_error("out of memory");
        finish(r, false, g_error);
        return;
    }

    /* Whatever came after the blank line is already body. */
    size_t head_length = (size_t)(split - r->head) + 4;
    if (r->head_used > head_length) {
        deliver(r, r->head + head_length, r->head_used - head_length);
    }

    /* And anything this read could not fit into the head buffer. */
    if (take < length && !r->finished) {
        deliver(r, bytes + take, length - take);
    }
}

static void on_closed(void *user, struct recon_net_stream *stream,
        enum recon_net_result reason) {
    struct recon_http_request *r = user;
    (void)stream;

    r->stream = NULL;
    if (r->finished) {
        return;
    }

    if (reason == RECON_NET_UNTRUSTED) {
        set_error("%s", recon_net_last_error());
        finish(r, false, g_error);
        return;
    }

    /*
     * A connection that ends without a length having been given is how a
     * document ends. Not a failure -- it is the oldest framing in HTTP and
     * still what a server does when it does not know the length in advance.
     */
    if (r->head_done && r->kind == BODY_UNTIL_CLOSE) {
        finish(r, true, "");
        return;
    }
    if (r->head_done && r->kind == BODY_LENGTH &&
            r->body_used >= r->announced) {
        finish(r, true, "");
        return;
    }

    if (!r->head_done) {
        set_error("%s did not answer: %s", r->url.host,
            recon_net_result_name(reason));
    } else {
        set_error("the connection to %s ended before the page did",
            r->url.host);
    }
    finish(r, false, g_error);
}

static const struct recon_net_stream_handlers STREAM_HANDLERS = {
    .opened = on_opened,
    .received = on_received,
    .closed = on_closed,
};

/* --- Starting one --- */

/* Open the connection for whatever is in r->url, and reset the reply state. */
static bool begin(struct recon_http_request *r) {
    r->head_used = 0;
    r->head[0] = '\0';
    r->head_done = false;
    r->status = 0;
    r->location[0] = '\0';
    r->kind = BODY_UNTIL_CLOSE;
    r->announced = 0;
    r->body_used = 0;
    r->chunk_left = 0;
    r->chunk_in_size = false;
    r->chunk_size_used = 0;
    r->chunked_done = false;
    r->finished = false;
    free(r->body);
    r->body = NULL;

    r->stream = r->url.secure
        ? recon_net_stream_open_tls(r->application, r->url.host, r->url.port,
            &STREAM_HANDLERS, r)
        : recon_net_stream_open(r->application, r->url.host, r->url.port,
            &STREAM_HANDLERS, r);

    if (r->stream == NULL) {
        set_error("%s", recon_net_last_error());
        return false;
    }
    return true;
}

static bool follow(struct recon_http_request *r) {
    if (++r->redirects > RECON_HTTP_REDIRECTS_MAX) {
        set_error("that address kept redirecting and never arrived anywhere");
        return false;
    }

    struct recon_http_url next;
    if (!recon_http_parse_url(r->location, &r->url, &next)) {
        return false;
    }

    /*
     * https may become http only over this system's dead body.
     *
     * A server answering an encrypted request with "ask me again in the clear"
     * is broken or hostile, and following it silently undoes the thing the
     * encryption was for -- with the address bar the only place it shows, and
     * nobody looks there. Refused, and said out loud.
     */
    if (r->url.secure && !next.secure) {
        set_error("%s tried to send this to an unencrypted address, which "
            "would undo the encryption -- refused", r->url.host);
        return false;
    }

    r->url = next;
    return begin(r);
}

struct recon_http_request *recon_http_get(const char *application,
        const struct recon_http_url *url,
        const struct recon_http_handlers *handlers, void *user) {
    if (url == NULL || url->host[0] == '\0') {
        set_error("there is no address to fetch");
        return NULL;
    }

    struct recon_http_request *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        set_error("out of memory");
        return NULL;
    }

    snprintf(r->application, sizeof(r->application), "%s",
        application != NULL ? application : "");
    r->url = *url;
    r->user = user;
    if (handlers != NULL) {
        r->handlers = *handlers;
    }

    if (!begin(r)) {
        free(r);
        return NULL;
    }
    return r;
}

void recon_http_cancel(struct recon_http_request *request) {
    if (request == NULL) {
        return;
    }
    /* The handlers are dropped first, so nothing calls back into an owner
     * that has already decided it is finished with this. */
    memset(&request->handlers, 0, sizeof(request->handlers));

    if (request->stream != NULL) {
        recon_net_stream_close(request->stream);
        request->stream = NULL;
    }
    free(request->body);
    free(request);
}
