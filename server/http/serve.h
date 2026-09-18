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

/*
 * The largest block handed to `send` in one call.
 *
 * **This is a workaround for a kernel fault, not a tuning knob**, and it should
 * be removed when that fault is fixed rather than kept because it works.
 *
 * `tcp_write` in `kernel/core/tcp.c` copies up to 4096 bytes into the
 * connection's send buffer, transmits only the first `chunk[512]` of them, and
 * then **returns the number it buffered rather than the number it sent**. A
 * caller that handles short writes correctly is truncated anyway, because it
 * was told everything went.
 *
 * Measured on 15 September 2026 against the running machine, three responses
 * on one boot:
 *
 *     /health       declared 3     received 3
 *     /api/status   declared 207   received 207
 *     /             declared 1194  received 512
 *
 * Keeping every call at or under the kernel's own segment size means each one
 * is transmitted in full before the next is offered, so nothing is left
 * buffered and unsent. Reported to the kernel session; see `docs/SERVER.md`.
 */
#define HTTP_SEND_CHUNK 512

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

/* --- streaming --------------------------------------------------------------
 *
 * The response above is written whole: a handler fills in a body and the
 * server sends it. That is right for a page built in memory and wrong for
 * everything large -- it is why the body cap exists, why a file bigger than
 * the cap is refused rather than served, and why a feed that produces output
 * over time cannot work at all.
 *
 * A sink is the other shape. The handler is handed somewhere to write, and
 * writes as it goes.
 *
 * --- The rule that makes this safe ---
 *
 * **A declared length is a promise, and a promise this server cannot keep must
 * close the connection rather than be broken quietly.** If a handler says
 * `Content-Length: 4096` and writes 3000 bytes, the client waits for 1096 more
 * -- and on a keep-alive connection it reads the *next* response's head as the
 * tail of this body. Every request after that is answered with the wrong
 * bytes. So `http_stream_end` checks what was written against what was
 * declared, and a mismatch marks the connection unusable.
 *
 * That is the same failure request smuggling causes, arrived at from the
 * server's side instead of the client's, and it deserves the same treatment:
 * refuse rather than hope.
 */

/* Pass as `length` when the size is not known before the body is produced. */
#define HTTP_LENGTH_UNKNOWN (-1L)

/*
 * Where a streaming handler writes.
 *
 * Declared here rather than hidden so a caller can put one on the stack; the
 * fields are the server's and a handler should touch none of them. `written`
 * is readable and is occasionally the honest thing to log.
 */
/* How many headers a handler may add beyond the ones the server writes. Four
 * covers a validator, a cache directive, a location and one spare; a handler
 * needing more is a handler that wants a general header list, which
 * `docs/WEB.md` specifies and nothing yet needs. */
#define HTTP_SINK_EXTRA_MAX 4

struct http_sink {
	int  fd;
	int  chunked;		/* framing chosen when the head was written */
	int  head_only;		/* a HEAD: count bytes, send none */
	int  failed;		/* a write failed, or a promise was broken */
	int  keep_alive;
	int  begun;
	int  minor;		/* the request's HTTP/1.x */
	long declared;		/* HTTP_LENGTH_UNKNOWN, or the promised length */
	int  status;		/* what `http_stream_begin` was told to send */
	unsigned long  written;
	unsigned long *bytes_sent;
	const char    *server_name;

	/* Set by `http_stream_header` before the head is written. The strings
	 * are borrowed, not copied, so they must outlive `http_stream_begin` --
	 * which in practice means a literal or a buffer the handler owns. */
	struct {
		const char *name;
		const char *value;
	} extra[HTTP_SINK_EXTRA_MAX];
	size_t extras;
};

/*
 * Add one header to the head that has not been written yet.
 *
 * Must be called **before** `http_stream_begin`; afterwards the head is on the
 * wire and a header added then would arrive in the body, which is a corrupt
 * response rather than a missing header. Calling it late is refused for that
 * reason rather than ignored.
 *
 * Returns HTTP_OK, or a negative verdict when there is no room or the head has
 * already gone.
 */
int http_stream_header(struct http_sink *sink, const char *name,
                       const char *value);

/*
 * Write the head and choose the framing. Must be called exactly once, before
 * any write.
 *
 * With a `length` of zero or more, that becomes `Content-Length` and the
 * handler is held to it.
 *
 * With `HTTP_LENGTH_UNKNOWN`, the framing depends on the client: HTTP/1.1 gets
 * `Transfer-Encoding: chunked`, and HTTP/1.0 -- which has no chunked -- is
 * told the connection will close and the body is delimited by that close.
 *
 * **Chunked is refused on the way in and used on the way out, and that is not
 * a contradiction.** A request framed two ways is dangerous because two
 * *different* parsers must agree about a body neither of them wrote. A
 * response this server frames is written by this server, once, with one
 * framing chosen here.
 */
