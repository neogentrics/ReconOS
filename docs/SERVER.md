# The server role

What a ReconOS machine does when the role chosen at first boot is **server**.

`docs/ROLES.md` is the plan all five roles share and this file does not repeat
it. The one sentence worth carrying over, because everything below leans on
it: **the installer installs the whole operating system every time, and the
role is chosen on the first boot afterwards.** A server is configuration, not
a build. Nothing here may be conditionally compiled.

Opened 15 September 2026, on branch `server`.

---

## What this role owes the network

Joshua's framing: *it is supposed to have all the features a regular server
would have — think Windows Server, think a Linux server.* So the target is not
a demonstration that a socket works. It is the set of services a machine on a
network is expected to answer for, and the list below is the beginning of that
set rather than the whole of it.

The order is by what other things need, not by what is easy. DHCP and DNS come
first because every other machine on the wire needs an address and a name
before it needs anything else this box can offer.

---

## The audit

Every subsystem `GUI_Server_Operating_System_Architecture.docx` asks for, plus
what this role has found it needs. The document is the base list and not the
whole of it; rows get added as work finds them.

**Status means what it says.** `built` is running and tested. `partial` runs
and does less than its name claims. `blocked` cannot be started at all and
names its blocker. `spec` means the design is written down and no code exists.
Nothing is marked built on the strength of having been written.

**Owner** says which branch does the work. A row owned by `kernel` is not this
role's to build and is listed because this role is what will be waiting on it.

### Network and infrastructure

| subsystem | owner | status | note |
|---|---|---|---|
| Web / API server | server | **built** | `server/http/` — 736 checks across eighteen suites, several connections at once, writes guarded, running on the machine |
| Static file serving | server | **built** | `server/http/files.c` — read off ReconFS on the machine, and **uploads now proved to survive a reboot** (VF-025). `scripts/server-disk.sh` makes the volume |
| DNS **resolver** (client) | server | **built** | `server/dns.c` — 72 checks, **resolving real names on the machine**. A resolver is a *connected* datagram, which this kernel has had all along |
| DNS (authoritative, recursive, split-horizon) | server | **blocked** | a *server* must reply to whoever asked, which needs `recvfrom`. The blocked half; see VF-017 for how the entry came to cover both |
| DHCP (leases, reservations, PXE staging) | server | **blocked** | same |
| DDNS | server | **blocked** | follows DNS |
| NTP **client** (measuring) | server | **built** | `server/ntp.c` — 41 checks, measuring against `time.cloudflare.com` on the machine, which it resolves itself. Reports the offset; **cannot correct it** |
| NTP time *setting* | server | **blocked** | nothing can set this clock: `SYS_TIME` and `SYS_WALLTIME` both read and nothing writes. And `SYS_WALLTIME` counts whole seconds — VF-021 |
| PTP | server | not started | needs hardware timestamping |
| Reverse proxy | server | **unblocked, not built** | `connect` is fixed (KF-244); `server/dial.c` is the client half |
| Multi-queue NIC drivers | kernel | **blocked** | virtio-net only |
| LACP bonding, VLAN, bridging | kernel | not started | firewall role needs it first |
| Stateful firewall / NAT | kernel | not started | firewall role owns it |

### Identity and security

| subsystem | owner | status | note |
|---|---|---|---|
| LDAP directory service | server | **blocked** | no LDAP client or server |
| Kerberos KDC | server | **blocked** | no GSSAPI, no crypto |
| TLS termination and certificates | server | **blocked** | no TLS, no certificate store |
| HTTP auth (Basic, session, bearer) | server | **partial** | Bearer is **built** for the two writing endpoints — `server/auth.c`, a boot token on the console, 33 checks. Basic and sessions still wait on TLS; the rule in `docs/WEB.md` §5 is amended there rather than bent |
| Audit log daemon | server | **partial** | `server/log.c` — a ring of recent requests at `GET /api/log`. **In memory only**: appending to a file needs `O_APPEND`, which the C library drops |
| POSIX ACLs | kernel | partial | uid/gid and caps exist |

### Storage

| subsystem | owner | status | note |
|---|---|---|---|
| SMB / CIFS server | NAS | not started | NAS role owns it |
| NFS server | NAS | not started | same |
| iSCSI / NVMe-oF target | NAS | not started | same |
| CoW filesystem, checksumming | kernel | partial | ReconFS exists |
| Software RAID | kernel | not started | |
| Snapshots and clones | kernel | not started | |

### Lifecycle and management

