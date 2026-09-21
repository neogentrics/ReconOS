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
#include "chunked.h"
#include "accept.h"
#include "cache.h"
#include "deflate.h"

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
 * How long a connection that has sent nothing is held.
 *
 * Separate from `RECV_DEADLINE_MS` because the two wait for different things.
 * That one waits for bytes a client has already committed to, which this
 * kernel may deliver at about a kilobyte a second, so it is fifteen seconds.
 * This one waits only for the gap between a connection being accepted and its
 * first packet being processed -- measured at about a millisecond in the
 * capture behind VF-042 -- so two seconds is four orders of magnitude of
 * margin.
 *
 * It is short for a reason as well as long for one: the pool holds four
 * connections, and a client that opened one and went away should not keep a
 * quarter of this server for a quarter of a minute.
 */
#define HTTP_IDLE_MS 2000

/*
 * And how long a connection that has already answered something is held
 * while it waits for the next request.
 *
 * A third number, because it is a third question. `RECV_DEADLINE_MS` waits
 * for bytes already committed to; `HTTP_IDLE_MS` waits for the gap between
 * accepting a connection and its first packet arriving; this waits for a
 * client to decide what to ask next.
 *
 * **Found by giving the first two one number.** Two seconds is right for a
 * race measured in milliseconds and wrong for a browser that fetches a page,
 * parses it, and comes back for the stylesheet: measured, a client pausing
 * three seconds between requests got one answer out of three. Reconnecting is
 * not free here -- a handshake costs about half a second at this machine's
 * sustained rate -- so the connection is worth keeping.
 *
 * Ten seconds rather than the sixty or seventy-five a server with thousands
 * of descriptors would use, because this pool holds **four**. A client that
 * has gone away must not keep a quarter of this server for a minute.
 */
