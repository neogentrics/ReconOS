/*
 * HTTP, for the administrative console.
 *
 * `GUI_Server_Operating_System_Architecture.docx` section 5 says what this is
 * for: *hosts internal administrative web applications, REST APIs, and SSL/TLS
 * termination*. So this is not a file server that happens to speak HTTP. It is
 * the transport the Server Manager Dashboard arrives over, and the machine it
 * administers is the machine it runs on.
 *
 * TLS is not here and is not pretended at. There is no certificate store and
 * no TLS implementation in this system yet; a server that answered on 443 in
 * cleartext would be worse than one that does not answer at all, because the
 * port is a claim. `docs/SERVER.md` carries the row.
 *
 * --- Why the parser is its own file, with no sockets in it ---
 *
 * Because parsing is the part that faces an attacker, and it is the part that
 * can be tested exhaustively without a network. `request.c` takes a buffer and
 * returns a verdict. Every hostile case in `server/tests/test_http.c` is a
 * string literal, so the suite runs anywhere, in milliseconds, with no
 * listener and no timing.
 *
 * The socket loop in `serve.c` is the part that cannot be tested that way, so
 * it is kept as small as it can be and holds no parsing at all.
 *
 * --- The rule this file follows ---
 *
 * **Refuse rather than guess, and refuse rather than normalise.** The largest
 * class of HTTP vulnerability is two implementations reading one request two
 * different ways -- a proxy sees one request where the origin sees two. Every
 * ambiguity below is therefore answered by rejecting the message, never by
 * picking the more likely reading. A request this parser accepts has exactly
 * one interpretation.
 */

#ifndef RECON_HTTP_H
#define RECON_HTTP_H

#include <stddef.h>

/* Bounds. Each is a refusal, never a truncation -- a truncated target is a
 * request for a different resource, and a truncated header is a different
 * header. */
#define HTTP_METHOD_MAX        16
#define HTTP_TARGET_MAX       512
#define HTTP_QUERY_MAX        512
#define HTTP_HEADERS_MAX       32
#define HTTP_NAME_MAX          64
#define HTTP_VALUE_MAX        512
#define HTTP_REQUEST_MAX     8192	/* the whole head, request line included */
/*
 * 64 KiB. A console form, not an upload -- and **larger than this server can
 * actually receive today.**
 *
 * The limit that bites first is time, not size. A body sent as one burst
 * arrives at roughly a kilobyte a second on this kernel (measured: 60000 bytes
 * in 64.5 seconds), so `RECV_DEADLINE_MS` in `serve.c` cuts a request off long
 * before this bound is reached. A body paced by its sender arrives at full
 * speed and the bound is real again.
 *
 * Both numbers are kept rather than reconciled, because they measure different
 * things: this is what the buffer holds, and the deadline is what the kernel
 * can deliver in time. They will agree again when `docs/KERNEL-WANTS.md` gets
 * its answer.
 */
#define HTTP_BODY_MAX        65536

/* What one connection holds: the head, plus the largest body that head may
 * frame. Stated as the sum so that the two cannot drift -- a declared body
 * limit larger than the buffer that receives it is a limit that lies, and the
 * requests between the two numbers are refused with the wrong status. */
#define HTTP_CONN_BUF  (HTTP_REQUEST_MAX + HTTP_BODY_MAX + 1)

/* A verdict. Zero is a request that has exactly one meaning.
 *
 * `HTTP_PARTIAL` is not a failure: it means the head has not arrived in full
 * and the caller should read more. It is distinguished from every error so a
 * slow client is never answered with a refusal it did not earn. */
#define HTTP_OK              0
#define HTTP_PARTIAL       (-1)
#define HTTP_EMALFORMED    (-2)	/* syntax; 400 */
#define HTTP_ELINE_LONG    (-3)	/* request line over bounds; 414 */
#define HTTP_EFIELD_LONG   (-4)	/* a header over bounds; 431 */
#define HTTP_ETOOMANY      (-5)	/* more headers than HTTP_HEADERS_MAX; 431 */
#define HTTP_EMETHOD       (-6)	/* a method this server does not implement; 501 */
#define HTTP_EVERSION      (-7)	/* not HTTP/1.0 or HTTP/1.1; 505 */
#define HTTP_ESMUGGLE      (-8)	/* framed two ways at once; 400 */
#define HTTP_ETRAVERSAL    (-9)	/* the path escapes the root; 400 */
#define HTTP_EBODY_LONG   (-10)	/* Content-Length over HTTP_BODY_MAX; 413 */

/*
 * The server could not build its own answer: a value that would not fit the
 * buffer it was being written into, or text an escaper refused.
 *
 * The only code here that is not about the request, and it exists because
 * `http_status_for` answers 400 for anything it does not recognise. That
 * default is right for a request-side code nobody has mapped yet and wrong for
 * this one: a handler that ran out of room would have told the client its
 * request was bad. The client's request was fine.
 */
#define HTTP_EINTERNAL    (-11)	/* the server's own answer failed; 500 */

/*
 * Two that are somebody else's fault, and are deliberately not 500.
 *
 * A proxy whose upstream is unreachable has not failed -- it has correctly
 * reported that a machine it depends on is not answering, which is a different
 * thing for whoever reads the log and a different thing for whoever is paged.
 * 500 would say this server is broken. It is not.
 */
