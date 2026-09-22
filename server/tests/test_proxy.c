/*
 * Passing a request to another machine, and the headers that must not go with it.
 *
 * --- What this suite is mostly about ---
 *
 * A proxy is two message parsers pointed at each other, and the interesting
 * faults are not "does the body arrive". They are the ones where a client can
 * reach past this server and affect a connection it cannot see:
 *
 *   * a `Connection:` header forwarded upstream, which lets a client close
 *     somebody else's link;
 *   * a `Transfer-Encoding:` forwarded alongside the `Content-Length` this
 *     server writes -- a message framed two ways, which is the exact shape
 *     `request.c` refuses on the way in, constructed on the way out by the
 *     thing that refuses it;
 *   * a decoded path put back on the wire unencoded, where a space ends the
 *     request line early and the rest becomes a second request.
 *
 * So there are two kinds of check here. The first read the request the
 * upstream actually received -- the fake upstream records it and hands it back
 * on a second connection, because asserting on what this server *meant* to
 * send would be asserting on this server's opinion of itself. The second are
 * ordinary round trips.
 *
 * Three parties: a fake upstream, the real `serve.c` with a proxying route,
 * and the parent as client.
 */

#include "../http/proxy.h"
#include "../http/serve.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
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

/* --- the clock and the yield this suite hands the proxy --------------------- */

static unsigned long host_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, 0);
	return (unsigned long)tv.tv_sec * 1000UL
	     + (unsigned long)tv.tv_usec / 1000UL;
}

static void host_wait(void)
{
	sched_yield();
}

/* --- the fake upstream ------------------------------------------------------ */

/*
 * Shared with the child, because the request it received is the thing most of
 * these checks are about and it has to cross a process boundary to be read.
 */
struct seen {
	char   request[4096];
	size_t len;
	int    connections;
};

static struct seen *SEEN;
static unsigned UPSTREAM_PORT;

/* What the upstream answers. Set before the child forks. */
static int UPSTREAM_MODE;
#define MODE_NORMAL   0
#define MODE_SILENT   1	/* accepts and never answers */
#define MODE_GARBAGE  2	/* answers something that is not HTTP */
#define MODE_MANY     3	/* answers with more headers than a sink can hold */

/* `write` is warn_unused_result and a cast does not satisfy it. The value is
 * genuinely unwanted here -- this is a fake upstream talking to a socket it is
 * about to close -- so it is taken and discarded rather than hidden. */
static void say(int fd, const char *text, size_t len)
{
	long got = write(fd, text, len);

	(void)got;
}

static void upstream_child(int listener)
{
	for (;;) {
		int c = accept(listener, 0, 0);
		char buf[4096];
		size_t at = 0;
		ssize_t n;

		if (c < 0)
			continue;

		/* Read the head. The client always sends Connection: close and
		 * a bounded body, so this reads until the blank line. */
		while (at + 1 < sizeof(buf)) {
			n = read(c, buf + at, sizeof(buf) - 1 - at);
			if (n <= 0)
				break;
			at += (size_t)n;
			buf[at] = '\0';
			if (strstr(buf, "\r\n\r\n"))
				break;
		}

		if (at && at < sizeof(SEEN->request)) {
			memcpy(SEEN->request, buf, at);
			SEEN->len = at;
		}
		SEEN->connections++;

		switch (UPSTREAM_MODE) {
		case MODE_SILENT:
			/* Accept and say nothing. The proxy's deadline is what
			 * has to end this, which is the point of the case. */
			sleep(30);
			break;
		case MODE_GARBAGE:
			say(c, "NOT-HTTP AT ALL\r\n\r\n", 19);
			break;
		case MODE_MANY: {
			char many[2048];
			int used = snprintf(many, sizeof(many),
			                    "HTTP/1.1 200 OK\r\n"
			                    "Content-Type: text/plain\r\n");
			int k;

			for (k = 0; k < 40; k++)
				used += snprintf(many + used,
				                 sizeof(many) - (size_t)used,
				                 "X-Pad-%d: v\r\n", k);
			used += snprintf(many + used, sizeof(many) - (size_t)used,
			                 "\r\nbody\n");
			say(c, many, (size_t)used);
			break;
		}
		default: {
			/*
			 * Length from the literal rather than counted by hand.
			 * The first draft said 138 and the string is 135, which
			 * the compiler refused -- reading past the end of it. A
			 * number that has to agree with a string beside it is
			 * two things that can disagree, and this one already
			 * had.
			 */
			static const char answer[] =
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: text/plain\r\n"
				"X-Upstream: yes\r\n"
				"Connection: keep-alive\r\n"
				"Transfer-Encoding: chunked\r\n"
				"\r\n"
				"hello from upstream\n";

			say(c, answer, sizeof(answer) - 1);
			break;
		}
		}
		close(c);
	}
}

