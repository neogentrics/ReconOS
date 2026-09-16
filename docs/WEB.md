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
| Static files from the volume | **built** | `files.c` — 30 checks; on the machine |
| Streaming responses | **built** | `serve.c` — 23 checks; a declared length is a promise |
| Form decoding, and the write side | **built** | `form.c` — 39 checks; `POST /api/name` renames the machine |
| Conditional requests and caching | **built** | `cache.c` — ETag, `If-None-Match`, 304, `Cache-Control` |
| Chunked responses (`Transfer-Encoding` out) | **built** | for HTTP/1.1; 1.0 gets a close-delimited body |
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
| Conditional requests (`ETag`, `If-None-Match`, `304`) | **built** | `cache.c` — 31 checks; a strong validator, because there is no `stat` for `Last-Modified` |
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
| **Form submission** | **built** | `server/http/form.c` — `urlencoded` decoded, 39 checks. A field given twice has **no** value: see below. `multipart/form-data` is still specified, not written. |
| **File upload** | specified | needs `multipart/form-data` and streaming to disk; the 64 KiB body cap exists because nothing streams yet. |
| **File download** | **built** | streams from the volume in 8 KiB blocks. Range requests are still specified, not written. |
| **Server-sent events** | **unblocked** | the sink it needed now exists; the event framing is not written. |
| **WebSocket** | specified | needs the `Upgrade` handshake, SHA-1 for the accept key, and a framing layer. The connection stops being HTTP after the handshake, so it needs its own loop. |
| **Long-polling** | **blocked** | needs a request to be parked without occupying the only process. See concurrency below. |
| **CGI-style external programs** | **blocked** | nothing in user mode can start a program — `KERNEL-WANTS.md` carries the entry. |
| **FastCGI / a persistent app backend** | **blocked** | same, plus a socket to talk to it over. |
| **Reverse proxy to another machine** | specified | `connect` exists, so this is buildable today. The docx names it explicitly. |
| **Template rendering** | specified | with escaping by default; a template engine that escapes on request is one that is forgotten once. |

### The two that shape the others

~~**Streaming responses.**~~ **Built**, 15 September. A handler is handed a
sink and writes as it goes, so the size of a response is no longer the size of
what a program can hold. The static file handler was rewritten onto it and a
file far larger than `HTTP_RESPONSE_MAX` is now served whole.

**The rule that makes it safe:** a declared length is a promise, and one this
server cannot keep closes the connection rather than being broken quietly. A
handler that says `Content-Length: 4096` and writes 3000 leaves the client
waiting -- and on a kept connection the client reads the *next* response's head
as this body's tail. That is request smuggling's desynchronisation arrived at
from the server's own side, and it gets the same treatment: refuse rather than
hope.

**Chunked is refused coming in and used going out, which is not a
contradiction.** A request framed two ways is dangerous because two *different*
parsers must agree about a body neither wrote. A response this server frames is
written by this server, once, with one framing chosen at `http_stream_begin`.

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

### A field given twice has no value

`a=1&a=2` is legal to send and there is no agreement about what it means.
Different stacks take the first, the last, both, or join them with a comma.
**That disagreement is the vulnerability** — where a filter and the thing
behind it read one request differently, a value walks past the filter. It is
called HTTP parameter pollution, and it is request smuggling's shape one layer
up.

So `http_form_get` refuses a duplicated name. It answers NULL, exactly as it
does for a name that is absent, and `http_form_count` tells the two apart for a
caller who needs to know. Two values is not an answer, so there is no answer.

`role=user&role=admin` is the case worth picturing.

### `+` is a space here and a plus sign in a path

Two decoders rather than one with a flag. A path decoder used on a form turns
`1+2` into `1 2`; a form decoder used on a path turns a file named `a+b` into
`a b`. They look like one encoding and are not, and a flag is a thing a caller
passes wrong exactly once.

---

### The validator is a content hash, and that was not a free choice

`Last-Modified` is the cheap validator everywhere else and is not available:
this C library has no `stat`, so a file's modification time cannot be asked
for. What is left is the content, so the ETag is a hash of the bytes.

That is **strong** — it changes when the content changes and not when anything
else does, so a file touched without being edited keeps its tag. It costs a
second read of every file, because the tag must be in the head and the head
goes out before the body. For a console's assets that is the right trade; for a
large file served once it is a real cost, and it is written down rather than
hidden.

**The hazard it creates is worse than a wrong length.** Hashing and sending are
two observations, and something may edit the file between them. A length
mismatch breaks one connection; a validator that does not match its bytes is
stored by the client and served from that store until it expires, so one bad
answer becomes every answer. So the bytes are hashed again on the way out and
compared, and a disagreement closes the connection — the same treatment a
broken length promise gets, for the same reason.

`no-cache`, not `no-store`. The first means *keep it and ask before using it*,
which is what the ETag exists to enable. The second forbids keeping it and
would throw all of that away.

---

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
3. ~~**Streaming responses.**~~ **Built**, 15 September, and it is what let
   the file handler stop reading whole files into memory.
4. **The admin console's first real page**, served by a handler, reading real
   machine facts through `SYS_MACHINE`.
5. **JSON in and out**, which turns the route table into a REST API.
6. **Form decoding**, `urlencoded` first.
7. ~~**Conditional requests and caching**~~ **Built**, 15 September. A content
   hash rather than a modification time, because this C library has no `stat`
   — which costs a second read of every file and buys a validator that changes
   when the content does and not when anything else does.
8. **Concurrency**, once the kernel can report readiness on more than a
   listener.
9. **TLS**, and with it Basic auth, HSTS, HTTP/2 and SNI. Everything in
   section 5 waits on this.

---

*Related: `docs/SERVER.md` (the role and its audit), `docs/KERNEL-WANTS.md`
(the datagram entry, and the entry for starting a program), `docs/ROLES.md`.*
