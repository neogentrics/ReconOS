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
| Service registry | **built** | `service.c` — the web server is one service; `/api/services` reports them |
| Range requests and `If-Range` | **built** | `range.c` — 206, 416, `Accept-Ranges`, and a stale `If-Range` sends the whole file |
| Security headers | **built** | `nosniff`, `DENY`, `no-referrer` and a real CSP on every response |
| `Expect: 100-continue` | **built** | `curl` sends it on any body over ~1 KB; the server no longer makes it wait |
| HTML escaping | **built** | `escape.c` — 21 checks; the dashboard no longer depends on the name validator for its safety |
| JSON escaping | **built** | `json.c` — 28 checks; ASCII-only by refusal, and every string in every endpoint goes through it. See VF-010 |
| `multipart/form-data` | **built** | `multipart.c` — 71 checks; the CRLF before a delimiter belongs to the delimiter, and a filename that climbs is refused rather than repaired |
| 201 / 408 / 409 / 415 | **built** | the upload endpoint and the receive deadline needed all four |
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
| Range requests (`206`) | **built** | `range.c` — 45 checks. One range only; a list is ignored and the whole file served |
| Conditional requests (`ETag`, `If-None-Match`, `304`) | **built** | `cache.c` — 31 checks; a strong validator, because there is no `stat` for `Last-Modified` |
| `Expect: 100-continue` | **built** | answered before the body is read; an expectation this server cannot meet gets 417 |
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
| **Form submission** | **built** | `server/http/form.c` — `urlencoded` decoded, 39 checks. A field given twice has **no** value: see below. `multipart/form-data` is built too — `multipart.c`, 71 checks. |
| **File upload** | **built, small** | `POST /api/upload` — `multipart.c` parses it and it is written to `/System/Uploads`, which **nothing serves**. Bounded by time rather than by the 64 KiB cap: see VF-013, about fifteen kilobytes today. |
| **Streaming a request body to disk** | **blocked** | a body is read whole into one buffer before a handler runs, so an upload cannot exceed it. Needs the request side to stream, which is the mirror of the response sink built in 0.8.0. |
| **File download** | **built** | streams from the volume in 8 KiB blocks, and resumes: `Range`, `If-Range`, 206 and 416. |
| **Server-sent events** | **unblocked** | the sink it needed now exists; the event framing is not written. |
| **WebSocket** | specified | needs the `Upgrade` handshake, SHA-1 for the accept key, and a framing layer. The connection stops being HTTP after the handshake, so it needs its own loop. |
| **Long-polling** | **unblocked, not built** | a request can now be parked without occupying the only process — `HTTP_CONNS_MAX` of them. What is missing is a handler that can be resumed, since dispatch is still synchronous. |
| **CGI-style external programs** | **blocked** | nothing in user mode can start a program — `KERNEL-WANTS.md` carries the entry. |
| **FastCGI / a persistent app backend** | **blocked** | same, plus a socket to talk to it over. |
| **Reverse proxy to another machine** | **blocked** | `connect` exists and does not work: it answers `SYS_OK` for a closed port and returns before the handshake. Measured 16 September; `KERNEL-WANTS.md` has it. This row previously said *buildable today*, which was wrong — see VF-009. |
| **Template rendering** | partial | `escape.c` exists and the dashboard uses it. A template engine does not, and when one arrives it must escape by default — one that escapes on request is one that is forgotten once. |
| **A JSON API** | **built** | `/api/status`, `/api/services` and the reply from `POST /api/name`. Escaped through `json.c` since 0.14.0; before that the machine name went in raw and was safe by coincidence — VF-010. |
| **The access log as JSON** | **unblocked** | 0.12.0 refused it because a request target can carry a quote via `%22` and there was no escaper. There is one now. Still not built, and the reason has changed: it would make one endpoint serve two formats, and two representations of one thing drift exactly like two lists do. It needs a decision about content negotiation first, not a few more lines. |

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

~~**Concurrency.**~~ **Built**, 16 September, and the paragraph that stood here
was wrong about why it could not be.

It said the server was single-threaded and served one connection to completion
before accepting the next, which was true, and that the way out needed *the
kernel to report readiness on more than a listener; today `accept` is the only
call that answers `EAGAIN`*. That last part was not true. `recv` reports
readiness too — it answers 0 with `errno` 0 when nothing has arrived — and it
had been doing so all along. Nobody knew, because the code read that zero as a
closed connection, which is the fault VF-013 is about.

So the capability that was said to be missing was the same behaviour that was
silently breaking every request over 4 KiB. Finding the bug is what revealed
the feature.

