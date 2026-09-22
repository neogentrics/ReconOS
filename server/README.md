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
| **Version** | 0.38.0 |
| **Runs on** | x86_64 under QEMU, with virtio-net |
| **Verified** | on the machine, 21 September 2026 |
| **Checks** | 1517 across twenty-four suites, by `scripts/server-tests.sh`,
98 more on a booted machine by `scripts/machine-tests.sh`, and 19 across two
boots by `scripts/config-round-trip.sh` |
| **Kernel** | 0.5.14, merged from `origin/kernel` — **plus one fix that is not in their tree**, see below |

The check figure is the first one this project has that was not assembled by
hand. See **VF-012**: the suites had been run one `gcc` line at a time for
thirteen versions, and a suite nobody remembers to run is a suite that cannot
fail.

---

### What kernel this branch actually runs

**0.5.14 from `origin/kernel`, plus `tcp_tick()` in `socket_accept`.** That
line is the difference between a server that answers twelve requests and one
that keeps going (VF-034). It has been offered to the kernel session three
times and is not on their branch yet, so it is carried here.

Which means a machine built from this branch **prints `ReconOS kernel 0.5.14`
and does not behave like their 0.5.14.** That is worth stating in the place a
person reads, because the version string is the only thing you can read off a
running kernel — and the network session raised exactly this as NW-020, where
three branches were answering to one number with three different trees.

The number is deliberately **not** changed here. Renumbering the kernel belongs
to the kernel session, who own it at merge time; a seat that renumbers
unilaterally is how one number comes to mean four things. So this branch says
what it has instead of claiming a number for it.

`kernel/Makefile` also differs, and that one is not a behaviour change: it is
the `ROLE=server` build, written as an override with a default so that
`make ARCH=x86_64` with nothing else said produces exactly the bytes it
produced before this branch existed.



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
| `GET /` | the dashboard — what this machine is, what it has done, what its clock and services are doing, and a form that renames it |
| `GET /api/status` | the same facts as JSON, from the same structure — including the clock offset with its uncertainty |
| `GET /health` | `ok`, for something that is not a person |
| `GET /api/services` | every registered service: state, polls, faults, restarts |
| `GET /api/log` | the last 64 requests answered, and how many were dropped — the durable copy is in `/System/Logs` |
| `GET /api/log/segments` | which durable segments the volume holds. **Needs the token** — the ring is a snapshot, this is the whole history |
| `GET /api/log/segment?n=` | one segment, as text. The path is **built** from the number, never taken from the caller |
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

### With a volume, for the four things that need one

Most of this role runs from a diskless boot — the web server, the resolver, the
clock and the guard all work with no disk at all. Four things do not, and they
are the four easiest to leave untested: **serving a file** off the volume,
**`/console.css`** without which the console renders unstyled, an **upload**
landing somewhere rather than being accepted and dropped, and whether any of it
**survives a reboot**.

```bash
./scripts/server-disk.sh
```

It builds the server role, makes a 16 GB image under `kernel/build/` and runs
the installer onto it, then prints the QEMU line to boot it with. Regenerated
rather than committed, and **not** kept in `/tmp`: that is shared with the other
sessions on this machine and is cleaned without warning — an image and every
reference boot log went with it twice on 17 September, and the second time the
loss sent a diagnosis down the wrong path for four boots.

Sixteen gigabytes is not a round number picked for comfort. At one gigabyte the
installer answers *there is room for ReconOS but not for a separate partition to
install programs into* and lays down nothing.

### The suites

They run on the host and need no machine:

```bash
./scripts/server-tests.sh
```

### And the ones that need a machine

```bash
./scripts/machine-tests.sh
```

```bash
./scripts/config-round-trip.sh
```

The second one needs **two** boots, because that is the design: a configuration
written over the network takes effect at the next start, so a single boot cannot
show it working. It makes its own volume, refuses a candidate that will not
parse, writes one that will, restarts the machine and finds it running the new
generation. The shape is VF-025's, which is the entry about an upload endpoint
that had never been shown to keep anything.

Boots the server role under QEMU, waits for the line that says it is listening,
reads the boot token off the console, and asks the machine eighty-six questions
over a real socket. About a minute.

