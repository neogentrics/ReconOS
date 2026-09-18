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

/*
 * A body worth compressing: over the threshold, and text.
 *
 * Built rather than written out, so its size is stated once and the checks
 * below can compare against it.
 */
#define BIG_LEN 2000

static int handle_big(const struct http_request *r, const char *body,
                      size_t body_len, struct http_response *out, void *ctx)
{
	static char text[BIG_LEN + 1];
	static int built;
	size_t i;

	(void)r; (void)body; (void)body_len; (void)ctx;
	if (!built) {
		for (i = 0; i < BIG_LEN; i++)
			text[i] = (char)('a' + (int)(i % 7));
		built = 1;
	}
	http_response_simple(out, 200, "text/plain", text, BIG_LEN);
	return HTTP_OK;
}

static const struct http_route ROUTES[] = {
	{ .method = "GET", .prefix = "/big", .exact = 1, .handler = handle_big },
	{ .method = "GET", .prefix = "/", .exact = 1, .handler = handle_root },
	{ .method = "GET", .prefix = "/api/status",
	  .exact = 1, .handler = handle_status },
	{ .method = "POST", .prefix = "/api/echo",
	  .exact = 1, .handler = handle_echo },
};

/* The byte counter lives in shared memory, because the server runs in the
 * child and the assertion is made in the parent. A plain global would be
 * copied by `fork` and the parent would read its own zero -- which would look
 * exactly like a server that sent nothing. */
static unsigned long *BYTES;

/* What a host must do to a fresh socket so it behaves like one on the target.
 * ReconOS passes NULL here: its sockets never block. See `serve.h`. */
static void unblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

/* --- the guard, over a real socket -----------------------------------------
 *
 * `auth.c` decides whether a token is right and is tested exhaustively in its
 * own suite. What is tested here is the part only the server can do: that a
 * route marked `guarded` is **not reached at all** without permission, and
 * that a site which asks for a guard and supplies none fails closed.
 *
 * That second one needs a second site, which is what found the pool bug
 * recorded in `serve.c`: the connection slots are one static array, so a
 * connection accepted for one site must carry its own site or it gets stepped
 * with another's routes and another's policy. Two listeners in this suite are
 * what makes that impossible to regress.
 */

/*
 * How many times the policy was asked.
 *
 * **Shared, because the server runs in the child and the check is made in the
 * parent.** A plain `int` here was the first version, and it failed exactly as
 * this file's comment on the byte counter says it would: `fork` copies it, the
 * child counts in its own copy, and the parent reads a zero that looks
 * identical to a policy that was never consulted.
 *
 * Which is the failure this check exists to detect. It would have reported the
 * guard as never asked, on a server asking it correctly every time.
 */
static int *GUARD_SAW;

static int handle_secret(const struct http_request *r, const char *body,
                         size_t body_len, struct http_response *out, void *ctx)
{
	(void)r; (void)body; (void)body_len; (void)ctx;
	http_response_simple(out, 200, "text/plain", "secret\n", 7);
	return HTTP_OK;
}

static const struct http_route GUARDED_ROUTES[] = {
	{ .method = "GET", .prefix = "/open", .exact = 1, .handler = handle_root },
	{ .method = "GET", .prefix = "/secret",
	  .exact = 1, .handler = handle_secret, .guarded = 1 },
};

/* Allows only the exact header. Deliberately trivial: the real decision is
 * `auth_ok`, and repeating it here would test this file against itself. */
static int allow_if_word(const struct http_request *r, const char *body,
                         size_t body_len, void *ctx)
{
	const char *v = http_header_get(r, "authorization");

	/* The body is handed over so a policy can read a form field -- a
	 * browser form cannot send a header. This one does not need it; the
	 * real policy in `server_init.c` does. */
	(void)ctx; (void)body; (void)body_len;
	if (GUARD_SAW)
		(*GUARD_SAW)++;
	return v && strcmp(v, "Bearer opensesame") == 0;
}

static struct http_site GUARDED_SITE = {
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
	.routes = GUARDED_ROUTES,
	.route_count = sizeof(GUARDED_ROUTES) / sizeof(GUARDED_ROUTES[0]),
	.server_name = "ReconOS/guarded",
	.unblock = unblock,
	.allow = allow_if_word
};