/* --- the route under test --------------------------------------------------- */

static int stream_proxy(const struct http_request *r, const char *body,
                        size_t body_len, struct http_sink *sink, void *ctx)
{
	struct http_upstream up;

	(void)ctx;

	memset(&up, 0, sizeof(up));
	up.addr = 0x7F000001u;		/* 127.0.0.1, host order */
	up.port = (unsigned short)UPSTREAM_PORT;
	up.host = "upstream.example";
	up.now_ms = host_ms;
	up.wait = host_wait;

	return http_proxy(r, body, body_len, sink, &up);
}

/* Somewhere nothing is listening, so the dial is refused rather than timing
 * out -- the case where the wire guarantees an event. See VF-045. */
static int stream_nowhere(const struct http_request *r, const char *body,
                          size_t body_len, struct http_sink *sink, void *ctx)
{
	struct http_upstream up;

	(void)ctx;

	memset(&up, 0, sizeof(up));
	up.addr = 0x7F000001u;
	up.port = 9;			/* discard, and nothing serves it here */
	up.host = "nowhere.example";
	up.now_ms = host_ms;
	up.wait = host_wait;

	return http_proxy(r, body, body_len, sink, &up);
}

static const struct http_route ROUTES[] = {
	{ .method = "GET", .prefix = "/up", .stream = stream_proxy },
	{ .method = "POST", .prefix = "/up", .stream = stream_proxy },
	{ .method = "GET", .prefix = "/nowhere", .exact = 1,
	  .stream = stream_nowhere },
};

static void unblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static struct http_site SITE = {
	.routes = ROUTES,
	.route_count = sizeof(ROUTES) / sizeof(ROUTES[0]),
	.server_name = "ReconOS/proxy",
	.unblock = unblock
};

/* --- the client ------------------------------------------------------------- */

static int listen_somewhere(unsigned *port)
{
	struct sockaddr_in addr;
	socklen_t len = sizeof(addr);
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;

	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(0x7F000001);
	addr.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0
	    || listen(fd, 16) < 0
	    || getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
		close(fd);
		return -1;
	}
	*port = ntohs(addr.sin_port);
	return fd;
}

static size_t fetch(unsigned port, const char *request, char *into, size_t room)
{
	struct sockaddr_in addr;
	struct timeval tv;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	size_t at = 0;

	into[0] = '\0';
	if (fd < 0)
		return 0;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(0x7F000001);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return 0;
	}

	/*
	 * A deadline, and it is load-bearing for the same reason
	 * `test_http_stream.c` gives: the faults here present as waiting for
	 * ever, and a suite that can hang is a suite that gets disabled.
	 * Longer than the others because one case deliberately waits on a
	 * silent upstream.
	 */
	tv.tv_sec = 12;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	send(fd, request, strlen(request), 0);
	while (at + 1 < room) {
		ssize_t n = recv(fd, into + at, room - 1 - at, 0);

		if (n <= 0)
			break;
		at += (size_t)n;
	}
	into[at] = '\0';
	close(fd);
	return at;
}

