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
| Web / API server | server | **built** | `server/http/` — see `docs/WEB.md`; 92 checks |
| DNS (authoritative, recursive, split-horizon) | server | **blocked** | unconnected datagram |
| DHCP (leases, reservations, PXE staging) | server | **blocked** | same |
| DDNS | server | **blocked** | follows DNS |
| NTP / PTP time sync | server | **blocked** | same; and no user-mode timer |
| Reverse proxy | server | spec | `connect` exists, so buildable today |
| Multi-queue NIC drivers | kernel | **blocked** | virtio-net only |
| LACP bonding, VLAN, bridging | kernel | not started | firewall role needs it first |
| Stateful firewall / NAT | kernel | not started | firewall role owns it |

### Identity and security

| subsystem | owner | status | note |
|---|---|---|---|
| LDAP directory service | server | **blocked** | no LDAP client or server |
| Kerberos KDC | server | **blocked** | no GSSAPI, no crypto |
| TLS termination and certificates | server | **blocked** | no TLS, no certificate store |
| HTTP auth (Basic, session, bearer) | server | spec | **TLS first** — see `docs/WEB.md` §5 |
| Audit log daemon | server | not started | Event Viewer reads it |
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
| Peer discovery on first boot | server | **blocked** | no broadcast; sweep is the way through |
| Configuration clone onto unlike hardware | server | spec | discovery first |
| Service supervisor | server | not started | needed before there is a second service |
| Cron / job scheduler | server | **blocked** | no user-mode timer |
| Structured REST / RPC management API | server | partial | the transport is built; no API yet |
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
| Server Manager dashboard | spec | the first page to build |
| Storage / RAID manager | spec | waits on the kernel's RAID |
| Network and firewall centre | spec | |
| Services and daemon inspector | spec | waits on the supervisor |
| Directory and user manager | spec | waits on LDAP |
| Performance monitor | spec | `SYS_MACHINE` gives some of it today |
| Event viewer and log explorer | spec | waits on the audit daemon |
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
