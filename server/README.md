# ReconOS — the server role

A ReconOS machine whose role is **server**: it answers for a network rather
than sitting in front of a person.

This is one of five roles in `docs/ROLES.md`. The installer puts down the whole
operating system every time and the role is chosen on the first boot
afterwards, so a server is meant to be **configuration, not a build**: one
medium producing a workstation, a firewall or a NAS depending only on what it
is told it is.

**That is the design, and today it is a build.** The first-boot choice does not
exist yet — there is nowhere to record the answer and nothing to ask the
question. So `make ROLE=server` selects which init program is linked into the
kernel, which is conditional compilation, and an earlier draft of this README
said it was not. The line is kept here rather than quietly corrected because
the gap is the interesting part: everything below is written against the role
being a choice a machine makes, and the day it becomes one, nothing below
changes. See `docs/ROLES.md` for the mechanism, and **VF-008** in
`docs/VERIFICATION.md` for the two consecutive builds that reported success and
produced nothing while this stand-in was being wired up.

Branch `server`. This README covers this role; the repository's own `README.md`
covers the operating system.

---

## At a glance

| | |
|---|---|
| **Version** | 0.19.0 |
| **Runs on** | x86_64 under QEMU, with virtio-net |
| **Verified** | on the machine, 16 September 2026 |
| **Checks** | 684 across seventeen suites, by `scripts/server-tests.sh` |
| **Kernel** | 0.2.48, merged from `origin/kernel` |

The check figure is the first one this project has that was not assembled by
hand. See **VF-012**: the suites had been run one `gcc` line at a time for
thirteen versions, and a suite nobody remembers to run is a suite that cannot
fail.

---

## What works right now

**A web server, serving on port 80 from a running ReconOS machine.** Not a
demonstration that a socket opens — a browser on another machine asks for a
page and gets one.

```
$ curl -i http://10.0.2.15/
HTTP/1.1 200 OK
Server: ReconOS
Content-Length: 1194
Connection: keep-alive
Content-Type: text/html; charset=utf-8
```

The routes, which are the beginning of the administrative console the
architecture document asks for:

| route | what it answers |
|---|---|
| `GET /` | the dashboard — what this machine is, what it has done, and a form that renames it |
| `GET /api/status` | the same facts as JSON, from the same structure |
| `GET /health` | `ok`, for something that is not a person |
| `GET /api/services` | every registered service: state, polls, faults, restarts |
| `GET /api/log` | the last 64 requests answered, and how many were dropped |
| `POST /api/name` | renames the machine, validated by the same code that numbers a parallel. **Needs the token** |
| `POST /api/upload` | takes a `multipart/form-data` file and keeps it in `/System/Uploads`, which **nothing serves**. **Needs the token** |
| `GET /api/resolve?name=` | looks a name up in DNS and answers with its addresses. **Needs the token** — an open resolver endpoint is an open resolver |
| anything else | a file from `/System/Web` on the volume, or 404 |

Every number on that page is read from the kernel through `SYS_MACHINE` or
counted by the server. Nothing on it is illustrative.

**Parallel naming.** A machine cloning `M16` calls itself `M17`; one cloning
`srv007` calls itself `srv008`, because a parallel that drops the padding has
renamed the family rather than joined it. `gateway` is refused outright —
there is no next `gateway`, and inventing `gateway2` would be this code
deciding what an operator meant.

---

## What this role proved

The kernel session recorded, in their own words, that a socket descriptor
working with `read` and `write` was **argued, not measured** — their self-test
cannot hold a descriptor, because a kernel thread has no process and
`fd_install` fails there for sockets exactly as it does for pipes.

This role measured it. On 15 September 2026 a request from outside the machine
reached a program in ring 3, and that program's answer reached the client
intact. It also found a fault in the doing: `tcp_write` reported it had sent
1194 bytes when it had sent 512. That is written up in full, with the numbers,
in `docs/SERVER.md`.

---

## Building and running

The role is a `make` variable, standing in for the first-boot choice that does
not exist yet. **`make` with nothing said builds the workstation exactly as it
did before this role existed.**

```bash
cd kernel
make ARCH=x86_64 ROLE=server
```