/*
 * Count a header, at a line start, followed by a colon, and **case
 * insensitively** -- which is not a nicety.
 *
 * `request.c` lower-cases every field name it parses, and this proxy
 * lower-cases every one it relays, because a name is case-insensitive and
 * storing it two ways is how a duplicate gets past a duplicate check. So
 * `X-Client` goes out as `x-client`. A case-sensitive check here reported
 * *an ordinary client header is forwarded* as failing when the header was
 * there in full -- the suite disagreeing with HTTP rather than with the code.
 *
 * Counting rather than answering yes/no, because one of the checks below is
 * about a header appearing **twice**, which a boolean cannot see.
 */
static int count_in(const char *text, const char *end, const char *name)
{
	size_t n = strlen(name);
	const char *at = text;
	int found = 0;

	while (*at) {
		if ((at == text || at[-1] == '\n')
		    && (!end || at < end)
		    && strncasecmp(at, name, n) == 0 && at[n] == ':')
			found++;
		if (!end || at < end) {
			at++;
			continue;
		}
		break;
	}
	return found;
}

/* Only before the blank line: a body is arbitrary bytes, which is the trap
 * `test_http_stream.c` records. */
static int head_count(const char *reply, const char *name)
{
	const char *end = strstr(reply, "\r\n\r\n");

	return end ? count_in(reply, end, name) : 0;
}

static int head_has(const char *reply, const char *name)
{
	return head_count(reply, name) > 0;
}

/* The same, against what the upstream received. */
static int upstream_sent(const char *name)
{
	return count_in(SEEN->request, 0, name) > 0;
}

