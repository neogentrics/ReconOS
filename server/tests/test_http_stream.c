/*
 * Streaming a response, and the promise that must not be broken.
 *
 * The ordinary cases here -- a known length written in pieces, an unknown
 * length framed as chunked -- are the easy half. **The half worth the suite is
 * what happens when a handler does not write what it said it would.**
 *
 * A handler that declares `Content-Length: 4096` and writes 3000 bytes leaves
 * the client waiting for 1096 more. On a keep-alive connection the client then
 * reads the *next* response's head as the tail of this body, and every answer
 * after that is wrong -- the same desynchronisation request smuggling causes,
 * arrived at from the server's own side. So the checks below are not only that
 * the body is right; they are that a broken promise **closes the connection**
 * rather than being papered over.
 *
 * Runs `serve.c` unmodified over the host's loopback, as the other serving
 * suite does. A child serves, the parent is the client.
 */

#include "../http/serve.h"
#include "../http/deflate.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/time.h>

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL  %s\n", what);
	}
}

/* --- the handlers under test ---------------------------------------------- */

#define BIG_LEN (HTTP_RESPONSE_MAX + 4096)	/* past what a whole response can hold */

static char BIG[BIG_LEN];

/* A known length, written in many small pieces. The point is that the client
 * cannot tell how it was produced. */
static int stream_known(const struct http_request *r, const char *body,
                        size_t body_len, struct http_sink *sink, void *ctx)
{
	size_t at = 0;

	(void)r; (void)body; (void)body_len; (void)ctx;

	if (http_stream_begin(sink, 200, "text/plain", (long)BIG_LEN) != HTTP_OK)
		return HTTP_EMALFORMED;

	while (at < BIG_LEN) {
		size_t n = BIG_LEN - at;

		if (n > 700)
			n = 700;	/* deliberately not a round number */
		if (http_stream_write(sink, BIG + at, n) != HTTP_OK)
			return HTTP_EMALFORMED;
		at += n;
	}
	return HTTP_OK;
}

/* No length known in advance -- the shape a feed or a generated report has. */
static int stream_unknown(const struct http_request *r, const char *body,
                          size_t body_len, struct http_sink *sink, void *ctx)
{
	int i;

	(void)r; (void)body; (void)body_len; (void)ctx;

	if (http_stream_begin(sink, 200, "text/plain", HTTP_LENGTH_UNKNOWN)
	    != HTTP_OK)
		return HTTP_EMALFORMED;

	for (i = 0; i < 5; i++) {
		char piece[32];
		int n = snprintf(piece, sizeof(piece), "piece-%d\n", i);

		if (http_stream_write(sink, piece, (size_t)n) != HTTP_OK)
			return HTTP_EMALFORMED;
	}
	return HTTP_OK;
}

/* Promises 1000 and writes 400. The fault this suite exists for. */
static int stream_short(const struct http_request *r, const char *body,
                        size_t body_len, struct http_sink *sink, void *ctx)
{
	static char pad[400];

	(void)r; (void)body; (void)body_len; (void)ctx;
	memset(pad, 'x', sizeof(pad));

	if (http_stream_begin(sink, 200, "text/plain", 1000) != HTTP_OK)
		return HTTP_EMALFORMED;
	http_stream_write(sink, pad, sizeof(pad));
	return HTTP_OK;
}

/* Promises 100 and tries to write 200. Must be refused at the write. */
static int stream_over(const struct http_request *r, const char *body,
                       size_t body_len, struct http_sink *sink, void *ctx)
{
	static char pad[200];
	int rc;

	(void)r; (void)body; (void)body_len; (void)ctx;
	memset(pad, 'y', sizeof(pad));

	if (http_stream_begin(sink, 200, "text/plain", 100) != HTTP_OK)
		return HTTP_EMALFORMED;

	rc = http_stream_write(sink, pad, sizeof(pad));
	/* Reported back so the parent can check the refusal happened here,
	 * rather than only that the connection died. */
	return rc == HTTP_OK ? HTTP_OK : HTTP_EMALFORMED;
}

/* Refuses before writing anything, so a status can still be sent honestly. */
static int stream_refuses(const struct http_request *r, const char *body,
                          size_t body_len, struct http_sink *sink, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)sink; (void)ctx;
	return HTTP_EBODY_LONG;		/* 413 */
}