int http_stream_begin(struct http_sink *sink, int status,
                      const char *content_type, long length);

/* Write body bytes. Safe to call with zero length. After a failure every
 * further call is a no-op, so a handler's loop need not check each one -- the
 * verdict arrives from `http_stream_end`. */
int http_stream_write(struct http_sink *sink, const void *data, size_t len);

/* Finish: send the chunked terminator if one is owed, and check the promise.
 * Returns HTTP_OK, or a negative verdict when the response could not be
 * completed -- in which case the connection is closed rather than reused. */
int http_stream_end(struct http_sink *sink);

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

/*
 * A streaming handler.
 *
 * Calls `http_stream_begin` once, then `http_stream_write` as often as it
 * likes, and returns HTTP_OK. The server calls `http_stream_end`; a handler
 * that returns a negative verdict *before* beginning gets the matching status
 * instead, which is why beginning late is better than beginning early -- once
 * the head is on the wire the status cannot be taken back.
 */
typedef int (*http_stream_handler)(const struct http_request *request,
                                   const char *body, size_t body_len,
                                   struct http_sink *sink, void *ctx);

struct http_route {
	const char  *method;	/* NULL matches any method */

	/* Matched against the normalised path. A prefix match must end on a
	 * `/` boundary, so a route for `/api` never claims `/apifoo` -- a
	 * different resource with a similar spelling.
	 *
	 * **The empty string matches every path**, which is how a catch-all is
	 * written. `"/"` is *not* a catch-all: it is the boundary rule doing
	 * its job, and a route written that way matches only the root itself.
	 * Worth stating because the wrong one of those two is a site that
	 * silently serves nothing but its index. */
	const char  *prefix;

	int          exact;	/* the prefix must be the whole path */

	/* Exactly one of these. `handler` builds a whole response in memory;
	 * `stream` is handed a sink and writes as it goes. A route with both
	 * is a route whose author had not decided, and is refused at dispatch
	 * rather than resolved by a precedence rule nobody would remember. */
	http_handler        handler;
	http_stream_handler stream;

	/* This route's own context, or NULL to take the site's.
	 *
	 * Added when the first site needed two: a dashboard reading the
	 * machine's facts and a file handler reading its configuration. One
	 * context for the whole site would have meant a single structure
	 * holding both, which is two unrelated things kept together because of
	 * a limitation in the thing that calls them. */
	void        *ctx;

	/*
	 * Whether this route requires the site's permission before it runs.
	 *
	 * **The check lives in the server rather than in each handler**, for
	 * the same reason the security headers and the access log do: a check
	 * every handler must remember is a check the next handler will not do.
	 * A route is marked here once, and forgetting to mark one is a
	 * visible omission in a table rather than an invisible one inside a
	 * function.
	 *
	 * A guarded route on a site with no `allow` answers **500**, not 200.
	 * See `serve.c`: a site that asks for a guard it did not supply is
	 * misconfigured, and serving the request anyway would be the one
	 * outcome nobody wanted.
	 */
	int          guarded;
};

struct http_site {
	/*
	 * The name this site answers to, or NULL for "any".
	 *
	 * NULL is what a server with one site sets, and is the behaviour this
	 * had before virtual hosts existed: every request is this site's,
	 * whatever `Host` says.
	 *
	 * Set it, and a request whose `Host` names something else is **not**
	 * this site's -- it goes to the next in the chain, and if nothing
	 * claims it the server answers 421. A machine that answers to every
	 * name it is asked about is how a cache is poisoned and how a link in
	 * an email is made to point somewhere it should not.
	 */
	const char *host;

	/*
	 * The next site to try, or NULL.
	 *
	 * A chain rather than an array so that a server with one site is
	 * exactly what it was: `host` NULL, `next` NULL, and not one line of
	 * the dispatch behaves differently. Adding a second site is setting a
	 * pointer.
	 *
	 * Order matters and is the site author's: the first whose `host`
	 * matches wins, and a site with `host` NULL claims everything from
	 * there on -- so a default belongs last, and one placed first is a
	 * configuration that silently ignores every site after it.
	 */
	const struct http_site *next;

	const struct http_route *routes;
	size_t                   route_count;
	void                    *ctx;
	const char              *server_name;	/* for the Server header */