| subsystem | owner | status | note |
|---|---|---|---|
| Parallel naming (`M16` → `M17`) | server | **built** | `server/identity.c` — 34 checks |
| Peer discovery on first boot | server | **blocked on one thing now** | `connect` is fixed and `dial.c` is ready. What remains: **a program cannot learn its own address**, so nothing knows what range to sweep |
| Configuration clone onto unlike hardware | server | spec | discovery first |
| Service supervisor | server | **partial** | `server/service.c` — 37 checks, and **two services** now: the web server and the clock. Adding the second changed nothing in the loop, which is what the shape was for. In-process only: **nothing can start a program**, so this is not process supervision and does not pretend to be |
| Cron / job scheduler | server | **blocked** | no user-mode timer |
| Structured REST / RPC management API | server | **partial** | reads and one write: `POST /api/name` renames the machine |
| Hypervisor daemon | server | not started | needs VT-x from the kernel |
| Container runtime | server | **blocked** | no namespaces, no cgroups |
| Out-of-band IPMI / Redfish | server | not started | |
| Serial console redirection | kernel | partial | kernel has a serial path |
| Watchdog, kdump, cgroups | kernel | not started | |

### Administrative consoles

Each is a web application served by the role above, so every row here waits on
the same two things: a static file handler and a JSON API.

| console | status | note |
|---|---|---|
| Server Manager dashboard | **partial** | one page, real numbers, its own stylesheet on the volume, and a form that changes something **from a browser** — which it could not do between 0.17.0 and 0.21.0, see VF-022. Now shows services, clock offset and guard posture |
| Storage / RAID manager | spec | waits on the kernel's RAID |
| Network and firewall centre | spec | |
| Services and daemon inspector | **partial** | `GET /api/services` — state, polls, faults, restarts |
| Directory and user manager | spec | waits on LDAP |
| Performance monitor | spec | `SYS_MACHINE` gives some of it today |
| Event viewer and log explorer | **partial** | `GET /api/log` — every request answered, with the count of any dropped |
| Task and job scheduler | spec | waits on a timer |

### Graphical desktop and remote access

Owned by the desktop track, listed because a *GUI* server role is what the
document specifies and this role should not duplicate the work.

| subsystem | owner | status |
|---|---|---|
| Compositor, window manager, shell | desktop | being built |
| DRM / KMS, GPU drivers | kernel | not started |
| Remote desktop daemon (RDP / VNC / SPICE) | server | not started |
| Hardware video encode for remote desktop | kernel | not started |
| Multi-session broker | server | **blocked** — nothing can start a program |

---

## What is blocking the first two services, exactly

**DHCP and DNS are both unconnected-datagram protocols, and an unconnected
datagram socket cannot be opened from user mode today.**

This is not a gap anyone missed. It is written down where a caller will find
it, in `userland/include/sys/socket.h`:

> **Declared and not defined.** A caller fails to link, naming the symbol.
> `sendto` and `recvfrom` need an unconnected datagram to carry an address per
> message, and the kernel refuses such a socket rather than half-serving it.

That is the right call and this file is not asking for it to be reversed. What
it asks for is the doorway, and the shape of the ask is the same as the one
that got sockets built in the first place: **the hard part already exists.**
`udp_bind_port` is in `kernel/core/net.c` and works. `socket_sendto` and
`socket_recvfrom` are in `kernel/core/socket.c` and work. No system call
reaches them.

Filed as its own entry in `docs/KERNEL-WANTS.md`. Until it is answered:

- Neither DHCP nor DNS can be started, at all, in any partial form. There is
  no half of either that runs over a stream.
- **Discovery cannot use broadcast**, which is how a machine would normally
  find its peers. The design below works around it rather than waiting.
- A server can still be reached, because TCP works. That is why the web server
  was built first and is running: `docs/WEB.md` is its specification and
  `server/http/` is the code. It is also what will first prove that a socket
  descriptor moves bytes on this kernel — which nothing has yet, because
  everything so far is verified against the host's sockets.

---

## Reported to the kernel session: a socket write that says it sent more than it did

**Found** 15 September 2026, the first time this role's web server ran on the
machine. **Not fixed here, and it is not this role's to fix** -- the workaround
below is in `server/http/serve.h` and should be removed when the kernel is
fixed rather than kept because it works.

**No number is claimed for it.** `docs/BUGS.md` records what happened on
6 September, when two sessions each took the next number from the copy of the
register in front of them and twelve faults were named twice. A `KF` number is
the kernel session's to assign, so this entry names none.

### What it is

`tcp_write` in `kernel/core/tcp.c:789` copies up to `TCP_BUFFER_SIZE` (4096)
bytes into the connection's send buffer, transmits only the first
`chunk[512]` of them, and then **returns `n` -- the number it buffered, not the
number it sent.**

