/*
 * Passing a request to another machine and the answer back.
 *
 * See `proxy.h` for what this must not do, which is most of the design.
 */

#include "proxy.h"
#include "../dial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * --- Headers that describe the connection, not the message ------------------
 *
 * RFC 7230 §6.1. Written out rather than derived, and in the order the
 * specification lists them so that a reader can check this against the
 * document rather than against what the code happens to do.
 *
 * `Connection` is the dangerous one: a client sending `Connection: close`
 * would otherwise close the upstream link, and a client naming any other
 * header there would have it dropped on a connection it cannot see.
 *
 * `Transfer-Encoding` is the second: this server refuses a request framed two
 * ways on the way in (HTTP_ESMUGGLE), and forwarding the header while also
 * writing a `Content-Length` would construct exactly that message on the way
 * out -- request smuggling, built by the thing that refuses it.
 */
static const char *const HOP_BY_HOP[] = {
	"connection",
	"keep-alive",
	"proxy-authenticate",
	"proxy-authorization",
	"te",
	"trailer",
	"transfer-encoding",
	"upgrade",
	0
};

int http_proxy_hop_by_hop(const char *lower_name)
{
	int i;

	if (!lower_name)
		return 0;
	for (i = 0; HOP_BY_HOP[i]; i++) {
		if (strcmp(lower_name, HOP_BY_HOP[i]) == 0)
			return 1;
	}
	return 0;
}

/*
 * --- Re-encoding a path this server already decoded -------------------------
 *
 * `proxy.h` explains what is lost and why it cannot be recovered here. What
 * this does is make the target safe to put in a request line: a space or a
 * control byte in a decoded path would otherwise end the line early, which
 * turns one request into two -- the same fault as the response side's refusal
 * to write an unescaped header.
 *
 * The unreserved set plus the path characters that are legal unencoded. `%` is
 * **encoded**, so a decoded `%` cannot become the start of an escape upstream.
 */
static int plain(unsigned char c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
	    || (c >= '0' && c <= '9'))
		return 1;
	return strchr("-._~/!$&'()*+,;=:@", (char)c) != 0 && c != '\0';
}

long http_proxy_encode_path(const char *decoded, char *out, size_t room)
{
	static const char HEX[] = "0123456789ABCDEF";
	size_t at = 0;
	size_t i;

	if (!decoded || !out)
		return -1;

	for (i = 0; decoded[i]; i++) {
		unsigned char c = (unsigned char)decoded[i];

		if (plain(c)) {
			if (at + 1 >= room)
				return -1;
			out[at++] = (char)c;
			continue;
		}
		if (at + 3 >= room)
			return -1;
		out[at++] = '%';
		out[at++] = HEX[(c >> 4) & 0xF];
		out[at++] = HEX[c & 0xF];
	}
	out[at] = '\0';
	return (long)at;
}

/* --- reading the upstream's answer ----------------------------------------- */

/*
 * One proxied exchange's scratch space.
 *
 * Static and singular, for the same reason `GZIP_BODY` is: this server answers
 * one request at a time and a second concurrent proxy would be a change to
 * that shape rather than to this line.
 *
 * The header strings are kept here rather than pointed at the read buffer
 * because `http_stream_header` borrows -- `serve.h` says so -- and the sink
 * writes them after this function has moved on.
 */
static char HEAD[HTTP_PROXY_HEAD_MAX];
static char NAMES[HTTP_SINK_EXTRA_MAX][HTTP_NAME_MAX];
static char VALUES[HTTP_SINK_EXTRA_MAX][HTTP_VALUE_MAX];

/* Read until the blank line, or the deadline. Returns bytes in HEAD, or -1. */
static long read_head(int fd, unsigned long deadline,
                      const struct http_upstream *up)
{
	size_t at = 0;

	while (at + 1 < sizeof(HEAD)) {
		long n;

		if (up->now_ms() > deadline)
			return -1;

		n = (long)read(fd, HEAD + at, sizeof(HEAD) - 1 - at);
		if (n > 0) {
			at += (size_t)n;
			HEAD[at] = '\0';
			/*
			 * Searched from a little before the new bytes rather
			 * than from the start, because the terminator can
			 * straddle two reads -- and from the start would be
			 * quadratic on a large head, which is a denial of
			 * service an upstream could choose to cause.
			 */
			if (at >= 4) {
				size_t from = at > (size_t)n + 3
				            ? at - (size_t)n - 3 : 0;

				if (strstr(HEAD + from, "\r\n\r\n"))
					return (long)at;
			}
			continue;
		}

		/*
		 * Zero is **not** end of stream on this kernel -- VF-013, the
		 * finding that cost 0.35.0 a version. It means nothing is
		 * buffered. So this waits and asks again, and the deadline is
		 * what ends it.
		 */
		up->wait();
	}
	return -1;		/* a head larger than this will not be relayed */
}