/* The same routes with **no policy at all**. A guarded route here must answer
 * 500: the site asked for a guard it did not supply, and serving the request
 * anyway is the one outcome nobody wanted. */
static struct http_site BARE_SITE = {
	.routes = GUARDED_ROUTES,
	.route_count = sizeof(GUARDED_ROUTES) / sizeof(GUARDED_ROUTES[0]),
	.server_name = "ReconOS/bare",
	.unblock = unblock
};

static struct http_site SITE = {
	/* `idle` and `now_ms` are zero: on a host there is nothing to yield
	 * to. `unblock` is not -- the server holds several connections at
	 * once and a blocking socket would stop the loop on whichever one
	 * went quiet. See `serve.h`. */
	.routes = ROUTES,
	.route_count = sizeof(ROUTES) / sizeof(ROUTES[0]),
	.server_name = "ReconOS/0.1",
	.unblock = unblock
};


/* --- virtual hosts, over a real socket ------------------------------------
 *
 * `test_http.c` proves `http_host_matches` against strings. What only a socket
 * can show is that the server **dispatches** on the answer: that the site whose
 * name was asked for is the site that answers, with its own routes, its own
 * context and its own `Server` header -- and that a name nobody claims is
 * refused rather than handed to whoever happens to be first in the chain.
 *
 * Two named sites and **no default**, deliberately. A default would answer
 * everything and hide exactly the failure worth catching.
 */

/* Reports which site's context it was called with, which is how the parent
 * tells the two apart. The path cannot do it: both sites share this table, so
 * a check that looked at the path would pass with the dispatch removed
 * entirely. */
static int handle_whoami(const struct http_request *r, const char *body,
                         size_t body_len, struct http_response *out, void *ctx)
{
	const char *name = ctx ? (const char *)ctx : "(nobody)";

	(void)r; (void)body; (void)body_len;
	http_response_simple(out, 200, "text/plain", name, strlen(name));
	return HTTP_OK;
}

static const struct http_route VHOST_ROUTES[] = {
	{ .method = "GET", .prefix = "/who", .exact = 1, .handler = handle_whoami },
};

static struct http_site BETA_SITE = {
	.host = "beta.recon",
	.routes = VHOST_ROUTES,
	.route_count = sizeof(VHOST_ROUTES) / sizeof(VHOST_ROUTES[0]),
	.ctx = (void *)"beta",
	.server_name = "ReconOS/beta",
	.unblock = unblock
};

static struct http_site ALPHA_SITE = {
	.host = "alpha.recon",
	.next = &BETA_SITE,
	.routes = VHOST_ROUTES,
	.route_count = sizeof(VHOST_ROUTES) / sizeof(VHOST_ROUTES[0]),
	.ctx = (void *)"alpha",
	.server_name = "ReconOS/alpha",
	.unblock = unblock
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
	unsigned guard_port = 0, bare_port = 0, vhost_port = 0;
	int guard_listener = -1, bare_listener = -1, vhost_listener = -1;
	int listener = -1;
	pid_t child;
	char reply[8192];

	printf("the server, over a real socket\n");

	/*
	 * A write to a socket the other end has closed must be an error here,
	 * not a signal.
	 *
	 * The default for `SIGPIPE` is to kill the process, and this suite now
	 * sends requests the server is **meant** to refuse: it answers 400 and
	 * closes while the client is still writing. That killed the suite
	 * outright -- exit 141, no output, nothing printed about which check
	 * was running, which is the least informative failure there is.
	 *
	 * Set before the fork, so the server side is covered too: it writes to
	 * clients that have gone away, and a server that dies of a signal when
	 * a browser closes a tab is not a server.
	 *
	 * ReconOS has no `SIGPIPE` on a socket, so this is one more thing the
	 * host needs in order to behave like the target -- the same category
	 * as `unblock`.
	 */
	signal(SIGPIPE, SIG_IGN);

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