/* --- compression on the streaming path ------------------------------------
 *
 * Files are served by this path, and they are the largest bodies this server
 * sends, so from 0.37.0 the streaming path compresses too. What follows is not
 * a test of the compressor -- `test_deflate.c` does that against an
 * independent inflater, and `scripts/gzip-probe.py` against zlib. These are
 * tests of the **decision**: when a response is compressed, when it must not
 * be, and whether the stream that comes out is whole.
 */

#define TEXT_LEN 2000
static char TEXT[TEXT_LEN];

/* Compressible, above the threshold, and asked for with gzip. The yes case. */
static int stream_text(const struct http_request *r, const char *body,
                       size_t body_len, struct http_sink *sink, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;

	if (http_stream_begin(sink, 200, "text/plain", (long)TEXT_LEN)
	    != HTTP_OK)
		return HTTP_EMALFORMED;
	if (http_stream_write(sink, TEXT, TEXT_LEN) != HTTP_OK)
		return HTTP_EMALFORMED;
	return HTTP_OK;
}

/*
 * The same bytes, dripped out sixty-four at a time, with no length declared.
 *
 * This is the shape a real streaming handler has, and it is the one that finds
 * the bug `/text` cannot. A compressor holds bytes back while it looks for a
 * match that might continue into input it has not been handed yet, so a write
 * smaller than the longest match **produces no output at all** -- and a
 * zero-length write, framed as a chunk, is the chunked terminator. The body
 * would end in the middle of itself.
 *
 * `/text` hands over two thousand bytes in one call and never produces a
 * zero-length piece, so it cannot catch that. This does.
 */
static int stream_drip(const struct http_request *r, const char *body,
                       size_t body_len, struct http_sink *sink, void *ctx)
{
	size_t at = 0;

	(void)r; (void)body; (void)body_len; (void)ctx;

	if (http_stream_begin(sink, 200, "text/plain", HTTP_LENGTH_UNKNOWN)
	    != HTTP_OK)
		return HTTP_EMALFORMED;

	while (at < TEXT_LEN) {
		size_t n = TEXT_LEN - at;

		if (n > 64)
			n = 64;
		if (http_stream_write(sink, TEXT + at, n) != HTTP_OK)
			return HTTP_EMALFORMED;
		at += n;
	}
	return HTTP_OK;
}

/*
 * A range of the same resource.
 *
 * The case the guard exists for. `Content-Range: bytes 0-999/2000` counts the
 * bytes of the *resource*; compressing the body would leave those numbers
 * describing something the client was never sent, and there is no honest way
 * to restate them -- the compressed size of a range is not a range of anything.
 */
static int stream_partial(const struct http_request *r, const char *body,
                          size_t body_len, struct http_sink *sink, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;

	http_stream_header(sink, "Content-Range", "bytes 0-999/2000");
	if (http_stream_begin(sink, 206, "text/plain", 1000) != HTTP_OK)
		return HTTP_EMALFORMED;
	if (http_stream_write(sink, TEXT, 1000) != HTTP_OK)
		return HTTP_EMALFORMED;
	return HTTP_OK;
}

/* Already compressed, as far as the type is concerned. */
static int stream_binary(const struct http_request *r, const char *body,
                         size_t body_len, struct http_sink *sink, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;

	if (http_stream_begin(sink, 200, "application/octet-stream",
	                      (long)TEXT_LEN) != HTTP_OK)
		return HTTP_EMALFORMED;
	if (http_stream_write(sink, TEXT, TEXT_LEN) != HTTP_OK)
		return HTTP_EMALFORMED;
	return HTTP_OK;
}

/* Under the threshold, where framing costs more than compression saves. */
static int stream_tiny(const struct http_request *r, const char *body,
                       size_t body_len, struct http_sink *sink, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;

	if (http_stream_begin(sink, 200, "text/plain", 100) != HTTP_OK)
		return HTTP_EMALFORMED;
	if (http_stream_write(sink, TEXT, 100) != HTTP_OK)
		return HTTP_EMALFORMED;
	return HTTP_OK;
}

