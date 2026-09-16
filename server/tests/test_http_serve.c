/*
 * The server, over a real socket.
 *
 * `test_http.c` proves the parser by handing it strings. This proves the half
 * that strings cannot reach: that a listener accepts, that a handler is found
 * and run, that a response arrives with the right status and the right number
 * of bytes, and that a second request on the same connection is answered.
 *
 * **It runs `serve.c` unmodified.** There is no stand-in for the network here.
 * The file calls `socket`, `bind`, `listen`, `accept`, `recv`, `send` and
 * `close`; on the host those are POSIX's, and on the machine they are ReconOS's
 * library over the five system calls the kernel took numbers for. A stand-in
 * would be looser than the real call in exactly the places a server breaks, so
 * there is not one.
 *
 * The one thing the host cannot show is the kernel's `accept` answering EAGAIN
 * with nobody waiting -- here it blocks instead. `http_serve_once` treats both
 * identically, and that is written down rather than demonstrated, because a
 * host cannot demonstrate it.
 *
 * A child process serves; the parent is the client. Separate processes rather
 * than threads because ReconOS has no threads, and a test that needed them
 * would be testing something this system cannot run.
 */

#include "../http/serve.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/mman.h>

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

/* --- the site under test ------------------------------------------------- */

static const char DASHBOARD[] =
	"<!doctype html><title>M16</title><h1>ReconOS Server</h1>";

static int handle_root(const struct http_request *r, const char *body,
                       size_t body_len, struct http_response *out, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;
	http_response_simple(out, 200, "text/html", DASHBOARD,
	                     sizeof(DASHBOARD) - 1);
	return HTTP_OK;
}

/* A JSON endpoint, because the point of the routing table is that this is not
 * a special case of serving a file. */
static int handle_status(const struct http_request *r, const char *body,
                         size_t body_len, struct http_response *out, void *ctx)
{
	static char json[128];
	int n;

	(void)body; (void)body_len; (void)ctx;
	n = snprintf(json, sizeof(json),
	             "{\"role\":\"server\",\"query\":\"%s\"}", r->query);
	http_response_simple(out, 200, "application/json", json, (size_t)n);
	return HTTP_OK;
}

/* Echoes the request body back, which is how the test sees that the body was
 * read in full and handed over with the right length. */
static int handle_echo(const struct http_request *r, const char *body,
                       size_t body_len, struct http_response *out, void *ctx)
{
	static char copy[HTTP_BODY_MAX];

	(void)r; (void)ctx;
	if (body_len > sizeof(copy))
		return HTTP_EBODY_LONG;
	if (body && body_len)
		memcpy(copy, body, body_len);
	http_response_simple(out, 200, "text/plain", copy, body_len);
	return HTTP_OK;
}

static const struct http_route ROUTES[] = {
	{ "GET",  "/",            1, handle_root,   0, 0 },
	{ "GET",  "/api/status",  1, handle_status, 0, 0 },
	{ "POST", "/api/echo",    1, handle_echo,   0, 0 },
};

/* The byte counter lives in shared memory, because the server runs in the
 * child and the assertion is made in the parent. A plain global would be
 * copied by `fork` and the parent would read its own zero -- which would look
 * exactly like a server that sent nothing. */
static unsigned long *BYTES;

static struct http_site SITE = {
	ROUTES, sizeof(ROUTES) / sizeof(ROUTES[0]), 0, "ReconOS/0.1", 0, 0, 0
};

/* --- the client ---------------------------------------------------------- */

static int dial(unsigned port)
{
	struct sockaddr_in addr;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(0x7F000001);	/* 127.0.0.1 */

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
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
		break;
	}
	into[have] = '\0';
	return have;
}

