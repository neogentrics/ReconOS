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
 * --- accept does not block on this kernel ---
 *
 * There is no wait queue on a listener, so `accept` answers `EAGAIN` when
 * nobody is waiting. This file treats that as "not yet" rather than as an
 * error, which is also correct on a host where `accept` blocks and simply
 * never returns it. One loop, both systems, no `fcntl` -- which ReconOS does
 * not have.
 */

#include "serve.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdio.h>

/* --- small helpers ------------------------------------------------------ */

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

/* --- one connection ------------------------------------------------------ */

static void serve_connection(int fd, const struct http_site *site)
{
	/* Static rather than automatic: one connection is served at a time, and
	 * 72 KiB is more stack than a user process on this system should assume
	 * it has. Safe precisely because the server is single-threaded, which is
	 * itself recorded as a limitation in `docs/WEB.md`. */
	static char buf[HTTP_CONN_BUF];
	size_t have = 0;
	int requests = 0;

	/* A bound on requests per connection. Not a performance decision: a
	 * connection that is never closed is a descriptor that is never given
	 * back, and this server has no timer to close an idle one with. */
	while (requests < 64) {
		struct http_request req;
		struct http_response res;
		int verdict, status, keep, head_only, i, matched = 0;
		size_t need;

		/* Read until the head is complete. */
		for (;;) {
			long n;

			verdict = http_request_parse(buf, have, &req);
			if (verdict != HTTP_PARTIAL)
				break;
			if (have >= sizeof(buf)) {
				send_status(fd, 431, site->server_name, site->bytes_sent);
				return;
			}
			n = recv(fd, buf + have, sizeof(buf) - have, 0);
			if (n > 0) {
				have += (size_t)n;
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			return;		/* closed, or failed; nothing to say */
		}

		if (verdict != HTTP_OK) {
			status = http_status_for(verdict);
			send_status(fd, status, site->server_name, site->bytes_sent);
			return;		/* the framing is in doubt; do not
					 * try to find the next request */
		}

		/* Read the body, if one was framed. */
		need = req.head_length + req.content_length;
		if (need > sizeof(buf)) {
			send_status(fd, 413, site->server_name, site->bytes_sent);
			return;
		}
		while (have < need) {
			long n = recv(fd, buf + have, sizeof(buf) - have, 0);

			if (n > 0) {
				have += (size_t)n;
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			return;
		}

		head_only = (strcmp(req.method, "HEAD") == 0);
		keep = req.keep_alive;

		/* Dispatch. A HEAD is routed as the GET it mirrors, so a site
		 * never has to write each handler twice. */
		http_response_simple(&res, 404, "text/plain", "404 Not Found\n",
		                     14);
		for (i = 0; (size_t)i < site->route_count; i++) {
			const struct http_route *rt = &site->routes[i];
			const char *m = head_only ? "GET" : req.method;

			if (rt->method && strcmp(rt->method, m) != 0)
				continue;
			if (!prefix_matches(req.target, rt->prefix, rt->exact))
				continue;

			/* A route that is both, or neither, is a route whose
			 * author had not decided. 500 rather than a precedence
			 * rule: the fault is in the site, and answering it
			 * plausibly would hide it. */
			if ((rt->handler && rt->stream)
			    || (!rt->handler && !rt->stream)) {
				send_status(fd, 500, site->server_name,
				            site->bytes_sent);
				return;
			}

			if (rt->stream) {
				struct http_sink sink;

				memset(&sink, 0, sizeof(sink));
				sink.fd = fd;
				sink.head_only = head_only;
				sink.keep_alive = keep;
				sink.minor = req.minor;
				sink.declared = HTTP_LENGTH_UNKNOWN;
				sink.bytes_sent = site->bytes_sent;
				sink.server_name = site->server_name;

				verdict = rt->stream(&req,
				                     req.content_length ?
				                         buf + req.head_length : 0,
				                     req.content_length,
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
					return;
				}

				if (http_stream_end(&sink) != HTTP_OK
				    || verdict != HTTP_OK)
					return;	/* the framing is in doubt;
						 * close rather than reuse */

				if (!sink.keep_alive)
					return;

				/* Carry any pipelined bytes and go round. */
				{
					size_t used = req.head_length
					            + req.content_length;
					size_t left = have - used;
					size_t k;

					for (k = 0; k < left; k++)
						buf[k] = buf[used + k];
					have = left;
				}
				requests++;
				matched = 2;
				break;
			}

			verdict = rt->handler(&req,
			                      req.content_length ?
			                          buf + req.head_length : 0,
			                      req.content_length,
			                      &res,
			                      rt->ctx ? rt->ctx : site->ctx);
			if (verdict != HTTP_OK) {
				int s = http_status_for(verdict);

				if (s == 0)
					s = 500;
				send_status(fd, s, site->server_name,
				            site->bytes_sent);
				return;
			}
			matched = 1;
			break;
		}

		/* A streaming route has already answered in full and moved the
		 * buffer on. Nothing below applies to it. */
		if (matched == 2)
			continue;

		/* A path that exists under a route that does not take this
		 * method is 405, not 404 -- the difference is whether the
		 * resource is there, and a client can act on that. */
		if (!matched) {
			for (i = 0; (size_t)i < site->route_count; i++) {
				const struct http_route *rt = &site->routes[i];

				if (prefix_matches(req.target, rt->prefix,
				                   rt->exact)) {
					http_response_simple(&res, 405,
					                     "text/plain",
					                     "405 Method Not Allowed\n",
					                     23);
					break;
				}
			}
		}

		if (res.close)
			keep = 0;

		if (send_response(fd, &req, &res, site->server_name, keep,
		                  head_only, site->bytes_sent) != 0)
			return;

		if (!keep)
			return;

		/* Carry any bytes of the next request that already arrived. */
		{
			size_t used = req.head_length + req.content_length;
			size_t left = have - used;
			size_t k;

			for (k = 0; k < left; k++)
				buf[k] = buf[used + k];
			have = left;
		}
		requests++;
	}
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
	int fd = accept(listener, 0, 0);

	if (fd < 0) {
		/* Nobody waiting. The common answer on this kernel. */
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return 0;
		return -1;
	}

	serve_connection(fd, site);
	close(fd);
	return 1;
}

int http_serve_forever(int listener, const struct http_site *site)
{
	for (;;) {
		int rc = http_serve_once(listener, site);

		if (rc < 0)
			return rc;
		/* rc == 0 is "nobody yet". There is no sleep here because
		 * ReconOS has no timer call in user mode; `SYS_YIELD` is the
		 * right thing and is reached through the library's `sched_
		 * yield` when one exists. Until then this spins, and
		 * `docs/WEB.md` carries the row. */
	}
}