#define HTTP_KEEPALIVE_MS 10000

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
                     unsigned long deadline_ms, unsigned long *stalls,
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
	if (n != 0)
		return 0;

	/*
	 * Nothing taken, and a zero does not say why.
	 *
	 * **This used to close the connection when nothing had been read
	 * yet**, on the reasoning that a partly-read request means more is
	 * coming while an untouched one means the client is finished. That is
	 * true on a host, where `recv` answers 0 only at end of stream. It is
	 * false on the target, where 0 means *nothing buffered* -- which is
	 * the whole of VF-013 -- and this file is compiled unchanged for both.
	 *
	 * What it cost: a connection accepted a moment before its request
	 * packet was processed was hung up on. Roughly one in four hundred,
	 * and the capture is unambiguous -- the server's FIN acknowledged the
	 * SYN and not the fifty-four bytes that had already arrived. VF-042.
	 *
	 * So there is one path now and a deadline decides on both systems. It
	 * is correct on the host too, merely later: a client that has closed
	 * will not send again, and waiting a bounded time before agreeing is
	 * not a different answer.
	 */
	if (site->now_ms) {
		if (site->now_ms() - since >= deadline_ms)
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

/* --- compressing a response -------------------------------------------------
 *
 * --- Why this is here and not in each handler ---
 *
 * The same reason the security headers and the access log are: a transformation
 * every handler must remember is one the next handler will not do. A handler
 * fills in a body; whether that body goes on the wire compressed is a property
 * of the response and the client, and both are known here.
 *
 * --- What is compressed, and what is not ---
 *
 * **Both shapes, by two different routes.** What follows compresses a whole
 * response in one call, which is right for the console, the JSON API and the
 * log -- the bodies this server builds in memory. Files are served by the
 * streaming path instead and are compressed there, by `deflate_stream_*` and
 * `http_stream_begin`, since 0.37.0. Until then they were not compressed at
 * all, which this comment said plainly rather than leaving for somebody to
 * discover; the gap is closed and the sentence is kept so that the next person
 * to read it knows there are two paths and not one.
 *
 * **Only text.** A media type that is already compressed -- an image, a
 * download -- costs processor time to grow slightly. The list below is the
 * types this server actually produces.
 *
 * **Only above a threshold.** gzip framing is eighteen bytes before a single
 * byte of data, so a short body always grows. The threshold is measured against
 * that, not chosen for neatness.
 *
 * **And only if the result is actually smaller**, which `deflate.c` already
 * decides for itself by falling back to a stored block. Checked again here
 * because a stored block plus framing is still larger than the body.
 */

/* Below this, framing costs more than compression saves. */
#define HTTP_GZIP_MIN  256

/*
 * Where a streamed body is compressed.
 *
 * The compressor's state is large -- a 32 KiB window and two hash tables over
 * it, about 300 KiB -- and there is one of it, for the same reason there is
 * one `GZIP_BODY`: the server is single-process and a response runs to
 * completion before the next one starts. A second concurrent response would be
 * a change to that shape rather than to this line.
 *
 * Input is fed in fixed pieces so that the output buffer can be a fixed size
 * whatever a handler hands over in one call. A handler that writes a megabyte
 * in one `http_stream_write` is compressed a piece at a time here rather than
 * refused, because the alternative is a buffer sized for the largest write
 * anybody might ever make.
 */
#define HTTP_GZIP_PIECE 8192

static struct deflate_stream GZIP_STREAM;
static unsigned char GZIP_OUT[DEFLATE_STREAM_BOUND(HTTP_GZIP_PIECE)];

/*
 * Where a compressed body lives.
 *
 * Static, sized for the largest body this server builds in memory, and one of
 * them because the server is single-process and `conn_answer` runs to
 * completion. A second concurrent compression would be a change to that shape
 * rather than to this line -- the same note `deflate.c` makes about its
 * matcher.
 */
static unsigned char GZIP_BODY[HTTP_BODY_MAX + 1024];

/* The types worth compressing: the ones this server produces that are text. */
static int compressible(const char *type)
{
	static const char *const TYPES[] = {
		"text/", "application/json", "image/svg+xml"
	};
	size_t k;

	if (!type)
		return 0;
	for (k = 0; k < sizeof(TYPES) / sizeof(TYPES[0]); k++) {
		const char *want = TYPES[k];
		size_t i;

		for (i = 0; want[i]; i++) {
			char c = type[i];

			if (c >= 'A' && c <= 'Z')
				c = (char)(c + 32);
			if (c != want[i])
				break;
		}
		if (!want[i])
			return 1;
	}
	return 0;
}

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
	out->etag = 0;
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
                         int head_only, int vary_accept,
                         unsigned long *counter)
{
	char head[2048];
	const char *body = res->body;
	size_t body_len = res->body_len;
	const char *encoding = 0;
	int n;

	/*
	 * Compress, if everything lines up. Decided here rather than by the
	 * handler -- see the block above `HTTP_GZIP_MIN`.
	 *
	 * `req` is NULL for the refusals `send_status` writes, which are a
	 * line of text each and below the threshold anyway.
	 */
	if (req && body_len >= HTTP_GZIP_MIN && compressible(res->content_type)
	    && body_len <= HTTP_BODY_MAX
	    && http_accept_gzip(http_header_get(req, "accept-encoding"))) {
		long got = deflate_gzip(body, body_len, GZIP_BODY,
		                        sizeof(GZIP_BODY));

		/* Only if it actually got smaller. `deflate.c` already falls
		 * back to a stored block rather than growing the data, and a
		 * stored block plus eighteen bytes of framing is still larger
		 * than the body it wraps. */
		if (got > 0 && (size_t)got < body_len) {
			body = (const char *)GZIP_BODY;
			body_len = (size_t)got;
			encoding = "gzip";
		}
	}

	n = snprintf(head, sizeof(head),
	             "HTTP/1.1 %d %s\r\n"
	             "Server: %s\r\n"
	             "Content-Length: %lu\r\n"
	             "Connection: %s\r\n"
	             SECURITY_HEADERS,
	             res->status, http_reason(res->status),
	             server_name ? server_name : "ReconOS",
	             (unsigned long)body_len,
	             keep_alive ? "keep-alive" : "close");
	if (n < 0 || (size_t)n >= sizeof(head))
		return -1;

	if (encoding) {
		int m = snprintf(head + n, sizeof(head) - (size_t)n,
		                 "Content-Encoding: %s\r\n", encoding);
		if (m < 0 || (size_t)(n + m) >= sizeof(head))
			return -1;
		n += m;
	}

	/*
	 * The validator goes on the 304 as well as the 200.
	 *
	 * A 304 without one is a 304 a client cannot use for its *next*
	 * conditional request, so the saving happens once and then stops.
	 */
	if (res->etag) {
		int m = snprintf(head + n, sizeof(head) - (size_t)n,
		                 "ETag: %s\r\n", res->etag);
		if (m < 0 || (size_t)(n + m) >= sizeof(head))
			return -1;
		n += m;
	}

	if (res->content_type && body_len > 0) {
		int m = snprintf(head + n, sizeof(head) - (size_t)n,
		                 "Content-Type: %s\r\n", res->content_type);
		if (m < 0 || (size_t)(n + m) >= sizeof(head))
			return -1;
		n += m;
	}

	/*
	 * See `negotiated` in `serve.h`: the route said its body depends on
	 * `Accept`, and a response that does not say so is one a cache serves
	 * to the next client that asked for something else.
	 *
	 * **`Accept-Encoding` is named whenever the body could have been
	 * compressed, not only when it was.** A cache that stored the identity
	 * form of a compressible response and served it to a client that asked
	 * for gzip is harmless; one that stored the gzip form and served it to
	 * a client that did not ask is a page of binary. The condition is
	 * therefore the same one the compression decision started from, minus
	 * the client's own header.
	 */
	{
		const char *vary = 0;

		if (vary_accept && req && compressible(res->content_type))
			vary = "Accept, Accept-Encoding";
		else if (vary_accept)
			vary = "Accept";
		else if (req && compressible(res->content_type)
		         && res->body_len >= HTTP_GZIP_MIN)
			vary = "Accept-Encoding";

		if (vary) {
			int m = snprintf(head + n, sizeof(head) - (size_t)n,
			                 "Vary: %s\r\n", vary);
			if (m < 0 || (size_t)(n + m) >= sizeof(head))
				return -1;
			n += m;
		}
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
	/* `body` and `body_len`, not `res->`: they are the compressed form when
	 * one was made, and the declared `Content-Length` above is theirs. A
	 * mismatch here is a response whose length lies, which is a connection
	 * the next request is read out of the middle of. */
	if (head_only || body_len == 0)
		return 0;
	return send_all(fd, body, body_len, counter);
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

/*
 * Body bytes, framed for the wire. No counting and no promise-checking: those
 * are about the resource, and this is only about the connection.
 *
 * Split out of `http_stream_write` when compression arrived, because there are
 * now three things that put bytes in the body -- the handler's writes, gzip's
 * ten-byte header, and the compressor's tail -- and only the first of them is
 * the handler's declared length. One function frames all three; the counting
 * stays where the handler's bytes come in.
 *
 * **Zero length returns without sending**, and that is not an optimisation. A
 * zero-length chunk is the chunked terminator. Sending one here would end the
 * body in the middle of it, and the compressor produces nothing rather often
 * -- it holds bytes back while it looks for a match -- so this would not have
 * been a rare path.
 *
 * Returns 0 or -1, and does not set `failed`: what the caller does about a
 * failure differs between them.
 */
static int send_body(struct http_sink *sink, const void *data, size_t len)
{
	char size[32];
	int n;

	if (sink->head_only || len == 0)
		return 0;

	if (!sink->chunked)
		return send_all(sink->fd, (const char *)data, len,
		                sink->bytes_sent);

	n = snprintf(size, sizeof(size), "%lx\r\n", (unsigned long)len);
	if (n < 0 || (size_t)n >= sizeof(size))
		return -1;
	if (send_all(sink->fd, size, (size_t)n, sink->bytes_sent) != 0
	    || send_all(sink->fd, (const char *)data, len, sink->bytes_sent) != 0
	    || send_all(sink->fd, "\r\n", 2, sink->bytes_sent) != 0)
		return -1;
	return 0;
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
	 * Compress, or not -- decided here because it is the last moment it can
	 * be. Compressing changes the length, which changes the framing, and
	 * both are about to go on the wire.
	 *
	 * **Never on a partial response.** A 206 sends a byte range of the
	 * resource and `Content-Range` names the bytes of the *resource*. A
	 * compressed 206 would have to describe a range of the compressed form
	 * -- which is not what the client asked for, and not something this
	 * server can name honestly. The status is the structural guard, because
	 * a 206 is the only way a range is served; the `Content-Range` check
	 * beside it is there so that a partial path added later, by somebody
	 * who did not read this, still cannot get past it.
	 *
	 * **Never below the threshold, when the length is known.** The same
	 * eighteen bytes of framing `send_response` weighs. When the length is
	 * *not* known there is nothing to weigh it against, and a body of
	 * unknown length is the case worth compressing, so it goes ahead.
	 *
	 * The declared length is kept even so. It is no longer a
	 * `Content-Length` -- the body on the wire is a different size -- but it
	 * is still the handler's promise about the resource, and
	 * `http_stream_end` still holds it to it. A handler that says 4096 and
	 * writes 3000 has truncated the file whether or not it was compressed
	 * on the way out.
	 */
	sink->gzipping = 0;
	if (sink->may_gzip && status == 200 && compressible(content_type)
	    && !(length >= 0 && length < HTTP_GZIP_MIN)) {
		size_t i;
		int partial = 0;

		for (i = 0; i < sink->extras; i++) {
			if (seq_fold(sink->extra[i].name, "content-range"))
				partial = 1;
		}
		if (!partial)
			sink->gzipping = 1;
	}
	if (sink->gzipping)
		length = HTTP_LENGTH_UNKNOWN;

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

	/*
	 * Written here rather than added through `http_stream_header`, because
	 * these two are the server's and `extra[]` is the handler's -- and the
	 * handler may already have filled it. `files.c` serving a range uses
	 * all four slots.
	 *
	 * `Vary` goes out whenever the response *could* have been compressed,
	 * not only when it was, for the reason `send_response` gives at length:
	 * a cache that stored the gzip form under a key that did not mention
	 * `Accept-Encoding` would serve it to a client that never asked for it.
	 * It is the **type** that decides and not this request, because the
	 * question a cache is asking is whether some other client would have
	 * been answered differently -- and for a PNG none ever would be.
	 */
	if (compressible(content_type)) {
		int m = snprintf(head + n, sizeof(head) - (size_t)n,
		                 "Vary: Accept-Encoding\r\n");
		if (m < 0 || (size_t)(n + m) >= sizeof(head))
			return -1;
		n += m;
	}
	if (sink->gzipping) {
		int m = snprintf(head + n, sizeof(head) - (size_t)n,
		                 "Content-Encoding: gzip\r\n");
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

	/*
	 * The gzip header is body, not head, so it goes out here -- after the
	 * blank line and through whatever framing was just chosen. Starting the
	 * compressor earlier would have meant holding these ten bytes somewhere
	 * until the head was written, which is a place for them to be
	 * forgotten on the path where a handler writes nothing at all.
	 */
	if (sink->gzipping) {
		long n10 = deflate_stream_begin(&GZIP_STREAM, GZIP_OUT,
		                                sizeof(GZIP_OUT));

		if (n10 < 0 || send_body(sink, GZIP_OUT, (size_t)n10) != 0) {
			sink->failed = 1;
			return -1;
		}
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

	/* Counted above, sent nowhere: a HEAD reports the length and no body.
	 * The compressor is not run either -- there is nothing to send, and a
	 * HEAD that left the stream half-started would be a stream the next
	 * response inherits. */
	if (sink->head_only)
		return HTTP_OK;

	if (!sink->gzipping) {
		if (send_body(sink, data, len) != 0) {
			sink->failed = 1;
			return -1;
		}
		return HTTP_OK;
	}

	/*
	 * A piece at a time, so `GZIP_OUT` can be a fixed size. A write may
	 * legitimately produce nothing -- the compressor holds bytes back while
	 * it looks for a match that might continue into input that has not
	 * arrived -- and `send_body` of nothing is nothing, which is why this
	 * loop has no special case for it.
	 */
	{
		const unsigned char *at = (const unsigned char *)data;
		size_t left = len;

		while (left) {
			size_t take = left > HTTP_GZIP_PIECE
			            ? HTTP_GZIP_PIECE : left;
			long out = deflate_stream_write(&GZIP_STREAM, at, take,
			                                GZIP_OUT,
			                                sizeof(GZIP_OUT));

			if (out < 0
			    || send_body(sink, GZIP_OUT, (size_t)out) != 0) {
				sink->failed = 1;
				return -1;
			}
			at += take;
			left -= take;
		}
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

	/*
	 * The compressor's tail, and it must go out **before** the chunked
	 * terminator: it is body, and a terminator sent first would end the
	 * body without it. What is left here is the bytes held back for a match
	 * that might have continued, the end-of-block, and gzip's checksum and
	 * length -- so a stream ended without it decompresses to a truncated
	 * file, and the checksum the client computes does not match the one it
	 * never received.
	 */
	if (!sink->failed && sink->gzipping && !sink->head_only) {
		long out = deflate_stream_end(&GZIP_STREAM, GZIP_OUT,
		                              sizeof(GZIP_OUT));

		if (out < 0 || send_body(sink, GZIP_OUT, (size_t)out) != 0)
			sink->failed = 1;
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
	return send_response(fd, 0, &res, server_name, 0, 0, 0, counter);
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

/*
 * The site a request is for, or NULL if none of them will own it.
 *
 * The first whose `host` matches wins; a site with `host` NULL claims
 * anything that reaches it, which is what makes a single-site server behave
 * exactly as it did before virtual hosts existed.
 *
 * A request with no `Host` cannot reach here on HTTP/1.1 -- `request.c`
 * refuses that outright -- so the only callers with an empty one are 1.0
 * clients, which predate the field and get the default site.
 */
static const struct http_site *pick_site(const struct http_site *first,
                                         const struct http_request *req)
{
	const char *host = http_header_get(req, "host");
	const struct http_site *s;

	for (s = first; s; s = s->next) {
		if (!s->host)
			return s;		/* the default claims it */
		if (host && http_host_matches(s->host, host))
			return s;
	}
	return 0;
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

	/*
	 * The site this connection was accepted for.
	 *
	 * **Held per connection rather than passed in**, because the pool is
	 * one static array and a process may serve more than one listener. A
	 * connection accepted for one site and then stepped with another
	 * would be routed by the wrong table and -- far worse -- guarded by
	 * the wrong policy, which is a site's writes protected by a different
	 * site's secret.
	 *
	 * Nothing does that yet. It was found by trying to write the suite for
	 * the fail-closed case, which needed a second site to have no `allow`,
	 * and it is four bytes to make impossible rather than a paragraph
	 * telling the next person not to.
	 */
	const struct http_site *site;
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

	/*
	 * Where a chunked body has got to. Meaningless unless `req.chunked`.
	 *
	 * Per connection rather than per call, because a body arrives across
	 * as many reads as the network chooses and the decoder has to resume
	 * exactly where it stopped -- which is the same reason `have` and
	 * `need` live here.
	 */
	struct http_chunked dec;

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
	/*
	 * `need`, not head plus `content_length`.
	 *
	 * They are the same number for a body framed by a length and they are
	 * **not** for a chunked one: `content_length` is then how long the
	 * body decoded to, while the bytes this request occupied include every
	 * chunk header, every CRLF and the trailer section. Taking the decoded
	 * length here would leave the difference in the buffer to be read as
	 * the next request -- which is the desynchronisation `chunked.h` is
	 * about, arriving by the back door.
	 */
	size_t used = c->need;
	size_t left = c->have - used;
	size_t k;

	for (k = 0; k < left; k++)
		c->buf[k] = c->buf[used + k];
	c->have = left;
	c->requests++;
	c->state = CONN_HEAD;
	c->stalls = 0;

	/*
	 * The idle clock starts again here, because this connection is now
	 * waiting for a *new* request rather than the rest of an old one.
	 * Without this, a keep-alive client's second request would be measured
	 * against the moment the connection was accepted, and a connection
	 * held open longer than `HTTP_IDLE_MS` would be dropped between
	 * requests it was about to make.
	 */
	c->since = c->site && c->site->now_ms ? c->site->now_ms() : 0;
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

	/*
	 * The route that answered, kept because one thing after the loop needs
	 * it: whether to send `Vary: Accept`. Held as a pointer rather than
	 * copying the flag, so the next question the server wants to ask a
	 * route after dispatch does not need a second variable beside it.
	 */
	const struct http_route *chosen = 0;

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

		/*
		 * Permission, before the handler runs.
		 *
		 * Checked here rather than inside each handler, for the reason
		 * the security headers are written here: a check every handler
		 * must remember is a check the next handler will not do.
		 *
		 * **A guarded route on a site with no policy is 500, not 200.**
		 * The site asked for a guard and did not supply one, which is
		 * the same class of fault as the route above that is both a
		 * handler and a stream -- and answering the request anyway
		 * would be the single outcome nobody wanted. It fails closed
		 * and loudly, rather than open and quietly.
		 */
		if (rt->guarded) {
			if (!site->allow) {
				send_status(fd, 500, site->server_name,
				            site->bytes_sent);
				return 0;
			}
			if (!site->allow(req,
			                 req->content_length ?
			                     buf + req->head_length : 0,
			                 req->content_length,
			                 site->allow_ctx)) {
				/* Logged. A client that cannot get in is
				 * exactly the entry somebody goes looking for,
				 * and a refusal that leaves no trace is
				 * indistinguishable from a request that never
				 * arrived. */
				http_response_simple(&res, 401, "text/plain",
				                     "401 Unauthorized\n", 17);
				res.extra_name = "WWW-Authenticate";
				res.extra_value = "Bearer";
				res.close = 1;
				if (send_response(fd, req, &res,
				                  site->server_name, 0,
				                  head_only, rt->negotiated,
				                  site->bytes_sent) != 0) {
					note(site, req, 401, sent_before);
					return 0;
				}
				note(site, req, 401, sent_before);
				return 0;
			}
		}

		if (rt->stream) {
			struct http_sink sink;

			memset(&sink, 0, sizeof(sink));
			sink.fd = fd;
			sink.head_only = head_only;
			sink.keep_alive = keep;
			sink.minor = req->minor;
			sink.declared = HTTP_LENGTH_UNKNOWN;
			sink.may_gzip = http_accept_gzip(
				http_header_get(req, "accept-encoding"));
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
		chosen = rt;
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

	/*
	 * A conditional read, answered here rather than in the handler.
	 *
	 * The handler has built the body and said what it is worth as a
	 * validator; whether this particular client already has it is a
	 * question about the request, and the server is holding both. Leaving
	 * it to each handler would mean every future handler remembering, and
	 * the one that forgets sends a body nobody needed and says nothing
	 * about it.
	 *
	 * **Only on a safe method and only on a 200.** A 304 in answer to a
	 * POST would tell a client its write was unnecessary, which is not
	 * what it means; and a 404 or a 500 carries no representation to be
	 * unchanged.
	 *
	 * The body is dropped and the length goes to zero, which is what makes
	 * this worth anything -- `send_response` then sends a head and stops.
	 */
	if (res.status == 200 && res.etag
	    && (head_only || strcmp(req->method, "GET") == 0)) {
		const char *asked = http_header_get(req, "if-none-match");

		if (asked && http_if_none_match(asked, res.etag)) {
			res.status = 304;
			res.body = 0;
			res.body_len = 0;
			/* The type goes too. A 304 carries no body, so a
			 * `Content-Type` on it describes nothing. */
			res.content_type = 0;
		}
	}

	if (res.close)
		keep = 0;

	if (send_response(fd, req, &res, site->server_name, keep,
	                  head_only, chosen ? chosen->negotiated : 0,
	                  site->bytes_sent) != 0) {
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
/*
 * How long to wait for a request head, which depends on what is being waited
 * for.
 *
 *   part of one has arrived   the rest is committed and this kernel may
 *                             deliver it slowly -- `RECV_DEADLINE_MS`
 *   nothing, first request    the gap between accept and the first packet,
 *                             about a millisecond -- `HTTP_IDLE_MS`
 *   nothing, but it has
 *   answered before           a client deciding what to ask next --
 *                             `HTTP_KEEPALIVE_MS`
 *
 * Three questions, three numbers. They were one for a while and the middle
 * case is the one that suffered: see VF-042.
 */
static unsigned long head_deadline(const struct http_conn *c)
{
	if (c->have)
		return RECV_DEADLINE_MS;
	return c->requests ? HTTP_KEEPALIVE_MS : HTTP_IDLE_MS;
}

static int conn_step(struct http_conn *c)
{
	const struct http_site *s = c->site;
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
			/*
			 * A connection that has sent nothing is given a
			 * shorter grace than one mid-request, and the
			 * difference is what each is waiting for.
			 *
			 * Mid-request, the wait is for bytes the client has
			 * already committed to and the kernel may deliver at
			 * a kilobyte a second -- that is `RECV_DEADLINE_MS`.
			 * With nothing read, the wait is only for the gap
			 * between accepting a connection and its first packet
			 * being processed, which the capture in VF-042 puts
			 * at about a millisecond. `HTTP_IDLE_MS` is four
			 * orders of magnitude more than that, and still short
			 * enough that a client which has gone away does not
			 * hold one of four slots for fifteen seconds.
			 */
			/*
			 * `since` is **not** restarted here, and the first
			 * draft of this fix restarted it.
			 *
			 * It used to, harmlessly, because with nothing read
			 * the connection was closed on the spot and the
			 * deadline was never consulted. The moment a deadline
			 * decided instead, resetting the clock on every pass
			 * meant it could never elapse -- and a connection
			 * that said nothing would have held one of four slots
			 * **for ever**, which is worse than the fault being
			 * fixed.
			 *
			 * Caught by the check written for the other half of
			 * this change, in the same run. It is set where a
			 * connection becomes ready for a request instead: at
			 * accept, and in `conn_next` for the one after it.
			 */
			got = read_more(c->fd, c->buf, &c->have,
			                sizeof(c->buf),
			                head_deadline(c),
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

		/*
		 * The body, if one was framed.
		 *
		 * A chunked body has no length in its head -- that is the
		 * whole reason the framing exists -- so `need` is not knowable
		 * yet and is filled in below, once the terminator has arrived.
		 * Until then it is the head alone, so that a decoder that
		 * refuses leaves the connection with a consistent idea of what
		 * this request occupied.
		 */
		if (c->req.chunked) {
			http_chunked_begin(&c->dec);
			c->need = c->req.head_length;
		} else {
			c->need = c->req.head_length + c->req.content_length;
		}
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
		if (c->req.minor >= 1
		    && (c->req.content_length > 0 || c->req.chunked)) {
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

	/*
	 * CONN_BODY, chunked.
	 *
	 * Fed whatever has arrived, every time round, and it resumes where it
	 * stopped. The decoder is what decides the body is over -- there is no
	 * count to compare against, which is why this cannot be folded into
	 * the branch below.
	 */
	if (c->state == CONN_BODY && c->req.chunked) {
		int rc = http_chunked_feed(&c->dec,
		                           c->buf + c->req.head_length,
		                           c->have - c->req.head_length,
		                           HTTP_BODY_MAX);

		if (rc == HTTP_PARTIAL) {
			/*
			 * The buffer is full and the body is not finished.
			 *
			 * 413 rather than reading on, because there is nowhere
			 * to put the next byte. The equivalent of the declared
			 * length being too large, arriving later because a
			 * chunked client never declares one.
			 */
			if (c->have >= sizeof(c->buf)) {
				send_status(c->fd, 413, s->server_name,
				            s->bytes_sent);
				note(s, &c->req, 413, c->sent_before);
				conn_drop(c);
				return 1;
			}

			got = read_more(c->fd, c->buf, &c->have, sizeof(c->buf),
			                RECV_DEADLINE_MS, &c->stalls, c->since,
			                s);
			if (got < 0) {
				send_status(c->fd, 408, s->server_name,
				            s->bytes_sent);
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

		if (rc != HTTP_OK) {
			int status = http_status_for(rc);

			send_status(c->fd, status, s->server_name,
			            s->bytes_sent);
			note(s, &c->req, status, c->sent_before);
			/*
			 * Dropped, never reused. A refused body means the
			 * framing is in doubt, and there is no way to know
			 * where the next request would start -- which is
			 * exactly the state a smuggled request wants the
			 * server left in.
			 */
			conn_drop(c);
			return 1;
		}

		/*
		 * Decoded, in place, over the encoded bytes. From here the
		 * request looks like any other: a body at `head_length`, this
		 * long. `need` becomes what the request actually occupied, so
		 * `conn_next` finds the pipelined remainder.
		 */
		c->req.content_length = c->dec.out;
		c->need = c->req.head_length + c->dec.in;
	}

	/* CONN_BODY, framed by a length. Always in progress by definition: the
	 * head has been read and declared a length, so the bytes are promised.
	 * A zero here is *not yet* every time. */
	if (c->have < c->need) {
		got = read_more(c->fd, c->buf, &c->have, sizeof(c->buf),
		                RECV_DEADLINE_MS, &c->stalls, c->since, s);
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

	/*
	 * Which site this request is for.
	 *
	 * Walked here rather than at accept, because `Host` is in the request
	 * and a connection may carry several -- a keep-alive client is
	 * entitled to ask two different sites down one socket, and a server
	 * that decided once at accept would answer the second with the first
	 * one's routes.
	 */
	{
		const struct http_site *pick = pick_site(c->site, &c->req);

		if (!pick) {
			/*
			 * 421, and not 404. The resource may well exist; this
			 * machine is not the one that has it, and 421 is the
			 * status that says exactly that. A 404 would tell a
			 * client the name was right and the path was wrong.
			 */
			send_status(c->fd, 421, c->site->server_name,
			            c->site->bytes_sent);
			note(c->site, &c->req, 421, c->sent_before);
			conn_drop(c);
			return 1;
		}
		if (!conn_answer(c, pick))
			conn_drop(c);
	}
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
			c->site = site;
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

	/*
	 * Give every live connection a turn -- each with the site it arrived
	 * on, not the one this call was made for. With a single site those are
	 * the same; with two they are not, and the difference is which policy
	 * guards which writes.
	 */
	for (i = 0; i < HTTP_CONNS_MAX; i++)
		if (CONNS[i].fd >= 0)
			worked |= conn_step(&CONNS[i]);

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
