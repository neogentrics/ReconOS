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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mman.h>
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

/* A route that is both, which is a site with a bug in it. */
static int both_handler(const struct http_request *r, const char *body,
                        size_t body_len, struct http_response *out, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;
	http_response_simple(out, 200, "text/plain", "no\n", 3);
	return HTTP_OK;
}

static const struct http_route ROUTES[] = {
	{ "GET", "/known",    1, 0, stream_known,   0 },
	{ "GET", "/unknown",  1, 0, stream_unknown, 0 },
	{ "GET", "/short",    1, 0, stream_short,   0 },
	{ "GET", "/over",     1, 0, stream_over,    0 },
	{ "GET", "/refuses",  1, 0, stream_refuses, 0 },
	{ "GET", "/both",     1, both_handler, stream_known, 0 },
};

static unsigned long *BYTES;

static struct http_site SITE = {
	ROUTES, sizeof(ROUTES) / sizeof(ROUTES[0]), 0, "ReconOS/stream", 0, 0, 0
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

	child = fork();
	if (child < 0) {
		printf("  FAIL  fork\n");
		return 1;
	}
	if (child == 0) {
		int served = 0;

		alarm(30);	/* an orphan that answers is worse than one
				 * that hangs -- see test_http_serve.c */
		while (served < 16) {
			int rc = http_serve_once(listener, &SITE);

			if (rc < 0)
				break;
			served += rc;
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

	kill(child, SIGTERM);
	waitpid(child, 0, 0);
	close(listener);

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
