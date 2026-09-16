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
static int send_all(int fd, const char *buf, size_t len)
{
	size_t sent = 0;

	while (sent < len) {
		long n = send(fd, buf + sent, len - sent, 0);

		if (n > 0) {
			sent += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		return -1;
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
                         const char *server_name, int keep_alive, int head_only)
{
	char head[1024];
	int n;

	(void)req;

	n = snprintf(head, sizeof(head),
	             "HTTP/1.1 %d %s\r\n"
	             "Server: %s\r\n"
	             "Content-Length: %lu\r\n"
	             "Connection: %s\r\n",
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

	if (send_all(fd, head, (size_t)n) != 0)
		return -1;
	if (head_only || res->body_len == 0)
		return 0;
	return send_all(fd, res->body, res->body_len);
}

/* An error answered before any handler ran. Kept deliberately plain: an error
 * page that reports what was wrong with the request tells an attacker which of
 * their probes the parser noticed. */
static int send_status(int fd, int status, const char *server_name)
{
	struct http_response res;
	char body[128];
	int n = snprintf(body, sizeof(body), "%d %s\n", status,
	                 http_reason(status));

	if (n < 0)
		return -1;
	http_response_simple(&res, status, "text/plain", body, (size_t)n);
	return send_response(fd, 0, &res, server_name, 0, 0);
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
				send_status(fd, 431, site->server_name);
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
			send_status(fd, status, site->server_name);
			return;		/* the framing is in doubt; do not
					 * try to find the next request */
		}

		/* Read the body, if one was framed. */
		need = req.head_length + req.content_length;
		if (need > sizeof(buf)) {
			send_status(fd, 413, site->server_name);
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

			verdict = rt->handler(&req,
			                      req.content_length ?
			                          buf + req.head_length : 0,
			                      req.content_length,
			                      &res, site->ctx);
			if (verdict != HTTP_OK) {
				int s = http_status_for(verdict);

				if (s == 0)
					s = 500;
				send_status(fd, s, site->server_name);
				return;
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
		                  head_only) != 0)
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
