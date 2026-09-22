/*
 * Passing a request to another machine and the answer back.
 *
 * --- Why this exists now and not a year ago ---
 *
 * The reverse proxy sat on this role's blocked list for five versions behind
 * KF-257, on a diagnosis that was wrong. `connect` was never broken: a program
 * polling it and doing nothing else never caused the reply to be taken off the
 * card's receive ring, so the handshake completed and nobody collected it. The
 * network session read it correctly, VF-045 measured it, and `server/dial.c`
 * now services the device between attempts. An outbound connection to a real
 * listener is measured at 1 ms.
 *
 * So this is the first thing built on top of outbound TCP that a server
 * actually needs.
 *
 * --- What a proxy must not do, which is most of the design ---
 *
 * **Hop-by-hop headers stop here.** `Connection`, `Keep-Alive`, `TE`,
 * `Trailer`, `Transfer-Encoding`, `Upgrade`, `Proxy-Authorization` and
 * `Proxy-Authenticate` describe *this* connection, not the message. Forwarding
 * them hands a client control of a connection it cannot see: a request
 * carrying `Connection: close` would close the upstream link, and one carrying
 * `Transfer-Encoding: chunked` alongside a length is the exact shape of
 * request smuggling this server already refuses on the way in. RFC 7230 §6.1
 * lists them; `HOP_BY_HOP` in `proxy.c` is that list and nothing else.
 *
 * **The body is forwarded by length, never by re-framing.** Whatever framing
 * arrived has already been decoded by `request.c` and `chunked.c`; what goes
 * out is a `Content-Length` and that many bytes. One framing, chosen here,
 * written by this program -- the same rule `serve.h` gives for responses.
 *
 * **A deadline, because this process answers one request at a time.** An
 * upstream that accepts a connection and never answers would otherwise hold
 * the entire server. Every wait here is bounded and a proxy that runs out of
 * patience answers 504 rather than waiting to be helpful.
 *
 * --- Two things it cannot do, stated rather than discovered ---
 *
 * **It cannot send `X-Forwarded-For`.** `accept` does not report who
 * connected, so this machine does not know the client's address to forward.
 * That is `{#kw-peer-address}` in `docs/KERNEL-WANTS.md`, and a proxy that
 * invented the header would be worse than one that omits it.
 *
 * **It forwards the normalised target, not the bytes the client sent.**
 * `request.c` percent-decodes and normalises into `r->target` and keeps no raw
 * copy. So a client asking for `/a%2Fb` -- one path segment containing a
 * slash -- is proxied as `/a/b`, which is two segments and a different
 * resource. This is re-encoded conservatively on the way out, and that
 * recovers a space or a `?`; it cannot recover an encoded separator, because
 * the information is gone before this function is called.
 *
 * **That is a real limitation and it is the parser's to fix**, not this file's:
 * the request would have to keep the raw target beside the decoded one. Until
 * it does, an upstream that distinguishes `%2F` from `/` is served wrongly by
 * this proxy, and `docs/WEB.md` says so in the row rather than leaving it for
 * somebody to find. Guessing an encoding back is not available -- there is no
 * way to tell which slashes were literal.
 */
#ifndef RECON_HTTP_PROXY_H
#define RECON_HTTP_PROXY_H

#include "http.h"
#include "serve.h"

/* How long to wait for the upstream, at each stage. Generous against a machine
 * on the same network, and short against a person waiting for a page. */
#define HTTP_PROXY_CONNECT_MS 3000
#define HTTP_PROXY_HEAD_MS    5000
#define HTTP_PROXY_BODY_MS   10000

/* Where one proxied answer goes to. Declared so a caller can see the cost:
 * there is one of these, because the server answers one request at a time. */
#define HTTP_PROXY_HEAD_MAX 4096

/*
 * Where to send it, and how to wait.
 *
 * The clock and the yield are function pointers for the same reason
 * `struct http_site` carries its own: this file is compiled unchanged for the
 * target and for the host suites, and those two have nothing in common about
 * how a program waits. A suite hands it a host clock and `sched_yield`; the
 * machine hands it `clock_ms` and `recon_yield`. Neither is written into this
 * file, so neither can be wrong for the other.
 *
 * `host` is what to put in the upstream's `Host:` header. It is the caller's
 * because the right answer depends on the deployment -- the upstream's own
 * name when it serves several sites, this server's name when it does not --
 * and a proxy that chose for you would be wrong half the time.
 */
struct http_upstream {
	unsigned int   addr;		/* host order, as `dial.h` takes it */
	unsigned short port;
	const char    *host;		/* the Host: header to send */

	unsigned long (*now_ms)(void);
	void          (*wait)(void);	/* called when there is nothing to do */
};

/*
 * Send this request upstream and stream the answer into `sink`.
 *
 * Returns HTTP_OK once the answer has been streamed, or a verdict:
 *
 *   HTTP_EUPSTREAM      could not reach it, or it answered nothing usable
 *   HTTP_EUPSTREAM_SLOW it accepted and did not answer in time
 *
 * **On a failure before anything is written, the caller still owes the client a
 * status.** Once the head has gone out this returns HTTP_OK even for a
 * truncated body, because the status is already on the wire and the sink's own
 * promise check is what reports the truncation -- see `serve.h`.
 */
int http_proxy(const struct http_request *r, const char *body, size_t body_len,
               struct http_sink *sink, const struct http_upstream *up);

/* True for a header that describes this connection rather than the message,
 * and so must not be passed on. Exposed for the suite, which checks the list
 * against RFC 7230 §6.1 rather than against this implementation. */
int http_proxy_hop_by_hop(const char *lower_name);

/*
 * Re-encode a decoded path for sending upstream.
 *
 * Writes into `out`, returns the length, or -1 if it will not fit. See the
 * note above about what this can and cannot recover.
 */
long http_proxy_encode_path(const char *decoded, char *out, size_t room);

#endif