Then boot it with a card the kernel can actually drive. QEMU's default NIC is
an Intel e1000 and this kernel drives virtio-net only, so it must be asked for
by name — otherwise the machine comes up with *no card to configure* and
nothing listens:

```bash
qemu-system-x86_64 -kernel build/x86_64/reconos-kernel.elf -m 512M -no-reboot \
  -display none \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:8080-:80 \
  -device virtio-net-pci,netdev=n0 \
  -serial mon:stdio
```

The machine takes an address by DHCP from QEMU and says so, then says what the
server is doing:

```
net: eth0 is 10.0.2.15, via 10.0.2.2
  the web server: listening on :80 -- 0 requests served, 0 bytes out
  the web server: 1 served
```

`curl http://127.0.0.1:8080/` from the host reaches it.

### The suites

They run on the host and need no machine:

```bash
./scripts/server-tests.sh
```

This used to be three hand-typed `gcc` commands, which is the practice
**VF-012** is about: two of them said `-std=c11`, which is not what this
project builds with, and following them left two suites unbuildable.

| suite | checks | what it holds |
|---|---|---|
| `server_identity` | 34 | naming a parallel, and every way of naming it wrong |
| `server_http` | 93 | one request, and every way of writing two |
| `server_http_serve` | 43 | the server over a real socket, `serve.c` unmodified |
| `server_http_files` | 39 | serving a file, and every way of serving the wrong one |
| `server_http_stream` | 23 | streaming, and the promise that must not be broken |
| `server_http_form` | 39 | decoding a form, and the field that has two values |
| `server_http_cache` | 31 | validators, and reading an If-None-Match |
| `server_service` | 37 | services, their states, and the restart that has to stop |
| `server_http_range` | 45 | asking for part of a file, and the ways that hands over the wrong part |
| `server_http_escape` | 21 | escaping text for HTML, and the characters people forget |
| `server_http_json` | 28 | escaping text for JSON, and the byte that ends a string early |
| `server_http_multipart` | 71 | reading a multipart body, and the two bytes that ruin a file |
| `server_http_concurrent` | 12 | two clients, one of them stuck |
| `server_auth` | 33 | a guard, and every way of getting past one that is not the token |
| `server_dns` | 72 | asking for an address, and the answers that must not be believed |
| `server_log` | 24 | a ring of recent entries, and the count that stops it lying |
| `server_dial` | 38 | three answers, and the two ways of confusing them |

It reads its target list out of `CMakeLists.txt` rather than keeping one of its
own, for the same reason `http_reason` is generated from an X-macro: two lists
drift, and the drift is invisible until something is already wrong. A suite
added to the build is run by it the same day; a suite added only to it does not
exist.

**Each was watched failing before it was believed.** The naming suite was run
against the `atoi` shape its header rejects and nine cases failed; the HTTP
suite against a parser with the duplicate-length check removed and `..` clamped
instead of refused, and six failed. A suite nobody has seen fail proves only
that the code and the suite agree.

The files suite taught that lesson a second time. With `escapes()` removed its
four traversal cases failed; relaxing the over-size check changed **nothing**,
because the handler guards that twice and the suite was only exercising one
half. A check that survives the removal of the thing it checks is not evidence
about that thing.

The JSON suite taught it a third time, about the mutant rather than the suite.
A scripted edit meant to strip `json.c` back to a quote-and-backslash escaper
landed on two of its three sites, leaving the short forms in place — 8 failures
where the real mutant gives 13. Rewritten by hand, and the header now carries
the number so the claim can be checked rather than taken. **A weaker mutant
reported as a stronger one is a suite that has been overrated, not tested.**

---

## Layout

```
server/
  README.md          this file
  identity.c         naming a parallel -- M16 -> M17
  include/
    recon_server.h   the role's own interface
  http/
    http.h           bounds, verdicts, and what a request is
    request.c        parsing. No sockets in it, on purpose
    serve.h          routes, handlers, responses
    serve.c          the socket loop. No parsing in it, on purpose
  init/
    server_init.c    the first program on a server-role machine
  tests/             the three suites above
```

The split between `request.c` and `serve.c` is the important one. Parsing is
what faces an attacker and can be tested exhaustively with string literals;
serving is what cannot. Neither file contains the other's job.

---

## What is not here