/* --- the exchange ----------------------------------------------------------- */

int http_proxy(const struct http_request *r, const char *body, size_t body_len,
               struct http_sink *sink, const struct http_upstream *up)
{
	struct dial d;
	char path[HTTP_TARGET_MAX * 3 + 2];
	char request[HTTP_PROXY_HEAD_MAX];
	unsigned long started, deadline;
	int verdict, fd = -1;
	int status = 0;
	long head_len;
	size_t used = 0;
	size_t extras = 0;
	const char *type = 0;
	char *line, *end;
	int n;
	size_t i;

	if (!r || !sink || !up || !up->now_ms || !up->wait)
		return HTTP_EINTERNAL;

	if (http_proxy_encode_path(r->target, path, sizeof(path)) < 0)
		return HTTP_EINTERNAL;

	/* --- the request head, built before anything is dialled -------------- */
	n = snprintf(request, sizeof(request), "%s %s%s%s HTTP/1.1\r\n",
	             r->method, path,
	             r->query[0] ? "?" : "", r->query);
	if (n < 0 || (size_t)n >= sizeof(request))
		return HTTP_EINTERNAL;
	used = (size_t)n;

	/*
	 * `Host` first and from the caller. `proxy.h` says why it is not
	 * chosen here.
	 */
	n = snprintf(request + used, sizeof(request) - used,
	             "Host: %s\r\nConnection: close\r\n",
	             up->host ? up->host : "upstream");
	if (n < 0 || used + (size_t)n >= sizeof(request))
		return HTTP_EINTERNAL;
	used += (size_t)n;

	for (i = 0; i < r->header_count; i++) {
		const char *name = r->headers[i].name;

		if (http_proxy_hop_by_hop(name))
			continue;
		/* The server writes these itself, above and below. A second
		 * copy is two values for one field, which is how a length
		 * disagreement gets built. */
		if (strcmp(name, "host") == 0
		    || strcmp(name, "content-length") == 0)
			continue;

		n = snprintf(request + used, sizeof(request) - used,
		             "%s: %s\r\n", name, r->headers[i].value);
		if (n < 0 || used + (size_t)n >= sizeof(request))
			return HTTP_EINTERNAL;
		used += (size_t)n;
	}

	/* One framing, chosen here. See `proxy.h`. */
	if (body_len) {
		n = snprintf(request + used, sizeof(request) - used,
		             "Content-Length: %lu\r\n",
		             (unsigned long)body_len);
		if (n < 0 || used + (size_t)n >= sizeof(request))
			return HTTP_EINTERNAL;
		used += (size_t)n;
	}
	if (used + 2 >= sizeof(request))
		return HTTP_EINTERNAL;
	request[used++] = '\r';
	request[used++] = '\n';

	/* --- reaching it ----------------------------------------------------- */
	started = up->now_ms();
	verdict = dial_begin(&d, up->addr, up->port, started,
	                     started + HTTP_PROXY_CONNECT_MS);
	while (verdict == DIAL_PENDING) {
		up->wait();
		verdict = dial_poll(&d, up->now_ms());
	}
	if (verdict != DIAL_READY) {
		dial_close(&d);
		/* Refused and timed out are different facts about the upstream
		 * and the log should not merge them, but both are 502 to the
		 * client: it asked for a resource and this server could not
		 * get it. */
		return verdict == DIAL_TIMEDOUT ? HTTP_EUPSTREAM_SLOW
		                                : HTTP_EUPSTREAM;
	}

	fd = dial_take(&d);
	if (fd < 0) {
		dial_close(&d);
		return HTTP_EUPSTREAM;
	}

	if ((size_t)write(fd, request, used) != used
	    || (body_len && (size_t)write(fd, body, body_len) != body_len)) {
		close(fd);
		dial_close(&d);
		return HTTP_EUPSTREAM;
	}

	/* --- its answer ------------------------------------------------------ */
	deadline = up->now_ms() + HTTP_PROXY_HEAD_MS;
	head_len = read_head(fd, deadline, up);
	if (head_len < 0) {
		close(fd);
		dial_close(&d);
		return HTTP_EUPSTREAM_SLOW;
	}

