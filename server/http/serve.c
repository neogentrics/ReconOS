/*
 * The socket half, kept as small as it can be.
 *
 * Every decision about what a request *means* is in `request.c`, which has no
 * sockets in it and is tested exhaustively. This file moves bytes and dispatches
 * to a handler, and is the part that cannot be tested by handing it a string.
 *
 * --- One source, two systems ---
 *
 * This compiles unchanged against ReconOS's C library and against the host's,
 * because it calls only `socket`, `bind`, `listen`, `accept`, `recv`, `send`
 * and `close` -- the names are POSIX's and ReconOS's library answers them over
 * the five system calls the kernel took numbers for. `server/tests/
 * test_http_serve.c` runs this exact file over the host's loopback.
 *
 * That is worth the discipline it costs. The alternative is a stand-in for the
 * network, and a stand-in is looser than the real call in exactly the places a
 * server breaks.
 *
 * --- nothing blocks on this kernel, and that is what makes the pool possible ---
 *
 * There is no wait queue on a listener, so `accept` answers `EAGAIN` when
 * nobody is waiting. `recv` is the same: it answers 0 with `errno` 0 when
 * nothing has arrived. Both are already non-blocking, which is why this file
 * can hold several connections and give each a turn without `poll` or
 * `select`, neither of which exists.
 *
 * **That was not understood for a long time.** `recv`'s zero was read as a
 * closed connection, which silently dropped every request over about 4 KiB
 * (VF-013), while `docs/WEB.md` recorded that concurrency was blocked on the
 * kernel gaining exactly the capability it already had (VF-014).
 *
 * A host is the odd one out: there both calls block, and a blocking call stops
 * the whole loop including the connections that would have freed it. So the
 * host suites make their sockets non-blocking with `fcntl`, which this file
 * cannot call because ReconOS does not have it -- the site supplies it through
 * the `unblock` hook instead. One loop, both systems, and the one call that
 * differs lives on the side that knows which system it is.
 */

#include "serve.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>

/* --- small helpers ------------------------------------------------------ */

/* Case-insensitive against a lower-case literal. Header values are compared
 * without regard to case, and `Expect: 100-Continue` is a spelling clients
 * really send. */
static int seq_fold(const char *a, const char *lower_b)
{
	size_t i = 0;

	while (a[i] && lower_b[i]) {
		char c = (a[i] >= 'A' && a[i] <= 'Z')
			? (char)(a[i] - 'A' + 'a') : a[i];

		if (c != lower_b[i])
			return 0;
		i++;
	}
	/* Both ended together, or one is a prefix of the other. */
	return a[i] == 0 && lower_b[i] == 0;
}

static int prefix_matches(const char *path, const char *prefix, int exact)
{
	size_t i = 0;

	while (prefix[i] && path[i] && path[i] == prefix[i])
		i++;
	if (prefix[i] != '\0')
		return 0;
	if (exact)
		return path[i] == '\0';

	/* A prefix match must end on a boundary. Without this, a route for
	 * `/api` would also claim `/apifoo`, which is a different resource
	 * with a similar spelling -- the same class of fault as a name that
	 * folds two ways. */
	return path[i] == '\0' || path[i] == '/';
}

/*
 * Send the whole buffer, or fail.
 *
 * `send` may take fewer bytes than it was offered. Treating a short write as
 * success is how a response arrives truncated and a client sees a body that
 * does not match its own Content-Length -- which, on a keep-alive connection,
 * desynchronises everything after it.
 */
static int send_all(int fd, const char *buf, size_t len,
                    unsigned long *counter)
{
	size_t sent = 0;

	int stalls = 0;

	while (sent < len) {
		size_t want = len - sent;
		long n;

		/* Never more than the kernel transmits in one segment. See
		 * HTTP_SEND_CHUNK for the measurement that forced this. */
		if (want > HTTP_SEND_CHUNK)
			want = HTTP_SEND_CHUNK;

		n = send(fd, buf + sent, want, 0);

		if (n > 0) {
			sent += (size_t)n;
			if (counter)
				*counter += (unsigned long)n;
			stalls = 0;
			continue;
		}
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;

		/* Zero taken, with bytes still to go.
		 *
		 * On this kernel that means the connection's send buffer is
		 * full and nothing has been acknowledged yet. It is temporary
		 * and it is not an error -- but it is only temporary if the
		 * far end is still reading, so this gives up rather than
		 * spinning forever against a peer that has stopped. A bounded
		 * retry is the difference between a slow client and a wedged
		 * server. */
		if (n == 0 && stalls < 4096) {
			stalls++;
			continue;
		}
		return -1;
	}
	return 0;
}