So `send` reports complete success for a 1194-byte response of which 512 bytes
were put on the wire. The rest sits in `tx_buf` with nothing to promptly drain
it: the acknowledgement path at `tcp.c:519` advances `tx_head` and shrinks
`tx_len` without transmitting what follows, and the only other sender is the
retransmission timer. A server that closes the connection when its write
returns success -- which is what a correct server does -- closes before that
timer fires, and the tail is never sent.

**This is not a caller that mishandled a short write.** `send_all` in
`serve.c` loops on the returned count and always did. The count was wrong.

### Measured, not deduced

Three responses on one boot, before any change:

| request | declared | received |
|---|---|---|
| `/health` | 3 | 3 |
| `/api/status` | 207 | 207 |
| `/` | 1194 | **512** |

`curl` ended the third with *transfer closed with 679 bytes remaining to read*.
512 is `sizeof(chunk)` in `tcp_write`, exactly.

### What would fix it

Either return `send_now` rather than `n`, so a caller's existing short-write
loop does the right thing -- which is the smaller change and needs nothing from
the caller -- or keep returning `n` and transmit the next segment when an
acknowledgement frees window, which is what a send buffer is normally for.

The first is correct today. The second is what a buffer that exists for
retransmission should eventually do anyway.

### What this role did meanwhile

`HTTP_SEND_CHUNK` in `server/http/serve.h`: no call to `send` is handed more
than 512 bytes, so each one is transmitted in full before the next is offered.
After it, on the same machine, declared and received match on every response,
including five consecutive 1194-byte pages.

**A second fault this uncovered, in this role's own test.** The suite's child
process outlived a crashed parent and sat holding the port. The next thing to
ask that port for a page was answered -- by the wrong process, looking exactly
like a pass. It is now bounded by `alarm(20)`: an orphan that answers is worse
than one that hangs, because it cannot be told apart from success.

---

## Reported to the desktop session: `open(O_CREAT)` never creates

**Found** 15 September 2026, building the static file handler. **Not fixed
here** -- `userland/libc/posix.c` belongs to the desktop track. **No number is
claimed**; a `BG` is theirs to assign, for the reason `docs/BUGS.md` records
about 6 September.

### What it is

`recon_flags_from_posix` translates the access mode and nothing else:

```c
switch (flags & O_ACCMODE) {
case O_WRONLY: out = RECON_O_WRITE;               break;
case O_RDWR:   out = RECON_O_READ | RECON_O_WRITE; break;
default:       out = RECON_O_READ;                 break;
}
return out;
```

`O_CREAT`, `O_TRUNC`, `O_EXCL` and `O_APPEND` are dropped. So

    open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)

opens an existing file for writing and, for a file that is not there, answers
`ENOENT` -- **which is exactly the condition `O_CREAT` was passed to fix.** The
caller is told the file does not exist by the call it made to bring it into
existence.

`O_EXCL` is the sharper one. A caller using `O_CREAT | O_EXCL` is asking for
"create this, and fail if somebody else got there first" -- the standard way to
take a lock or claim a name without a race. Here both flags vanish, the call
opens whatever is already at that path, and the exclusion silently does not
happen. Nothing reports that the guarantee was not provided.

### Measured

On the machine, for a path whose directory had just answered `EEXIST`:

    the web root: read=-1/e2 write=-1/e2 wrote=-1

`e2` is `ENOENT`, from the call carrying `O_CREAT`.

### What would fix it

Either translate the flags, or **refuse the ones that are not implemented**.
Refusing is the smaller change and the more honest one, and it is the choice
this library already makes elsewhere: `sendto`, `recvfrom` and `shutdown` are
declared and deliberately not defined, so a caller fails to link rather than
getting a plausible wrong answer. A silently ignored flag is the case that
rule exists to prevent, and this one got past it because `open` is defined --
it is the *argument* that is unimplemented, which nothing was checking.

### What this role did meanwhile

`server_init.c` creates its default page with `SYS_CREATE` directly, which
takes the path and the contents in one call and suits a file written once at
boot better than open-then-write would anyway. The comment there says it is a
workaround and names this entry.

**A fault of this role's own, found the same way.** `lay_out_the_site` read
`mkdir` answering `-1` as "the web root could not be made" -- but `-1` with
`EEXIST` is the ordinary answer on every boot after the first. A server with a
perfectly good web root spent three boots reporting it had none and answering
404 to every file. The directories are made and their answers discarded now;
the only question that decides anything is whether the file opens, which is the
thing actually needed, asked directly.

---

## Numbers in a message, twice, and what it cost

