/*
 * Serving, and what a response is.
 *
 * **The routing table is the reason this file exists.** Joshua's requirement:
 * *make sure it's supportive of all the different write types for web servers
 * and web applications, not just web pages or basic HTML.* A server built
 * around "find the file, send the file" has that decision baked into its
 * middle, and every dynamic thing afterwards is a special case cut into the
 * path that serves files.
 *
 * So the middle of this server is a **handler**, and serving a file is one
 * handler among others rather than the thing the server is. A JSON endpoint, a
 * form post, a status feed and a static page are the same shape to this code:
 * a function that is handed a parsed request and fills in a response.
 *
 * `docs/WEB.md` is the full specification and says what is planned on top of
 * this -- streaming bodies, WebSocket upgrade, virtual hosts, TLS, compression
 * and the rest. What is here is the part that runs today.
 */

#ifndef RECON_HTTP_SERVE_H
#define RECON_HTTP_SERVE_H

#include "http.h"

/* The largest response body a handler may return in one piece.
 *
 * A cap rather than a stream, stated plainly because it is a limitation and
 * not a design: this server holds a whole response in memory before it sends
 * any of it. Streaming is specified in `docs/WEB.md` and is not built. A
 * handler that needs to return more than this must wait for it. */
#define HTTP_RESPONSE_MAX 262144

struct http_response {
	int         status;
	const char *content_type;	/* NULL means the status carries no body */
	const char *body;
	size_t      body_len;
	int         close;		/* ask for the connection to end */

	/* One extra header, for the handlers that need exactly one --
	 * `Location` on a redirect, `WWW-Authenticate` on a 401. A general
	 * header list is specified in `docs/WEB.md`; one slot is what the
	 * handlers that exist today actually use, and a list nobody fills is
	 * a list that goes untested. */
	const char *extra_name;
	const char *extra_value;
};

/*
 * A handler.
 *
 * `body` is the request body, already read in full and `body_len` long, or
 * NULL when there was none. It is **not** NUL-terminated: a form field may
 * legally contain any byte, and a handler that treats it as a C string has
 * accepted a truncation the client chose.
 *
 * Returns HTTP_OK having filled `out`, or a negative verdict from `http.h`,
 * which the server turns into the matching status. A handler that cannot
 * answer should say so rather than filling in a 200 with an error page --
 * the status is what a monitoring system reads.
 */
typedef int (*http_handler)(const struct http_request *request,
                            const char *body, size_t body_len,
                            struct http_response *out, void *ctx);

struct http_route {
	const char  *method;	/* NULL matches any method */
	const char  *prefix;	/* matched against the normalised path */
	int          exact;	/* prefix must be the whole path */
	http_handler handler;
};

struct http_site {
	const struct http_route *routes;
	size_t                   route_count;
	void                    *ctx;
	const char              *server_name;	/* for the Server header */
};

/* Open a listening socket on `port`, bound to every address.
 *
 * Returns the descriptor, or a negative number. The caller owns it and closes
 * it with `close`, because a socket is a `struct file` on this system and
 * needs no special call. */
int http_listen(unsigned port);

/*
 * Accept at most one connection and serve it to completion.
 *
 * Returns 1 when a connection was served, 0 when nobody was waiting, and a
 * negative number when the listener itself failed.
 *
 * **Zero is the common answer and is not an error.** `accept` on this kernel
 * does not block -- there is no wait queue on a listener -- so a caller polls.
 * That is documented in `userland/include/sys/socket.h` and is the reason this
 * is spelled as "once" rather than as a loop that owns the process.
 */
int http_serve_once(int listener, const struct http_site *site);

/* Poll `listener` forever, serving what arrives. Returns only on a listener
 * failure, because there is nothing else it could mean. */
int http_serve_forever(int listener, const struct http_site *site);

/* Fill `out` with a plain response. Used by the built-in error pages and by
 * handlers that have nothing more to say. */
void http_response_simple(struct http_response *out, int status,
                          const char *content_type,
                          const char *body, size_t body_len);

#endif