/*
 * How long a request that has stopped arriving is waited for.
 *
 * --- Why there is a bound at all ---
 *
 * A client that sends half a request and stops would otherwise hold a slot for
 * ever, and it does not have to be malicious to do it -- a laptop closing its
 * lid does the same thing.
 *
 * Since the connection pool this is no longer the whole machine, which is the
 * point of the pool. It is still a slot out of `HTTP_CONNS_MAX`, and enough
 * quiet clients would still take all of them.
 *
 * --- Why fifteen seconds, and what it actually buys on this kernel ---
 *
 * Not much, and the number is honest about that. **Measured**: a request sent
 * as one burst stalls after about 2880 bytes and then arrives at roughly a
 * kilobyte a second, because the bytes beyond that are dropped and have to be
 * retransmitted. 3700 bytes took 5.25s; 20000 took 20.1s; 60000 took 64.5s.
 *
 * So fifteen seconds admits an upload of something like fifteen kilobytes,
 * against an `HTTP_BODY_MAX` of 64 KiB. **That gap is a kernel fault and not a
 * design**, it is filed in `docs/KERNEL-WANTS.md`, and it is written here
 * rather than rounded off because the alternative is a limit nobody can
 * explain.
 *
 * The same request paced by its sender -- 400 bytes every 20 milliseconds --
 * arrives at full size and full speed. The size was never the limit.
 */
#define RECV_DEADLINE_MS 15000

/*
 * The fallback bound, for a site with no clock.
 *
 * An attempt count is not a duration and this file says so rather than
 * pretending otherwise. It exists for the host suites, where `recv` blocks
 * properly and a stalled read never happens -- so this is the bound that is
 * never reached, kept only so that a site without a clock cannot spin for
 * ever.
 */
#define RECV_STALLS_MAX 2000000

/*
 * Read more of a request, knowing what a zero means on this kernel.
 *
 * --- The fault this exists for ---
 *
 * **`recv` answers 0 with `errno` 0 when nothing has arrived yet.** Not on a
 * closed connection -- on a connection that is open, with more bytes on the
 * way. Measured: a 5000-byte upload gave `have=2880 need=5414 n=0 errno=0`,
 * and the loop that called it read that zero as end-of-stream and dropped the
 * connection without answering.
 *
 * Every request over about 4 KiB failed that way. `curl` reported an empty
 * reply, the server logged nothing because nothing was answered, and the
 * documented 64 KiB body limit had never been reachable.
 *
 * **The same file already knew this.** `send_all`, five hundred lines up,
 * carries a stall counter and a comment explaining that a zero from `send`
 * means the buffer is full rather than that anything is wrong. The receive
 * side is the mirror image and never got the same treatment -- the fault was
 * understood in one direction and not looked for in the other.
 *
 * --- Why a zero is not always retried ---
 *
 * On an idle keep-alive connection a zero means exactly what it appears to:
 * no further request is coming. Retrying there would spin `RECV_STALLS_MAX`
 * times on every connection a browser leaves open, which is most of them.
 *
 * So the caller says whether a request is in progress. Mid-request, a zero is
 * *not yet*; between requests, it is *nothing more*. The same byte, and the
 * difference is context the reader has and the kernel does not.
 *
 * Returns 1 to continue, 0 when the connection is finished, and -1 when the
 * deadline passed with a request half-arrived.
 *
 * Those last two are deliberately not the same answer. A client that closed has
 * nothing to be told; a client that went quiet mid-request is still there, is
 * owed a 408, and is an entry the log should carry. Collapsing them is how a
 * request that was cut off leaves no trace anywhere -- which it did, until a
 * 60000-byte upload was abandoned at the deadline and nothing in the log said
 * so.
 */
static int read_more(int fd, char *buf, size_t *have, size_t room,
                     int in_progress, unsigned long *stalls,
                     unsigned long since, const struct http_site *site)
{
	long n;

	errno = 0;
	n = recv(fd, buf + *have, room - *have, 0);

	if (n > 0) {
		*have += (size_t)n;
		*stalls = 0;
		return 1;
	}
	if (n < 0 && (errno == EINTR || errno == EAGAIN))
		return 1;
	if (n != 0 || !in_progress)
		return 0;

	/* Nothing taken, and there is known to be more. See above. */
	if (site->now_ms) {
		if (site->now_ms() - since >= RECV_DEADLINE_MS)
			return -1;
	} else if (*stalls >= RECV_STALLS_MAX) {
		return -1;
	}
	(*stalls)++;

	/*
	 * Give the rest of the system a turn before asking again.
	 *
	 * On its own this does not rescue a stalled burst -- that waits on a
	 * retransmission and no amount of yielding hurries it. What it does is
	 * stop a server that is waiting from consuming a whole processor to do
	 * it, which matters when the thing it is waiting for is the network
	 * stack on the same machine.
	 */
	if (site->idle)
		site->idle();
	return 1;
}

/* --- what every response carries ------------------------------------------ */