**It exists because every suite above was green on a server that answered twelve
requests and then went silent for the rest of the boot.** That is VF-034, and
the way it was found is the point: a measurement for an unrelated feature
happened to need a thirteenth connection. Nothing in this repository would
otherwise have asked for one.

A host has thousands of descriptors, a real TCP stack and a `recv` that blocks.
The target has sixteen connections, a `recv` that answers 0 with nothing
buffered, and a volume. **Everything in the gap between those two lists was
unchecked**, and that is what this covers: two hundred connections in a row,
forty requests down one, a six-kilobyte body, the guard, the volume, gzip
decompressed by a library this project did not write, and every status carrying
its phrase on the wire rather than in a table.

Its first run found two statuses that were wrong in the same way — a form field
past its bound answered **431**, which names headers, and an upload on a
diskless boot answered **500**, which says the server broke. VF-036.

This used to be three hand-typed `gcc` commands, which is the practice
**VF-012** is about: two of them said `-std=c11`, which is not what this
project builds with, and following them left two suites unbuildable.

| suite | checks | what it holds |
|---|---|---|
| `server_identity` | 34 | naming a parallel, and every way of naming it wrong |
| `server_http` | 136 | one request, and every way of writing two |
| `server_http_serve` | 108 | the server over a real socket, `serve.c` unmodified |
| `server_http_files` | 39 | serving a file, and every way of serving the wrong one |
| `server_http_stream` | 47 | streaming, and the promise that must not be broken |
| `server_http_form` | 39 | decoding a form, and the field that has two values |
| `server_http_cache` | 46 | validators, and the two headers that read them opposite ways |
| `server_service` | 37 | services, their states, and the restart that has to stop |
| `server_http_range` | 45 | asking for part of a file, and the ways that hands over the wrong part |
| `server_http_escape` | 21 | escaping text for HTML, and the characters people forget |
| `server_http_json` | 28 | escaping text for JSON, and the byte that ends a string early |
| `server_http_multipart` | 71 | reading a multipart body, and the two bytes that ruin a file |
| `server_http_concurrent` | 12 | two clients, one of them stuck |
| `server_auth` | 44 | a guard, and every way of getting past one that is not the token |
| `server_dns` | 72 | asking for an address, and the answers that must not be believed |
| `server_ntp` | 41 | asking the time, and the replies that must not set a clock |
| `server_log` | 24 | a ring of recent entries, and the count that stops it lying |
| `server_logfile` | 45 | numbering segments, and the sort order that makes a log readable |
| `server_dial` | 38 | three answers, and the two ways of confusing them |
| `server_config` | 172 | reading a configuration, and the files that must not be believed |
| `server_chunked` | 116 | reading a chunked body, and every way of framing one twice |
| `server_jsonread` | 128 | reading JSON, and every document that means two things |
| `server_accept` | 93 | choosing what to send, and the headers that are read wrong |
| `server_deflate` | 81 | compressing, and decompressing it with something else |

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
| **0.38.0** | **A diagnosis this seat had been repeating for three versions was wrong, and the control was why.** KF-257 said `connect` never reports a completed handshake. The network session read it the other way: on this kernel frames leave the card's receive ring only when somebody calls `netdev_service`, which `socket_recvfrom` and `socket_accept` do and `socket_connect_progress` does not — so a program polling `connect` and doing nothing else never causes the reply to be looked at. They stated it as a prediction with a way to be wrong, which is why it got machine time rather than agreement. **The first run appeared to refute it**, and reporting that would have been the expensive mistake: both loops were dialling this machine's own address, which cannot come back through QEMU's user networking whatever the stack does. Every attempt this seat had ever made at a connection that *should* succeed had dialled it. A working connection and a broken poll produced identical output, so three versions of measurement could not tell them apart — **a control that cannot succeed is not a control, it is a second copy of the failure.** The A/B that settles it uses a closed port, certain to answer RST: polled with a read, **refused in 0 ms**; polled without one, **timed out at 3000 ms**; a real listener, **ready in 1 ms**. Swapped the order and ran it again, identical. Fixed in `dial.c` with one read per poll that reads nothing and is there for its side effect. **An outbound TCP connection works, and the reverse proxy is off the blocked list.** Also: `scripts/check-board.py`, a third reader for `docs/SERVER.md` and `docs/WEB.md`, which found the reverse-proxy row claiming *unblocked* while `KERNEL-WANTS.md` called it open — two of this project's own documents disagreeing about whether a day of work was possible, with nothing able to notice. VF-045. |
| **0.37.0** | **The gap 0.29.0 named about itself is closed: files are compressed.** That entry said plainly what it did not do — *what is not compressed: files* — because compression lived in `send_response`, which builds a whole body in one call, and files go through the sink a block at a time so that a file can be larger than this program's memory. What that needed was not a loop around the old function: the **window** has to survive between calls or no match spans a block, the **bit writer** has to, or every eight kilobytes costs a new block header, and the **checksum** is computed over everything. One deflate block now spans the whole body. It also cannot look ahead — a match near the end of what has arrived might continue into bytes nobody has handed over — so a write may legitimately produce **nothing at all**, and that turns out to matter twice. **A comment claimed a measurement nobody had taken:** sliding the window cleared the hash chains, said to cost *within a per cent*, and the first probe that compared both compressors on the same files said **three to four points** on everything large enough to slide. Fixed by walking the kept history back through the same `z_insert` everything else uses — one insert per kept byte, once per 16 KiB — and the gap closed to zero or one. Verified three ways: the suite's own inflater written from RFC 1951, Python's `zlib` which checks a CRC it never saw written, and a booted machine serving `/console.css` at **687 bytes → 421**, de-chunked and compared to the plain fetch byte for byte. **Never on a 206**, because `Content-Range` counts the resource's bytes and a compressed range names nothing the client asked for. **And four mutants, two of which survived.** One survived because the belt-and-braces guard caught it, which meant the guard it was aimed at was untested. The other survived because a zero-length chunk *is* the chunked terminator and nothing in the suite ever made the compressor return zero — `/text` hands over two thousand bytes in one call. `/drip` writes sixty-four at a time, which is the shape a real handler has, and kills it. The mutant survived because the test was wrong, not because the guard was unnecessary. VF-044. |
| **0.36.0** | **The gap VF-028 wrote down in 0.24.0 is closed, by two versions that were not about it.** That entry admitted plainly what its own verification did not cover: *a second named site* — the machine had only ever run one site with no name, and the chain was proved on a host socket. Nothing was built for this. A configuration file arrived in 0.25.0 and a way to write one over the network in 0.33.0, and between them the site is now **configured rather than compiled**: told to the machine by HTTP, effective after a restart. Measured on the second boot — `shop.example` gets the volume's own file at 564 bytes, every other name and the machine's own address get the console at 2588. Same path, same machine, two documents, told apart by what they serve rather than by a header. `/System/Uploads` would have been the tidier root to demonstrate with and is deliberately not used: nothing serves that directory, and a test pointing a site at it would have quietly removed the property it exists to keep, and passed. **And the other half:** `config.c` cannot check that a root exists — it is pure, and the volume is not its business — so a configuration that parses can name a directory that is not there and answer 404 to everything with nothing saying why. The machine cannot refuse it (the directory may be populated later) so it **says** it: a console line at boot and `index_readable` per site in `GET /api/config`. VF-043. |
| **0.35.0** | **The server hung up on one client in four hundred, and the line that did it carried a comment explaining why it was right.** VF-041 left this open: a connection made, a request sent, nothing back, and the guest's own counter saying it had answered. The capture settles it without inference — the server's FIN acknowledges the SYN and **not** the fifty-four bytes that had already arrived, so it closed without ever reading the request. The cause is `read_more` being told *not in progress* when nothing has been read yet, on the reasoning that a zero from `recv` then means the client is finished. True on a host. **On the target a zero means nothing buffered** — which is VF-013, this project's own finding, in the same file three hundred lines above — and `serve.c` is compiled unchanged for both. Fixed by deleting the distinction rather than inverting it: one path, and a deadline decides. Measured 1 empty in 400 before, **0 in 800 after**. Then the check written for it caught a bug in it: the idle clock was restarted on every pass, harmless while the connection was closed on the spot and **unreachable** the moment a deadline depended on it — a silent connection would have held one of four slots for ever. And then one number turned out to be three questions: 15 s for bytes already committed, **2 s** for the gap between accept and a first packet, **10 s** for a client deciding what to ask next. All three measured on the machine. One of those measurements was wrong twice first, and it was the client both times — a scratch script reading one `recv` per request took the body of the first answer for the head of the second, which is the fault 0.30.0 already fixed once in the suite. VF-042. |
| **0.34.0** | **Merged kernel 0.5.0, and the clock this project said could not be measured now can be.** Two of the three socket fixes this branch was carrying are theirs: KF-254 and KF-255, found here and numbered there. The third — `tcp_tick` in `socket_accept`, the line between a server that answers twelve requests and one that keeps going — is still not in their tree. **And KF-251 made one of this project's own statements false.** *+/- a second, this machine's clock counts whole ones* was exactly right when written and is exactly what this register exists to catch; the same five-read probe that produced VF-021 now shows the wall clock moving in tens of milliseconds, so the console says **behind by about 950 ms (+/- 7, half a round trip of 14)** and `/api/status` reports the delay beside the uncertainty it came from. The 0.20.0 comment refusing to print the round trip ended *a finer wall clock makes this useful; nothing else has to change* — and nothing else did. Then the machine suite went intermittent on *two hundred connections*, which measured properly is **twelve, every time, recovering in half a second**: the table holds sixteen, a closed connection sits in TIME_WAIT for two seconds, and a SYN arriving when it is full is dropped rather than refused, so the sustained rate is about **two connections a second**. VF-034's *200 of 200* was true and hid that. The check is forty now and **prints the rate rather than asserting one**, because the table size is the kernel's decision. The suite also gained the two checks it should always have had: the resolver and the clock — and the resolver is what the *previous* kernel merge broke. VF-040, VF-041. |
| **0.33.0** | **The machine can be configured over the network, on a kernel that cannot replace a file.** That was the sharpest entry in `docs/KERNEL-WANTS.md` since 0.25.0: `SYS_CREATE` writes a file whole and refuses a name that exists, there is no unlink and no rename, so a configuration could be **written once and never corrected** — which is not a configuration. It is the same wall `logfile.h` hit, and it has the same answer: *a log that appends is impossible here; a log that rotates is natural.* A configuration is not edited, it is **superseded**. `POST /api/config` parses a candidate, refuses it with the line and the reason if it will not read, and otherwise writes it as the next numbered generation in `/System/Config`; the machine reads the highest it finds at boot. Three things fall out of that and only one was designed: every configuration this machine has ever run is still on the volume, a generation is whole or absent because ReconFS writes in one transaction, and getting it wrong is recoverable because writing another is the same operation. **It takes effect at the next boot, deliberately** — applying it live would change the listening port underneath the connection asking for the change, and a mistake would take the console with it before anybody could write the correction. `scripts/config-round-trip.sh` makes its own volume, boots, refuses a bad candidate, writes a good one, reboots, and finds the machine running it: 13 checks across two boots. The numbering comes from the directory, not from memory, which is the mistake `logfile.h` records — a machine starting again at one does not clobber anything, it fails every write from its second boot onwards and says nothing. VF-039. |
| **0.32.0** | **The console shows what the machine is configured as, and what it has recently been asked for.** Both were facts with no reader: the configuration was applied at boot and then let go, so a person asking *is my file in force?* had to read the serial console from boot time or guess; and the access log had an API and no place on the page. `GET /api/config` reports what is running — **guarded**, because a site's document root is a path somebody chose and a reader who can list them has the shape of the filesystem for free — and says **where it came from**, so a machine running its defaults because a file would not parse is not the same answer as one running them because there is no file. The page and the endpoint are built from one structure, because two readers of one fact eventually become two facts. **The log on the page is the first place on this machine where text a stranger sent reaches a document somebody else's browser parses.** `request.c` admits every printable byte in a target, including `<`, so every line goes through `escape.c` — and the machine suite now asks for `/%3Cscript%3E...` and reads the page back to prove it. Watched failing: with the escaping removed, `<script>alert(1)` arrives as markup. The same suite reads the console's own form rather than only the API it posts to, which is what VF-022 was about. 74 machine checks. VF-038. |
| **0.31.0** | **A program can now read the machine's name, cache it, and change it only if it is still what it read.** `POST /api/name` has been an unconditional write since 0.3.0: two clients that both read and both write leave whichever arrived second in charge, and the first is never told its change was lost. A shrug with one administrator; the oldest fault in shared state once 0.27.0 made the API drivable by a program. So the name is a resource with a validator — `GET /api/name` answers it with an `ETag`, and `POST` honours `If-Match` and answers **412** when the client's view is stale. **The server does the conditional read, not the handler**: a handler sets `etag` and knows nothing else, and `serve.c` turns a matching `If-None-Match` into a 304 with no body — the same argument as the security headers, `Vary` and the access log. `cache.h` had already written down the rule this needed: `If-None-Match` compares **weakly** and `If-Match` **strongly**, because a weak tag says two bodies are equivalent, not that they are the same bytes somebody may overwrite. One walker with a flag rather than two, and fifteen checks that every malformed header refuses the write rather than allowing it. Without an `If-Match` the write is unconditional exactly as before, because the console's own form sends none and VF-022 is this project's entry about an API change that quietly broke it. VF-037. |
| **0.30.0** | **A suite that runs on the machine, because every other one runs on a host.** VF-034 was found by accident — 1423 checks green on a server that answered twelve requests and then went silent — and the answer to a fault found by accident is not to be more careful. `scripts/machine-tests.sh` boots the server role, waits for the line that says it is listening, reads the boot token off the console, and asks forty-four questions over a real socket: two hundred connections in a row, forty down one, a six-kilobyte body, the guard, the volume, gzip decompressed by a library this project did not write, and every status carrying its phrase **on the wire** rather than in a table. A minute, and a separate command from the suites for that reason. **Its first run failed seven checks, and two of them were faults in the checks rather than in the server** — a client that cannot reassemble a response across two reads reported two of forty against a server answering all forty, and a check that posted six kilobytes into a 512-byte form field measured the bound rather than the property. The two real ones are the same fault twice: **a status that names the wrong end of the request.** A form field past its bound answered `431 Request Header Fields Too Large`, sending a client to look at the one part of its request that was fine; an upload on a diskless boot answered `500`, which claims the server broke when the truth is that there is nowhere to put the file. 413 and 503 now. VF-036. |
| **0.29.0** | **Responses are compressed — and the machine turned out to answer twelve requests and then stop for ever.** The compression is arithmetic against a measured constraint rather than a feature: bytes on this wire cost about a millisecond each (VF-013), and everything this server builds in memory is text. `server/http/deflate.c` — gzip, with fixed Huffman codes and a stored-block fallback whenever compressing would grow the body. On the machine: the console page 1547 → 980 bytes, the JSON log **3818 → 815**. A compressor cannot be checked by reading it, so it is checked three ways: the suite carries **its own inflater, written from RFC 1951 in the opposite shape** — reading the fixed tables by their bit patterns where the encoder writes them by their ranges — `scripts/gzip-probe.py` puts the same bytes through Python's `zlib`, and the CRC is checked against the published value for `123456789`. Five compressor faults were put back on purpose; four failed the suite loudly and **the fifth only cost bytes**, which is the kind nothing reports, so a corpus shaped like prose was added with a bound between the two numbers it produces. Then the real finding: measuring bytes out needed more than a dozen requests, and **the server answered twelve and went silent for the rest of the boot** — in 0.28.0 too, and for who knows how long, because no probe had ever opened a thirteenth connection. A packet capture showed every one of those twelve closing cleanly, four-way; the kernel's own allocator said `tcp: no free connection`. `tcp_tick` is the only thing that frees a connection out of `TIME_WAIT` and it was called from `socket_recvfrom` alone — so a server with no live connection, sitting in `accept`, never expired anything, and **the only thing that could free a slot was a connection the table was too full to accept.** One line in `socket_accept`, and the same machine now answers 200 of 200. VF-034, VF-035. |
| **0.28.0** | **One endpoint, two representations — and two statuses that would have gone out as `Unknown`.** `docs/WEB.md` has said since 0.12.0 that the access log as JSON was *unblocked and not built* because it needed a decision about content negotiation first: two renderings of one thing drift exactly like two lists do. The decision is `server/http/accept.c` — 74 checks — and the answer is one handler, one walk over the entries, and a branch on which bytes to emit. `Accept` is the header most often handled by looking for a substring, which fails in **both** directions: `application/json` is a substring of `application/jsonrequest`, and a client sending only a wildcard matches nothing. Both silently. So: `q` orders the ranges and the header is not in preference order; `q=0` means *not acceptable*, which is the only way a client can say "anything but this"; and the **most specific** matching range decides an offer, so `text/*;q=1, text/plain;q=0` excludes plain rather than preferring it. Parameters other than `q` are checked for shape and ignored, because Chrome sends `v=b3` on every request and refusing it would answer 400 to every browser. Watched failing against four plausible implementations — and one of them survived, which is how the missing case was found: every substring check used a hostile *range* and none a hostile *offer*, so `text` matched `textual/plain` with all sixty-nine checks green. **Then the machine printed `HTTP/1.1 406 Unknown`**: the X-macro that stopped `http_reason` and its suite drifting could never catch a status missing from the table entirely, because both read the same table. `scripts/check-statuses.py` reads the other direction and found a second one the same minute — a 502 the resolver has been able to send since 0.18.0. VF-033. |
| **0.27.0** | **The API can be driven by a program, not only by a person.** Everything this server accepted was what an HTML form sends; `POST /api/name` now also takes `application/json`, chosen by `Content-Type` and **never by sniffing the body** — a body valid in both shapes would otherwise mean whichever reader was tried first. A media type this cannot read is **415**, not 400: the request is well formed and this cannot read it, and those are different facts. `server/http/jsonread.c` is the reader, 128 checks, and most of them are documents that must be refused: a trailing comma, single quotes, an unquoted key, `NaN`, `Infinity`, `01`, `+1`, `.5`, `1e`, a lone surrogate, a raw control byte, a second document after the first. **And the same key twice**, which RFC 8259 leaves undefined and real parsers split on — so `{"role":"reader","role":"admin"}` is two documents depending on who reads it, and the reader that checked a permission may not be the one that acted. Two narrowings come from the machine rather than from taste: **no floating point**, because the init program is built with `-mno-80387 -mno-sse`, so a number is kept as text and `json_int` refuses anything that is not an exact integer rather than rounding; and **ASCII only**, to stay symmetric with the writer, which cannot emit a byte above 0x7F. Watched failing against four lenient parsers (5, 4, 2 and 2 of 128). On the machine: a document, a document with a charset parameter, a form and no content type at all all set the name; five malformed documents and one unreadable media type were refused, each naming the byte. VF-032. |
| **0.26.0** | **A chunked request body is read instead of refused — and every way of framing one twice is still refused.** `docs/WEB.md` has carried the condition since 0.0.2: *when chunked is implemented it must be implemented fully, including trailers and the terminator, and the dual-framing rejection stays.* It is the last framing a client can use that this server had no answer to, and it is the only one available when the sender does not know the length in advance, which is every upload produced as it is sent. 116 checks, and **each one runs twice — whole, and a byte at a time**, because a decoder can be correct on whole inputs and wrong on every real connection. Refused: a bare LF where CRLF belongs, a chunk extension, a size with `+`, `-` or `0x` in front of it, a size padded past the digit bound, a chunk shorter or longer than it said, a trailer section past its bounds. **Trailers are read, checked and discarded**: a trailer arrives after every decision this server has already made about the request, so one that became a header would be a credential presented after it was accepted. Watched failing against three decoders somebody would plausibly write — one that stops at `0\r\n` (18 of 116), one that takes a bare LF (4), one that does not insist on the CRLF after the data (4). On the machine, with the boot token: a form split mid-value across two chunks arrived joined, a trailer was consumed and did not become a header, and all four malformed framings answered 400. VF-031. |
| **0.25.0** | **The server reads a configuration file, so a virtual host is something you can have rather than something the source can.** `server/config.c` -- 159 checks, and most of them are files that must be **refused**: an unknown key, a key twice, a key in the wrong section, two sites with one name, a site with no root, a port of 0 or 65536, a resolver given as a name, a root that climbs out of itself. **A configuration parser that ignores a line it does not understand produces a machine running something nobody wrote**, and the person who wrote the file has no way to find out. So the whole file is taken or none of it is, the console names the line and the word, and the built-in console keeps serving -- the one place here that does not refuse outright, because the alternative is a machine whose only repair route is the web console it has stopped serving. Watched failing against three lenient parsers: one that skips unknown keys (7 of 155), one that does not check roots (10), one that allows two sites with one name (1). Measured on the machine across two boots and then with two names: `shop.example` gets the volume's `index.html` at 564 bytes, every other name gets the console. **And the machine cannot edit the file it just wrote** -- nothing in user mode can replace or remove a file, which is now the sharpest entry in `docs/KERNEL-WANTS.md` rather than a footnote. VF-030. |
| **0.24.1** | **The virtual hosts shipped a version earlier dispatched on a name out of uninitialised memory.** `serve.c` was right and the init program was not: `struct http_site site;` on the stack, ten fields assigned by hand, and the two fields added that same version -- `host` and `next`, the ones the dispatch reads -- among the ones nobody assigned. Instrumented and booted, the previous shape printed `host=0 next=0`, which is why every check in 0.24.0 passed. **It behaved correctly for a reason nobody chose**, and the same shape on a host, in a frame other work has used, gives `host` pointing at a string the previous call left behind -- a server comparing `Host:` against its own leftovers and answering 421 to everything. The site is a file-scope designated initializer now, so every field added after this line is zero by the language rather than by anybody remembering. `scripts/check-site-init.py` runs with the suites and refuses the old shape. VF-029. |
| **0.24.0** | **Name-based virtual hosts -- and the header they dispatch on had never been enforced.** A site now carries a name and a `next`; the first whose name matches answers, one with no name claims everything, and a name nobody claims gets **421** rather than 404, because the resource may well exist and this is simply not the machine that has it. Picked **per request**, not per connection: a keep-alive client may ask two sites down one socket, and the pipelined pair proving it is the check that fails against every shape that decides once. Watched failing at 9 of 59 against a dispatch that always answers with the head of the chain -- and the 200s still passed, which is why the body is the site's own context rather than the route's. Building it found the older fault: `request.c` accepted **HTTP/1.1 with no `Host` at all**, and accepted **two `Host` headers**, in the same file that has refused two `Content-Length` headers since 0.0.2 on the stated grounds that *agreement is not the property that makes a message safe, being unambiguous is.* The one header that says which machine is being addressed was the one header that rule had never been applied to. Both are 400 now, HTTP/1.0 untouched, measured on the machine before and after. VF-028. |
| **0.23.0** | **The log can be read back, including from boots that have already ended.** Writing segments without a way to read them is half a feature: `GET /api/log/segments` lists what the volume holds and `GET /api/log/segment?n=` returns one as text. **The caller gives a number and never a name** — the path is built by the same function that wrote the file, so traversal is not refused, it is unreachable. Confirmed against `../../etc/passwd`, `%2e%2e%2f`, a bare filename, a negative, scientific notation and a value given twice: every one 400 or 404, none of them by a path check. These two are **guarded** while `GET /api/log` stays open, which is the first place on this server where the line falls between *current state* and *accumulated record* rather than between reading and writing — a snapshot says what is happening, an archive says what the people who use this machine do. |
| **0.22.0** | **The access log survives a reboot, and the reason it could not was wrong.** It had been recorded since 0.12.0 that appending needed `O_APPEND`, which the C library drops — but appending needs the *ability* to append, and `SYS_SEEK` exists. The real blocker, measured: plain `OPEN_WRITE` is refused, `OPEN_CREATE` (*must not already exist*) **succeeds** on a file that does, and `OPEN_REPLACE` (*must exist*) is **refused** on one. The only door that opens contradicts its own documentation. So the shape changed instead: `SYS_CREATE` writes a file whole and refuses to overwrite, because ReconFS writes whole files — a log that appends is impossible here and a log that **rotates** is natural. Names are zero-padded so a lexical sort is a chronological one, and the next number comes from the directory, because a server starting again at zero would fail every write from its second boot onwards **silently**. Watched failing at 10 of 34 against exactly that version. And the number that exposed a second fault: 211 requests against two segments when six were due — `requests_served` had been counting connection-pool *steps* since 0.16.0, reading three times the truth on the dashboard for five versions. Eleven requests moved it by thirty-three. VF-026, VF-027. |
| **0.21.2** | **The upload endpoint had never been shown to keep anything.** It has existed since 0.15.0 and was verified every way except the one that matters: no test had ever checked that a file was still there after a reboot — and inside one boot, a write that reached the volume and one that did not look identical. The cause was not difficulty, it was a missing disk: the 16 GB image lived in `/tmp`, which is shared with the other sessions here and cleaned without warning, and it vanished twice in one day. `scripts/server-disk.sh` now builds it under `kernel/build/` where nothing else reaches. Measured across two boots on one volume: 201, then 409 for the same name, then a **restart with a new token**, the web root reporting *already there* rather than rewritten, the same name still 409, and an unused name 201. Accepted and kept are different claims. VF-025. |
| **0.21.1** | **The README claimed 736 checks and its own table summed to 735.** The userland session sent word, through Joshua, about blunt search-and-replace edits — and named the gap as doing the careful thing for code and not for `docs/`. Adding the table up took a minute: one row said 93 where the suite had grown to 94, a number quoted in the README, the board and three commit messages. My code edits all asserted their match count; the documentation edits beside them did not, **so the discipline was applied where a compiler would have caught the mistake anyway and dropped where nothing would.** `scripts/server-tests.sh` now checks the README against the run it just did — every row, the total, the suite count — and that uncovered a latent fault of its own: the runner had been parsing CRLF target names out of `CMakeLists.txt` since the day it was written, carrying a trailing carriage return that only mattered once a second reader compared it against text. Also: `/api/status` now reports the clock, which the dashboard had been showing alone for a version — in a structure whose stated purpose is that the two cannot disagree. VF-024. |
| **0.21.0** | **The console works in a browser again, and had not since 0.17.0.** The dashboard has offered a rename form since long before there was a guard; the moment `POST /api/name` became guarded, that form answered 401 to every submission — on the one page a person actually looks at. Three versions of testing missed it because every check was `curl -H`, which is the client that *can* send a header. **Testing the API is not testing the console.** Fixed by accepting the token from a form field as well: the policy hook now gets the request body, which it needed and did not have. A token in a **query string** is still refused — that is logged, sent in `Referer` and kept in history, while a POST body is none of those. The dashboard also shows what the machine has learned to do: services running, the clock offset with its real uncertainty, and whether writes are guarded. VF-022, VF-023. |
| **0.20.0** | **This machine knows how wrong its clock is — and found out its clock cannot say.** An NTP client, the same connected-datagram shape as the resolver, and the supervisor's long-awaited **second service**: adding it changed nothing in the main loop, which is what that shape was built for. It resolves `time.cloudflare.com` with this role's own resolver, which is the first thing here to use one built capability to reach another. **The danger is in believing the reply** — a clock is what every expiry is read against, so the server must echo the exact 64-bit timestamp sent, the same shape as the DNS identifier. Watched failing at 9 of 41 against a parser that takes the server's word, and every one of the nine returned `NTP_OK` where a refusal belonged. Then the line it printed said **round trip 0 ms** to a server thousands of kilometres away, which cannot be true: `SYS_WALLTIME` is declared in nanoseconds and counts whole seconds. The broken number was the one nobody was looking at; the number under test was the one hiding it. VF-021. |
| **0.19.1** | **Outbound TCP had never worked, and it was two bytes of arithmetic.** Every SYN this machine ever sent carried a wrong TCP checksum, so every correct peer discarded it in silence — no SYN+ACK, not even a RST from a closed port. Found by capturing packets on the virtual NIC instead of reasoning about the code: the IP checksum was right, the TCP one wrong, and **wrong by the same 0x0C0F on every packet**, which is `0x0A00 + 0x020F` — the two halves of this machine's own address. `socket_connect` binds to `IPV4_ANY`, so `tcp_open` summed the pseudo-header over a source of zero while the IP layer wrote the real address into the header on the way out. An accepted connection never had this, which is why inbound always worked and nobody had looked. Fixed and proved on the wire: a closed port now refuses, an open one completes the full handshake, and the host listener logs `ACCEPTED`. A second fault underneath is the kernel session's, reported with its timings — the handshake completes and `connect` still never says so. VF-020. |
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
