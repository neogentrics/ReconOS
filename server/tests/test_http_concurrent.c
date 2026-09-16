/*
 * Two clients, one of them stuck.
 *
 * This is the suite for the one property the connection pool exists to
 * provide, and it is a property that cannot be seen by looking at a single
 * request: **a client that has stopped sending must not stop anybody else.**
 *
 * --- Why this is worth its own file ---
 *
 * Until the pool, `http_serve_once` accepted one connection and served it to
 * completion. Every other client waited, and the wait was not theoretical: a
 * request that stalls mid-body is held until `RECV_DEADLINE_MS`, which is
 * fifteen seconds. One upload from one client meant fifteen seconds of nothing
 * for everyone -- and on the measured kernel numbers in VF-013, an upload that
 * stalls is the ordinary case rather than the unusual one.
 *
 * Nothing in the other suites would have noticed. Each of them opens one
 * connection at a time, which is exactly the shape that works either way.
 *
 * --- Watched failing first ---
 *
 * Against the previous `serve.c`, recovered from git and compiled unchanged:
 * **4 of 12 checks failed**, and they are exactly the four about somebody
 * other than the stuck client being served. The stalled client is accepted
 * first and served to completion, so nobody else's connection is ever
 * accepted, and every read against it times out.
 *
 * The other eight passed, which is the part worth noticing: the stuck client
 * is still answered correctly, with the whole body, across both halves. A
 * suite that only followed one client would have called the old server
 * perfectly good.
 *
 * --- What this does not prove ---
 *
 * That two requests are *answered* at once. They are not: dispatch and the
 * response are synchronous, deliberately, and `serve.c` says why. What is
 * proved here is that the waiting is shared, which is where the seconds are.
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
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <fcntl.h>

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

/* --- the site ------------------------------------------------------------- */

static int handle_hello(const struct http_request *r, const char *body,
                        size_t body_len, struct http_response *out, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;
	http_response_simple(out, 200, "text/plain", "hello\n", 6);
	return HTTP_OK;
}

static int handle_take(const struct http_request *r, const char *body,
                       size_t body_len, struct http_response *out, void *ctx)
{
	static char seen[64];
	int n;

	(void)r; (void)ctx; (void)body;
	n = snprintf(seen, sizeof(seen), "took %lu\n", (unsigned long)body_len);
	if (n < 0 || (size_t)n >= sizeof(seen))
		return HTTP_EINTERNAL;
	http_response_simple(out, 200, "text/plain", seen, (size_t)n);
	return HTTP_OK;
}

static const struct http_route ROUTES[] = {
	{ "GET",  "/hello", 1, handle_hello, 0, 0 },
	{ "POST", "/take",  1, handle_take,  0, 0 },
};

/* What the host must do to a socket so it behaves like one on the target.
 * See `serve.h`: ReconOS passes NULL because its sockets already do. */
static void unblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static unsigned long *BYTES;

static struct http_site SITE = {
	/* routes, count, ctx, name, log, log_ctx, bytes_sent, idle, now_ms,
	 * unblock. No `idle` and no clock: on a host there is nothing to yield
	 * to, and the deadline this suite cares about is the client's. */
	ROUTES, sizeof(ROUTES) / sizeof(ROUTES[0]), 0, "ReconOS/conc",
	0, 0, 0, 0, 0, unblock
};

/* --- clients --------------------------------------------------------------- */

/* Connect, with a receive timeout so a failure is a failed check rather than a
 * suite that hangs. A suite that can hang is a suite that gets disabled. */
static int dial(unsigned port, unsigned ms)
{
	struct sockaddr_in a;
	struct timeval tv;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;

	if (fd < 0)
		return -1;

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons((unsigned short)port);
	a.sin_addr.s_addr = htonl(0x7F000001);

	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}

	/* Nagle off: this suite sends a head and then, deliberately, nothing.
	 * A held-back segment would look exactly like the stall being tested
	 * and would make a pass meaningless. */
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	tv.tv_sec = (time_t)(ms / 1000);
	tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	return fd;
}

static void put(int fd, const char *text)
{
	size_t len = strlen(text);
	size_t at = 0;

	while (at < len) {
		long n = send(fd, text + at, len - at, 0);

		if (n <= 0)
			return;
		at += (size_t)n;
	}
}

/* Read whatever arrives within the socket's timeout. Returns the count. */
static size_t slurp(int fd, char *out, size_t room)
{
	size_t at = 0;

	for (;;) {
		long n = recv(fd, out + at, room - 1 - at, 0);

		if (n <= 0)
			break;
		at += (size_t)n;
		if (at >= room - 1)
			break;
		/* Deliberately not stopping at the blank line. That is the end
		 * of the *head*, and the first version of this stopped there --
		 * so a check looking for something in the body read an empty
		 * buffer and failed for a reason that had nothing to do with
		 * the server. The socket's own receive timeout ends this. */
	}
	out[at] = '\0';
	return at;
}