/*
 * Headers written on every response, by the server rather than by a handler.
 *
 * They are site-wide facts, not per-response decisions, so a handler cannot
 * forget one and a new handler gets them without knowing they exist. That is
 * the point: a security header that each handler must remember is a security
 * header that the next handler will not have.
 *
 * **`nosniff`** stops a browser guessing a type from content. This server
 * decides the type from the file's extension -- a statement by whoever put the
 * file there -- and sniffing overrides that with a statement by whoever wrote
 * the *contents*. A text file a user uploaded that happens to begin with a tag
 * is the whole of that attack.
 *
 * **`DENY`** keeps the console out of a frame on somebody else's page, where a
 * click aimed at their button lands on the console's. It is an administrative
 * interface with a form that changes the machine's name; there is no reason it
 * should ever be framed.
 *
 * **`no-referrer`** stops the console's paths leaking to anything it links to.
 * An internal address in a Referer header is a small map of the network handed
 * to whatever the link pointed at.
 *
 * --- The policy, and why it can finally be sent ---
 *
 * This comment used to say CSP was absent and why: the dashboard was built from
 * inline `style=` attributes and a `<style>` block, so any policy shippable
 * then needed `'unsafe-inline'` -- which permits exactly what CSP exists to
 * stop, while the header's presence suggests otherwise. The styles moved to
 * `/console.css`, so the policy below is true rather than decorative.
 *
 * Each directive, and what it is actually refusing:
 *
 *   `default-src 'none'`   nothing loads unless a directive below allows it.
 *                          Starting from nothing and permitting what is needed
 *                          is the only way round that fails closed; starting
 *                          from `'self'` and forbidding things means every
 *                          content type nobody thought about is permitted.
 *   `style-src 'self'`     the one stylesheet, from this machine. **No
 *                          `'unsafe-inline'`** -- that is the whole point, and
 *                          it means a `style=` attribute added to a page later
 *                          stops working rather than silently reopening this.
 *   `form-action 'self'`   the rename form may post here and nowhere else. A
 *                          page that has been tampered with cannot retarget it
 *                          at somebody else's collector.
 *   `frame-ancestors 'none'` the same ground as `X-Frame-Options`, in the
 *                          header that superseded it. Both are sent: the older
 *                          one for anything that does not read this.
 *   `base-uri 'none'`      no `<base>` may be injected to re-point every
 *                          relative URL on the page, which is how an injection
 *                          that cannot add a script still redirects a form.
 *
 * **No `script-src`, because `default-src 'none'` already refuses scripts** and
 * this console has none. If one is ever added, adding a directive for it is a
 * deliberate act rather than something that quietly already worked.
 */
#define SECURITY_HEADERS \
	"X-Content-Type-Options: nosniff\r\n" \
	"X-Frame-Options: DENY\r\n" \
	"Referrer-Policy: no-referrer\r\n" \
	"Content-Security-Policy: default-src 'none'; style-src 'self'; " \
	"form-action 'self'; frame-ancestors 'none'; base-uri 'none'\r\n"

/* --- the response ------------------------------------------------------- */

void http_response_simple(struct http_response *out, int status,
                          const char *content_type,
                          const char *body, size_t body_len)
{
	out->status = status;
	out->content_type = content_type;
	out->body = body;
	out->body_len = body_len;
	out->close = 0;
	out->extra_name = 0;
	out->extra_value = 0;
}

/*
 * Write the head and the body.
 *
 * `Content-Length` is always present and always the real length, including on
 * a HEAD, where the body is not sent but its length is still the truth about
 * the resource. A HEAD that reported zero would be a different answer from the
 * GET it is supposed to mirror.
 */
static int send_response(int fd, const struct http_request *req,
                         const struct http_response *res,
                         const char *server_name, int keep_alive,
                         int head_only, unsigned long *counter)
{
	char head[2048];
	int n;

	(void)req;

	n = snprintf(head, sizeof(head),
	             "HTTP/1.1 %d %s\r\n"
	             "Server: %s\r\n"
	             "Content-Length: %lu\r\n"
	             "Connection: %s\r\n"
	             SECURITY_HEADERS,
	             res->status, http_reason(res->status),
	             server_name ? server_name : "ReconOS",
	             (unsigned long)res->body_len,
	             keep_alive ? "keep-alive" : "close");
	if (n < 0 || (size_t)n >= sizeof(head))
		return -1;

	if (res->content_type && res->body_len > 0) {
		int m = snprintf(head + n, sizeof(head) - (size_t)n,
		                 "Content-Type: %s\r\n", res->content_type);
		if (m < 0 || (size_t)(n + m) >= sizeof(head))
			return -1;
		n += m;
	}

	if (res->extra_name && res->extra_value) {
		int m = snprintf(head + n, sizeof(head) - (size_t)n,
		                 "%s: %s\r\n", res->extra_name,
		                 res->extra_value);
		if (m < 0 || (size_t)(n + m) >= sizeof(head))
			return -1;
		n += m;
	}

	if ((size_t)n + 2 >= sizeof(head))
		return -1;
	head[n++] = '\r';
	head[n++] = '\n';

	if (send_all(fd, head, (size_t)n, counter) != 0)
		return -1;
	if (head_only || res->body_len == 0)
		return 0;
	return send_all(fd, res->body, res->body_len, counter);
}

/* --- streaming ------------------------------------------------------------ */