/* A route that is both, which is a site with a bug in it. */
static int both_handler(const struct http_request *r, const char *body,
                        size_t body_len, struct http_response *out, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;
	http_response_simple(out, 200, "text/plain", "no\n", 3);
	return HTTP_OK;
}

static const struct http_route ROUTES[] = {
	{ .method = "GET", .prefix = "/known", .exact = 1, .stream = stream_known },
	{ .method = "GET", .prefix = "/unknown",
	  .exact = 1, .stream = stream_unknown },
	{ .method = "GET", .prefix = "/short", .exact = 1, .stream = stream_short },
	{ .method = "GET", .prefix = "/over", .exact = 1, .stream = stream_over },
	{ .method = "GET", .prefix = "/refuses",
	  .exact = 1, .stream = stream_refuses },
	{ .method = "GET", .prefix = "/both",
	  .exact = 1, .handler = both_handler, .stream = stream_known },
	{ .method = "GET", .prefix = "/text", .exact = 1, .stream = stream_text },
	{ .method = "GET", .prefix = "/partial",
	  .exact = 1, .stream = stream_partial },
	{ .method = "GET", .prefix = "/binary",
	  .exact = 1, .stream = stream_binary },
	{ .method = "GET", .prefix = "/tiny", .exact = 1, .stream = stream_tiny },
	{ .method = "GET", .prefix = "/drip", .exact = 1, .stream = stream_drip },
};

static unsigned long *BYTES;

/* What a host must do to a fresh socket so it behaves like one on the target.
 * ReconOS passes NULL here: its sockets never block. See `serve.h`. */
static void unblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static struct http_site SITE = {
	/* `idle` and `now_ms` are zero: on a host there is nothing to yield
	 * to. `unblock` is not -- the server holds several connections at
	 * once and a blocking socket would stop the loop on whichever one
	 * went quiet. See `serve.h`. */
	/*
	 * Named rather than positional.
	 *
	 * Every field added to `struct http_site` used to break all three of
	 * these at once -- `idle`, then `now_ms`, then `unblock`, then `host`
	 * and `next` -- each time a compiler error in a test that had nothing
	 * to do with the change. Naming them means a new field is simply
	 * absent here, which is what a test that does not care about it should
	 * say.
	 */
	.routes = ROUTES,
	.route_count = sizeof(ROUTES) / sizeof(ROUTES[0]),
	.server_name = "ReconOS/stream",
	.unblock = unblock
};

/* --- the client ------------------------------------------------------------ */

static int dial(unsigned port)
{
	struct sockaddr_in addr;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(0x7F000001);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}

	/*
	 * A deadline on reading, and it is load-bearing.
	 *
	 * The fault this suite exists to catch -- a declared length the server
	 * does not deliver -- presents to a client as *waiting forever*. Run
	 * against a server with the promise check removed, a parent with no
	 * deadline simply hangs, and the suite that was supposed to report the
	 * bug instead becomes the bug: a suite that can hang is a suite that
	 * gets disabled, and then it reports nothing at all.
	 *
	 * Two seconds is far longer than loopback needs and far shorter than a
	 * person waits.
	 */
	{
		struct timeval tv;

		tv.tv_sec = 2;
		tv.tv_usec = 0;
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	return fd;
}