**Nothing** -- which is the point of writing it down.

On 15 September a correction to the socket numbers moved `SYS_SOCKET` from 27 to
26. The enum says 27. Recorded as VF-001.

On 16 September the announcement of KF-244 gave `SYS_CONNECT` as 30, `SYS_EAGAIN`
as -12 and `SYS_EIO` as -4. The enum says 31, -4 and -9; -12 is `SYS_EPERM`.

Both times the *names* were right and only the numbers were wrong. Both times
the check was one command against the header. The second would have been the
expensive one: -4 is `EAGAIN`, so a caller built from that table would have read
every handshake still in flight as a refusal and abandoned connections that were
simply unfinished -- intermittent failures with nothing visibly wrong anywhere.

`server/dial.c` compares no numbers. It reads `errno` by name, which is correct
whatever the numbers are, and it is the reason a wrong table changed nothing.

This is not a complaint about either message. It is the argument for the habit:
**a number in prose is not the number in the header until somebody looks**, and
looking is cheap enough that there is no reason not to.

---

## Discovery and parallels

The goal, from `docs/ROLES.md` and from Joshua directly: a second server
brought up on a wire that already has one should offer to become its parallel
rather than a stranger. Clone the configuration, adapt it to whatever hardware
is actually present, and take the next name in the family. A box with four
disks cloning a box with three lays out what it has rather than refusing.

### Naming — built

`server/identity.c`. A name ends in a run of digits or it names no family, and
a name that names no family gets no parallel:

- `M16` on the wire gives `M17`. `M16` and `M17` both present gives `M18`.
- `srv007` gives `srv008`, not `srv8` — a parallel that drops the padding has
  renamed the family rather than joined it.
- `M99` gives `M100`. The family's width is a floor, never a ceiling.
- `m17` on the wire blocks `M17`, because a host name is case-insensitive and
  a collision does not care about spelling.
- `gateway` is **refused**. There is no next `gateway`, and inventing
  `gateway2` would be this code deciding what the operator meant.
- An over-long name, or a number past the last one, is refused rather than
  truncated or wrapped. A truncated name is a name that belongs to a different
  machine.

Counting goes *upward from the peer*, never from one. A parallel of `M16`
handed `M1` would read as the original to anyone looking at the pair.

### Finding the peer — blocked, and the way through was not one

Broadcast is the obvious mechanism and is not available: no system call opens an
unconnected datagram. That was known.

**This file previously said a TCP sweep was the way through** — connect to a
known port on each address in the local range and see who answers, "with exactly
the five calls that exist today, needs nothing from the kernel". That was
written on 15 September and is wrong. It is recorded as VF-009 in
`docs/VERIFICATION.md` rather than quietly deleted, because the way it was
arrived at is worth keeping: the five calls were verified to *exist*, carefully,
and "exists" was then read as "works as a caller would expect".

Measured on the machine on 16 September:

```
the client side: connect(closed port)=0 connect(own :80)=0 write=-1 read=0
```

**`connect` answers `SYS_OK` for a port nothing is listening on.** It calls
`tcp_open`, which starts a handshake and does not finish one, and returns. So
the reachable host and the unreachable one are indistinguishable, and the
connection that should have worked was not usable when `connect` returned — the
write straight after it failed. There is no call that waits for a handshake and
none that reports its outcome.

A sweep whose probe cannot tell a hit from a miss is not a slow way to discover
peers. It is not a way to discover peers.

**And there is a second blocker underneath it.** A program cannot learn its own
address. `struct recon_machine` carries the processor, the memory, the page size
and the architecture, and nothing about the network — while the kernel prints
`net: eth0 is 10.0.2.15, via 10.0.2.2` at boot. Even with a working `connect`,
discovery would not know what range to sweep.

Both are filed at the top of `docs/KERNEL-WANTS.md` with the measurement. The
smaller fix would answer both halves of the first one at once: **`SYS_CONNECT`
returning `SYS_EAGAIN` while the handshake is in flight**, exactly as `accept`
already does, so a caller polls with no new call and no new idea.

Until then discovery is not deferred, it is blocked, and the naming in
`server/identity.c` has nothing to discover.

### Cloning — not started

`ROLES.md` already scoped this correctly and the scoping is worth keeping:
cloning configuration onto unlike hardware is tractable and close to what
`install_plan_run` does in miniature. Two machines presenting as *one storage
unit* is distributed storage and is a different order of problem. The server
role wants the first and does not want the second; the NAS role owns it.

---

*Related: `docs/ROLES.md`, `docs/KERNEL-WANTS.md`, `docs/NAMING.md`.*