int http_stream_header(struct http_sink *sink, const char *name,
                       const char *value)
{
	if (!sink || !name || !value)
		return HTTP_EMALFORMED;

	/* Late is refused, not ignored. After `begin` the head is on the wire,
	 * and a header written then lands in the body -- which is a corrupt
	 * response rather than a missing header, and much harder to notice. */
	if (sink->begun)
		return HTTP_EMALFORMED;

	if (sink->extras >= HTTP_SINK_EXTRA_MAX)
		return HTTP_ETOOMANY;

	sink->extra[sink->extras].name = name;
	sink->extra[sink->extras].value = value;
	sink->extras++;
	return HTTP_OK;
}

int http_stream_begin(struct http_sink *sink, int status,
                      const char *content_type, long length)
{
	char head[2048];
	int n;

	if (!sink || sink->begun)
		return -1;
	sink->begun = 1;
	sink->declared = length;
	sink->status = status;

	/*
	 * Choose the framing, and it is the client's version that decides.
	 *
	 * Chunked arrived in HTTP/1.1. A 1.0 client handed a chunked body reads
	 * the hex sizes as part of the content, so an unknown length there has
	 * exactly one honest framing: write the bytes and close, and say so in
	 * the head. That costs the connection, which is the price of not
	 * knowing the size.
	 */
	sink->chunked = (length == HTTP_LENGTH_UNKNOWN && sink->minor >= 1);
	if (length == HTTP_LENGTH_UNKNOWN && !sink->chunked)
		sink->keep_alive = 0;

	n = snprintf(head, sizeof(head),
	             "HTTP/1.1 %d %s\r\nServer: %s\r\nConnection: %s\r\n"
	             SECURITY_HEADERS,
	             status, http_reason(status),
	             sink->server_name ? sink->server_name : "ReconOS",
	             sink->keep_alive ? "keep-alive" : "close");
	if (n < 0 || (size_t)n >= sizeof(head))
		return -1;

	if (length >= 0) {
		int m = snprintf(head + n, sizeof(head) - (size_t)n,
		                 "Content-Length: %lu\r\n",
		                 (unsigned long)length);
		if (m < 0 || (size_t)(n + m) >= sizeof(head))
			return -1;
		n += m;
	} else if (sink->chunked) {
		int m = snprintf(head + n, sizeof(head) - (size_t)n,
		                 "Transfer-Encoding: chunked\r\n");
		if (m < 0 || (size_t)(n + m) >= sizeof(head))
			return -1;
		n += m;
	}

	if (content_type) {
		int m = snprintf(head + n, sizeof(head) - (size_t)n,
		                 "Content-Type: %s\r\n", content_type);
		if (m < 0 || (size_t)(n + m) >= sizeof(head))
			return -1;
		n += m;
	}

	{
		size_t i;

		for (i = 0; i < sink->extras; i++) {
			int m = snprintf(head + n, sizeof(head) - (size_t)n,
			                 "%s: %s\r\n", sink->extra[i].name,
			                 sink->extra[i].value);

			/* A head that does not fit is refused rather than cut.
			 * A header truncated mid-value is a different header,
			 * and one truncated mid-line ends the head early --
			 * which makes the rest of it arrive as the body. */
			if (m < 0 || (size_t)(n + m) >= sizeof(head))
				return -1;
			n += m;
		}
	}

	if ((size_t)n + 2 >= sizeof(head))
		return -1;
	head[n++] = '\r';
	head[n++] = '\n';

	if (send_all(sink->fd, head, (size_t)n, sink->bytes_sent) != 0) {
		sink->failed = 1;
		return -1;
	}
	return HTTP_OK;
}

int http_stream_write(struct http_sink *sink, const void *data, size_t len)
{
	if (!sink || !sink->begun)
		return -1;
	if (sink->failed)
		return -1;	/* already lost; say so but do nothing more */
	if (len == 0)
		return HTTP_OK;

	/* Counted whether or not it is sent, because on a HEAD the length is
	 * still the truth about the resource -- a HEAD that reported a
	 * different figure from the GET it mirrors would be a different
	 * answer to the same question. */
	sink->written += (unsigned long)len;

	/* A declared length that is about to be exceeded. Refused here rather
	 * than sent, because the bytes past the promise are read by the client
	 * as the beginning of the next response. */
	if (sink->declared >= 0
	    && sink->written > (unsigned long)sink->declared) {
		sink->failed = 1;
		return -1;
	}

	if (sink->head_only)
		return HTTP_OK;

	if (sink->chunked) {
		char size[32];
		int n = snprintf(size, sizeof(size), "%lx\r\n",
		                 (unsigned long)len);

		if (n < 0 || (size_t)n >= sizeof(size)) {
			sink->failed = 1;
			return -1;
		}
		if (send_all(sink->fd, size, (size_t)n, sink->bytes_sent) != 0
		    || send_all(sink->fd, (const char *)data, len,
		                sink->bytes_sent) != 0
		    || send_all(sink->fd, "\r\n", 2, sink->bytes_sent) != 0) {
			sink->failed = 1;
			return -1;
		}
		return HTTP_OK;
	}

	if (send_all(sink->fd, (const char *)data, len, sink->bytes_sent) != 0) {
		sink->failed = 1;
		return -1;
	}
	return HTTP_OK;
}