Stated plainly, because a list of features with no list of gaps is a sales
page. `docs/SERVER.md` carries the full audit and `docs/WEB.md` the web
server's specification.

- **DNS, DHCP, DDNS and NTP cannot be started at all.** All four are
  unconnected-datagram protocols and no system call opens an unconnected
  datagram. Filed at the top of `docs/KERNEL-WANTS.md`; the kernel already has
  `udp_bind_port`, `socket_sendto` and `socket_recvfrom` working internally,
  with nothing reaching them from ring 3.
- **No TLS**, so no HTTP authentication of any kind. A login form served over
  cleartext is a credential given away, and this role will not offer one.
- **One connection at a time.** A slow client blocks every other, which is a
  denial of service costing the attacker one socket. It needs non-blocking
  sockets, and today `accept` is the only call that reports readiness.
- **No process supervision**, and there cannot be: nothing in user mode can
  start a program. `server/service.c` supervises what this one process does,
  which is a different thing and says so.
- **No peer discovery, and not for the reason this file used to give.** It
  needs broadcast — and the TCP sweep that was written down as the way around
  that does not work either. Measured on the machine: `connect` answers
  `SYS_OK` for a port nothing is listening on, so a probe cannot tell a
  reachable host from an unreachable one, and a write straight after a
  successful `connect` fails because the handshake has not finished. A program
  also cannot learn its own address, so it would not know what to sweep. Both
  are filed in `docs/KERNEL-WANTS.md`; the wrong claim is VF-009.
- **No outbound connections at all**, for the same reason. Everything this role
  has proved is the server half of a socket.

---

## Version history

Newest first. The number tracks what works, not what is planned.