	/* The status line. Anything this cannot read is nonsense from upstream
	 * and becomes a 502 rather than being passed on -- a client should not
	 * be handed a malformed message because somebody else sent one. */
	if (strncmp(HEAD, "HTTP/1.", 7) != 0) {
		close(fd);
		dial_close(&d);
		return HTTP_EUPSTREAM;
	}
	line = strchr(HEAD, ' ');
	if (!line) {
		close(fd);
		dial_close(&d);
		return HTTP_EUPSTREAM;
	}
	status = (int)strtol(line, &end, 10);
	if (status < 100 || status > 599) {
		close(fd);
		dial_close(&d);
		return HTTP_EUPSTREAM;
	}

	end = strstr(HEAD, "\r\n");
	if (!end) {
		close(fd);
		dial_close(&d);
		return HTTP_EUPSTREAM;
	}

	/* --- its headers, filtered ------------------------------------------- */
	{
		char *at = end + 2;

		while (at < HEAD + head_len) {
			char *colon, *stop;
			size_t name_len, value_len;
			char lower[HTTP_NAME_MAX];
			size_t k;

			if (at[0] == '\r' && at[1] == '\n')
				break;		/* the blank line */

			stop = strstr(at, "\r\n");
			if (!stop)
				break;
			colon = memchr(at, ':', (size_t)(stop - at));
			if (!colon) {
				at = stop + 2;
				continue;	/* not a header; not relayed */
			}

			name_len = (size_t)(colon - at);
			if (name_len >= sizeof(lower)) {
				at = stop + 2;
				continue;
			}
			for (k = 0; k < name_len; k++) {
				char c = at[k];

				if (c >= 'A' && c <= 'Z')
					c = (char)(c + 32);
				lower[k] = c;
			}
			lower[name_len] = '\0';

			colon++;
			while (colon < stop && (*colon == ' ' || *colon == '\t'))
				colon++;
			value_len = (size_t)(stop - colon);

			at = stop + 2;

			if (http_proxy_hop_by_hop(lower))
				continue;
			/*
			 * These four are the sink's to decide, not the
			 * upstream's. The body is re-framed here -- chunked in,
			 * chunked out, and possibly compressed on the way --
			 * so relaying the upstream's framing headers would
			 * describe a message this server is not sending.
			 */
			if (strcmp(lower, "content-length") == 0
			    || strcmp(lower, "connection") == 0
			    || strcmp(lower, "content-encoding") == 0)
				continue;

			if (strcmp(lower, "content-type") == 0) {
				if (value_len >= HTTP_VALUE_MAX)
					continue;
				memcpy(VALUES[HTTP_SINK_EXTRA_MAX - 1], colon,
				       value_len);
				VALUES[HTTP_SINK_EXTRA_MAX - 1][value_len] = '\0';
				type = VALUES[HTTP_SINK_EXTRA_MAX - 1];
				continue;
			}

			/* Refused rather than truncated. See `serve.h`: a
			 * response missing headers nobody chose to drop is a
			 * wrong answer that looks like a right one. */
			if (extras >= HTTP_SINK_EXTRA_MAX - 1
			    || value_len >= HTTP_VALUE_MAX) {
				close(fd);
				dial_close(&d);
				return HTTP_EUPSTREAM;
			}

			memcpy(NAMES[extras], lower, name_len + 1);
			memcpy(VALUES[extras], colon, value_len);
			VALUES[extras][value_len] = '\0';
			http_stream_header(sink, NAMES[extras], VALUES[extras]);
			extras++;
		}

		/* --- the body, streamed ------------------------------------- */
		if (http_stream_begin(sink, status, type,
		                      HTTP_LENGTH_UNKNOWN) != HTTP_OK) {
			close(fd);
			dial_close(&d);
			return HTTP_EUPSTREAM;
		}

		/* Whatever of the body arrived with the head. */
		{
			char *rest = strstr(HEAD, "\r\n\r\n");

			if (rest) {
				size_t already;

				rest += 4;
				already = (size_t)(HEAD + head_len - rest);
				if (already)
					http_stream_write(sink, rest, already);
			}
		}

		deadline = up->now_ms() + HTTP_PROXY_BODY_MS;
		for (;;) {
			char block[2048];
			long got;

			if (up->now_ms() > deadline)
				break;	/* the head is long gone; the sink's
					 * own check reports the truncation */

			got = (long)read(fd, block, sizeof(block));
			if (got > 0) {
				http_stream_write(sink, block, (size_t)got);
				continue;
			}

			/*
			 * Zero means nothing buffered, not end of stream --
			 * VF-013 again. **So how does this ever finish?** The
			 * upstream was sent `Connection: close`, so when it has
			 * finished it closes, and a closed socket answers a
			 * negative rather than zero. That is the end condition,
			 * and it is why `Connection: close` is not optional in
			 * the request above.
			 */
			if (got < 0)
				break;
			up->wait();
		}
	}

	close(fd);
	dial_close(&d);
	return HTTP_OK;
}