int http_stream_end(struct http_sink *sink)
{
	if (!sink)
		return -1;
	if (!sink->begun) {
		/* A handler that wrote nothing at all. Not this function's to
		 * answer -- the caller still owes the client a status. */
		return -1;
	}

	if (!sink->failed && sink->chunked && !sink->head_only) {
		if (send_all(sink->fd, "0\r\n\r\n", 5, sink->bytes_sent) != 0)
			sink->failed = 1;
	}

	/*
	 * The promise, checked.
	 *
	 * A handler that declared a length and wrote fewer bytes has left the
	 * client waiting for the rest -- and on a kept connection the client
	 * reads the next response's head as this body's tail, so every answer
	 * after it is wrong. There is no way to fix that from here: the head
	 * is long gone. All this can do is refuse to reuse the connection, and
	 * say so.
	 */
	if (!sink->failed && sink->declared >= 0
	    && sink->written != (unsigned long)sink->declared)
		sink->failed = 1;

	if (sink->failed) {
		sink->keep_alive = 0;
		return HTTP_EMALFORMED;
	}
	return HTTP_OK;
}

/* An error answered before any handler ran. Kept deliberately plain: an error
 * page that reports what was wrong with the request tells an attacker which of
 * their probes the parser noticed. */
static int send_status(int fd, int status, const char *server_name,
                       unsigned long *counter)
{
	struct http_response res;
	char body[128];
	int n = snprintf(body, sizeof(body), "%d %s\n", status,
	                 http_reason(status));

	if (n < 0)
		return -1;
	http_response_simple(&res, status, "text/plain", body, (size_t)n);
	return send_response(fd, 0, &res, server_name, 0, 0, counter);
}

/*
 * Tell the site what was answered.
 *
 * Bytes are the difference in the counter the server was already keeping,
 * rather than a figure the caller works out -- so the log reports what actually
 * went on the wire, head included, and cannot disagree with `bytes_sent`.
 */
static void note(const struct http_site *site, const struct http_request *req,
                 int status, unsigned long before)
{
	unsigned long now;

	if (!site->log)
		return;

	now = site->bytes_sent ? *site->bytes_sent : 0;
	site->log(site->log_ctx, req, status, now - before);
}

/* --- connections, several at once ----------------------------------------
 *
 * --- Why there is a pool here and not a process per connection ---
 *
 * There is no `fork`. A connection cannot be handed to anything else, so
 * serving more than one at a time means holding more than one here.
 *
 * What makes that possible without `poll` or `select` -- neither of which
 * exists -- is the same behaviour that caused VF-013: on this kernel `accept`
 * answers `EAGAIN` with nobody waiting and `recv` answers 0 with nothing
 * buffered. Both are already non-blocking. A loop that asks each connection
 * for whatever it has and moves on is therefore the natural shape, not a
 * workaround for a missing call.
 *
 * --- What is concurrent, stated exactly ---
 *
 * **Reading is. Answering is not.**
 *
 * Reading is the part that waits: a request arriving in a burst can take
 * seconds, and until this existed those seconds belonged to nobody else. One
 * upload held the whole machine -- measured at 15 seconds, because that is
 * where `RECV_DEADLINE_MS` cut it off.
 *
 * Once a whole request is in hand, it is dispatched and answered without
 * interruption. That is deliberate rather than unfinished: a handler writes
 * into a response, or into a sink that goes straight to the socket, and making
 * either resumable would mean every handler becoming a state machine. The
 * answering is also the fast half -- handlers here read memory and `send_all`
 * is bounded.
 *
 * So a slow *client* no longer blocks anyone. A slow *handler* still would.
 * None of the handlers in this tree is slow, and the day one is, this comment
 * is where the next person should start.
 */

/*
 * How many connections are held at once.
 *
 * Each slot carries its own `HTTP_CONN_BUF`, so this is the multiplier on the
 * server's memory and not a free number. Four is chosen to be obviously
 * affordable rather than by measurement; nothing here has been under enough
 * load to justify a different one, and a number invented under no load is not
 * a tuning decision worth pretending to.
 *
 * A fifth client is not refused -- it waits in the listen backlog, which is
 * where a connection waits on every other server too.
 */
#define HTTP_CONNS_MAX 4

/* Which half of a request a connection is in the middle of. */
#define CONN_FREE  0
#define CONN_HEAD  1	/* reading the request head */
#define CONN_BODY  2	/* head parsed; reading the body it declared */

struct http_conn {
	int    fd;
	int    state;
	size_t have;		/* bytes in `buf` */
	size_t need;		/* head + body, once the head is parsed */
	int    requests;	/* answered on this connection so far */