/* Send one request on its own connection and return the whole reply. */
static size_t exchange(unsigned port, const char *request, char *into,
                       size_t room)
{
	int fd = dial(port);
	size_t n;

	if (fd < 0) {
		into[0] = '\0';
		return 0;
	}
	if (send(fd, request, strlen(request), 0) < 0) {
		close(fd);
		into[0] = '\0';
		return 0;
	}
	n = slurp(fd, into, room);
	close(fd);
	return n;
}

static int starts_with(const char *s, const char *p)
{
	return strncmp(s, p, strlen(p)) == 0;
}

int main(void)
{
	unsigned port = 18080;
	int listener = -1;
	pid_t child;
	char reply[8192];

	printf("the server, over a real socket\n");

	/* The counter must live in memory both processes see. A plain global
	 * would be copied by `fork`, and the parent would read its own zero --
	 * which looks exactly like a server that sent nothing. That failure
	 * would have been indistinguishable from the bug this check exists to
	 * catch, which is why it is shared rather than returned. */
	BYTES = mmap(0, sizeof(*BYTES), PROT_READ | PROT_WRITE,
	             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (BYTES == MAP_FAILED) {
		printf("  FAIL  no shared page for the byte counter\n");
		return 1;
	}
	*BYTES = 0;
	SITE.bytes_sent = BYTES;

	/* A busy port is not a failure of this suite. Walk until one is free. */
	{
		int tries = 0;

		while (tries < 32 && (listener = http_listen(port)) < 0) {
			port++;
			tries++;
		}
	}
	if (listener < 0) {
		printf("  FAIL  no free port to listen on\n");
		return 1;
	}

	child = fork();
	if (child < 0) {
		printf("  FAIL  fork\n");
		return 1;
	}
	if (child == 0) {
		int served = 0;

		/* A hard ceiling on this child's life.
		 *
		 * The child outlived the suite once: the parent crashed before
		 * reaching its `kill`, and the orphan sat on the port holding a
		 * listener. The next thing to ask that port for a page got an
		 * answer -- from the wrong process, looking exactly like a
		 * pass. An orphan that answers is worse than one that hangs,
		 * because it is indistinguishable from success. */
		alarm(20);

		/* Serve exactly as many connections as the parent opens, then
		 * leave. A child that ran forever would hang the suite on any
		 * failure, and a suite that can hang is a suite that gets
		 * disabled. */
		while (served < 28) {
			int rc = http_serve_once(listener, &SITE);

			if (rc < 0)
				break;
			served += rc;
		}
		close(listener);
		_exit(0);
	}

	/* --- an ordinary page ---------------------------------------------- */
	exchange(port, "GET / HTTP/1.1\r\nHost: m16\r\nConnection: close\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"), "GET / answers 200");
	ok(strstr(reply, "Content-Type: text/html") != 0,
	   "the handler's content type is sent");
	ok(strstr(reply, "<h1>ReconOS Server</h1>") != 0,
	   "the body arrives");
	ok(strstr(reply, "Content-Length: 56\r\n") != 0,
	   "Content-Length is the real length");
	ok(strstr(reply, "Server: ReconOS/0.1") != 0, "the server names itself");

	/* --- a JSON endpoint, and the raw query ----------------------------- */
	exchange(port, "GET /api/status?verbose=1 HTTP/1.1\r\nHost: m16\r\n"
	               "Connection: close\r\n\r\n", reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"), "the API answers 200");
	ok(strstr(reply, "application/json") != 0, "it is served as JSON");
	ok(strstr(reply, "\"query\":\"verbose=1\"") != 0,
	   "the query reaches the handler unchanged");

	/* --- a body, read in full and handed over --------------------------- */
	exchange(port, "POST /api/echo HTTP/1.1\r\nHost: m16\r\n"
	               "Content-Length: 11\r\nConnection: close\r\n\r\n"
	               "hello world", reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"), "POST answers 200");
	ok(strstr(reply, "\r\n\r\nhello world") != 0,
	   "the whole body reached the handler");
	ok(strstr(reply, "Content-Length: 11\r\n") != 0,
	   "the echoed length is right");

	/* --- HEAD mirrors GET, without the body ------------------------------ */
	exchange(port, "HEAD / HTTP/1.1\r\nHost: m16\r\nConnection: close\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"), "HEAD answers 200");
	ok(strstr(reply, "Content-Length: 56\r\n") != 0,
	   "HEAD reports the length it would have sent");
	ok(strstr(reply, "<h1>") == 0, "HEAD sends no body");

	/* --- what is not there, and what is there by another method ---------- */
	exchange(port, "GET /nothing HTTP/1.1\r\nHost: m16\r\nConnection: close\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 404 Not Found\r\n"),
	   "an unrouted path answers 404");

	exchange(port, "POST /api/status HTTP/1.1\r\nHost: m16\r\n"
	               "Content-Length: 0\r\nConnection: close\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 405 Method Not Allowed\r\n"),
	   "a real path with the wrong method answers 405, not 404");

	/* --- Expect: 100-continue -----------------------------------------------
	 *
	 * A client sending a large body may ask permission first: head, wait,
	 * then body. **A server that never answers leaves it waiting until its
	 * own timeout expires** -- and then it sends the body anyway, so nothing
	 * fails and nothing is reported. The request merely takes a second
	 * longer than it should, every time. `curl` does this on any body over
	 * about a kilobyte, so it is the most ordinary large POST there is.
	 *
	 * The suite sends head and body together, which a client that gave up
	 * waiting would also do; the interim reply must still come first. */
	{
		size_t n = exchange(port,
		                    "POST /api/echo HTTP/1.1\r\nHost: m\r\n"
		                    "Content-Length: 5\r\n"
		                    "Expect: 100-continue\r\n"
		                    "Connection: close\r\n\r\nhello",
		                    reply, sizeof(reply));

		ok(n > 0 && starts_with(reply, "HTTP/1.1 100 Continue\r\n\r\n"),
		   "an expectation is answered before the body is read");
		ok(strstr(reply, "HTTP/1.1 200 OK\r\n") != 0,
		   "and the real response follows it");
		ok(strstr(reply, "\r\n\r\nhello") != 0,
		   "carrying the body the handler was given");

		/* The interim reply carries no headers of its own. One that did
		 * would have the client read them as the real reply's. */
		{
			const char *real = strstr(reply, "HTTP/1.1 200");
			size_t interim = real ? (size_t)(real - reply) : 0;

			ok(interim == sizeof("HTTP/1.1 100 Continue\r\n\r\n") - 1,
			   "the interim reply is a status line and nothing else");
		}

		/* Matched without regard to case, which is a spelling clients
		 * really send. */
		exchange(port, "POST /api/echo HTTP/1.1\r\nHost: m\r\n"
		               "Content-Length: 5\r\n"
		               "Expect: 100-Continue\r\n"
		               "Connection: close\r\n\r\nhello",
		         reply, sizeof(reply));
		ok(starts_with(reply, "HTTP/1.1 100 Continue\r\n"),
		   "Expect is matched without regard to case");

		/* An expectation this server does not implement. 417 rather
		 * than silence: the client asked whether something would be
		 * honoured, and saying nothing would be read as yes. */
		exchange(port, "POST /api/echo HTTP/1.1\r\nHost: m\r\n"
		               "Content-Length: 5\r\n"
		               "Expect: something-else\r\n\r\nhello",
		         reply, sizeof(reply));
		ok(starts_with(reply, "HTTP/1.1 417 Expectation Failed\r\n"),
		   "an expectation this server cannot meet is refused, not ignored");

		/* HTTP/1.0 has no such mechanism, and a 1.0 client handed an
		 * interim reply reads it as *the* reply. */
		exchange(port, "POST /api/echo HTTP/1.0\r\n"
		               "Content-Length: 5\r\n"
		               "Expect: 100-continue\r\n\r\nhello",
		         reply, sizeof(reply));
		ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"),
		   "a 1.0 client is never sent an interim reply");
	}

	/* --- the security headers, on every kind of response --------------------
	 *
	 * They are written by the server rather than by a handler, so that a
	 * handler cannot forget one. This checks the property that makes that
	 * worth doing: they are on *everything*, not only on the paths somebody
	 * remembered to look at.
	 *
	 * **It caught the fault it was written for.** The headers went on the
	 * whole-response path and not the streaming one, because a patch script
	 * stopped half way through -- so pages carried them and files, 404s and
	 * 206s did not. Nothing else would have noticed. */
	{
		static const char *WANTED[] = {
			"X-Content-Type-Options: nosniff\r\n",
			"X-Frame-Options: DENY\r\n",
			"Referrer-Policy: no-referrer\r\n"
		};
		static const char *REQUESTS[] = {
			"GET / HTTP/1.1\r\nHost: m\r\nConnection: close\r\n\r\n",
			"GET /api/status HTTP/1.1\r\nHost: m\r\nConnection: close\r\n\r\n",
			"GET /nothing HTTP/1.1\r\nHost: m\r\nConnection: close\r\n\r\n",
			"DELETE / HTTP/1.1\r\nHost: m\r\n\r\n",
			"GET /../etc HTTP/1.1\r\nHost: m\r\n\r\n"
		};
		size_t r, h;
		int all = 1;

		for (r = 0; r < sizeof(REQUESTS) / sizeof(REQUESTS[0]); r++) {
			exchange(port, REQUESTS[r], reply, sizeof(reply));
			for (h = 0; h < sizeof(WANTED) / sizeof(WANTED[0]); h++)
				if (!strstr(reply, WANTED[h]))
					all = 0;
		}
		ok(all,
		   "every response carries the security headers -- pages, APIs,"
		   " 404s, 501s and refusals alike");
	}

	/* --- the refusals reach the wire ------------------------------------- */
	exchange(port, "GET /../etc/passwd HTTP/1.1\r\nHost: m16\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 400 Bad Request\r\n"),
	   "traversal is refused on the wire, not just in the parser");

	exchange(port, "POST / HTTP/1.1\r\nHost: m16\r\nContent-Length: 6\r\n"
	               "Transfer-Encoding: chunked\r\n\r\n", reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 400 Bad Request\r\n"),
	   "a smuggling attempt is refused on the wire");

	exchange(port, "DELETE / HTTP/1.1\r\nHost: m16\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 501 Not Implemented\r\n"),
	   "an unimplemented method answers 501");

	/* --- two requests on one connection ---------------------------------- */
	{
		int fd = dial(port);
		char buf[4096];
		size_t n;

		ok(fd >= 0, "a connection for keep-alive");
		if (fd >= 0) {
			const char *one =
				"GET /api/status HTTP/1.1\r\nHost: m16\r\n\r\n";
			const char *two =
				"GET / HTTP/1.1\r\nHost: m16\r\n"
				"Connection: close\r\n\r\n";

			send(fd, one, strlen(one), 0);
			send(fd, two, strlen(two), 0);
			n = slurp(fd, buf, sizeof(buf));
			close(fd);

			ok(n > 0 && starts_with(buf, "HTTP/1.1 200 OK\r\n"),
			   "the first of two requests is answered");
			ok(strstr(buf, "application/json") != 0
			   && strstr(buf, "text/html") != 0,
			   "both requests on one connection are answered");
		}
	}

	/* The counter the server actually incremented, read from the shared
	 * page. It is a lower bound on what reached the client and an upper
	 * bound on nothing -- which is the direction that matters: a server
	 * reporting bytes it did not send would be worse than one reporting
	 * none. */
	ok(*BYTES > 400, "the server counted the bytes it put on the wire");

	kill(child, SIGTERM);
	waitpid(child, 0, 0);
	close(listener);

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