	/*
	 * Called once per request, after it has been answered. May be NULL.
	 *
	 * The server calls it rather than each handler, for the reason the
	 * security headers are written by the server: a log every handler must
	 * remember is a log the next handler will not write. It sees what was
	 * actually sent -- the status the client got, including the ones
	 * produced by a refusal before any handler ran.
	 *
	 * `request` may be NULL, for a request too malformed to parse. That is
	 * exactly the entry worth having, so it is passed rather than skipped.
	 */
	void (*log)(void *ctx, const struct http_request *request, int status,
	            unsigned long bytes);
	void *log_ctx;

	/* Where to count bytes actually put on the wire, or NULL.
	 *
	 * A pointer rather than a field, because the site is handed to the
	 * server as `const` -- it is configuration, and configuration that
	 * mutates is configuration two readers disagree about. The counter it
	 * points at is the caller's, and this is the only thing in here that
	 * moves.
	 *
	 * Counted at the point of sending, not from the response's declared
	 * length: a short write that was retried has still sent those bytes,
	 * and a figure taken from `body_len` would be what the server meant to
	 * send rather than what it did. */
	unsigned long           *bytes_sent;

	/*
	 * Called while waiting for bytes that have not arrived. May be NULL.
	 *
	 * --- The fault this exists for ---
	 *
	 * On ReconOS a `recv` with nothing buffered answers 0 with `errno` 0 --
	 * not an error and not a close. The obvious response is to ask again,
	 * and asking again in a tight loop is what broke: **a burst larger than
	 * about 2880 bytes stalled and never recovered**, while the same bytes
	 * paced by the sender arrived in full at twenty times the size.
	 *
	 * The size was never the limit. The spin was: a single-process server
	 * asking for bytes as fast as it can leaves nothing for the work that
	 * delivers them. Measured both ways -- see VF-013.
	 *
	 * So the site supplies whatever *this* system does to let other work
	 * run. ReconOS passes `recon_yield`. A host passes NULL, because there
	 * `recv` blocks properly and there is nothing to yield to.
	 *
	 * It lives here rather than in `serve.c` because `serve.c` is built for
	 * the host as well, and the call is the one thing in the loop that
	 * cannot be. See the note in `CMakeLists.txt` about why the suites do
	 * not get ReconOS's headers.
	 */
	void (*idle)(void);

	/*
	 * Milliseconds since some fixed point, or NULL.
	 *
	 * Only ever used for differences, so where it counts from does not
	 * matter -- it must only go forwards. ReconOS passes a wrapper over
	 * `SYS_TIME`.
	 *
	 * **Why a clock is needed at all.** Waiting for a slow request has to
	 * be bounded or one client wedges a single-process server for ever.
	 * Without a clock the only available bound is a number of attempts,
	 * which is not a duration: the same count is a moment on one machine
	 * and a minute on another, and it changes meaning again the first time
	 * the loop around it is edited.
	 *
	 * With this, the bound is `RECV_DEADLINE_MS` and means what it says.
	 * Without it, the server falls back to the attempt count, which is
	 * honest for the host suites -- there `recv` blocks and the fallback
	 * is never reached.
	 */
	unsigned long (*now_ms)(void);

	/*
	 * Called on each newly accepted socket, or NULL.
	 *
	 * **It exists so a host can behave like the target, and ReconOS passes
	 * NULL.** On ReconOS a socket never blocks: `recv` with nothing
	 * buffered answers 0 and returns. That is what lets this server hold
	 * several connections and give each a turn without `poll`, which does
	 * not exist here.
	 *
	 * A host socket blocks, so a connection that has gone quiet stops the
	 * whole loop -- including the other connections, which is the exact
	 * property the pool exists to provide. The host suites pass a function
	 * that sets `O_NONBLOCK`.
	 *
	 * `serve.c` cannot do it itself: `fcntl` is not available on ReconOS,
	 * and this file is compiled unchanged for both. The hook keeps the one
	 * call that differs on the side that knows which system it is.
	 */
	void (*unblock)(int fd);

	/*
	 * May this request run a route marked `guarded`? May be NULL.
	 *
	 * The policy is the site's -- this server knows only that some routes
	 * need permission and that it must ask. `server_init.c` answers with
	 * the boot token in `server/auth.c`.
	 *
	 * Returns non-zero to allow. A refusal is answered 401 with a
	 * `WWW-Authenticate` header, and logged, so a locked-out client is an
	 * entry somebody can find rather than a silence.
	 */
	int (*allow)(const struct http_request *r, const char *body,
	             size_t body_len, void *ctx);
	void *allow_ctx;
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