	/* Where the current wait started, and how many empty reads it has
	 * taken. Reset when the head starts and again when the body does, so
	 * the deadline is on silence rather than on size -- a long request
	 * that keeps arriving is never cut off for being long. */
	unsigned long stalls;
	unsigned long since;

	/* What the byte counter said when this request started, so the log can
	 * report what this one cost. */
	unsigned long sent_before;

	struct http_request req;

	char buf[HTTP_CONN_BUF];
};

/*
 * The pool.
 *
 * Static rather than automatic: this is far more than a user process on this
 * system should assume it has in stack, and it must outlive any one call.
 */
static struct http_conn CONNS[HTTP_CONNS_MAX];

static void conn_drop(struct http_conn *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	c->state = CONN_FREE;
	c->have = 0;
	c->requests = 0;
}

/* Move whatever arrived after this request to the front, and go round again.
 * A client may pipeline, and those bytes are the next request. */
static void conn_next(struct http_conn *c)
{
	size_t used = c->req.head_length + c->req.content_length;
	size_t left = c->have - used;
	size_t k;

	for (k = 0; k < left; k++)
		c->buf[k] = c->buf[used + k];
	c->have = left;
	c->requests++;
	c->state = CONN_HEAD;
	c->stalls = 0;
}

/*
 * Answer one complete request.
 *
 * Everything from here to the end of the function ran inside the old
 * `serve_connection` loop and is unchanged in substance: the request is whole,
 * in `c->buf`, and this decides what to send back.
 *
 * Returns 1 to keep the connection, 0 to close it.
 */
static int conn_answer(struct http_conn *c, const struct http_site *site)
{
	struct http_request *req = &c->req;
	struct http_response res;
	char *buf = c->buf;
	int fd = c->fd;
	unsigned long sent_before = c->sent_before;
	int verdict, keep, head_only, i, matched = 0;

	head_only = (strcmp(req->method, "HEAD") == 0);
	keep = req->keep_alive;

	/* A bound on requests per connection. Not a performance decision: a
	 * connection that is never closed is a descriptor that is never given
	 * back, and this server has no timer to close an idle one with. */
	if (c->requests >= 64)
		keep = 0;

	/* Dispatch. A HEAD is routed as the GET it mirrors, so a site
	 * never has to write each handler twice. */
	http_response_simple(&res, 404, "text/plain", "404 Not Found\n", 14);
	for (i = 0; (size_t)i < site->route_count; i++) {
		const struct http_route *rt = &site->routes[i];
		const char *m = head_only ? "GET" : req->method;

		if (rt->method && strcmp(rt->method, m) != 0)
			continue;
		if (!prefix_matches(req->target, rt->prefix, rt->exact))
			continue;

		/* A route that is both, or neither, is a route whose
		 * author had not decided. 500 rather than a precedence
		 * rule: the fault is in the site, and answering it
		 * plausibly would hide it. */
		if ((rt->handler && rt->stream)
		    || (!rt->handler && !rt->stream)) {
			send_status(fd, 500, site->server_name,
			            site->bytes_sent);
			return 0;
		}

		if (rt->stream) {
			struct http_sink sink;

			memset(&sink, 0, sizeof(sink));
			sink.fd = fd;
			sink.head_only = head_only;
			sink.keep_alive = keep;
			sink.minor = req->minor;
			sink.declared = HTTP_LENGTH_UNKNOWN;
			sink.bytes_sent = site->bytes_sent;
			sink.server_name = site->server_name;

			verdict = rt->stream(req,
			                     req->content_length ?
			                         buf + req->head_length : 0,
			                     req->content_length,
			                     &sink,
			                     rt->ctx ? rt->ctx : site->ctx);

			/* Refused before anything reached the wire, so
			 * a status can still be sent honestly. */
			if (verdict != HTTP_OK && !sink.begun) {
				int s = http_status_for(verdict);

				if (s == 0)
					s = 500;
				send_status(fd, s, site->server_name,
				            site->bytes_sent);
				return 0;
			}

			{
				int done = http_stream_end(&sink);

				note(site, req, sink.status, sent_before);
				if (done != HTTP_OK || verdict != HTTP_OK)
					return 0;	/* framing in doubt;
							 * close, do not reuse */
			}

			if (!sink.keep_alive)
				return 0;

			conn_next(c);
			return 1;
		}

		verdict = rt->handler(req,
		                      req->content_length ?
		                          buf + req->head_length : 0,
		                      req->content_length,
		                      &res,
		                      rt->ctx ? rt->ctx : site->ctx);
		if (verdict != HTTP_OK) {
			int s = http_status_for(verdict);

			if (s == 0)
				s = 500;
			send_status(fd, s, site->server_name,
			            site->bytes_sent);
			return 0;
		}
		matched = 1;
		break;
	}

	/* A path that exists under a route that does not take this
	 * method is 405, not 404 -- the difference is whether the
	 * resource is there, and a client can act on that. */
	if (!matched) {
		for (i = 0; (size_t)i < site->route_count; i++) {
			const struct http_route *rt = &site->routes[i];

			if (prefix_matches(req->target, rt->prefix,
			                   rt->exact)) {
				http_response_simple(&res, 405, "text/plain",
				                     "405 Method Not Allowed\n",
				                     23);
				break;
			}
		}
	}

	if (res.close)
		keep = 0;

	if (send_response(fd, req, &res, site->server_name, keep,
	                  head_only, site->bytes_sent) != 0) {
		/* Logged even though the send failed. What was
		 * attempted is the useful record; a request that
		 * vanishes from the log because the client went away
		 * is a request nobody can account for. */
		note(site, req, res.status, sent_before);
		return 0;
	}
	note(site, req, res.status, sent_before);

	if (!keep)
		return 0;

	conn_next(c);
	return 1;
}

