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

**Status means what it says.** `built` is running and tested. `partial` runs
and does less than its name claims. `blocked` cannot be started at all, and
the blocker is named. Nothing is marked built on the strength of having been
written.

| subsystem | status | note |
|---|---|---|
| Parallel naming (`M16` → `M17`) | **built** | `server/identity.c`, 34 checks |
| Peer discovery on first boot | **blocked** | needs datagram sockets, see below |
| Configuration clone from a peer | not started | design below; discovery first |
| DHCP server | **blocked** | unconnected datagram; see `KERNEL-WANTS.md` |
| DNS server | **blocked** | same |
| DDNS | **blocked** | follows DNS |
| Web server | not started | TCP only, so **not** blocked — next buildable thing |
| Service supervisor | not started | needed before more than one service exists |
| Static addressing | not started | the sidestep that makes a server usable without DHCP |
| Lease renewal (as a client) | **blocked** | kernel has no renewal; `ROLES.md` records it |
| TLS / certificates | not started | |
| Remote administration | not started | |
| Logging and audit | not started | |
| Time service | not started | |
| Directory / accounts | not started | |
| File sharing | not started | NAS role owns the storage half |

Joshua is sending a fuller list of what a server is expected to carry. This
table is the shape it lands in — a row per subsystem, a status that is
checkable, and a named blocker where there is one. It is an audit and not a
roadmap: a row may sit at `not started` for months without that being a fault,
but a row marked `built` that is not is a fault of the worst kind, because it
is the kind nobody goes looking for.

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
- A server can still be reached, because TCP works — so the web server is the
  next thing that can actually be built, and it is what will first prove that
  a socket descriptor moves bytes on this kernel. Nothing has yet.

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

### Finding the peer — blocked, with a way through

Broadcast is the obvious mechanism and is not available. Two candidates, and
the choice is deferred rather than guessed:

**A sweep over TCP.** Connect to a known port on each address in the local
range and see who answers. Works with exactly the five calls that exist today,
needs nothing from the kernel, and is how this will be built if the datagram
entry is not answered first. It is slower and noisier than broadcast, and on a
wide subnet it is slow enough to be felt on a first boot — a `/16` is 65,534
connects. Bounded, then, and the bound is a decision rather than a constant to
be picked here.

**UDP broadcast**, once the datagram call exists. Correct, immediate, and how
this would be done if it could be.

The second is better and the first is available. Since discovery has to run
*before the first screen* — `ROLES.md` is explicit about that — the sweep is
worth building rather than waiting on, and worth writing so that the transport
can be replaced without the discovery logic changing.

### Cloning — not started

`ROLES.md` already scoped this correctly and the scoping is worth keeping:
cloning configuration onto unlike hardware is tractable and close to what
`install_plan_run` does in miniature. Two machines presenting as *one storage
unit* is distributed storage and is a different order of problem. The server
role wants the first and does not want the second; the NAS role owns it.

---

*Related: `docs/ROLES.md`, `docs/KERNEL-WANTS.md`, `docs/NAMING.md`.*