int main(void)
{
	unsigned port = 18600;
	int listener = -1;
	pid_t child;
	int tries = 0;

	printf("two clients, one of them stuck\n");

	BYTES = mmap(0, sizeof(unsigned long), PROT_READ | PROT_WRITE,
	             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (BYTES == MAP_FAILED) {
		printf("  FAIL  no shared page\n");
		return 1;
	}
	*BYTES = 0;
	SITE.bytes_sent = BYTES;

	while (tries < 64 && (listener = http_listen(port)) < 0) {
		port++;
		tries++;
	}
	if (listener < 0) {
		printf("  FAIL  no free port\n");
		return 1;
	}

	/* The listener must not block either -- see test_http_serve.c. */
	fcntl(listener, F_SETFL, fcntl(listener, F_GETFL, 0) | O_NONBLOCK);

	child = fork();
	if (child < 0) {
		printf("  FAIL  fork\n");
		return 1;
	}
	if (child == 0) {
		alarm(30);	/* an orphan that answers is worse than one
				 * that hangs -- see test_http_serve.c */
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

	/* --- the case this file exists for ------------------------------------- */
	{
		char reply[1024];
		int stuck, healthy;
		size_t n;

		/*
		 * A client that announces a body and sends only part of it.
		 * It never sends the rest. Against a server that takes one
		 * connection at a time, everything after this waits for the
		 * receive deadline.
		 */
		stuck = dial(port, 2000);
		ok(stuck >= 0, "a client that will go quiet mid-request");
		put(stuck,
		    "POST /take HTTP/1.1\r\nHost: m16\r\n"
		    "Content-Length: 40\r\n\r\n"
		    "only-ten-b");

		/* Let the server notice it and settle into waiting. */
		usleep(200000);

		/* A second client, arriving while the first is stuck, asking
		 * for something trivial. */
		healthy = dial(port, 2000);
		ok(healthy >= 0, "a second client arrives while it is stuck");
		put(healthy,
		    "GET /hello HTTP/1.1\r\nHost: m16\r\n"
		    "Connection: close\r\n\r\n");

		n = slurp(healthy, reply, sizeof(reply));
		ok(n > 0, "the second client is answered at all");
		ok(n > 0 && strstr(reply, "200 OK") != 0,
		   "and answered 200 -- not made to wait for the first");
		close(healthy);

		/*
		 * Now let the first client finish. It must still be there:
		 * holding a connection open must not have cost it its place.
		 */
		/* Exactly thirty, to the ten already sent. The first draft of
		 * this sent thirty-nine against a declared forty total; the
		 * extra bytes became the start of a second request on the same
		 * connection, and the check failed for that rather than for
		 * anything the server had done. */
		put(stuck, "and-here-are-thirty-more-bytes");
		n = slurp(stuck, reply, sizeof(reply));
		ok(n > 0, "the stuck client is still connected afterwards");
		ok(n > 0 && strstr(reply, "200 OK") != 0,
		   "and is answered once it finishes");
		ok(n > 0 && strstr(reply, "took 40") != 0,
		   "with the whole body it sent, across both halves");
		close(stuck);
	}

	/* --- several at once, all answered -------------------------------------
	 *
	 * Opened together and read afterwards, so they overlap rather than
	 * queue. One more than the pool holds: the extra must be served too,
	 * from the listen backlog, rather than refused. */
	{
		int fds[6];
		int i, answered = 0, connected = 0;

		for (i = 0; i < 6; i++) {
			fds[i] = dial(port, 3000);
			if (fds[i] >= 0) {
				connected++;
				put(fds[i],
				    "GET /hello HTTP/1.1\r\nHost: m16\r\n"
				    "Connection: close\r\n\r\n");
			}
		}
		ok(connected == 6, "six clients connect at once");

		for (i = 0; i < 6; i++) {
			char reply[512];

			if (fds[i] < 0)
				continue;
			if (slurp(fds[i], reply, sizeof(reply)) > 0
			    && strstr(reply, "200 OK"))
				answered++;
			close(fds[i]);
		}
		ok(answered == 6,
		   "and all six are answered, including the two past the pool");
	}

	/* --- a connection that opens and says nothing ---------------------------
	 *
	 * The idle keep-alive shape. It must be dropped rather than waited on,
	 * or one silent client would hold a slot until the deadline. */
	{
		int quiet = dial(port, 2000);
		int after;
		char reply[512];

		ok(quiet >= 0, "a client that connects and says nothing");
		usleep(200000);

		after = dial(port, 2000);
		put(after, "GET /hello HTTP/1.1\r\nHost: m16\r\n"
		           "Connection: close\r\n\r\n");
		ok(slurp(after, reply, sizeof(reply)) > 0
		   && strstr(reply, "200 OK") != 0,
		   "does not stop the next one being served");
		close(after);
		close(quiet);
	}

	kill(child, SIGTERM);
	waitpid(child, 0, 0);
	close(listener);

	ok(*BYTES > 0, "the server counted the bytes it sent");

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
