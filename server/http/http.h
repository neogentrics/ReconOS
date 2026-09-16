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
#define HTTP_BODY_MAX        65536	/* 64 KiB; a console form, not an upload */

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

/* The reason phrase for a status this server sends. Never NULL: an unknown
 * status is a bug in the caller, and "Unknown" on the wire is easier to find
 * than a crash. */
const char *http_reason(int status);

/* Look a header up by name. `name` must be lower-case. NULL when absent. */
const char *http_header_get(const struct http_request *r, const char *name);

#endif