static size_t slurp(int fd, char *into, size_t room)
{
	size_t have = 0;

	for (;;) {
		long n = recv(fd, into + have, room - 1 - have, 0);

		if (n > 0) {
			have += (size_t)n;
			if (have >= room - 1)
				break;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		break;		/* closed, failed, or the deadline passed */
	}
	into[have] = '\0';
	return have;
}

/*
 * Is `name` a header of this reply?
 *
 * Searching the whole reply with `strstr` is the trap this project has walked
 * into twice. Once on the name: `X-Content-Type-Options` contains
 * `Content-Type`, so a check for the latter passed on a response that had only
 * the former. And now once on the body: a **compressed body is arbitrary
 * bytes**, so `strstr(reply, "Content-Encoding")` can match inside the body of
 * a response whose head says no such thing -- which is the exact check these
 * tests turn on, passing for the wrong reason.
 *
 * So: only before the blank line, only at the start of a line, and only when
 * followed by a colon.
 */
static int head_has(const char *reply, const char *name)
{
	const char *end = strstr(reply, "\r\n\r\n");
	const char *at = reply;
	size_t n = strlen(name);

	if (!end)
		return 0;
	while ((at = strstr(at, name)) != 0 && at < end) {
		if (at > reply && at[-1] == '\n' && at[n] == ':')
			return 1;
		at += n;
	}
	return 0;
}

/* Four bytes, little-endian, which is how gzip writes its trailer. */
static unsigned long le32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
	     | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static size_t fetch(unsigned port, const char *request, char *into, size_t room)
{
	int fd = dial(port);
	size_t n;

	if (fd < 0) {
		into[0] = '\0';
		return 0;
	}
	send(fd, request, strlen(request), 0);
	n = slurp(fd, into, room);
	close(fd);
	return n;
}

/* Where the head ends and the body begins, or NULL. */
static const char *body_of(const char *reply)
{
	const char *at = strstr(reply, "\r\n\r\n");

	return at ? at + 4 : 0;
}

/* Decode a chunked body into `out`, returning its length, or -1 if the
 * framing is wrong. Written out rather than trusted, because "the client
 * accepted it" is not the same claim as "the framing is right". */
static long dechunk(const char *in, size_t len, char *out, size_t room)
{
	size_t at = 0, o = 0;

	for (;;) {
		unsigned long size = 0;
		int digits = 0;

		while (at < len && in[at] != '\r') {
			int d;

			if (in[at] >= '0' && in[at] <= '9')
				d = in[at] - '0';
			else if (in[at] >= 'a' && in[at] <= 'f')
				d = in[at] - 'a' + 10;
			else if (in[at] >= 'A' && in[at] <= 'F')
				d = in[at] - 'A' + 10;
			else
				return -1;
			size = size * 16 + (unsigned long)d;
			digits++;
			at++;
		}
		if (!digits || at + 2 > len || in[at] != '\r' || in[at + 1] != '\n')
			return -1;
		at += 2;

		if (size == 0)
			break;
		if (at + size + 2 > len || o + size > room)
			return -1;
		memcpy(out + o, in + at, size);
		o += size;
		at += size;
		if (in[at] != '\r' || in[at + 1] != '\n')
			return -1;
		at += 2;
	}
	return (long)o;
}

int main(void)
{
	unsigned port = 18300;
	int listener = -1;
	pid_t child;
	static char reply[BIG_LEN + 8192];
	static char decoded[8192];
	size_t i;

	printf("streaming a response, and the promise that must not be broken\n");

	for (i = 0; i < BIG_LEN; i++)
		BIG[i] = (char)('A' + (i % 26));

	/*
	 * Something worth compressing, and deliberately not a run of one byte.
	 *
	 * Zeros -- which is what this array was until somebody noticed -- are
	 * compressed by a single long match, so a stream that lost every match
	 * but the first would still come out small and the size check would
	 * still pass. Repeating words give the matcher something it has to
	 * actually find, and they are closer to what this server sends.
	 */
	{
		static const char *const W[] = {
			"the ", "server ", "answers ", "a ", "request ",
			"with ", "bytes ", "and ", "the ", "bytes "
		};
		size_t at = 0;
		size_t k = 0;

		while (at < TEXT_LEN) {
			const char *w = W[k++ % (sizeof(W) / sizeof(W[0]))];
			size_t n = strlen(w);

			if (at + n > TEXT_LEN)
				n = TEXT_LEN - at;
			memcpy(TEXT + at, w, n);
			at += n;
		}
	}

	BYTES = mmap(0, sizeof(*BYTES), PROT_READ | PROT_WRITE,
	             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (BYTES == MAP_FAILED) {
		printf("  FAIL  no shared page\n");
		return 1;
	}
	*BYTES = 0;
	SITE.bytes_sent = BYTES;

	{
		int tries = 0;

		while (tries < 32 && (listener = http_listen(port)) < 0) {
			port++;
			tries++;
		}
	}
	if (listener < 0) {
		printf("  FAIL  no free port\n");
		return 1;
	}


	/*
	 * Make `accept` non-blocking, the way it already is on the kernel this
	 * code targets.
	 *
	 * `serve.c` holds several connections at once and gives each a turn, so
	 * it must be able to ask "is anyone new waiting?" and be told no. On
	 * ReconOS there is no wait queue on a listener and `accept` answers
	 * `EAGAIN` by itself; the host is the odd one out, and a blocking
	 * `accept` there stops the whole loop -- including the connections
	 * already in flight that are the only reason it would ever return.
	 *
	 * So the suite makes the host behave like the target. That is more
	 * faithful than testing against a listener the real system does not
	 * have, and `fcntl` is used here, in host-only test code, precisely
	 * because `serve.c` cannot use it.
	 */
	fcntl(listener, F_SETFL, fcntl(listener, F_GETFL, 0) | O_NONBLOCK);

	child = fork();
	if (child < 0) {
		printf("  FAIL  fork\n");
		return 1;
	}
	if (child == 0) {
		int served = 0;

		alarm(30);	/* an orphan that answers is worse than one
				 * that hangs -- see test_http_serve.c */
		/* Not a connection count any more -- see test_http_serve.c. */
		(void)served;
		for (;;) {
			int rc = http_serve_once(listener, &SITE);

			if (rc < 0)
				break;
			if (rc == 0)
				usleep(200);
		}
		close(listener);
		_exit(0);
	}

	/* --- a known length, past what a whole response could hold ----------- */
	{
		size_t n = fetch(port,
		                 "GET /known HTTP/1.1\r\nHost: m\r\n"
		                 "Connection: close\r\n\r\n",
		                 reply, sizeof(reply));
		const char *b = body_of(reply);
		char want[64];

		snprintf(want, sizeof(want), "Content-Length: %d\r\n", BIG_LEN);

		ok(strncmp(reply, "HTTP/1.1 200 OK\r\n", 17) == 0,
		   "a streamed response answers 200");
		ok(strstr(reply, want) != 0,
		   "the declared length is the one asked for");
		ok(strstr(reply, "Transfer-Encoding") == 0,
		   "a known length is not chunked");
		ok(b && (size_t)(b - reply) + BIG_LEN == n,
		   "exactly the declared number of body bytes arrived");
		ok(b && memcmp(b, BIG, BIG_LEN) == 0,
		   "the body is the bytes the handler wrote, in order");
		ok(BIG_LEN > HTTP_RESPONSE_MAX,
		   "and it is larger than a whole response can carry");
	}

	/* --- an unknown length, framed as chunked ---------------------------- */
	{
		size_t n = fetch(port,
		                 "GET /unknown HTTP/1.1\r\nHost: m\r\n"
		                 "Connection: close\r\n\r\n",
		                 reply, sizeof(reply));
		const char *b = body_of(reply);
		long len = -1;

		ok(strstr(reply, "Transfer-Encoding: chunked\r\n") != 0,
		   "an unknown length is chunked for a 1.1 client");
		ok(strstr(reply, "Content-Length:") == 0,
		   "and carries no Content-Length, which would contradict it");
		if (b)
			len = dechunk(b, n - (size_t)(b - reply), decoded,
			              sizeof(decoded));
		ok(len > 0, "the chunked framing decodes");
		ok(len == 40 && memcmp(decoded,
		                       "piece-0\npiece-1\npiece-2\npiece-3\npiece-4\n",
		                       40) == 0,
		   "and yields exactly what the handler wrote");
	}

	/* --- an unknown length to a 1.0 client, which has no chunked --------- */
	{
		size_t n = fetch(port,
		                 "GET /unknown HTTP/1.0\r\n\r\n",
		                 reply, sizeof(reply));
		const char *b = body_of(reply);

		ok(strstr(reply, "Transfer-Encoding") == 0,
		   "a 1.0 client is never sent chunked");
		ok(strstr(reply, "Connection: close\r\n") != 0,
		   "it is told the close delimits the body");
		ok(b && (size_t)(n - (size_t)(b - reply)) == 40
		   && memcmp(b, "piece-0\npiece-1\npiece-2\npiece-3\npiece-4\n",
		             40) == 0,
		   "and gets the bytes raw");
	}

	/* --- HEAD: the length, and none of the body -------------------------- */
	{
		size_t n = fetch(port,
		                 "HEAD /known HTTP/1.1\r\nHost: m\r\n"
		                 "Connection: close\r\n\r\n",
		                 reply, sizeof(reply));
		const char *b = body_of(reply);
		char want[64];

		snprintf(want, sizeof(want), "Content-Length: %d\r\n", BIG_LEN);
		ok(strstr(reply, want) != 0,
		   "HEAD reports the length it would have sent");
		ok(b && (size_t)(b - reply) == n,
		   "and sends no body at all");
	}

	/* --- the promise, broken short --------------------------------------
	 *
	 * Two requests on one connection. The first declares 1000 and writes
	 * 400. If the server carried on, the client would read the second
	 * response's head as the tail of the first body. The connection must
	 * close instead, so the second request is never answered. */
	{
		int fd = dial(port);
		char buf[4096];
		size_t n = 0;

		ok(fd >= 0, "a connection for the short-write case");
		if (fd >= 0) {
			const char *one = "GET /short HTTP/1.1\r\nHost: m\r\n\r\n";
			const char *two = "GET /unknown HTTP/1.1\r\nHost: m\r\n"
			                  "Connection: close\r\n\r\n";

			send(fd, one, strlen(one), 0);
			send(fd, two, strlen(two), 0);
			n = slurp(fd, buf, sizeof(buf));
			close(fd);

			ok(strstr(buf, "Content-Length: 1000\r\n") != 0,
			   "the short response declared 1000");
			ok(strstr(buf, "piece-0") == 0,
			   "the next request was NOT answered on that"
			   " connection -- a broken promise closes it");
			ok(n < 1000 + 200,
			   "and no more than the short body arrived");
		}
	}

	/* --- the promise, broken long ---------------------------------------- */
	{
		size_t n = fetch(port, "GET /over HTTP/1.1\r\nHost: m\r\n\r\n",
		                 reply, sizeof(reply));
		const char *b = body_of(reply);
		size_t body = b ? n - (size_t)(b - reply) : 0;

		ok(strstr(reply, "Content-Length: 100\r\n") != 0,
		   "the over-writing response declared 100");
		ok(body <= 100,
		   "and not one byte past the promise reached the client");
	}

	/* --- a refusal before anything is written ---------------------------- */
	{
		fetch(port, "GET /refuses HTTP/1.1\r\nHost: m\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strncmp(reply, "HTTP/1.1 413", 12) == 0,
		   "a stream that refuses before beginning still sends a status");
	}

	/* --- a route that is both, which is a bug in the site ---------------- */
	{
		fetch(port, "GET /both HTTP/1.1\r\nHost: m\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strncmp(reply, "HTTP/1.1 500", 12) == 0,
		   "a route with both a handler and a stream answers 500");
	}

	/* --- compressed, and not ---------------------------------------------- */
	{
		static char body[TEXT_LEN * 2];
		const char *raw;
		size_t got;
		long n;

		got = fetch(port, "GET /text HTTP/1.1\r\nHost: m\r\n"
		                  "Accept-Encoding: gzip\r\n"
		                  "Connection: close\r\n\r\n",
		            reply, sizeof(reply));

		ok(head_has(reply, "Content-Encoding"),
		   "a compressible stream asked for with gzip is compressed");
		ok(head_has(reply, "Transfer-Encoding"),
		   "and is framed chunked, because its length is no longer known");
		ok(!head_has(reply, "Content-Length"),
		   "and carries no Content-Length, which would now be the wrong "
		   "number");
		ok(head_has(reply, "Vary"),
		   "and tells caches the answer depends on Accept-Encoding");

		raw = body_of(reply);
		n = raw ? dechunk(raw, got - (size_t)(raw - reply),
		                  body, sizeof(body))
		        : -1;
		ok(n > 18, "the chunked framing holds together");
		ok(n > 18 && (unsigned char)body[0] == 0x1F
		   && (unsigned char)body[1] == 0x8B,
		   "and what is inside it begins with gzip's magic");
		ok(n > 0 && n < TEXT_LEN,
		   "and is smaller than what went in");

		/*
		 * The trailer, rather than inflating it here.
		 *
		 * `test_deflate.c` already decodes this compressor with an
		 * inflater written from the specification, and `gzip-probe.py`
		 * already decodes it with zlib. What is unproven at this point
		 * is not the compressor -- it is whether **this path delivered
		 * the whole stream**: whether `http_stream_end` flushed the
		 * bytes the compressor was still holding, and whether it did so
		 * before the chunked terminator rather than after it.
		 *
		 * gzip's trailer answers exactly that. It is written last, it
		 * covers every input byte, and a stream that ended early has
		 * either no trailer or the wrong one.
		 */
		if (n >= 8) {
			const unsigned char *tail =
				(const unsigned char *)body + n - 8;

			ok(le32(tail) == deflate_crc32(TEXT, TEXT_LEN),
			   "the trailer's checksum covers every byte written");
			ok(le32(tail + 4) == (unsigned long)TEXT_LEN,
			   "and its length is the whole body's, not a chunk's");
		} else {
			ok(0, "the trailer's checksum covers every byte written");
			ok(0, "and its length is the whole body's, not a chunk's");
		}
	}

	/* The same bytes, written in pieces too small to compress on their own. */
	{
		static char body[TEXT_LEN * 2];
		const char *raw;
		size_t got;
		long n;

		got = fetch(port, "GET /drip HTTP/1.1\r\nHost: m\r\n"
		                  "Accept-Encoding: gzip\r\n"
		                  "Connection: close\r\n\r\n",
		            reply, sizeof(reply));

		ok(head_has(reply, "Content-Encoding"),
		   "a body of unknown length is compressed too -- there is no "
		   "length to weigh the framing against, and it is the case "
		   "most worth compressing");

		raw = body_of(reply);
		n = raw ? dechunk(raw, got - (size_t)(raw - reply),
		                  body, sizeof(body))
		        : -1;
		ok(n > 18, "and its framing survives writes that compress to "
		           "nothing at all");

		if (n >= 8) {
			const unsigned char *tail =
				(const unsigned char *)body + n - 8;

			ok(le32(tail) == deflate_crc32(TEXT, TEXT_LEN),
			   "and every one of those writes reached the client");
			ok(le32(tail + 4) == (unsigned long)TEXT_LEN,
			   "all two thousand bytes of them");
		} else {
			ok(0, "and every one of those writes reached the client");
			ok(0, "all two thousand bytes of them");
		}
	}

	/* The same resource, not asked for compressed. */
	{
		fetch(port, "GET /text HTTP/1.1\r\nHost: m\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(!head_has(reply, "Content-Encoding"),
		   "a client that did not ask for gzip is not sent gzip");
		ok(head_has(reply, "Content-Length"),
		   "and gets the length it would have got before");
		ok(body_of(reply) && strncmp(body_of(reply), TEXT, 16) == 0,
		   "and the bytes themselves");
	}

	/* --- the three refusals ----------------------------------------------- */
	{
		fetch(port, "GET /partial HTTP/1.1\r\nHost: m\r\n"
		            "Accept-Encoding: gzip\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strncmp(reply, "HTTP/1.1 206", 12) == 0,
		   "a partial response is still a partial response");
		ok(head_has(reply, "Content-Range"),
		   "and still names the range it sent");
		ok(!head_has(reply, "Content-Encoding"),
		   "and is never compressed, because Content-Range counts the "
		   "resource's bytes and not the compressed form's");

		fetch(port, "GET /binary HTTP/1.1\r\nHost: m\r\n"
		            "Accept-Encoding: gzip\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(!head_has(reply, "Content-Encoding"),
		   "a type that is already compressed is left alone");
		ok(head_has(reply, "Content-Length"),
		   "and keeps its length");

		fetch(port, "GET /tiny HTTP/1.1\r\nHost: m\r\n"
		            "Accept-Encoding: gzip\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(!head_has(reply, "Content-Encoding"),
		   "a body below the threshold is left alone, because framing "
		   "would cost more than compression saves");
	}

	/* --- a HEAD of a compressed resource ---------------------------------- */
	{
		size_t got = fetch(port, "HEAD /text HTTP/1.1\r\nHost: m\r\n"
		                         "Accept-Encoding: gzip\r\n"
		                         "Connection: close\r\n\r\n",
		                   reply, sizeof(reply));
		const char *end = strstr(reply, "\r\n\r\n");

		ok(head_has(reply, "Content-Encoding"),
		   "a HEAD reports the encoding the GET would have used");
		ok(end && got == (size_t)(end - reply) + 4,
		   "and sends no body at all, compressed or otherwise");
	}

	kill(child, SIGTERM);
	waitpid(child, 0, 0);
	close(listener);

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
