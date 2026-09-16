# The web server

Full specification. Opened 15 September 2026 on branch `server`.

`GUI_Server_Operating_System_Architecture.docx` section 5 gives the one-line
requirement — *hosts internal administrative web applications, REST APIs, and
SSL/TLS termination* — and Joshua's addition is the one that shapes everything
below:

> Make sure it's supportive of all the different write types for web servers
> and web applications, not just web pages or the basic HTML pages.

So this is specified as an **application server that also serves files**, not a
file server with dynamic content bolted on. That distinction is structural and
is made in the code today: the middle of `serve.c` is a routing table of
handlers, and serving a static file is one handler among others rather than the
thing the server is.

**Status columns mean what they say.** `built` is running and tested here.
`partial` runs and does less than its name implies. `blocked` names its
blocker. `specified` means the design below is settled and nothing is written.
Nothing is marked built on the strength of having been written.

---

## 1. What runs today

| piece | status | where |
|---|---|---|
| Request parser | **built** | `server/http/request.c` — 70 checks |
| Routing and dispatch | **built** | `server/http/serve.c` |
| Response writer | **built** | `serve.c` — 22 checks over a real socket |
| Listener, accept, keep-alive | **built** | `serve.c`, verified on the machine |
| `GET` / `HEAD` / `POST` | **built** | |
| Request bodies, framed by `Content-Length` | **built** | 64 KiB cap |
| Static files from the volume | **built** | `files.c` — 29 checks; on the machine |
| 400 / 404 / 405 / 413 / 414 / 431 / 501 / 505 | **built** | |

The suites run on the host: `recon_server_http_tests` (parsing) and
`recon_server_serve_tests` (a real socket, a child process serving, the parent
as the client). The serving suite runs `serve.c` **unmodified** — there is no
stand-in for the network, because a stand-in is looser than the real call in
exactly the places a server breaks.

**It has now run on ReconOS**, 15 September 2026. A request from outside the
machine reached this server in ring 3 and the answer reached the client whole:
`/health`, `/api/status` and the dashboard all arrived with the declared length,
five consecutive times for the largest of them. That settles the kernel's own
open question -- a socket descriptor working with `read` and `write` was
*argued, not measured*, and it is measured now.

It also found a fault. `tcp_write` reported 1194 bytes sent when it had sent
512, so the first page served arrived truncated with `Content-Length` promising
the rest. `docs/SERVER.md` carries the measurement and the report; the
workaround is `HTTP_SEND_CHUNK` and should be removed when the kernel is
fixed.

---

## 2. Protocol surface

| | status | note |
|---|---|---|
| HTTP/1.0 | **built** | including its default of closing |
| HTTP/1.1 keep-alive | **built** | 64 requests per connection, then closed |
| Pipelining | **partial** | queued requests are answered in order; not tested under load |
| Chunked transfer (`Transfer-Encoding`) | **refused** | see below — this is deliberate |
| Range requests (`206`) | specified | needed before the file handler is useful for media |
| Conditional requests (`ETag`, `If-None-Match`, `304`) | specified | |
| `Expect: 100-continue` | specified | a client that waits for it currently stalls until timeout |
| HTTP/2 | specified | needs TLS and ALPN first |
| HTTP/3, QUIC | not planned yet | needs UDP, which the kernel has no call for |
| TLS | **blocked** | no certificate store, no TLS implementation |

### `Transfer-Encoding` is refused, and that is a decision

Chunked transfer is not implemented, and a request carrying
`Transfer-Encoding` is **rejected** rather than ignored. This is the single
most important thing in this document.

A server that ignores a framing header something upstream honours is the exact
shape of **request smuggling**: the proxy frames the body one way, the origin
frames it another, and the tail of one request becomes the head of the next —
which is how one client's request gets answered with another's session. Every
ambiguity in framing is therefore answered by rejecting the message:

- `Content-Length` **and** `Transfer-Encoding` together → refused.
- `Transfer-Encoding` alone → refused, because chunked is not implemented.
- **Two `Content-Length` headers, even identical ones** → refused. Agreement is
  not the property that makes a message safe; being unambiguous is, and it
  already is not.