	GUARD_SAW = mmap(0, sizeof(*GUARD_SAW), PROT_READ | PROT_WRITE,
	                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (GUARD_SAW == MAP_FAILED) {
		printf("  FAIL  no shared page for the guard counter\n");
		return 1;
	}
	*GUARD_SAW = 0;

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

	/* Two more, for the guard. One site with a policy and one without, on
	 * their own ports, because the fail-closed case is a property of a site
	 * rather than of a route -- and because two sites in one process is
	 * exactly what found the pool bug recorded in `serve.c`. */
	{
		int tries = 0;

		guard_port = port + 1;
		while (tries < 32 && (guard_listener = http_listen(guard_port)) < 0) {
			guard_port++;
			tries++;
		}
		tries = 0;
		bare_port = guard_port + 1;
		while (tries < 32 && (bare_listener = http_listen(bare_port)) < 0) {
			bare_port++;
			tries++;
		}
		/* And one for the chain of named sites. Its own port, because
		 * each of the other three has a site with no name, which claims
		 * every request that reaches it -- the opposite of what this
		 * one is for. */
		tries = 0;
		vhost_port = bare_port + 1;
		while (tries < 32 && (vhost_listener = http_listen(vhost_port)) < 0) {
			vhost_port++;
			tries++;
		}
	}
	if (guard_listener < 0 || bare_listener < 0 || vhost_listener < 0) {
		printf("  FAIL  no free port for the guarded sites\n");
		return 1;
	}
	fcntl(guard_listener, F_SETFL,
	      fcntl(guard_listener, F_GETFL, 0) | O_NONBLOCK);
	fcntl(bare_listener, F_SETFL,
	      fcntl(bare_listener, F_GETFL, 0) | O_NONBLOCK);
	fcntl(vhost_listener, F_SETFL,
	      fcntl(vhost_listener, F_GETFL, 0) | O_NONBLOCK);

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

		/*
		 * Run until the parent signals, bounded by the alarm above.
		 *
		 * This used to count connections and stop at 28, which stopped
		 * meaning anything when `http_serve_once` became one step of a
		 * multiplexing loop rather than one whole connection served.
		 * The count is gone rather than adjusted: a number that has to
		 * track how many steps a request takes is a number that will
		 * be wrong after the next change to the loop.
		 */
		(void)served;
		for (;;) {
			int rc = http_serve_once(listener, &SITE);
			int g  = http_serve_once(guard_listener, &GUARDED_SITE);
			int b  = http_serve_once(bare_listener, &BARE_SITE);
			/* The head of the chain, not each site in turn: one
			 * listener, and `serve.c` walks from there. Looping
			 * over the chain here would be this suite doing the
			 * dispatch it exists to check. */
			int v  = http_serve_once(vhost_listener, &ALPHA_SITE);

			if (rc < 0 || g < 0 || b < 0 || v < 0)
				break;
			if (rc == 0 && g == 0 && b == 0 && v == 0)
				usleep(200);	/* nothing to do; do not spin */
		}
		close(listener);
		close(guard_listener);
		close(bare_listener);
		close(vhost_listener);
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

	/* --- the guard --------------------------------------------------------
	 *
	 * Whether a token is the right one is `auth.c`'s job and is tested to
	 * destruction there. What is checked here is that the server asks at
	 * all, asks *before* the handler runs, and fails closed when a site
	 * asked for a guard it did not supply.
	 */
	{
		char reply[2048];

		/* An unguarded route on the guarded site is unaffected. A guard
		 * that quietly covered everything would look like this one
		 * working, right up until somebody could not read a status
		 * page. */
		exchange(guard_port,
		         "GET /open HTTP/1.1\r\nHost: m16\r\n"
		         "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strstr(reply, "200 OK") != 0,
		   "an unguarded route is served without a token");

		/* No credentials at all. */
		exchange(guard_port,
		         "GET /secret HTTP/1.1\r\nHost: m16\r\n"
		         "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strstr(reply, "401 Unauthorized") != 0,
		   "a guarded route with no token answers 401");
		ok(strstr(reply, "WWW-Authenticate: Bearer") != 0,
		   "and says what it wants, which is what makes it a 401");
		ok(strstr(reply, "secret") == 0,
		   "and the handler did not run -- no part of its answer leaked");

		/* Wrong credentials. Same answer as none, deliberately: telling
		 * a caller which half they got right is what a guard must not
		 * do. */
		exchange(guard_port,
		         "GET /secret HTTP/1.1\r\nHost: m16\r\n"
		         "Authorization: Bearer wrong\r\n"
		         "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strstr(reply, "401 Unauthorized") != 0,
		   "a wrong token answers 401 too");
		ok(strstr(reply, "secret") == 0, "and still does not run it");

		/* The right one. */
		exchange(guard_port,
		         "GET /secret HTTP/1.1\r\nHost: m16\r\n"
		         "Authorization: Bearer opensesame\r\n"
		         "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strstr(reply, "200 OK") != 0, "the right token gets through");
		ok(strstr(reply, "secret") != 0, "and the handler runs");

		/*
		 * The case this needed a second site for.
		 *
		 * A route marked `guarded` on a site with no policy. 500,
		 * because the site is misconfigured -- and emphatically not
		 * 200, which is what a guard that "falls back to open when
		 * nothing is configured" would give. That fallback is the most
		 * common way a guard turns out never to have been guarding.
		 */
		exchange(bare_port,
		         "GET /secret HTTP/1.1\r\nHost: m16\r\n"
		         "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strstr(reply, "500") != 0,
		   "a guarded route with no policy answers 500, not 200");
		ok(strstr(reply, "secret") == 0,
		   "and fails closed: the handler never ran");

		/* The unguarded route on that same site still works, so the
		 * 500 above is about the guard and not about the site being
		 * broken outright. */
		exchange(bare_port,
		         "GET /open HTTP/1.1\r\nHost: m16\r\n"
		         "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strstr(reply, "200 OK") != 0,
		   "while its unguarded route is served normally");

		ok(*GUARD_SAW > 0, "the policy was actually consulted");
	}

	/* --- compressing a response ---------------------------------------------
	 *
	 * The decision is the server's, not the handler's, so these checks are
	 * about a handler that knows nothing about compression at all.
	 */
	exchange(port, "GET /big HTTP/1.1\r\nHost: m16\r\n"
	               "Accept-Encoding: gzip\r\nConnection: close\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"),
	   "a body worth compressing is answered");
	ok(strstr(reply, "Content-Encoding: gzip\r\n") != 0,
	   "and says it is compressed");
	ok(strstr(reply, "\r\n\r\n\x1f\x8b") != 0,
	   "and the body begins with the gzip magic");
	ok(strstr(reply, "Vary: Accept-Encoding\r\n") != 0,
	   "and names the header its body depends on, so a cache cannot "
	   "hand it to a client that did not ask");

	/*
	 * The length, which is the check that matters most.
	 *
	 * A `Content-Length` that described the *uncompressed* body while the
	 * compressed one was sent would leave the connection holding the
	 * difference, and the next request would be read out of the middle of
	 * it -- which is the desynchronisation the whole framing section of
	 * `docs/WEB.md` is about, arriving from the server's own side.
	 */
	{
		/*
		 * Counted from what arrived, not with `strlen`.
		 *
		 * A gzip body contains NUL bytes, so every `strstr` and
		 * `strlen` in this file stops early inside one. The first
		 * draft of this check used `strlen` and would have passed
		 * against a server that sent nothing at all. `exchange`
		 * already returns the real count; this uses it.
		 */
		size_t got = exchange(port, "GET /big HTTP/1.1\r\nHost: m16\r\n"
		                            "Accept-Encoding: gzip\r\n"
		                            "Connection: close\r\n\r\n",
		                      reply, sizeof(reply));
		const char *at = strstr(reply, "Content-Length: ");
		const char *body = strstr(reply, "\r\n\r\n");
		unsigned long said = 0;

		ok(at != 0, "the answer carries a length");
		if (at)
			said = strtoul(at + 16, 0, 10);
		ok(said > 0 && said < BIG_LEN,
		   "which is smaller than the body the handler produced");
		ok(body != 0 && got == (size_t)(body + 4 - reply) + said,
		   "and exactly that many body bytes arrived");
	}

	/* A client that did not ask gets the bytes it can read. */
	exchange(port, "GET /big HTTP/1.1\r\nHost: m16\r\nConnection: close\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"),
	   "a client that said nothing about encodings is answered");
	ok(strstr(reply, "Content-Encoding") == 0,
	   "and gets no encoding it did not ask for");
	ok(strstr(reply, "Content-Length: 2000\r\n") != 0,
	   "and the whole body");
	ok(strstr(reply, "Vary: Accept-Encoding\r\n") != 0,
	   "and the answer still names the header, because it could have "
	   "depended on it");