int main(void)
{
	char reply[8192];
	pid_t up_child, server_child;
	int up_listener, listener;
	unsigned port;

	printf("passing a request on, and the headers that must not go with it\n");

	/* --- the two that need no sockets at all ------------------------------ */
	{
		/*
		 * Checked against RFC 7230 6.1 rather than against the
		 * implementation, which is the point of exporting it: a list
		 * that only agreed with itself would pass while missing one.
		 */
		ok(http_proxy_hop_by_hop("connection"), "Connection stops here");
		ok(http_proxy_hop_by_hop("keep-alive"), "Keep-Alive stops here");
		ok(http_proxy_hop_by_hop("proxy-authenticate"),
		   "Proxy-Authenticate stops here");
		ok(http_proxy_hop_by_hop("proxy-authorization"),
		   "Proxy-Authorization stops here");
		ok(http_proxy_hop_by_hop("te"), "TE stops here");
		ok(http_proxy_hop_by_hop("trailer"), "Trailer stops here");
		ok(http_proxy_hop_by_hop("transfer-encoding"),
		   "Transfer-Encoding stops here");
		ok(http_proxy_hop_by_hop("upgrade"), "Upgrade stops here");

		ok(!http_proxy_hop_by_hop("content-type"),
		   "and an ordinary header does not");
		ok(!http_proxy_hop_by_hop("authorization"),
		   "nor Authorization, which is the message's and not the "
		   "connection's -- the one most easily confused with "
		   "Proxy-Authorization");
		ok(!http_proxy_hop_by_hop(""), "nor an empty name");
		ok(!http_proxy_hop_by_hop(0), "nor no name at all");
	}

	{
		char out[64];

		ok(http_proxy_encode_path("/a/b", out, sizeof(out)) == 4
		   && strcmp(out, "/a/b") == 0,
		   "an ordinary path is unchanged");

		ok(http_proxy_encode_path("/a b", out, sizeof(out)) == 6
		   && strcmp(out, "/a%20b") == 0,
		   "a space is encoded -- unencoded it ends the request line "
		   "and the rest becomes a second request");

		ok(http_proxy_encode_path("/a%b", out, sizeof(out)) == 6
		   && strcmp(out, "/a%25b") == 0,
		   "and a percent is encoded, so a decoded one cannot become "
		   "an escape upstream");

		/* The limitation `proxy.h` names, asserted so that it is a
		 * recorded fact rather than a surprise: this is what the
		 * parser has already lost. */
		ok(http_proxy_encode_path("/a/b", out, sizeof(out)) == 4,
		   "a slash that was `%2F` is indistinguishable from one that "
		   "was not, by the time this is called -- see proxy.h");

		ok(http_proxy_encode_path("/aaaaaaaaaa", out, 4) == -1,
		   "and a path that will not fit is refused, not cut");
	}

	/* --- now the sockets --------------------------------------------------- */
	SEEN = mmap(0, sizeof(*SEEN), PROT_READ | PROT_WRITE,
	            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (SEEN == MAP_FAILED) {
		printf("  FAIL  no shared page\n");
		return 1;
	}
	memset(SEEN, 0, sizeof(*SEEN));

	up_listener = listen_somewhere(&UPSTREAM_PORT);
	if (up_listener < 0) {
		printf("  FAIL  could not listen for the upstream\n");
		return 1;
	}

	/*
	 * The server's listener is `http_listen`, not the one above, because
	 * the thing under test is `serve.c` and it should be handed the socket
	 * it is written for.
	 */
	port = 18600;
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
	 * And non-blocking, the way it already is on the kernel this targets.
	 *
	 * `serve.c` holds several connections at once and must be able to ask
	 * "is anyone new waiting?" and be told no. On ReconOS `accept` answers
	 * EAGAIN by itself; the host is the odd one out, and a blocking
	 * `accept` here stops the whole loop. Leaving this out is what made
	 * every socket check in this suite fail with an empty reply on the
	 * first run -- the server was blocked in `accept` on its own listener
	 * and never reached the request already sitting in front of it.
	 */
	fcntl(listener, F_SETFL, fcntl(listener, F_GETFL, 0) | O_NONBLOCK);

	up_child = fork();
	if (up_child == 0) {
		close(listener);
		upstream_child(up_listener);
		_exit(0);
	}

	server_child = fork();
	if (server_child == 0) {
		close(up_listener);
		alarm(60);	/* an orphan that answers is worse than one
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

	/* --- a round trip ------------------------------------------------------ */
	{
		memset(SEEN, 0, sizeof(*SEEN));
		fetch(port, "GET /up/page HTTP/1.1\r\nHost: front\r\n"
		            "X-Client: yes\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));

		ok(strncmp(reply, "HTTP/1.1 200", 12) == 0,
		   "the upstream's status comes back");
		ok(strstr(reply, "hello from upstream") != 0,
		   "and its body");
		ok(head_has(reply, "X-Upstream"),
		   "and its headers");
		ok(head_has(reply, "Content-Type"),
		   "and its content type");
	}

	/* --- what the upstream was actually sent ------------------------------- */
	{
		ok(SEEN->len > 0, "the upstream received something");
		ok(strncmp(SEEN->request, "GET /up/page HTTP/1.1\r\n", 23) == 0,
		   "the method and path are passed on unchanged");
		ok(upstream_sent("Host"),
		   "with a Host header");
		ok(strstr(SEEN->request, "Host: upstream.example") != 0,
		   "and it is the one the caller chose, not the client's");
		ok(strstr(SEEN->request, "Host: front") == 0,
		   "the client's Host does not reach it -- two Host headers is "
		   "one request that names two servers");
		ok(upstream_sent("X-Client"),
		   "an ordinary client header is forwarded");
	}

	/* --- the ones that must not be forwarded -------------------------------- */
	{
		memset(SEEN, 0, sizeof(*SEEN));
		fetch(port, "GET /up/x HTTP/1.1\r\nHost: front\r\n"
		            "Connection: close\r\n"
		            "TE: trailers\r\n"
		            "Upgrade: websocket\r\n"
		            "Proxy-Authorization: Basic zzz\r\n\r\n",
		      reply, sizeof(reply));

		ok(!upstream_sent("TE"),
		   "TE does not reach the upstream");
		ok(!upstream_sent("Upgrade"),
		   "nor Upgrade");
		ok(!upstream_sent("Proxy-Authorization"),
		   "nor Proxy-Authorization");
		/*
		 * `Connection: close` IS sent -- written by the proxy itself,
		 * and it is how the body ends, because a zero from `read` means
		 * nothing buffered rather than end of stream on the target.
		 * What must not happen is the client's value being used.
		 */
		ok(strstr(SEEN->request, "Connection: close") != 0,
		   "the proxy writes its own Connection: close, which is how "
		   "it knows the body ended");
	}

	/* --- and the ones from the upstream that must not come back ------------- */
	{
		fetch(port, "GET /up/y HTTP/1.1\r\nHost: front\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));

		/*
		 * **Exactly one**, not zero, and the first draft of this check
		 * asked for zero and was wrong.
		 *
		 * The proxy does not know the body's length when the head goes
		 * out, so the sink frames the response chunked and writes
		 * `Transfer-Encoding: chunked` itself. That one is correct and
		 * must be there. What must NOT happen is the upstream's copy
		 * arriving beside it -- two framing headers on one message,
		 * which is the shape `request.c` refuses on the way in.
		 *
		 * Both say `chunked`, so they cannot be told apart by value.
		 * Counting is the only thing that distinguishes "the server
		 * framed this" from "the server framed this twice".
		 */
		ok(head_count(reply, "Transfer-Encoding") == 1,
		   "the response carries exactly one Transfer-Encoding -- the "
		   "server's own, with the upstream's not relayed beside it");
		ok(!head_has(reply, "Keep-Alive"),
		   "nor its Keep-Alive");
	}

	/* --- a body, forwarded by length ---------------------------------------- */
	{
		memset(SEEN, 0, sizeof(*SEEN));
		fetch(port, "POST /up/write HTTP/1.1\r\nHost: front\r\n"
		            "Content-Type: text/plain\r\n"
		            "Content-Length: 11\r\n"
		            "Connection: close\r\n\r\nhello there",
		      reply, sizeof(reply));

		ok(strstr(SEEN->request, "Content-Length: 11") != 0,
		   "a body is announced by length");
		ok(strstr(SEEN->request, "hello there") != 0,
		   "and the bytes arrive");
		ok(strstr(SEEN->request, "Transfer-Encoding") == 0,
		   "and never alongside a second framing, which is the "
		   "message request.c refuses on the way in");
	}

	/* --- an upstream that is not there --------------------------------------- */
	{
		fetch(port, "GET /nowhere HTTP/1.1\r\nHost: front\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strncmp(reply, "HTTP/1.1 502", 12) == 0,
		   "an unreachable upstream is 502 -- not 500, because this "
		   "server has not failed");
	}

	/* --- an upstream that answers nonsense ----------------------------------- */
	{
		kill(up_child, SIGTERM);
		waitpid(up_child, 0, 0);
		UPSTREAM_MODE = MODE_GARBAGE;
		up_child = fork();
		if (up_child == 0) {
			close(listener);
			upstream_child(up_listener);
			_exit(0);
		}

		fetch(port, "GET /up/z HTTP/1.1\r\nHost: front\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strncmp(reply, "HTTP/1.1 502", 12) == 0,
		   "an upstream that does not speak HTTP is 502, rather than "
		   "its nonsense being passed to the client");
	}

	/* --- an upstream with more headers than a sink can carry ----------------- */
	{
		kill(up_child, SIGTERM);
		waitpid(up_child, 0, 0);
		UPSTREAM_MODE = MODE_MANY;
		up_child = fork();
		if (up_child == 0) {
			close(listener);
			upstream_child(up_listener);
			_exit(0);
		}

		fetch(port, "GET /up/many HTTP/1.1\r\nHost: front\r\n"
		            "Connection: close\r\n\r\n", reply, sizeof(reply));
		ok(strncmp(reply, "HTTP/1.1 502", 12) == 0,
		   "more headers than can be carried is refused, not "
		   "truncated -- a response missing headers nobody chose to "
		   "drop is a wrong answer that looks like a right one");
	}

	kill(up_child, SIGTERM);
	kill(server_child, SIGTERM);
	waitpid(up_child, 0, 0);
	waitpid(server_child, 0, 0);
	close(up_listener);
	close(listener);

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