`serve.c` now holds `HTTP_CONNS_MAX` connections and gives each a turn. What is
concurrent is **reading**, which is where the seconds are: a stalled request no
longer holds anybody else for the fifteen seconds of `RECV_DEADLINE_MS`.
Dispatch and the response are still synchronous per request, deliberately —
making a handler resumable would mean every handler becoming a state machine,
and no handler here is slow.

Measured on the machine: with one client stuck mid-body, three others were
answered in about 0.2 s each. `server/tests/test_http_concurrent.c` is the
suite, and against the previous `serve.c` — recovered from git and compiled
unchanged — 4 of its 12 checks fail.

The other two ways out, for the record:

1. **A process per connection.** Still needs a way to start a program —
   blocked.
2. **A blocking `recv` with a timeout.** Would make the loop cheaper rather
   than more capable; it is in `docs/KERNEL-WANTS.md` for that reason.

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

### One range, and why a list is refused

The standard allows `bytes=0-99,200-299,5000-5099`, answered as a
`multipart/byteranges` body. This serves the whole file instead, which is
explicitly allowed — a server **may** ignore a `Range` it does not wish to
honour.

It is refused because a list costs the server far more than it costs the
client. A few hundred bytes of header can ask for ten thousand one-byte ranges,
each needing its own boundary, its own headers and its own seek: a small
request, an enormous response, and a great deal of work. That has been used as
an amplification attack against more than one well-known server, and the
multipart writer it needs exists only to serve that shape.

A client that genuinely wants several pieces can ask several times.

### `If-Range` is not optional

A client resuming a download sends the range it still needs. If the file changed
since the first half was fetched, the two halves are from different files and
what lands on disk is neither — with a 206 beside it saying all is well.

`If-Range` is the guard: honour the range only if the representation is
unchanged, otherwise send the whole thing. It is cheap here because the
validator already exists, and leaving it out would be shipping the
silent-corruption case on purpose.

**Its comparison is strong, and it is the only one here that is.** A weak
validator means "near enough the same representation" — fine for deciding
whether to re-send a body, and not fine for stitching half a file onto another
half. Two weakly-equal representations may differ byte for byte, which is
exactly what a range depends on.

A `Range` sent *without* an `If-Range` gets no protection from any of this,
which is the client having declined it.

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

**Three of these are now sent on every response**, written by the server rather
than by a handler so a handler cannot forget one:

- `X-Content-Type-Options: nosniff` — this server decides a type from the
  file's extension, a statement by whoever put the file there. Sniffing
  overrides that with a statement by whoever wrote the *contents*.
- `X-Frame-Options: DENY` — an administrative console with a form that renames
  the machine has no reason to be in anybody's frame.
- `Referrer-Policy: no-referrer` — an internal path in a `Referer` is a small
  map of the network handed to whatever the link pointed at.

**`Content-Security-Policy` is sent now**, and it says what it means:

```
default-src 'none'; style-src 'self'; form-action 'self';
frame-ancestors 'none'; base-uri 'none'
```

It could not be sent before. The dashboard was built from inline `style=`
attributes and a `<style>` block, so any policy shippable then needed
`'unsafe-inline'` — which permits exactly what CSP exists to stop while the
header's presence suggests otherwise. The styles moved to `/console.css`, which
the role writes to the volume at boot, and the policy became true rather than
decorative.

`default-src 'none'` starts from nothing and permits what is needed, which is
the only direction that fails closed. There is no `script-src`, because
`default-src 'none'` already refuses scripts and this console has none — adding
one later should be a deliberate act rather than something that quietly already
worked.

`Strict-Transport-Security` waits on TLS, and would be a lie without it.
CORS for the API is still to decide.

---

## 7. Observability

The docx names an audit daemon and an Event Viewer, and the web server is one
of the things they will read.

| | status |
|---|---|
| Access log | **built** — `server/log.c`, a ring in memory; `GET /api/log` |
| Error log, separate | specified |
| Per-request timing | partial — each entry carries the clock it was recorded at |
| Feeding the audit daemon | **blocked** — no audit daemon |
| A metrics endpoint for the dashboard | partial — `/api/status` and `/api/services` |

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
8. ~~**Concurrency**~~ **Built**, 16 September. Not by waiting for a new
   kernel call — by discovering that the one needed already existed and was
   being misread. See above, and VF-013.
9. **TLS**, and with it Basic auth, HSTS, HTTP/2 and SNI. Everything in
   section 5 waits on this.

---

*Related: `docs/SERVER.md` (the role and its audit), `docs/KERNEL-WANTS.md`
(the datagram entry, and the entry for starting a program), `docs/ROLES.md`.*