	exchange(port, "GET /big HTTP/1.1\r\nHost: m16\r\n"
	               "Accept-Encoding: gzip;q=0\r\nConnection: close\r\n\r\n",
	         reply, sizeof(reply));
	ok(strstr(reply, "Content-Encoding") == 0,
	   "a client that refused gzip by name does not get it");

	exchange(port, "GET /big HTTP/1.1\r\nHost: m16\r\n"
	               "Accept-Encoding: br, zstd\r\nConnection: close\r\n\r\n",
	         reply, sizeof(reply));
	ok(strstr(reply, "Content-Encoding") == 0,
	   "nor does one that asked only for codings this cannot send");

	/* Below the threshold, where framing costs more than it saves. */
	exchange(port, "GET / HTTP/1.1\r\nHost: m16\r\n"
	               "Accept-Encoding: gzip\r\nConnection: close\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"), "a short page is answered");
	ok(strstr(reply, "Content-Encoding") == 0,
	   "and is not compressed -- eighteen bytes of framing would make it "
	   "bigger");

	/* HEAD must report the length a GET would send, which is now the
	 * compressed one. A HEAD that reported the other number would be a
	 * different answer from the GET it mirrors. */
	exchange(port, "HEAD /big HTTP/1.1\r\nHost: m16\r\n"
	               "Accept-Encoding: gzip\r\nConnection: close\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"), "HEAD is answered");
	ok(strstr(reply, "Content-Encoding: gzip\r\n") != 0,
	   "and says what the body would have been encoded as");
	ok(strstr(reply, "Content-Length: 2000\r\n") == 0,
	   "and reports the compressed length, not the handler's");
	ok(strstr(reply, "\x1f\x8b") == 0, "and sends no body");

	/* --- a chunked body, over a real socket ---------------------------------
	 *
	 * `test_chunked.c` proves the decoder against strings. What only a
	 * socket shows is the part the decoder cannot: that the connection
	 * loop keeps feeding it as bytes arrive, that the handler is handed
	 * the **decoded** body, and that the connection afterwards is left
	 * pointing at the right byte.
	 */
	exchange(port, "POST /api/echo HTTP/1.1\r\nHost: m16\r\n"
	               "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
	               "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"),
	   "a chunked POST is answered 200");
	ok(strstr(reply, "\r\n\r\nhello world") != 0,
	   "and the handler is given the decoded body, joined");
	ok(strstr(reply, "Content-Length: 11\r\n") != 0,
	   "eleven bytes, not the twenty-four that arrived");

	exchange(port, "POST /api/echo HTTP/1.1\r\nHost: m16\r\n"
	               "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
	               "0\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"),
	   "an empty chunked body is a body");

	/* With a trailer, which must be consumed and must not become a
	 * header -- `chunked.h` says why at length. */
	exchange(port, "POST /api/echo HTTP/1.1\r\nHost: m16\r\n"
	               "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
	               "5\r\nhello\r\n0\r\nX-Checksum: 42\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"),
	   "a chunked body with a trailer is answered");
	ok(strstr(reply, "X-Checksum") == 0,
	   "and the trailer does not come back as a header");

	/*
	 * Sent in pieces, with a pause, which is the arrangement a real client
	 * produces and the one the whole-message cases above cannot show. A
	 * loop that fed the decoder a fresh view each time instead of resuming
	 * would pass everything else and fail here.
	 */
	{
		int fd = dial(port);
		size_t n;

		/* Counted by the compiler. A hand-written length here was
		 * three bytes too long on the first draft and sent whatever
		 * followed the literal -- the same mistake this file already
		 * made once, in the pipelined virtual-host case below. */
		static const char HEAD[] =
			"POST /api/echo HTTP/1.1\r\nHost: m16\r\n"
			"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n";

		ok(fd >= 0, "a connection for a body sent in pieces");
		send(fd, HEAD, sizeof(HEAD) - 1, 0);
		usleep(50000);
		send(fd, "3\r\nabc\r\n", 8, 0);
		usleep(50000);
		send(fd, "3\r\nd", 4, 0);
		usleep(50000);
		send(fd, "ef\r\n0\r\n\r\n", 9, 0);
		n = slurp(fd, reply, sizeof(reply));
		close(fd);

		ok(n > 0 && starts_with(reply, "HTTP/1.1 200 OK\r\n"),
		   "a body arriving in pieces, one of them mid-chunk, is answered");
		ok(n > 0 && strstr(reply, "\r\n\r\nabcdef") != 0,
		   "and decodes to the same six bytes");
	}

	/*
	 * The check that decides where the next request starts.
	 *
	 * A second request pipelined behind a chunked body. If the connection
	 * took the *decoded* length as the bytes this request occupied, the
	 * chunk headers and the terminator would be left in the buffer and
	 * read as the next request line.
	 */
	{
		static const char BOTH[] =
			"POST /api/echo HTTP/1.1\r\nHost: m16\r\n"
			"Transfer-Encoding: chunked\r\n\r\n"
			"5\r\nhello\r\n0\r\n\r\n"
			"GET / HTTP/1.1\r\nHost: m16\r\nConnection: close\r\n\r\n";
		int fd = dial(port);
		const char *first, *second;
		size_t n;

		ok(fd >= 0, "a connection for a chunked body and one behind it");
		send(fd, BOTH, sizeof(BOTH) - 1, 0);
		n = slurp(fd, reply, sizeof(reply));
		close(fd);

		first = strstr(reply, "\r\n\r\nhello");
		second = strstr(reply, "<h1>ReconOS Server</h1>");
		ok(n > 0 && first != 0, "the chunked request is answered");
		ok(second != 0,
		   "and so is the request pipelined behind it");
		ok(first != 0 && second != 0 && first < second,
		   "in that order, which is the one the client sent");
	}

	/* --- and a body that must not be believed -------------------------------- */
	exchange(port, "POST /api/echo HTTP/1.1\r\nHost: m16\r\n"
	               "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
	               "5\nhello\r\n0\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 400 Bad Request\r\n"),
	   "a chunk size ended with a bare LF is refused over the socket too");

	exchange(port, "POST /api/echo HTTP/1.1\r\nHost: m16\r\n"
	               "Transfer-Encoding: chunked\r\nContent-Length: 5\r\n"
	               "Connection: close\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 400 Bad Request\r\n"),
	   "and so is a request framed both ways");

	exchange(port, "POST /api/echo HTTP/1.0\r\n"
	               "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
	         reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 400 Bad Request\r\n"),
	   "and chunked on HTTP/1.0, which predates it");

	/* --- virtual hosts, and the name nobody claims ------------------------
	 *
	 * `vhost_port` is served by the chain ALPHA -> BETA. Neither has `host`
	 * NULL, so nothing on this listener answers to a name it was not given.
	 *
	 * Every check below would also pass against a server that ignored
	 * `Host` entirely **if both sites shared a context**, which is why they
	 * do not: the body is the picked site's own `ctx`, so a dispatch that
	 * always chose the head of the chain says `alpha` where `beta` belongs.
	 */
	exchange(vhost_port, "GET /who HTTP/1.1\r\nHost: alpha.recon\r\n"
	                     "Connection: close\r\n\r\n", reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"),
	   "a named site answers a request for its own name");
	ok(strstr(reply, "\r\n\r\nalpha") != 0,
	   "and the handler ran with that site's context");

	exchange(vhost_port, "GET /who HTTP/1.1\r\nHost: beta.recon\r\n"
	                     "Connection: close\r\n\r\n", reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"),
	   "so does the second site in the chain");
	ok(strstr(reply, "\r\n\r\nbeta") != 0,
	   "with its own context, not the head of the chain's");
	/* The `Server` header comes from the site, and the listener was started
	 * with ALPHA. If this says `alpha` the routes were picked and the rest
	 * of the site was not, which is the half-done dispatch worth naming. */
	ok(strstr(reply, "Server: ReconOS/beta") != 0,
	   "the whole site is picked, not only its routes");

	/* The three shapes `http_host_matches` exists for, now over a socket
	 * rather than as strings -- a rule with a suite and no caller is a rule
	 * nobody applies. */
	exchange(vhost_port, "GET /who HTTP/1.1\r\nHost: ALPHA.RECON\r\n"
	                     "Connection: close\r\n\r\n", reply, sizeof(reply));
	ok(strstr(reply, "\r\n\r\nalpha") != 0, "a shouted name is the same name");

	exchange(vhost_port, "GET /who HTTP/1.1\r\nHost: beta.recon:8080\r\n"
	                     "Connection: close\r\n\r\n", reply, sizeof(reply));
	ok(strstr(reply, "\r\n\r\nbeta") != 0, "a port does not change the machine");

	exchange(vhost_port, "GET /who HTTP/1.1\r\nHost: alpha.recon.\r\n"
	                     "Connection: close\r\n\r\n", reply, sizeof(reply));
	ok(strstr(reply, "\r\n\r\nalpha") != 0, "the root form is the same name");

	/* --- and what must not be claimed ------------------------------------ */
	exchange(vhost_port, "GET /who HTTP/1.1\r\nHost: gamma.recon\r\n"
	                     "Connection: close\r\n\r\n", reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 421 Misdirected Request\r\n"),
	   "a name no site claims answers 421, with the phrase");
	ok(strstr(reply, "\r\n\r\nalpha") == 0 && strstr(reply, "\r\n\r\nbeta") == 0,
	   "and runs no handler on the way");

	/* A name that begins with a site's name. A comparison that stopped at
	 * the pattern's length would hand `alpha.recon.example.com` to alpha,
	 * which is somebody else's machine answering for this one. */
	exchange(vhost_port, "GET /who HTTP/1.1\r\nHost: alpha.recon.example.com\r\n"
	                     "Connection: close\r\n\r\n", reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 421 Misdirected Request\r\n"),
	   "a longer name that starts with a site's name is not that site");

	/*
	 * No `Host` at all. Only HTTP/1.0 can get this far -- `request.c`
	 * refuses a 1.1 request without one -- and a chain with no default has
	 * nothing to give it. The head of the chain answering here would mean
	 * every nameless request landing on whichever site happened to be
	 * configured first.
	 */
	exchange(vhost_port, "GET /who HTTP/1.0\r\n\r\n", reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 421 Misdirected Request\r\n"),
	   "a request with no name does not fall to the first site");

	/*
	 * Two requests, two sites, one connection.
	 *
	 * This is the case that decides where the dispatch belongs. A server
	 * that picked the site when it accepted would answer both of these with
	 * the first one's site, and every single-request check above would
	 * still pass. Sent together rather than in turn, because the host's
	 * `recv` blocks and reading between them would stop on the second.
	 */
	{
		int fd = dial(vhost_port);
		const char *a, *b;

		ok(fd >= 0, "one connection for two sites");
		{
			/* Counted by the compiler rather than by hand. A length
			 * written out here and then edited is a second request
			 * that arrives truncated, and a check that fails for the
			 * suite's arithmetic rather than for the server. */
			static const char BOTH[] =
				"GET /who HTTP/1.1\r\nHost: alpha.recon\r\n\r\n"
				"GET /who HTTP/1.1\r\nHost: beta.recon\r\n"
				"Connection: close\r\n\r\n";

			send(fd, BOTH, sizeof(BOTH) - 1, 0);
		}
		slurp(fd, reply, sizeof(reply));
		close(fd);

		a = strstr(reply, "\r\n\r\nalpha");
		b = strstr(reply, "\r\n\r\nbeta");
		ok(a != 0 && b != 0,
		   "both sites answer down the one socket");
		ok(a != 0 && b != 0 && a < b,
		   "each request reaching the site it asked for, in order");
	}

	/* --- and the site with no name at all --------------------------------
	 *
	 * `port` is served by SITE, whose `host` is NULL. It must answer to
	 * anything, because that is what every server here did before virtual
	 * hosts existed and none of them was reconfigured. */
	exchange(port, "GET / HTTP/1.1\r\nHost: someone.else.example\r\n"
	               "Connection: close\r\n\r\n", reply, sizeof(reply));
	ok(starts_with(reply, "HTTP/1.1 200 OK\r\n"),
	   "a site with no name still answers to every name");

	kill(child, SIGTERM);
	waitpid(child, 0, 0);
	close(listener);

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