| Version | What it brought |
| --- | --- |
| **0.19.0** | **Merged kernel 0.2.48, and found what it broke.** KF-244 landed as announced: `connect` no longer answers success for a port nothing is listening on, so the standing measurement reads an honest *in flight* instead of a lie — and it is now made with `dial.c`, which was written three versions ago for this exact day and had never been able to run. But the same change made `connect` answer `SYS_EIO` for every **datagram** socket, because `sys_connect` asks a progress function that reads *not a stream* as *did not make it*. Those are different facts. That is the only shape of UDP a program can use, so DNS stopped resolving on the first boot after the merge. Diagnosed against three controls — inbound TCP still served, the kernel's own DHCP still completed, the network came up identically — then fixed, rebuilt, and proved by the same request answering with real addresses again. **All 684 checks passed before and after**, because every one is a host suite and the fault was in the kernel: a merge verified by suites alone would have been called clean. Reported in `docs/SIGNALS.md`, no KF claimed. VF-019. |
| **0.18.0** | **This machine resolves names.** The audit called DNS blocked on “an unconnected datagram socket”, and that was true of DHCP and of a DNS *server* — but a resolver is a **connected** datagram, which this kernel has supported since the socket layer landed. `kernel/core/socket_file.c` said so in as many words. Measured on the machine before a line was written, then built: `server/dns.c`, 72 checks. **The danger is not in asking, it is in believing.** A compression pointer must point strictly backwards — against a parser that follows them freely the suite does not fail, it **hangs**, which is a denial of service costing one datagram. And a response is only an answer if the identifier, the question, its type and its class all match; against a parser that skips those, four checks come back `DNS_OK` where a refusal belonged, which is off-path cache poisoning working. `GET /api/resolve` is guarded, because an open resolver endpoint is an open resolver. Two faults found after it worked: a trailing-dot name that encoded instead of being refused, and a failure path that reported an `rcode` from the previous request's stack. VF-017, VF-018. |
| **0.17.0** | **The writes stop being open to everybody.** `POST /api/name` renamed the machine and `POST /api/upload` wrote to its volume for anyone who could reach port 80 — a hole the upload endpoint had widened one version earlier. The obvious defence was unavailable and not for want of effort: `SYS_ACCEPT` takes only a descriptor, so the kernel cannot report who connected and an address check cannot be written. Filed. Instead, a random token from `SYS_RANDOM`, made at boot and printed on the console — a capability saying *whoever can read this machine's console*, not a password. The check is in the **server**, not the handlers, for the reason the security headers are: a guarded route on a site with no policy answers **500**, never 200, because falling back to open is how a guard turns out never to have guarded. Reads stay open. Honest about its limit: the token is in clear on the wire, so it stops a passer-by and not somebody on the path — `docs/WEB.md` §5's hard rule is amended there rather than bent. Also: the connection pool could have guarded one site with another's secret, found by writing the test for the 500; and the fourth patch-script-broken string literal became `scripts/check-c-literals.py`, which the runner now runs before it compiles anything. VF-015, VF-016. |
| **0.16.0** | **Several connections at once, and the capability that turned out to already exist.** `docs/WEB.md` called single-connection serving the most serious limitation in it — a denial of service costing one socket — and said the fix needed the kernel to report readiness on more than a listener. It already did: `recv` answers 0 with `errno` 0 when nothing has arrived, which is exactly the behaviour that had been silently dropping every request over 4 KiB one version earlier. The missing feature and the bug were one fact seen from two sides. `serve.c` now holds four connections and gives each a turn; **reading** is concurrent, which is where the seconds are, and dispatch stays synchronous on purpose. Measured on the machine: one client stuck mid-body, three others answered in 0.2s each. The new suite fails 4 of 12 against the old server — and the eight that pass are the point, because a suite that follows one client would have called it perfectly good. That is VF-014: fifteen suites, none of which could see the limitation everyone had written down. |
| **0.15.0** | **A file upload, and the reason no request over 4 KiB had ever worked.** `multipart.c` reads `multipart/form-data` — 71 checks, watched failing at 12 against the obvious parser, which returns every value two bytes too long because the CRLF before a delimiter belongs to the delimiter. A filename that climbs is refused rather than repaired, and uploads land in `/System/Uploads`, which nothing serves. Wiring it up found the bigger fault: `serve.c` read `recv` returning 0 as end-of-stream, when on this kernel it means *nothing yet*. Every request whose bytes did not all arrive in one read was dropped without an answer — which had never happened before, because a page request fits in one read and an upload does not. The same file already documented the identical behaviour on the **send** side and had never looked at the receive side. Fixed with a real clock rather than a spin count, and a request cut off at the deadline now gets 408 and a log entry instead of silence. **The kernel half is filed, with measurements from both ends**: a burst stalls at 2880 bytes and trickles in at a kilobyte a second, while the same bytes paced by the sender arrive at full speed and twenty times the size. |
| **0.14.0** | **The coincidence removed from HTML in 0.9.0, found still holding up the JSON.** `/api/status` and the reply from `POST /api/name` wrote the machine's name into a JSON string unescaped — safe only because the name validator happens to forbid a quote, which is exactly the argument 0.9.0 exists to have stopped making. Found by reading back 0.12.0's own refusal to serve the log as JSON, which named the hazard correctly and assumed its scope. `json.c` now escapes **every** string in every endpoint, including the ones that cannot hold a quote today, because a rule with an exception for known-safe values is a rule the next person has to apply silently. Two things fell out of the wiring: a handler could not say a failure was the *server's* (`http_status_for` answered 400 for anything unrecognised, blaming the client for the server running out of buffer) — now `HTTP_EINTERNAL`, 500; and thirteen suites had no way to be run except by hand, which `scripts/server-tests.sh` fixes, finding two that did not build under its own flags in its first minute. |
| **0.13.0** | **A client that can tell *not yet* from *never*.** The kernel fixed `connect` (KF-244) so it answers established, in-flight or refused, and made it idempotent so polling it is the interface. `dial.c` is that loop, with the deadline owned by the caller because the kernel has none and a sweep and a proxy want different ones. It compares **no numbers** — the announcement's table had all three wrong, and building against names instead is what made that harmless. Not yet run on the machine: the fix is committed locally and not pushed. |
| **0.12.0** | **An access log that admits what it lost.** Every answered request is recorded by the server, including the ones refused before any handler ran — those are the entries somebody comes looking for. A ring drops the oldest to make room, and a log that drops silently looks complete while missing exactly the burst being investigated, so the dropped count is reported beside the entries. Served as plain text, not JSON: a request target can contain a quote via `%22`, and there is no JSON escaper yet. |
| **0.11.0** | **`Expect: 100-continue`, which was costing every large POST a second.** A client that asks permission before sending a body was never answered, so it waited out its own timeout and sent the body anyway — nothing failed, nothing was reported, and every such request just took a second longer. `curl` does this on any body over about a kilobyte. An expectation the server cannot meet now gets 417 rather than silence, because silence reads as yes. |
| **0.10.0** | **A Content-Security-Policy that is true.** The console's styles moved out of the page and onto the volume as `/console.css`, which is what made `style-src 'self'` an honest claim rather than one needing `'unsafe-inline'`. Also fixed a bug created by adding the second file: the site layout returned as soon as `index.html` existed, so every machine that already had a page would never have got the stylesheet. |
| **0.9.0** | **Hardening what is served.** `nosniff`, `DENY` and `no-referrer` on every response, written by the server so no handler can forget one — CSP deliberately left out, because the only policy shippable today would need `'unsafe-inline'` and would read as protection it does not give. And the dashboard escapes the machine name, removing a documented dependency whose safety lived in a validator three files away. |
| **0.8.0** | **Resuming a download.** `Range`, `If-Range`, 206 and 416, with the real length on the 416 so a client can recover. One range only — a list can ask for ten thousand one-byte pieces from a few hundred bytes of header. `If-Range` compares strongly, because two weakly-equal representations may differ byte for byte and that is exactly what a range depends on. Also: every status phrase now comes from one table the suite walks, after the hand-written list failed twice. |
| **0.7.0** | **The client side, measured — and it does not work.** `connect` answers `SYS_OK` for a port nothing is listening on and returns before the handshake, so a write straight after it fails. Discovery, a reverse proxy and every outbound connection are blocked, and the TCP sweep this role had written down as the way around the missing broadcast was never possible. The measurement stays in as a standing check, so the day the kernel fixes it somebody finds out without looking. |
| **0.6.0** | **A service registry, and what it refuses to pretend.** Nothing on this system can start a program, so this supervises what *this process* does rather than daemons — the web server is one service, and discovery and DNS will register beside it without the main loop changing. A failing service is restarted a bounded number of times and then stays failed: restarting forever turns a crash into a crash loop, which reads as healthy and does nothing. |
| **0.5.0** | **Conditional requests.** A strong ETag from a hash of the content, because there is no `stat` and so no modification time to use — which costs a second read and buys a tag that survives a file being touched and changes when a same-length edit is made. A client offering the right tag gets 304 and no body. The bytes are hashed again on the way out, because a wrong validator is cached and served until it expires. |
| **0.4.0** | **A write side.** `urlencoded` form decoding, and `POST /api/name` renames the machine — validated by `server_name_split`, so one idea of a legal name serves both renaming and parallel numbering. A field given twice has **no** value, because two values is not an answer and choosing one is how a value walks past a filter. |
| **0.3.0** | **Streaming, and a promise that is checked.** A handler writes into a sink as it goes, so a response is no longer limited to what a program can hold — the file handler streams and serves files far past the old cap. A declared length that is not delivered closes the connection rather than desynchronising the next request. Chunked for HTTP/1.1, close-delimited for 1.0. |
| **0.2.0** | **Files off the volume.** A static file handler, as one handler among others rather than the server's middle — MIME by extension, an index for directories, never a listing. It found `open(O_CREAT)` in the C library silently dropping the flag, and a fault of its own reading `mkdir`'s `EEXIST` as failure. |
| **0.1.0** | **A page served from a ReconOS machine.** The web server runs in ring 3 on the real kernel and answers a client outside it — the first bytes ever moved over an accepted connection on this system. It found `tcp_write` reporting 1194 bytes sent when it had sent 512. |
| **0.0.2** | **The web server, and what it refuses.** 92 checks, host-verified: request smuggling by double framing, `%2e%2e%2f` traversal, `%00`, a space before a colon, a CR in a value. Built as a routing table of handlers so a page and an API are the same shape. |
| **0.0.1** | **The role opened, and a parallel named.** `M16` → `M17`, `srv007` → `srv008`, `gateway` refused. The audit table, and the datagram entry that blocks DNS and DHCP. |

---

*Related: `docs/SERVER.md` (the role and its audit), `docs/WEB.md` (the web
server in full), `docs/ROLES.md` (all five roles),
`docs/KERNEL-WANTS.md` (what this role needs next).*
