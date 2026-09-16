# ReconOS — the server role

A ReconOS machine whose role is **server**: it answers for a network rather
than sitting in front of a person.

This is one of five roles in `docs/ROLES.md`. The installer puts down the whole
operating system every time; the role is chosen on the first boot afterwards,
so a server is **configuration, not a build**. Nothing in here is conditionally
compiled, and the same medium produces a workstation, a firewall or a NAS
depending only on what it is told it is.

Branch `server`. This README covers this role; the repository's own `README.md`
covers the operating system.

---

## At a glance

| | |
|---|---|
| **Version** | 0.7.0 |
| **Runs on** | x86_64 under QEMU, with virtio-net |
| **Verified** | on the machine, 15 September 2026 |
| **Checks** | 303 across eight suites |
| **Kernel** | 0.2.41 |

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

Three routes, which are the beginning of the administrative console the
architecture document asks for:

| route | what it answers |
|---|---|
| `GET /` | the dashboard — what this machine is, what it has done, and a form that renames it |
| `GET /api/status` | the same facts as JSON, from the same structure |
| `GET /health` | `ok`, for something that is not a person |
| `GET /api/services` | every registered service: state, polls, faults, restarts |
| `POST /api/name` | renames the machine, validated by the same code that numbers a parallel |
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
gcc -std=c11   -Wall -Wextra -Werror -o t1 server/identity.c server/tests/test_identity.c
gcc -std=c11   -Wall -Wextra -Werror -o t2 server/http/request.c server/tests/test_http.c
gcc -std=gnu11 -Wall -Wextra -Werror -o t3 server/http/request.c server/http/serve.c \
                                           server/tests/test_http_serve.c
```

| suite | checks | what it holds |
|---|---|---|
| `server_identity` | 34 | naming a parallel, and every way of naming it wrong |
| `server_http` | 77 | one request, and every way of writing two |
| `server_http_serve` | 23 | the server over a real socket, `serve.c` unmodified |
| `server_http_files` | 39 | serving a file, and every way of serving the wrong one |
| `server_http_stream` | 23 | streaming, and the promise that must not be broken |
| `server_http_form` | 39 | decoding a form, and the field that has two values |
| `server_http_cache` | 31 | validators, and reading an If-None-Match |
| `server_service` | 37 | services, their states, and the restart that has to stop |

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