/*
 * Move one connection along by as much as it can go without waiting.
 *
 * Returns 1 if anything happened, 0 if the connection had nothing for us.
 * Closes and frees the slot itself when the connection is finished.
 *
 * **Every read here is a single attempt.** The loops that used to surround
 * them are gone; that is the whole change. A connection with nothing to give
 * returns immediately so the next one gets a turn, and comes back to exactly
 * where it was because the state is in the slot rather than on the stack.
 */
static int conn_step(struct http_conn *c, const struct http_site *site)
{
	const struct http_site *s = site;
	int verdict, got;

	if (c->state == CONN_HEAD) {
		c->sent_before = s->bytes_sent ? *s->bytes_sent : 0;

		verdict = http_request_parse(c->buf, c->have, &c->req);
		if (verdict == HTTP_PARTIAL) {
			if (c->have >= sizeof(c->buf)) {
				send_status(c->fd, 431, s->server_name,
				            s->bytes_sent);
				conn_drop(c);
				return 1;
			}
			/* `c->have > 0` is what tells a partly-read request
			 * from an idle keep-alive connection: some of a head
			 * has arrived, so the rest is coming. With nothing
			 * read yet, a zero means the client is finished and
			 * the connection should close rather than be spun on.
			 * See `read_more`. */
			if (c->have == 0)
				c->since = s->now_ms ? s->now_ms() : 0;
			got = read_more(c->fd, c->buf, &c->have,
			                sizeof(c->buf), c->have > 0,
			                &c->stalls, c->since, s);
			if (got < 0) {
				/* Half a request line, and then silence. The
				 * client is owed an answer and the log is owed
				 * an entry; `note` takes a NULL request
				 * because there is none that could be
				 * described. */
				send_status(c->fd, 408, s->server_name,
				            s->bytes_sent);
				note(s, 0, 408, c->sent_before);
				conn_drop(c);
				return 1;
			}
			if (!got) {
				conn_drop(c);
				return 1;
			}
			return 1;
		}

		if (verdict != HTTP_OK) {
			verdict = http_status_for(verdict);
			send_status(c->fd, verdict, s->server_name,
			            s->bytes_sent);

			/* Logged with no request, because there is none that
			 * could be described -- and a refused request is
			 * exactly the entry somebody will come looking for. */
			note(s, 0, verdict, c->sent_before);
			conn_drop(c);	/* the framing is in doubt; do not try
					 * to find the next request */
			return 1;
		}

		/* The body, if one was framed. */
		c->need = c->req.head_length + c->req.content_length;
		if (c->need > sizeof(c->buf)) {
			send_status(c->fd, 413, s->server_name, s->bytes_sent);
			conn_drop(c);
			return 1;
		}

		/*
		 * `Expect: 100-continue`, answered before the body is read.
		 *
		 * A client sending a large body may ask permission first: it
		 * sends the head, waits, and only sends the body once the
		 * server says carry on. **A server that never answers leaves
		 * it waiting until its own timeout expires** -- typically a
		 * second, sometimes much more -- and then it sends the body
		 * anyway. Nothing fails, so nothing is reported; the request
		 * merely takes a second longer than it should, every time.
		 *
		 * `curl` sends this on any body over about a kilobyte, so it
		 * is not a rare shape. It is the most ordinary large POST
		 * there is.
		 *
		 * The interim reply is written straight to the socket rather
		 * than through `send_response`, because it is not a response:
		 * it carries no body, no length and none of the security
		 * headers, and a real response follows it on the same
		 * connection. Putting headers on it would have the client read
		 * them as the real reply's.
		 *
		 * Only for HTTP/1.1. The mechanism did not exist in 1.0, and a
		 * 1.0 client handed an interim reply reads it as *the* reply.
		 */
		if (c->req.minor >= 1 && c->req.content_length > 0) {
			const char *expect = http_header_get(&c->req, "expect");

			if (expect && seq_fold(expect, "100-continue")) {
				static const char CARRY_ON[] =
					"HTTP/1.1 100 Continue\r\n\r\n";

				if (send_all(c->fd, CARRY_ON,
				             sizeof(CARRY_ON) - 1,
				             s->bytes_sent) != 0) {
					conn_drop(c);
					return 1;
				}
			} else if (expect) {
				/*
				 * An expectation this server does not
				 * implement. 417 rather than ignoring it: the
				 * client asked whether something would be
				 * honoured, and silence would be read as yes.
				 */
				send_status(c->fd, 417, s->server_name,
				            s->bytes_sent);
				conn_drop(c);
				return 1;
			}
		}

		c->state = CONN_BODY;
		c->stalls = 0;
		c->since = s->now_ms ? s->now_ms() : 0;
	}

	/* CONN_BODY. Always in progress by definition: the head has been read
	 * and declared a length, so the bytes are promised. A zero here is
	 * *not yet* every time. */
	if (c->have < c->need) {
		got = read_more(c->fd, c->buf, &c->have, sizeof(c->buf), 1,
		                &c->stalls, c->since, s);
		if (got < 0) {
			/* The head arrived and declared a length the body
			 * never reached. Unlike above there *is* a request to
			 * describe, so the entry names the target somebody
			 * will be looking for. */
			send_status(c->fd, 408, s->server_name, s->bytes_sent);
			note(s, &c->req, 408, c->sent_before);
			conn_drop(c);
			return 1;
		}
		if (!got) {
			conn_drop(c);
			return 1;
		}
		return 1;
	}

	if (!conn_answer(c, site))
		conn_drop(c);
	return 1;
}