- A `Content-Length` that is not a plain decimal number — `+5`, `5abc`, `0x10`,
  an empty value — → refused. `strtoul` accepts every one of those and reports
  success.

When chunked is implemented it must be implemented *fully*, including trailers
and the `0\r\n\r\n` terminator, and the dual-framing rejection stays.

---

## 3. Application models — the "write types"

This is the section Joshua's note is about. A web server is judged by what it
can run, not by what it can send.

| model | status | note |
|---|---|---|
| **In-process handlers** | **built** | `struct http_route` + `http_handler`. A C function is handed a parsed request and fills a response. This is how the admin console and the REST API are served. |
| **Static files** | **built** | `server/http/files.c` — one handler among others, as designed. MIME by extension, an index for directories, **never a listing**. Verified reading off ReconFS on the machine. |
| **JSON / REST APIs** | **built** | demonstrated in the serving suite: a handler returning `application/json` with the raw query string. Needs a JSON writer and parser next. |
| **Form submission** | **built** (transport) | `POST` bodies arrive whole, with their real length, not NUL-terminated. `application/x-www-form-urlencoded` and `multipart/form-data` decoders are specified, not written. |
| **File upload** | specified | needs `multipart/form-data` and streaming to disk; the 64 KiB body cap exists because nothing streams yet. |
| **File download** | specified | needs range requests and a sendfile-shaped path, or it reads whole files into memory. |
| **Server-sent events** | specified | needs a response the handler can write to incrementally. |
| **WebSocket** | specified | needs the `Upgrade` handshake, SHA-1 for the accept key, and a framing layer. The connection stops being HTTP after the handshake, so it needs its own loop. |
| **Long-polling** | **blocked** | needs a request to be parked without occupying the only process. See concurrency below. |
| **CGI-style external programs** | **blocked** | nothing in user mode can start a program — `KERNEL-WANTS.md` carries the entry. |
| **FastCGI / a persistent app backend** | **blocked** | same, plus a socket to talk to it over. |
| **Reverse proxy to another machine** | specified | `connect` exists, so this is buildable today. The docx names it explicitly. |
| **Template rendering** | specified | with escaping by default; a template engine that escapes on request is one that is forgotten once. |

### The two that shape the others

**Streaming responses.** Today a handler returns a whole body and the server
sends it. That is why the body cap is 64 KiB, why downloads are unspecified,
and why server-sent events cannot work. The fix is a response the handler
writes into progressively, and it should be designed before the static file
handler is written rather than after.

**Concurrency.** The server is single-threaded and serves one connection to
completion before accepting the next. A slow client therefore blocks every
other client — which is not merely slow, it is a denial of service that costs
the attacker one socket. This is the most serious limitation in this document
and it is why long-polling is marked blocked rather than unwritten.

The three ways out, in the order they become available:

1. **A read deadline.** Partial: it bounds the damage rather than removing it,
   and needs a timer call user mode does not have.
2. **A process per connection.** Needs a way to start a program — blocked.
3. **Non-blocking sockets and a poll loop.** The right answer. Needs the
   kernel to report readiness on more than a listener; today `accept` is the
   only call that answers `EAGAIN`.

---

## 4. Virtual hosts, routing and addressing

| | status | note |
|---|---|---|
| Path routing, prefix and exact | **built** | a prefix must end on a `/` boundary, so `/api` never claims `/apifoo` |
| Method routing, with 405 rather than 404 | **built** | the difference tells a client whether the resource exists |
| `Host`-based virtual hosts | specified | the header is parsed and kept; nothing dispatches on it |
| SNI | **blocked** | follows TLS |
| Path parameters (`/api/service/{name}`) | specified | |
| Redirects | **built** (transport) | `extra_name`/`extra_value` carries `Location` |

---

## 5. Identity and access

The docx puts LDAP and Kerberos in the same system, so the web server's auth
must be able to reach them rather than keeping its own parallel user list.