#define HTTP_EUPSTREAM    (-12)	/* could not reach it, or it spoke nonsense; 502 */
#define HTTP_EUPSTREAM_SLOW (-13) /* it accepted and did not answer in time; 504 */

struct http_header {
	char name[HTTP_NAME_MAX];	/* lower-cased; field names are
					 * case-insensitive and storing them
					 * two ways is how a duplicate gets
					 * past a duplicate check */
	char value[HTTP_VALUE_MAX];
};

struct http_request {
	char   method[HTTP_METHOD_MAX];
	char   target[HTTP_TARGET_MAX];	/* percent-decoded, normalised, rooted */
	char   query[HTTP_QUERY_MAX];	/* raw, exactly as sent, no decoding */
	int    minor;			/* 0 or 1, from HTTP/1.x */
	int    keep_alive;
	struct http_header headers[HTTP_HEADERS_MAX];
	size_t header_count;
	unsigned long content_length;
	int    has_length;

	/*
	 * The body is framed by `Transfer-Encoding: chunked` rather than by a
	 * length. `has_length` is then zero and `content_length` means nothing
	 * until the body has been decoded, because a chunked body's length is
	 * not knowable from its head -- which is the whole reason the framing
	 * exists.
	 *
	 * The two are never both set. A message that framed itself twice was
	 * refused before this field was reached: see `HTTP_ESMUGGLE`.
	 */
	int    chunked;
	size_t head_length;		/* bytes consumed, body starts here */
};

/*
 * Parse one request head out of `buf`.
 *
 * Returns HTTP_OK and fills `into`; or HTTP_PARTIAL if the terminating blank
 * line has not arrived; or a refusal. `len` is how many bytes are valid, and
 * the buffer need not be terminated -- a NUL inside the head is itself a
 * refusal, so this never reads past `len`.
 */
int http_request_parse(const char *buf, size_t len, struct http_request *into);

/* The status a refusal should be answered with. HTTP_OK and HTTP_PARTIAL have
 * no status and return 0, which a caller must not send. */
int http_status_for(int verdict);

/*
 * Does this `Host` header name the same machine as `pattern`?
 *
 * Pure, and here rather than in `serve.c` because it is a rule about what a
 * name means rather than about dispatching on one -- and because a rule with a
 * suite is a rule somebody can check.
 *
 * `pattern` is what a site was configured with, `header` is what arrived.
 *
 * **Case-insensitive**, because host names are. **The port is ignored**: a
 * client reaching `example.com` on a non-standard port sends
 * `example.com:8080`, and it is the same machine. **A single trailing dot is
 * the root form of the same name** and matches, which is the rule `dns.h`
 * already takes for the names it encodes.
 *
 * An address in brackets keeps its colons: `[::1]:80` is the literal `[::1]`
 * on port 80, and a port-stripper that cut at the first colon would turn it
 * into `[`. That is the one shape here that is easy to get wrong and silently
 * matches nothing afterwards.
 *
 * Returns 1 on a match, 0 otherwise. A NULL or empty pattern matches nothing --
 * a site with no name is not a site that answers to every name, which would be
 * the opposite of what a caller leaving it unset meant.
 */
int http_host_matches(const char *pattern, const char *header);

/*
 * Every status this server can send, with its reason phrase.
 *
 * **One list, because two drifted.** `http_reason` used to carry its own switch
 * and the suite its own hand-written checks, and the two got out of step twice:
 * 304 went out as `304 Unknown` the first time a conditional request was
 * answered, and 206 and 416 did exactly the same a day later -- *after* a case
 * had been added to the suite for every status then known. Enumerating by hand
 * in two places is not a thing that can be done carefully enough.
 *
 * So the list is here and both read it. Adding a status without a phrase is now
 * impossible rather than merely discouraged, and the suite walks the same table
 * the function is built from.
 */
#define HTTP_STATUSES(X)                                       \
	X(100, "Continue")                                     \
	X(200, "OK")                                           \
	X(201, "Created")                                      \
	X(204, "No Content")                                   \
	X(206, "Partial Content")                              \
	X(304, "Not Modified")                                 \
	X(400, "Bad Request")                                  \
	X(401, "Unauthorized")                                 \
	X(403, "Forbidden")                                    \
	X(404, "Not Found")                                    \
	X(405, "Method Not Allowed")                           \
	X(406, "Not Acceptable")                               \
	X(408, "Request Timeout")                              \
	X(409, "Conflict")                                     \
	X(412, "Precondition Failed")                          \
	X(413, "Content Too Large")                            \
	X(414, "URI Too Long")                                 \
	X(415, "Unsupported Media Type")                       \
	X(416, "Range Not Satisfiable")                        \
	X(417, "Expectation Failed")                           \
	X(421, "Misdirected Request")                          \
	X(431, "Request Header Fields Too Large")              \
	X(500, "Internal Server Error")                        \
	X(501, "Not Implemented")                              \
	X(502, "Bad Gateway")                                  \
	X(503, "Service Unavailable")                          \
	X(507, "Insufficient Storage")                        \
	X(505, "HTTP Version Not Supported")

/* The reason phrase for a status. Never NULL: a status not in the table above
 * gives "Unknown", which is easier to find on a wire than a crash -- and is
 * what the suite looks for to prove the table covers what is sent. */
const char *http_reason(int status);

/* Look a header up by name. `name` must be lower-case. NULL when absent. */
const char *http_header_get(const struct http_request *r, const char *name);

#endif