/* --- the listener --------------------------------------------------------- */

int http_listen(unsigned port)
{
	struct sockaddr_in addr;
	int fd;

	if (port == 0 || port > 65535)
		return -1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = 0;		/* every address */
	addr.sin_port = (unsigned short)((port >> 8) | (port << 8));

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	if (listen(fd, 16) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int http_serve_once(int listener, const struct http_site *site)
{
	static int started;
	int worked = 0;
	int i, free_slot = -1;

	if (!started) {
		for (i = 0; i < HTTP_CONNS_MAX; i++)
			CONNS[i].fd = -1;
		started = 1;
	}

	for (i = 0; i < HTTP_CONNS_MAX; i++)
		if (CONNS[i].fd < 0) {
			free_slot = i;
			break;
		}

	/*
	 * Accept only with somewhere to put it.
	 *
	 * With every slot busy the connection stays in the listen backlog,
	 * which is where a connection waits on every other server too.
	 * Accepting it anyway and closing it would turn "wait a moment" into
	 * "refused", and a client cannot tell the difference between a server
	 * that is busy and one that is broken.
	 *
	 * **It also matters that this is not attempted at all when full.** On
	 * a host `accept` blocks, so calling it with nowhere to put the result
	 * would stop the whole loop -- including the connections that are
	 * mid-request and the only reason a slot will ever free up.
	 */
	if (free_slot >= 0) {
		int fd = accept(listener, 0, 0);

		if (fd >= 0) {
			struct http_conn *c = &CONNS[free_slot];

			/* Whatever this system needs doing to a fresh socket
			 * so it does not block. Nothing, on the target. */
			if (site->unblock)
				site->unblock(fd);

			c->fd = fd;
			c->state = CONN_HEAD;
			c->have = 0;
			c->need = 0;
			c->requests = 0;
			c->stalls = 0;
			c->since = site->now_ms ? site->now_ms() : 0;
			worked = 1;
		} else if (errno != EAGAIN && errno != EWOULDBLOCK
		           && errno != EINTR) {
			/* Nobody waiting is the common answer on this kernel
			 * and is not a failure. Anything else is. */
			return -1;
		}
	}

	/* Give every live connection a turn. */
	for (i = 0; i < HTTP_CONNS_MAX; i++)
		if (CONNS[i].fd >= 0)
			worked |= conn_step(&CONNS[i], site);

	return worked;
}

int http_serve_forever(int listener, const struct http_site *site)
{
	for (;;) {
		int rc = http_serve_once(listener, site);

		if (rc < 0)
			return rc;

		/*
		 * `rc == 0` is "nobody yet", which on this kernel is the
		 * common answer -- `accept` does not block.
		 *
		 * This used to spin, with a comment saying `SYS_YIELD` was the
		 * right thing and would be reached "when one exists". It does
		 * exist, and the site now carries it for the receive loop, so
		 * the reason this was still a spin was that nobody came back
		 * to the comment after the hook arrived.
		 *
		 * **It was expected to help with VF-013 and does not.** The
		 * guess was that a server asking `accept` as fast as it can
		 * competes with the work that would deliver the bytes it is
		 * waiting for. Measured with the yield in both loops, a 3700-
		 * byte burst took 5.14s against 5.25s without it -- which is
		 * no change. Whatever stalls a burst is not this process
		 * taking the processor, and the guess is written down because
		 * it is the obvious one and the next person will have it too.
		 *
		 * Kept anyway. Burning a whole processor to wait is wrong on
		 * its own terms, and on a machine with anything else to do it
		 * is the difference between idle and busy.
		 */
		if (rc == 0 && site->idle)
			site->idle();
	}
}