| | status | note |
|---|---|---|
| HTTP Basic | specified | **over TLS only.** Basic without TLS puts the password on the wire in clear, so this must refuse to be enabled on a cleartext listener rather than warn. |
| Session cookies | specified | needs a random source — `SYS_RANDOM` exists — plus `HttpOnly`, `Secure`, `SameSite` |
| Bearer tokens for the API | specified | |
| LDAP directory bind | **blocked** | no LDAP client |
| Kerberos / SPNEGO | **blocked** | no KDC, no GSSAPI |
| POSIX ACLs for served files | specified | the kernel has `SYS_GETUID`/`SYS_GETGID` and caps |
| Rate limiting, per address | specified | the cheap half of DoS resistance |

**A hard rule:** no authentication mechanism ships enabled on a cleartext
listener. A login form served over HTTP is a credential given away, and a
server that offers one is worse than a server that offers nothing, because the
form is a claim that it is safe to type into.

---

## 6. Correctness and safety, already decided

These are built and are the properties the suites defend. Listed so that a
later change that breaks one is recognisable as a regression rather than a
refactor.

- **Path traversal is refused, not clamped.** `..` that would climb above the
  root rejects the request. Clamping to the root would silently serve a
  different file than the one asked for. Decoding happens *before*
  normalisation, so `%2e%2e%2f` is seen as `../` and refused.
- **`%00` is refused.** A NUL truncates a path in any C interface it later
  reaches, so `/safe.html%00.txt` passes an extension check and opens
  something else.
- **A `%` not followed by two hex digits is refused**, never passed through —
  something downstream will decode it later.
- **A backslash in a path is refused**, not translated.
- **Header values may hold printable ASCII and tab, nothing else.** A CR or LF
  inside a value is a forged header, and that is the whole of response
  splitting.
- **`obs-fold` is refused, not unfolded.** Two parsers disagree about where a
  folded value ends.
- **A space before a header's colon is refused.** Something that strips it sees
  a header something else does not.
- **Absolute-form targets are refused.** This is an origin server, not a proxy;
  accepting one would mean deciding whether the authority names this machine.
- **Error pages say only the status.** An error that reports what was wrong
  with the request tells an attacker which probe the parser noticed.
- **A short `send` is never treated as success.** A truncated body on a
  keep-alive connection desynchronises everything after it.
- **Bounds refuse rather than truncate.** A truncated target is a request for a
  different resource.

Still to decide, and worth deciding before the file handler exists: `Content-
Security-Policy`, `Strict-Transport-Security`, `X-Content-Type-Options`, and
CORS for the API. All four are cheap and all four are hard to add after
something depends on their absence.

---

## 7. Observability

The docx names an audit daemon and an Event Viewer, and the web server is one
of the things they will read.

| | status |
|---|---|
| Access log, structured | specified |
| Error log, separate | specified |
| Per-request timing | specified |
| Feeding the audit daemon | **blocked** — no audit daemon |
| A metrics endpoint for the dashboard | specified |

---

## 8. Build order

Each step is chosen so the thing before it is what makes it possible.

1. **Run it on ReconOS.** Everything above is host-verified only. This is also
   the first measurement of whether a socket descriptor moves bytes on this
   kernel — which has never been shown.
2. ~~**A static file handler**~~ — **built**, 15 September. It found that
   `open(O_CREAT)` in the C library never creates; see `docs/SERVER.md`.
3. **Streaming responses.** Before anything that returns a large body, not
   after.
4. **The admin console's first real page**, served by a handler, reading real
   machine facts through `SYS_MACHINE`.
5. **JSON in and out**, which turns the route table into a REST API.
6. **Form decoding**, `urlencoded` first.
7. **Conditional requests and caching**, which the console needs before it has
   many assets.
8. **Concurrency**, once the kernel can report readiness on more than a
   listener.
9. **TLS**, and with it Basic auth, HSTS, HTTP/2 and SNI. Everything in
   section 5 waits on this.

---

*Related: `docs/SERVER.md` (the role and its audit), `docs/KERNEL-WANTS.md`
(the datagram entry, and the entry for starting a program), `docs/ROLES.md`.*
