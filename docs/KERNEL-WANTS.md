# What the desktop wants from a kernel

Requirements written from use rather than from guesswork.

Every entry here is a place where ReconOS asks Linux for something and the
answer is awkward, or where it works around not being able to ask at all. They
are not feature requests. They are the shape of the seam between the desktop
and whatever is underneath it, found by hitting it.

This file is the compositor's side. `docs/KERNEL.md` is the kernel's, and the
two are written by different people at different times; where they disagree,
this one describes what the desktop actually does today and that one describes
what is being built.

Ordered by how sharply it is felt, not by how hard it would be.

---

## `accept` cannot say who connected, so nothing can be restricted or attributed

**Where:** guarding the server role's writing endpoints, 16 September 2026.

*Filed from the server role.*

### What is missing

`SYS_ACCEPT` takes a descriptor and returns a descriptor. There is no argument
for a peer address and no other call that would report one -- no
`getpeername`, no `getsockname`. The C library is explicit about it:

> Zeroed rather than filled in, because the kernel does not report who
> connected.

So `accept(fd, &addr, &len)` fills `addr` with 0.0.0.0 port 0. That is the
right thing for the library to do with nothing to report, and it means a server
cannot learn anything at all about the other end of a connection it is already
talking to.

### Three things it costs, all of them live today

**Access control by source.** The obvious guard for a management endpoint is
"only from this subnet", and it cannot be written. `POST /api/name` renames the
machine and `POST /api/upload` writes to its volume; both had no protection at
all until a boot token was added instead, and a token in clear over HTTP is a
weaker thing than an address check would have been.

**Attribution in the log.** `GET /api/log` reports every request answered,
including every refusal -- and cannot say who made any of them. An access log
that records a hundred 401s and cannot tell one client from a hundred is an
access log that cannot answer the question it exists for.

**Knowing what to sweep.** The discovery entry below needs this machine's own
address to know what range to look at. A peer address does not give that
directly, but a connection from 10.0.2.2 says a great deal about which network
this machine is on -- which is more than is known now, which is nothing.

### What would fix it

Either shape works:

- `SYS_ACCEPT` taking an optional buffer, filled with the peer's address and
  port, the way the POSIX call this library implements already expects. The
  library has the parameters already and throws them away.
- A separate call taking a connected descriptor and answering with its peer,
  and ideally a second answering its local end -- which would settle the
  discovery entry outright.

The first is less work and unblocks two of the three. The second unblocks all
three.

---

## A burst of more than about 2880 bytes stalls, and then trickles in at a kilobyte a second

**Where:** building file upload for the server role, 16 September 2026. Found
because every upload over about 4 KiB returned an empty reply; measured from
both ends before anything was concluded.

*Filed from the server role.*

### The measurement

A `recv` on a connection with bytes still outstanding answers **0 with `errno`
0** -- not an error, not a close. Instrumented in `serve.c`:

```
BODYREAD have=2880 need=5414 n=0 errno=0
```

2880 bytes had arrived, 2534 were still owed, and the connection was open. The
first fix was to ask again rather than treat the zero as end-of-stream. Asked
**200000 times**, yielding between attempts, `have` never moved off 2880:

```
GAVEUP have=2880 room=73729 n=0 errno=0 inprog=1 stalls=200000
```

Raising the bound showed the bytes do eventually arrive, and how slowly. All
three are one burst from `curl`, on an otherwise idle machine:

| body | time | effective rate |
|---|---|---|
| 3700 bytes | 5.25 s | ~700 B/s |
| 20000 bytes | 20.1 s | ~1 KB/s |
| 60000 bytes | 64.5 s | ~0.9 KB/s |

### It is not a size limit, and that is the useful half

The **same bytes, paced by the sender**, arrive at full speed. 400 bytes every
20 ms:

| body | pacing | result |
|---|---|---|
| 5000 | 400 B / 50 ms | complete, ~0.6 s |
| 5000 | 400 B / no pause | **stalls** |
| 5000 | 2000 B / 50 ms | complete |
| 20000 | 400 B / 20 ms | complete, ~1 s |

So the receive path handles 20 KB happily when it arrives in small pieces and
stalls on 3.7 KB when it arrives at once. The break is at roughly two segments'
worth -- 2880 is 2 x 1440.

### What it is not: the server taking the processor

The obvious guess is that a single-process server asking for bytes in a tight
loop starves whatever would deliver them. **Measured, and it is not that.**

`SYS_YIELD` was put in both loops -- the receive retry and the `accept` spin --
and the burst rate did not move: a 3700-byte burst took 5.14 s with the yields
against 5.25 s without. If contention were the mechanism, giving the processor
away several thousand times a second would have shown up.

Recorded because it is the first thing anyone will suspect, and ruling it out
is worth more than another table of the same numbers.

### What this looks like from here

A receive buffer of about two segments that, once full, stops accepting and
does not recover until the far end retransmits. Each stall then costs a
retransmission timeout, which is what produces the flat ~1 KB/s: not a
bandwidth figure at all, but one RTO per couple of kilobytes.

**That is a reading of the evidence, not a diagnosis** -- the kernel is not
this role's to read. What is measured is above.

### What would fix it

A receive buffer larger than a couple of segments, and -- more importantly -- a
window that reopens when the application drains it, so a sender is never made
to time out for bytes the server has already made room for.

**A blocking `recv` would be worth as much.** Today a read that has nothing
returns 0 immediately, so the only way to wait is to ask again in a loop; with
`SYS_YIELD` between attempts that is survivable, but it is a spin either way.
A `recv` that slept until data arrived or a deadline passed would remove both
this loop and the guesswork in `RECV_DEADLINE_MS`.

### What this role did meanwhile

`serve.c` treats a zero as *not yet* while a request is in progress and as
*nothing more* between requests, yields via a hook the site supplies, and
bounds the wait with a real clock rather than an attempt count -- 15 seconds,
which on these numbers is about 15 KB. A request cut off at that deadline now
gets **408** and a log entry, rather than a closed connection and silence.

None of that makes uploads fast. It makes them bounded, and it makes the
failure visible.

## ~~`connect` says yes to a closed port~~ -- fixed as KF-244, **not yet pushed**

> **Answered in kernel 0.2.48 by the kernel session, 16 September 2026.**
> `SYS_CONNECT` now returns `SYS_OK` when established, `SYS_EAGAIN` while the
> handshake is in flight, and `SYS_EIO` when the peer refused -- which is the
> shape this entry asked for, matching `accept`. It is also **idempotent**:
> calling it again reports where the first attempt got to rather than sending a
> second SYN, so a poll loop does not fill the socket table with attempts
> nobody is waiting on. That was part of the fix rather than a side effect.
>
> **It is not on `origin/kernel` yet** -- committed locally at `4e42d14`,
> awaiting a matrix run. This entry stays open until it is, because the branch
> tips at KF-242 / 0.2.46 and `socket_connect` there still sets
> `connected = true` straight after `tcp_open`.
>
> **Re-checked 16 September, later the same day, and still open.** `4e42d14`
> is reachable locally and is on the `kernel` branch; it is not on
> `origin/userland`, which is what the server role builds from. A boot of this
> branch's kernel (0.2.41) the same afternoon printed the standing measurement
> unchanged:
>
> ```
> the client side: connect(closed port)=0 connect(own :80)=0 write=-1 read=0
> ```
>
> That is the same line this entry was opened with. It is recorded again rather
> than assumed, because *the fix exists* and *the fix is in the tree I build
> from* are two different facts, and the first one is the one that is easy to
> start treating as the second.
>
> **The caller is built and tested against it already**: `server/dial.c`, 32
> checks, written against the *names* `EAGAIN` and `EISCONN` rather than any
> number. `server_init.c` carries a standing measurement that will change the
> moment the fix lands, so nobody has to remember to look.
>
> **No timeout at the syscall boundary, and none is wanted.** The kernel asked
> whether one was needed. It is not: a sweep of a subnet wants to give up on
> each address in a fraction of a second, and a proxy dialling a known peer
> wants to wait seconds. One number in the kernel would be wrong for one of
> them, and a caller that owns its deadline can have both. `dial.c` takes the
> deadline as an argument for exactly that reason.
>
> **What is still missing is the other half of this entry**, below: a program
> cannot learn its own address, so discovery does not know what to sweep.
>
> The rest of this entry is what it said before.

## `connect` says yes to a closed port, so nothing can find anything

**Where:** building peer discovery for the server role, 16 September 2026. Hit
before a line of it could be written, and **measured on the machine rather than
deduced** -- the code suggested it and a boot confirmed it.

*Filed from the server role. So is the datagram entry below it, for the same
reason `docs/ROLES.md` gives.*

### The measurement

Three calls, one boot, printed by `server_init.c`:

```
the client side: connect(closed port)=0 connect(own :80)=0 write=-1 read=0
```

- **`connect` to 10.0.2.2 port 9, where nothing is listening: `SYS_OK`.**
- `connect` to this machine's own listener, which certainly exists: `SYS_OK`.
- A `write` immediately after the second: **-1**.

So the two cases are indistinguishable, and the one that should have worked was
not usable when `connect` returned.

### What it is

`socket_connect` in `kernel/core/socket.c` calls `tcp_open` and returns as soon
as it has a connection index:

```c
s->conn = tcp_open(s->local_ip, s->local_port, addr, port);
if (s->conn < 0)
        return false;
s->connected = true;
return true;
```

`tcp_open` starts a handshake. It does not finish one. So `SYS_OK` means *a SYN
was sent*, which is not what any caller of `connect` means by it -- and there is
no call that waits for the handshake, and none that reports its outcome. A
program cannot ask whether it is connected, and `tcp_write` refuses anything not
`TCP_ESTABLISHED`, so the only signal is a write that fails for a reason that
might equally be a dead peer.

**This is the mirror of `accept`.** That one answers `EAGAIN` and says so in
`sys/socket.h`, which is honest and lets a caller poll. `connect` answers `OK`
and tells a caller nothing, which is the version that cannot be worked around.

### What it stops

- **Peer discovery**, which is the one feature `docs/ROLES.md` describes at
  length: a machine finding another ReconOS on the wire and offering to become
  its parallel. A sweep cannot tell who answered.
- **A reverse proxy**, and anything else that opens an outbound connection.
- It is the whole of the client side. Everything this role has proved so far is
  the server half.

### What would answer it

Smallest first, and the first is probably enough:

**`SYS_CONNECT` returns `SYS_EAGAIN` while the handshake is in flight**, `SYS_OK`
once established, and a refusal when the peer said no. A caller then polls
exactly as it already does for `accept`, with no new call and no new idea --
and the two halves of the socket interface start behaving the same way, which
is worth something on its own.

Failing that, anything that lets a program ask the state of a connection.

### The other half of the same problem

**A program cannot learn its own address.** `struct recon_machine` carries the
processor, the memory, the page size and the architecture, and nothing about
the network. The kernel knows -- it prints `net: eth0 is 10.0.2.15, via
10.0.2.2` at boot -- and there is no way to ask.

So even with a working `connect`, discovery would not know what to sweep. An
address and a mask on `struct recon_machine` would answer it; the structure is
already grown by appending, and `recon_machine_facts` already handles a kernel
newer than its caller.

---

## A file can be created and never changed, so nothing can be corrected in place

### What is missing

Nothing in user mode can **replace or remove a file**. `SYS_CREATE` writes one
whole and refuses a name that exists; there is no `SYS_UNLINK`, no rename, and
no truncate. The only door onto an existing file is
`OPEN_WRITE | OPEN_CREATE`, which succeeds on a file that already exists --
**the opposite of what its own documentation says**, which is why nothing here
is built on it. VF-027 has the measurement.

### What it costs, now that there is something to cost

`server/config.c` reads `/System/server.conf` at boot: the machine's name, its
port, its resolver, its clock, and its virtual hosts. It is the first piece of
this role that is configuration rather than a build, which is what
`docs/ROLES.md` says a role is meant to be.

**This machine cannot write that file a second time, and since 0.33.0 it does
not try.** A configuration is superseded rather than edited: `POST /api/config`
writes the next numbered generation into `/System/Config`, and the machine reads
the highest it finds at boot. That is the shape `logfile.c` was pushed into by
this same limitation, and it suits a configuration better than editing would
have -- every one the machine has ever run is still on the volume, and a
generation is whole or absent because ReconFS writes a file in one transaction.

**So this entry is narrowed rather than closed, and what is left is sharper.** A
generation that *parses* and is wrong -- a port nothing can reach -- takes
effect at the next boot and cannot be undone from the network. Every earlier
generation is sitting on the volume and **nothing can select one**, because
nothing here can read a boot argument and nothing can remove the newest file.
The machine is then recoverable only by somebody standing in front of it.

What this costs is no longer *configuration is impossible*. It is *a mistake is
permanent*, which is a smaller thing to ask about and a worse thing to leave
alone.

The same gap is why the access log rotates rather than appends. That one found
a good shape by being pushed into it; this one has no good shape to be pushed
into, because *the current contents of a file being wrong* is the ordinary case
for configuration.

### What would fix it

Any one of these, in the order they would be used here:

- **`SYS_UNLINK`**, and write-then-unlink-then-rename becomes available, which
  is how every system does an atomic replace. Rename would be needed too.
- **`SYS_CREATE` with a replace flag**, honestly documented. ReconFS already
  writes a whole file in one transaction, so replacing one is the same
  operation onto an existing name -- and *whole-file replacement is exactly
  what a configuration file wants*. This is the smallest of the three.
- **`OPEN_REPLACE` doing what its comment says**, with `OPEN_CREATE` likewise.
  That fixes a documented contradiction as well, but leaves a writer needing to
  truncate, which is a third thing that does not exist.

The middle one is the one this role would use tomorrow.

---

## A datagram cannot carry an address, so DNS and DHCP cannot be written

**Where:** opening the server role on branch `server`, 15 September 2026. Hit
before a line of either service could be written.

*Filed from the server role rather than the desktop. The file says it is the
compositor's side; this is the first entry that is not, and it is here because
`docs/ROLES.md` points every role at this file for exactly this purpose.*

**This is the same shape as the sockets entry at the foot of this file, and it
is deliberately not a request to reverse that entry's ruling.** Refusing an
unconnected datagram rather than half-serving it was right, and
`userland/include/sys/socket.h` says so where a caller will find it:

> **Declared and not defined.** A caller fails to link, naming the symbol.
> `sendto` and `recvfrom` need an unconnected datagram to carry an address per
> message, and the kernel refuses such a socket rather than half-serving it.

A refusal that names itself is worth more than a call that works twice out of
three times. The ask is for the doorway, not for the ruling to be softened.

### Why it stops two services rather than inconveniencing them

DHCP and DNS are both **unconnected** datagram protocols, and neither has a
half that runs over a stream.

- **DHCP cannot be connected, by definition.** A client with no address
  broadcasts to `255.255.255.255` from `0.0.0.0`, and the server answers a
  machine that does not yet have the address it is being given. There is no
  peer to `connect` to at either end. It is the one protocol on the network
  that exists precisely because nothing is addressable yet.
- **A DNS server answers hundreds of unrelated clients on one socket.** It
  must learn each query's sender from the datagram that carried it and reply
  to that sender. `connect` binds a socket to one peer; a resolver that had to
  `connect` per query would be a different protocol.

So this is not "DNS would be slower". It is that the first two services the
role exists to provide cannot be started in any partial form.

### The hard part is already built, again

Exactly as it was for sockets. In `kernel/core/`:

- `udp_bind_port` — `net.c`, works.
- `socket_sendto` and `socket_recvfrom` — `socket.c`, both present, and
  `net.c:374` already calls `socket_recvfrom` on the receive path.

The datagram machinery is written and running inside the kernel. What is
missing is the two system calls that let a program in ring 3 reach it.

### What would answer it, smallest first

Two calls, in the shape the existing five established:

```
SYS_SENDTO   (fd, buffer, length, addr, port) -> bytes sent
SYS_RECVFROM (fd, buffer, length, addr_out, port_out) -> bytes read
```

`addr` is a `u32` in host order and `port` is sixteen bits, matching `SYS_BIND`
and `SYS_CONNECT` rather than introducing a `sockaddr` at the system call
boundary — the existing calls deliberately do not take one, and one call that
disagreed would be worse than either convention applied throughout.

`addr_out` and `port_out` are written only on success. A caller that ignores
them gets `recv`'s behaviour and the kernel is not asked to guess whether it
meant to.

**What it does not need, so that it does not get built:** no `msghdr`, no
scatter-gather, no ancillary data, no `MSG_*` flags beyond zero. Every one of
those is a thing DNS and DHCP do not use, and the flags in particular are
where a stand-in becomes looser than the real call.

### What it unblocks

DHCP, DNS, DDNS, and **discovery by broadcast** — which `docs/ROLES.md`
requires to run before the first screen on a machine looking for a peer to
become a parallel of. The server role can reach a broadcast address no other
way, and is otherwise reduced to sweeping a subnet with `connect` one address
at a time.

Three of the five roles in `docs/ROLES.md` want this. The server and thin
client cannot work without it; the firewall needs it before it needs anything
else on its own list.

---

## ~~There is no way for a program to ask for memory~~ — answered, 15 September 2026

> **Answered in kernel 0.2.38 / v0.4.33.** `SYS_MAP` with an fd of -1 returns a
> demand-paged anonymous range, spelled exactly as this entry proposed, so
> nothing in `userland/` was rebuilt or relinked for it — `mem_recon.c` sent the
> call it had always sent and got an address back. An installed disk, booted by
> itself, now reports `the heap: 64 blocks and 512 KiB written, read back and
> freed; 1536 KiB from the kernel`.
>
> **The release half is still open**, and the browser-tab arithmetic below is
> why. It is kept as its own entry at the bottom of this file rather than left
> implied here.
>
> The rest of this entry is what it said before it was answered, kept because
> the measurements in it are what decided the shape of the call.

**Where:** writing `userland/libc/`, on 14 September 2026. Hit immediately and
unavoidably: `malloc` has nothing to be built on.

This is first on the list because it is the one currently stopping work rather
than the one that will stop the most. The C library is written and checked —
511,000 comparisons against the host's, and nine of the desktop's own sources
now compile with no glibc underneath them — and the next function in the file
cannot be written at all.

**What the desktop does today, measured rather than estimated:**

| | |
|---|---|
| `malloc` | 34 call sites |
| `calloc` | 80 |
| `realloc` | 4 |
| `free` | 312 |
| `strdup` | 1 |

`free` outnumbering the allocations three to one is not an error in the count:
most of what is allocated is freed on several different paths out of the same
function.

**And the sizes, which decide what shape the call has to be.** One parsed web
page:

```
  text        1,048,576     runs      800,000     blocks     112,000
  links       4,096,000     forms     135,424     fields     339,968
  options       593,920

  one page    7,125,888 bytes    largest single allocation  4,096,000
  twelve tabs    81 MiB
```

So this is **not** a call that hands out a page. The largest single request is
just under four megabytes — the link table, two thousand addresses of two
kilobytes each — and a browser window with twelve tabs open is eighty-one
megabytes live. An allocator that could only ask for one page at a time would
make four hundred calls to open one page of Wikipedia.

**What would replace it:** anonymous memory, and **a way to give it back**.

The giving back is not a refinement to add later. A tab that is closed releases
6.8 MiB, and without release a window where twelve tabs have been opened and
closed has lost eighty-one megabytes that nothing can reclaim. On a machine
with 512 MiB that is a browser that dies after sixty tabs and cannot say why.

The smallest thing that would work reuses what is already there: the kernel
reserves and demand-pages a stack for every process, so the mechanism for "a
range that exists and whose pages appear when touched" is built and tested.
`SYS_MAP` with no file — an fd of -1, or its own number — returning such a
range, and a companion that releases one, is two calls over machinery that
exists.

**What it does not need, so that it does not get built:**

- **Not `brk`.** A single growing break is the wrong shape for an allocator
  that frees in the middle, which this one will: those 312 frees are not in
  reverse order of the allocations.
- **Not protection flags yet.** Every one of the 430 sites wants readable and
  writable, and nothing in the desktop wants an executable allocation — if it
  ever did, that would be a decision to argue about rather than a flag to pass.
- **Not file-backed mapping yet.** The desktop maps exactly one thing, and it
  is `/dev/fb0`, which already works.

**Whose side:** `core/`. Address spaces, reservation and demand paging are all
there and all portable; nothing about this names a machine.

**The desktop's half is built, 14 September 2026 — this is now one call.**

`userland/libc/malloc.c` is a real allocator: boundary tags, coalescing on both
sides, free lists segregated by size, and a region handed back when nothing in
it is in use. 11,506 checks, the heap audited after every operation, run
against a source that can release and a source that cannot. The library now
answers **2,908 of the desktop's 3,113 call sites**, up from 2,478.

It takes its memory from two function pointers rather than a system call it
names, which is why it could be finished and proved before this entry was
answered.

**What one call has to do**, and there is a caller for it already in
`userland/libc/mem_recon.c`:

```c
static void *take(size_t bytes)
{
        i64 at = recon_map((int)-1, (u64)bytes);   /* fd -1: no file */

        return at < 0 ? 0 : (void *)(unsigned long)at;
}
```

`SYS_MAP` with **fd -1** answering a demand-paged anonymous range of `bytes`,
at an address the kernel chooses. Today `fd_get` finds nothing and the call is
refused with EBADF, so `malloc` answers NULL and
`recon_malloc_stats().refusals` counts it -- which is the honest state of the
machine rather than a stub pretending otherwise.

**The day that call works, this works with nothing rebuilt.** The program on
the disk already links the allocator.

**No system call number has been taken for it**, deliberately. Two sessions
build this kernel and a number claimed in advance by the half that does not own
`core/` is a number claimed twice -- which happened on 14 September and is
recorded in `docs/BUGS.md`. If `core/` would rather spell it as a call of its
own than as an fd of -1, `take` above is the one function that changes.

**The release half can wait.** `give_back` returning "there is no such call" is
a *supported* configuration, not a degraded one: the allocator keeps the region
and reuses it, and every scenario in the suite runs that way as well as the
other. What it costs is a program that cannot shrink -- see the browser tab
above -- so it is worth having, second.

---

## Nothing can ask what a file is

**Where:** `userland/libc/`, 15 September 2026, immediately after the socket
calls landed. With sockets answered this is **the largest group left** in the C
library, and it is the one standing between the desktop and its own filesystem
layer: eight of the eleven symbols are called from `src/recon_fs.c`.

**What the desktop does today, measured with `nm` over its objects rather than
with a grep** -- a grep counts the word and the linker counts the call:

| | call sites | where |
|---|---|---|
| `unlink` | 9 | recon_fs.c, recon_control.c |
| `stat` | 5 | recon_fs.c, recon_net.c |
| `access` | 5 | recon_fs.c, recon_error.c, recon_cmd.c |
| `chmod` | 4 | recon_modules.c, recon_fs.c, recon_control.c |
| `rmdir` | 3 | recon_fs.c |
| `lstat` | 2 | recon_fs.c |
| `umask` | 2 | recon_control.c |
| `rename` | 1 | recon_fs.c |
| `fstat`, `mmap`, `munmap` | 0 written | referenced through glibc's own macros |

**Thirty-one call sites, and nothing can be derived from what exists.**
`SYS_LIST` hands back *names* -- a buffer of them, which is what `readdir` is
built on -- and nothing else. Not a kind, not a size, not a time. So there is
no way to write `stat` on top of it, and no way to tell a directory from a file
without opening it and finding out.

### What would answer it, smallest first

**One call answers twelve of the thirty-one.**

```
SYS_STAT(path, path_len, out, out_len) -> SYS_OK, or the size it needs
```

filling the four facts the VFS already holds -- **kind, size, when it last
changed, and the mode it already stores and already enforces**. That is
`stat`, `lstat` and `fstat`-by-path at once, because there are no symbolic
links for `lstat` to differ about.

And it makes `access` library code: `stat` the path, compare the mode against
`SYS_GETUID`. **Approximate, and worth saying so** -- it would not account for
the capabilities a process holds, so a privileged caller could be told no and
then succeed. For the desktop's five sites, which ask *"is this file there and
can I read it"*, that is the right answer; if it ever needs to be exact, that is
a call of its own and not a reason to delay this one.

**A second call answers twelve more.**

```
SYS_REMOVE(path, path_len) -> SYS_OK
```

`unlink` and `rmdir` together, refusing a directory that is not empty -- which
is the one rule that makes them one call rather than two. A caller that wants
the directory gone empties it first, which is what every one of the three
`rmdir` sites already does.

**And one that cannot be built from anything else:**

```
SYS_RENAME(from, from_len, to, to_len) -> SYS_OK
```

One call site, and it is here because renaming is the operation that has to be
*atomic*. Copy-then-remove is not a rename: it is twice the disk, and a power
cut in the middle leaves two files or none. ReconFS is copy-on-write, so it is
the one filesystem where this ought to be cheap.

`chmod` and `umask` are four and two sites and are the least urgent: the kernel
already stores a mode and already refuses on it, so `chmod` is a setter for
something that exists, and `umask` is a number on the process.

### What it does not need, so that it does not get built

- **Not POSIX's `struct stat`.** It carries a device number, an inode number, a
  link count, three separate timestamps and a block count, and this system has
  none of them. The same argument as the socket calls, which took an address as
  a number rather than a `sockaddr`: a shape with fields that do not exist is a
  shape every caller has to be told to ignore.
- **No symbolic links**, which is why `lstat` and `stat` are one call. If links
  arrive, they arrive as a decision about what a path *means*, and that is a
  bigger conversation than a flag.
- **No `atime`.** It is a write on every read, and nothing in the desktop asks
  for it.
- **Not `mmap` of a file.** It is in the table above because glibc's headers
  put it there, not because anything calls it. `SYS_MAP` already maps a file
  when the file has memory to map.

**Whose side:** `core/`. The VFS has every one of these facts already -- it
enforces the mode bits, it knows the size, and `reconfs` carries the times. What
is missing is a way for a program to ask.

**And there is no caller waiting yet, deliberately.** The memory entry had one
-- `mem_recon.c` asked and was refused for a week, which is what made the day it
worked a day with nothing to rebuild. The same would be worth doing here, and
it needs a number first: **no syscall number has been taken for any of these**,
because a number claimed by the half that does not own `core/` is a number
claimed twice, which has happened and is in `docs/BUGS.md`. Name the numbers and
`posix.c` gains the callers the same afternoon.

---

## There is no way for a program to give memory back

**Where:** the other half of the entry above, left open when that one was
answered on 15 September 2026.

A program can ask for a range and cannot release one. `mem_recon.c`'s
`give_back` says so rather than pretending, and the allocator treats "cannot
give back" as a **supported** configuration -- it keeps the region and reuses
it, and every scenario in the suite runs both ways.

**What it costs is a program that cannot shrink.** A browser tab that is closed
releases 6.8 MiB to the allocator and none of it to the machine, so a window
where twelve tabs have been opened and closed is holding eighty-one megabytes
that nothing else can have. On 512 MiB that is a browser which dies after sixty
tabs and cannot say why.

**What would answer it:** a call that takes an address and a length and undoes
what `SYS_MAP` with fd -1 did -- dropping the region, unmapping whatever pages
it had, and handing them back to `pmm`. The region table already records the
bounds; what does not exist is the walk that frees the pages of one region
rather than of a whole address space at teardown.

**One thing it must decide** that the taking side did not have to: `map_next`
walks upwards and is never reused, deliberately, because a cursor that went
backwards after an unmap would put a second range where a program still believes
the first one is. A release call is the thing that makes that comment matter.
Reusing the addresses needs a free list of ranges; not reusing them means a
program that takes and releases for long enough walks the 1.75 GiB between
`USER_MAP_BASE` and `USER_MAP_END` and stops. The first is the right answer and
the second is an honest place to start, as long as it is chosen rather than
arrived at.

**Whose side:** `core/`, like the taking.

---

## The console and a program both own the screen

**Where:** `kernel/user/paint.c`, the first ReconOS program written in C, on
14 September 2026. Found by photographing the panel rather than by reading the
serial line, which said the program had succeeded — and it had.

A program opens `/dev/fb0`, is told the geometry by `SYS_SCREEN`, maps it with
`SYS_MAP` and fills it. That all works. Then the program exits, the kernel
prints its next self-test line, and **the framebuffer console draws straight
over the top of the picture** — not all of it, only the cells it has characters
in, so what is left is a screen with the program's background showing round the
edges of a block of kernel text.

There is no arbitration. Both are writing to the same pixels through different
paths, and the last writer wins.

**Why this is fatal for the desktop rather than untidy.** The compositor owns
every pixel: it decides what is on the screen, and something else drawing into
the middle of that is not a cosmetic problem, it is the compositor being wrong
about what is displayed. It will not redraw over the damage, because nothing
told it there was any. A kernel log line during a login screen would sit there
until something else happened to repaint that region.

**What would replace it:** a way for the program that has mapped `/dev/fb0` to
be the one that draws. Not a lock in the general sense — the simplest thing
that would do is for the console to stop painting to the *panel* while the
framebuffer is mapped, and keep painting to serial, which is where anybody
debugging is reading anyway. Give it back when the descriptor is closed or the
program exits, so a program that dies does not leave a machine with no console.

The desktop does not need to *share* the screen with the console. It needs the
console to stop, and it needs the stopping to be tied to something the kernel
can observe rather than to a promise the program makes.

**Hit again on 14 September, and worse, by `userland/init/recon_init.c`** --
the first program on this kernel that is a screen somebody reads rather than a
self-test. It draws a panel with the machine's own facts on it and then stays
up.

The first boot came back with the program's panel showing through a hole in
the kernel's boot log, exactly as above. Moving the call after the kernel's
last message was **not enough**, and that is the part worth recording: the
console does not only draw when it is handed something new. It repaints its
window when it scrolls. A single `kputs` after the program started was enough
to put the whole log back on top of a screen that had just been drawn.

What works today is that `main.c` starts the program as its **last statement,
with nothing printed after it at all** -- not even a success line. That is an
arrangement, not a fix, and it holds for exactly as long as there is one
program. It is written down in `core/main.c` where somebody would look.

**And the ordering makes the fault invisible rather than absent**, which is the
worse shape: the screen looks right, so the next person to add a diagnostic
print after that line will not find out what they broke until they photograph
a machine.

**Whose side:** `core/`. Nothing about it names a machine — it is a rule about
which of two writers is allowed to touch a mapping, and both of them are
already portable.

---

## Nothing can make a directory, so the volume has no shape

**Where:** working out what `userland/init/recon_init.c` could say about
storage, 14 September 2026. It asks `SYS_LIST` what is at the root of the
volume and reports the count, and on a freshly installed machine the honest
answer is zero -- the installer formats the System partition and writes nothing
into it.

**The desktop has a layout and cannot build it.** `include/recon_fs.h` has
described it since v0.1.0 and it is deliberately Windows-shaped: `/System` for
the operating system's own files, `/Programs` for installed applications,
`/Users` for documents, with the rule that a full disk of programs must not be
able to stop the system booting. Every one of those is a directory, and there
is no way to create one.

**What already exists, which is most of it.** `reconfs_create` takes a `type`
argument and `RECONFS_TYPE_DIR` is defined beside `RECONFS_TYPE_FILE`; the
format has directories, `reconfs_readdir` walks them, and `reconfs_lookup`
resolves a name in one. FAT32 has `fat32_mkdir` already and the installer uses
it on the ESP.

**What is missing is the two layers above that.** `core/rootfs.c` exposes
`rootfs_create_file`, `rootfs_read_file`, `rootfs_replace_file`,
`rootfs_remove_file` and `rootfs_list` -- and no `rootfs_create_directory`.
`SYS_CREATE` therefore makes files only, and a program has no way to ask for
anything else.

**What would replace it:** `rootfs_create_directory(path, mode)` over the
`reconfs_create` that is already there, and a system call that reaches it --
either a `SYS_MKDIR` or a directory bit in `SYS_CREATE`'s mode, whichever the
kernel would rather own. The desktop does not care which; it cares that
`/System` can exist.

**And then the installer can write a system rather than a bootloader.** Today
it writes an ESP with a loader and a kernel, formats a System partition and
stops. With directories it can put the layout on the volume, and the machine
that boots afterwards has somewhere for a program to live.

**Whose side:** `core/`. Nothing about it names a machine.

**Built, 14 September 2026 — as `SYS_MKDIR`, and it took four other faults
with it.** `rootfs_create_directory(path, mode)` over the `reconfs_create`
that was already there, a system call of its own rather than a bit in
`SYS_CREATE`'s mode, and `userland/init/layout.c` holding the ten paths.
`recon_init` lays them down on **every** boot, making what is missing and
leaving what is there -- which is the requirement rather than a nicety, since
a first boot that lost power half way and a second boot that finds everything
are the same code path. Installed onto a blank 8 GB disk from a real medium
and booted twice: *10 directories, laid out just now*, then *10 directories,
all already there*.

`/Apps`, not the `/Programs` this entry named. `include/recon_fs.h` says
`/Apps` and has since v0.1.0; the paragraph above was written from memory and
the header was written from the code. The suite compares the two in both
directions, so the next time they disagree it will be a test failure rather
than a paragraph.

**Making it work on a real volume is where the cost was**, and none of it was
in the call. See KF-226 to KF-229: one directory took 69-75 seconds to create
on an installed NVMe disk and none at all on the same image over virtio-blk;
the root of a volume could not be listed at all; every refusal from a listing
reached a program as *the disk failed*; and five self-tests passed exactly
once per volume -- a fault that had been sitting inside the one check written
to catch it.

---

## ~~Nothing in user mode can ask the machine to turn off~~ -- built, 14 September

**`SYS_POWER`, in kernel 0.2.30.** `recon_power(POWER_ACTION_OFF)` and
`recon_power(POWER_ACTION_RESTART)` in `userland/include/recon.h`. Behind
`CAP_SHUTDOWN`, which was already there.

Everything this section asked for, and the reasons it gave are the reasons it
was built this way:

- **It refuses with a reason.** `SYS_EPERM` (not allowed), `SYS_ENOPOWER` (the
  firmware named no way), `SYS_ENOSTATE` (no such state declared),
  `SYS_ENOMECH` (this kernel cannot reach it here), `SYS_EINVAL` (not an action
  this kernel knows). Five numbers, because "could not shut down" on a screen
  is not actionable for any of them.
- **An unknown action does not default to off.** A program built against a
  later kernel must not stop the machine by asking for something else.
- **Suspend is not in it**, for the reason stated below: it would be asking for
  the hard one to get the easy one.

Restart reaches the architecture first -- 0xCF9 and the 8042 on x86_64, PSCI
`SYSTEM_RESET` on aarch64 -- and the FADT's reset register second, which
`acpi.c` had parsed since the FADT was parsed and which nothing had ever read.
Both routes were watched to work and both are in the verification matrix.

**And the refusal is tested**, by a ring-3 program on both architectures that
asks to stop the machine without holding the capability and must be told no.
Watched to fail: with the check taken out of `power_off`, the guest stops in
the middle of its own self-tests and the log ends one line early.

<details><summary>What this section originally said</summary>



**Where:** 14 September 2026. Named directly: *"you said you couldn't build
the actual power system because you needed the kernel."*

**The kernel can do it.** `core/power.c` has `power_off()`, it returns an
`enum power_result` saying why when it cannot, and `power_off_or_say_why()`
wraps it. KF-162 is the entry about it reporting a refusal while the machine
was in the middle of obeying, which is a fault that only exists because the
path works. It is reachable **from the kernel command line** and from nowhere
else.

**Nothing in user mode can reach it.** There are twenty-five system calls and
none of them is about power. So the desktop's Shut Down, Restart, Sign Out and
Lock -- all of which exist, are drawn, and work on Linux today -- have nothing
to call.

**What would replace it:** one call, with an argument saying which of *off* and
*restart* is wanted, and **the capability check already written**. `SYS_GETCAPS`
and `SYS_DROPCAP` exist and the boot report already lists `shutdown` among the
capabilities a process holds -- so the permission half of this is built and
being tracked, and the thing it guards does not exist yet.

Two notes on shape, from the desktop's side:

- **It must be able to refuse and say why.** `power_result` already
  distinguishes the reasons; a program that asks a machine to turn off and gets
  a plain failure cannot tell "this machine has no ACPI" from "you are not
  allowed", and those need different words on a screen.
- **Sleep is a different question and can wait.** Shutdown and restart are a
  request the firmware either honours or does not. Suspend is a contract with
  every driver about state, and asking for it in the same call would be asking
  for the hard one to get the easy one.

**Whose side:** the call is `core/`; what it reaches is already split properly,
with `arch/` doing the machine-specific part.

</details>
---

## A program has to be inside the kernel image to run at all

**Where:** `userland/init/recon_init.c`, 14 September 2026, which is the first
ReconOS program that is a system rather than a self-test -- it reads the
machine's facts and draws the screen somebody sees on a first boot.

It is carried into the kernel as bytes. `kernel/core/user_elf.S` has a third
`.incbin` beside `hello.elf` and `paint.elf`, the kernel links it into
`.rodata`, and `main.c` starts it by pointer. That works and it is how the two
self-tests before it worked, which is the whole problem: **a self-test belongs
in the kernel image and a program does not.**

What it costs today, and it is already real:

- **121 KiB of the kernel image** is one program's ELF, and the kernel is
  539 KiB of text. A second program doubles the overhead of having any.
- **Changing the screen means rebuilding and reflashing the kernel.** On real
  hardware that is a stick, a reboot and a firmware menu for a change to a
  string.
- **The installer writes a bootloader and a kernel to a disk and nothing
  else.** The System partition is formatted and empty. There is no way to put
  a program on it that would ever be run.

**What would replace it:** the kernel loading an ELF from the volume at the end
of boot -- `/System/init.elf`, or whatever the layout settles on -- through the
VFS it already has. Every piece is built: `SYS_OPEN`, `SYS_READ` and
`user_elf_create` all exist and are exercised. What is missing is the four
lines that read a file into a buffer and hand it to the loader instead of
handing it a pointer into `.rodata`.

This is **much smaller than process creation** and worth separating from it. A
kernel that can start one named program from a volume is a system somebody can
install a new version of; a kernel that can only start what was compiled into
it is a kernel with a demo in it.

**Whose side:** `core/`. Nothing about it names a machine.

**Built, 14 September 2026 — and the four lines were the smallest part of it.**
`user_exec_path` already existed and already passed a self-test every boot, so
the kernel half really was four lines: ask the volume for
`/System/init.elf`, fall back to the copy inside the image, and **say which
one ran**.

What took the rest of it was that nothing put a program there. The medium now
carries `/reconos/init.elf` — the first file on a ReconOS install medium that
is neither a loader nor a kernel — and the installer writes it onto the System
volume, which is the first thing the installer has ever written into a ReconFS
volume at all. That needed `reconfs_place_file` and
`reconfs_place_directory`: the three moves every create makes, with the volume
named rather than assumed, because `rootfs_create_file` can only reach the
volume the kernel booted from and that is the one an installer must not touch.

**Shown rather than asserted.** A kernel binary was kept, a string in the
program was changed, the program alone was rebuilt, and a medium was made
carrying the old kernel and the new program. The installed disk booted the
kernel *byte for byte as kept* and drew the new string. That is the whole
claim, and before this it was impossible by construction.

`scripts/install-then-boot-test.sh` asserts it as its eighth check, because
the fallback is designed to be quiet: without the assertion, an installer that
stopped writing the program would still make a machine that boots and draws
and passes every other check.

**The copy inside the kernel stays**, as the fallback for a machine that has
not been installed onto — which is every blank disk in the rig. Taking it out
is a separate decision and wants a machine that can be recovered without it.

---

## Nothing in user mode can start a program

**Where:** everywhere, the moment there is more than one thing to run. Hit on
14 September 2026 while writing the C environment: there is a `crt0`, a syscall
header and a program that draws, and no way for that program to be started by
anything except the kernel deciding to start it.

The kernel loads and runs an ELF — `core/elf.c` does the whole job, and it does
it well enough that a C program with two segments and a `.bss` runs correctly.
What is missing is the call. There is no `fork`, no `exec`, no `spawn` and no
`wait`, so the set of programs that can run is the set the kernel was compiled
knowing about.

**Why this is the one that blocks everything else.** A desktop is not one
program. It is a compositor that starts a shell, a shell that starts
applications, and a session that restarts what dies. Every one of those is a
program starting another program and being told when it ends. Until that call
exists, the most the desktop can be on this kernel is a single binary with
everything linked into it — which is what it is on Linux today, and is the
thing the move to Wayland clients was meant to stop.

**What would replace it:** whatever shape suits the kernel. `fork` is not
required and arguably not wanted — copy-on-write of a whole address space to
immediately discard it is a lot of machinery for what is almost always
`spawn`. A call that takes a path, an argument vector and an environment and
returns something to wait on would do everything the desktop needs, and it
avoids `fork`'s hard cases entirely.

The one thing the desktop does need alongside it is **a way to be told a child
has ended and what its exit code was**, because a session that restarts what
dies has to know that something died.

**Whose side:** `core/` for the call and the process work; `arch/` only for
whatever entering a new address space costs on each machine.

---

## Creating a file with a mode

**Where:** `src/recon_tls.c`, generating the private key for remote access.

`recon_fs_write` creates a file and writes it. There is no way to say what the
file's mode should be, so the private key is written and *then* tightened with
`chmod`. Between those two calls it is a private key readable by anything on
the machine.

The window is small and the fix is not available at this layer: it needs a
filesystem call that takes a mode at creation. Every secret ReconOS writes
from here on has the same window, so this gets worse rather than better.

**What would replace it:** create-with-mode, or an open-then-write where the
mode is fixed before any content lands.

**Built, 8 September 2026 — the first form, and it closes the window rather
than narrowing it.** `SYS_CREATE(path, path_len, mode, data, len)` creates the
file, fills it, and sets its mode inside **one ReconFS transaction**. Copy-on-
write commits once, so the file becomes visible only when it is finished,
already carrying the permissions asked for. There is no instant at which it
exists with the wrong ones — and that holds across a power cut, because the
intermediate state is not a state the volume can be left in.

Creating a name that already exists is **refused**, not overwritten. A create
that silently replaces is how running a key-generation routine a second time
destroys the key that was working.

**What this does not do, said plainly: nothing enforces the mode.** It is
stored, reported and survives a remount, and no code anywhere consults it
before reading a file, because the kernel still has no idea who is asking. That
is the next entry on this list and it is a larger piece.

What is fixed is the window. Every file created from here has correct
permissions recorded from the first instant it exists, so when enforcement
arrives it has something true to enforce and no volume written in the meantime
has to be gone back over.

---

## Who somebody is, enforced by something underneath

**Where:** everywhere. Stated plainly in System Information and the README.

ReconOS has accounts, roles and an administrator check, and every one of them
is enforced by ReconOS asking itself. A standard account cannot install a
program because `recon_control_panel.c` declines to, not because anything
stops it. The whole tree runs as one host user who owns all of it.

This is the largest honest gap in the system and it is not close to being the
hardest thing on this list — it needs users, permissions, and a filesystem
that knows about both.

**What would replace it:** an identity the kernel enforces, so that "this
account may not do that" is true even when the thing asking is not ReconOS.

**Built, 11 September 2026.** A process running as uid 1000 is refused a `0600`
file owned by the kernel, and the refusal comes from the kernel rather than from
anything asking itself. The policy lives in one place — three filesystems
deciding would be three decisions, and the day they disagree is the day a file is
readable through one path and not another.

The rule is **first matching class decides**, not an or across the classes a
caller belongs to: mode `0004` means the owner may *not* read it and everybody
else may. Under an or, the owner reads it.

**And a program can ask.** `SYS_GETUID` and `SYS_GETGID`, because a process
refused a file could otherwise not tell "I am the wrong user" from "the file is
not there" — which is the difference between asking somebody to log in and
reporting a bug.

**Capabilities came with it**, and they are the part that matters for an
installer: `CAP_FILE_OVERRIDE`, `CAP_RAW_DISK` and `CAP_SHUTDOWN`, held and then
**dropped, never regained**. `SYS_DROPCAP` answers with what is still held, and
there is deliberately no call that grants — a set that can be regained protects
nothing. So a privileged step can hold a power for the window it needs and give
it up, and every bug after that cannot reach a disk.

**Not built, and said plainly:** directory traversal is not checked — reaching
`/a/b/file` does not require the right to traverse `/a` — and neither is
set-user-id.

---

## Processes

**Where:** `src/recon_modules.c` loads applications with `dlopen`; launching
anything external is `fork()`. Watchtower reads `/proc`.

Applications are shared objects inside the compositor's own process. That is a
deliberate choice for now and it works, but it means an application that
crashes takes the desktop with it, and "End Task" in Watchtower cannot end a
task that is a function pointer in the same address space.

The Processes tab reads `/proc` and reports the host's processes, which are
not ReconOS's processes because ReconOS has none.

**What would replace it:** real processes, so an application is something that
can be stopped without stopping the thing that drew its window.

---

## Storage that knows how big it is

**Where:** `src/recon_fs.c`, and the Storage page in the Control Panel.

`recon_volume_*` presents three spaces — System, Programs, User — each with
its own recycle bin. They are three directories. There is no capacity, so the
Storage page measures what is in each and says, honestly, that how much is
left is the host's to answer. The share bars are shares of the total measured,
not of a disk, because there is no disk to be a fraction of.

The abstraction was written this way on purpose so it can sit on real volumes
later without the pages above it changing.

**What would replace it:** block devices, a partition table, and a filesystem.
Then a volume has a size, a free figure, and something to format.

---

## Setting a display mode

**Where:** Display Settings, where screen resolution is the last row still
marked not built.

ReconOS takes whatever size the window or the screen it was given is.

**Note:** this one is *not* fully blocked. wlroots on a DRM backend can already
enumerate and set modes on real hardware, so the userland half is buildable
now and the backend can be swapped later. Listed here because the eventual
answer comes from the kernel, not because the work has to wait for it.

---

## Machine facts read out of the host's filesystem

**Where:** `src/recon_procinfo.c` reads `/proc/cpuinfo` and `/proc/meminfo`.
`src/recon_net.c` reads `/sys/class/net/*/statistics/*` for the Data Used page
and `/proc/net/route` for the gateway.

Every number System Information and Network show is scraped out of a text file
the host happens to publish, parsed with `sscanf`. It works and it is fast and
it will not survive contact with a machine that does not have `/proc`.

**What would replace it:** the kernel answering these directly — processor,
core count, memory in use, per-interface byte counts.

**Partly built, 8 September 2026.** `SYS_MACHINE` answers processor vendor and
model, processors found *and* online, total and free memory, page size, and how
much entropy the pool holds. The caller passes the size of its own structure and
gets back the size the kernel would have written, so a program built against one
kernel version and run on another gets a valid prefix and can see that it got
one. Growing the structure is allowed; reordering it is not.

Two counts for processors rather than one, because a machine where they differ
is a machine with something wrong with it, and a single number hides exactly
that case.

**Per-interface byte counts are not in it**, because there is no network stack
to count. That half stays open.

---

## Randomness

**Where:** `src/recon_tls.c`, seeding the certificate's key, and
`src/recon_control.c` making a remote-access key.

Both come from the host's entropy source through mbedTLS. A machine generating
its own long-lived private key on first boot is exactly the situation where a
weak entropy source produces keys that are quietly guessable, and ReconOS has
no way to know how good the one underneath it is.

**What would replace it:** an entropy source the kernel owns, and a way to ask
how much it has.

**Built, 8 September 2026.** `random_bytes()` runs a ChaCha20 generator over a
pool seeded from the processor's own generator where there is one -- RDSEED on
x86_64, RNDRRS on aarch64, both preferred over their weaker siblings because a
seed wants the noise source and not an expansion of it -- and from timing
jitter where there is not. `random_entropy_bits()` is the way to ask.

Three things about it are worth knowing before building on it:

- **It refuses rather than returning weak bytes**, and there is no override.
  A caller that ignores the return value has generated a key from an
  uninitialised buffer, so the bool is not advisory.
- **The estimate is deliberately low.** The hardware generator is credited at
  half its width because nothing can check a sealed box whose output looks
  identical working or failed; timing is credited one bit per sample.
- **"No hardware generator" and "a hardware generator that did not answer" are
  different lines in the summary**, because they call for different responses.

**And `SYS_RANDOM` reaches it from a program**, added the same day. It returns
`SYS_EAGAIN` rather than `SYS_EINVAL` when the pool is not seeded, and the
difference is deliberate: a bad argument means the program is wrong and should
stop, while this means the machine is not ready and the same call may work
later. A key generator told `EINVAL` would report *itself* broken.

---

## The control socket's proof of identity

**Where:** `include/recon_control.h`.

A connection over the Unix socket is trusted without a key because it was able
to open a file only its owner can open. That is the host's filesystem
permissions doing the work, and it is the reason the local socket needs no
authentication at all.

It is a good mechanism. It is also entirely borrowed, and it is the thing that
will need replacing first when the host goes away — before anything about the
network port matters, because this is the path everything local uses.

**The kernel half exists as of 11 September 2026**, and the desktop half does
not, so this entry stays open rather than being ticked.

What the borrowed mechanism actually does is ask the filesystem *who opened
this*, and both halves of that answer are now available without a host: files
carry an owner the kernel enforces, and `SYS_GETUID` tells a program what it is
running as. That is enough to build the same proof natively — a socket whose
node only its owner may open, and a server that asks the kernel rather than
asking itself.

It is worth saying which part is still missing: the kernel has no sockets at
all, so there is nothing yet to apply this to. See 1.3's IPC row in the audit —
pipes and shared memory are built, sockets are not.

---

---

## An open bug is not a closed door

**Read this before deciding the kernel is too broken to build against.**

`docs/BUGS.md` records every fault found, and it is long because faults are
looked for rather than waited for. It is **not** a list of reasons to stop.
The kernel boots on twenty-eight paths, passes 1530 self-tests with none
skipped, runs programs, draws on the screen and writes to disks -- with those
entries open, because almost all of them describe something already fixed or
something narrow.

The rule, and it came from Joshua directly after the desktop session stalled on
this twice:

> Ignore the open KF entries. Build against the kernel. Only stop if an entry
> **names the thing you are working on** and says it does not work.

Everything else in the kernel is live. A recorded bug means somebody went
looking, which is the opposite of a warning.

**What to check instead of the bug list:** this file, and the audit. This file
says what the kernel offers and what it does not. If a capability is not here
and not in `docs/KERNEL.md`, it is missing -- and *missing* is a different
question from *buggy*.

---

## ~~The one gap that blocks the most work: sockets~~ -- answered, 15 September 2026

> **Answered in kernel 0.2.41 / v0.4.38.** Five calls -- `SYS_SOCKET`,
> `SYS_BIND`, `SYS_LISTEN`, `SYS_ACCEPT`, `SYS_CONNECT` -- and the design
> underneath them is why there are only five: **a socket is a `struct file`**,
> so `SYS_READ`, `SYS_WRITE` and `SYS_CLOSE` already serve a connection and
> `posix.c` took no changes at all.
>
> `userland/libc/socket.c` is the caller, held against the host's sockets by
> `recon_libc_socket_tests`. The kernel's own half -- that a socket really is a
> descriptor -- could not be tested kernel-side, because a kernel thread has no
> process and `fd_install` fails there for sockets exactly as it does for pipes.
> `recon_init` settles it on the machine, and the installed-disk test asserts
> the line: *a descriptor, closed once, refused twice; listening, and accept
> says EAGAIN.*
>
> **Two limits are deliberate and are not gaps in this entry.** `accept` does
> not block -- a listener has no wait queue, so a server polls -- and there is
> no `sendto`/`recvfrom`, so an unconnected datagram is refused rather than
> half-served. Both are written into `userland/include/sys/socket.h` where a
> caller will find them.
>
> The rest of this entry is what it said before it was answered.


Recorded at the top because it is the answer to "what can the OS side build
next", and the answer is smaller than it looks.

The kernel has **27 system calls** and not one of them touches the network:

```
SYS_OPEN SYS_READ SYS_WRITE SYS_CLOSE SYS_CREATE SYS_MKDIR SYS_LIST SYS_SEEK
SYS_PIPE SYS_MAP SYS_SCREEN SYS_POWER SYS_TIME SYS_WALLTIME SYS_RANDOM
SYS_MACHINE SYS_KILL SYS_EXIT SYS_YIELD SYS_GETPID SYS_GETUID SYS_GETGID
SYS_GETCAPS SYS_DROPCAP SYS_SIGACTION SYS_SIGMASK SYS_SIGRETURN
```

**A userland program cannot open a socket, so it cannot serve anything.** That
is why no network role -- server, thin client, firewall -- can be started on
the OS side today. Not because they are hard: because the doorway is absent.

**The hard part is already built and passing.** `tcp_open_listener`,
`tcp_accept_ready` and `udp_bind_port` exist in `kernel/core/net.c` and work;
the TCP state machine, the listener path and the random-ISN rule are all in
place and tested. What is missing is the system call that lets a program reach
them.

So this is one well-defined kernel-side item that unblocks three of the five
roles in `docs/ROLES.md` simultaneously, and it is the next thing the kernel
track is building.


## Time

**Where:** file timestamps in the explorer, the clock, the certificate's
validity dates.

All the host's. The certificate's validity is hard-coded to a ten-year window
rather than computed, partly because expiry is meaningless for a pinned
self-signed certificate and partly because there is no clock ReconOS owns to
compute it from.

**What would replace it:** a real-time clock the kernel reads, and a monotonic
clock that does not go backwards.

**Built, 8 September 2026.** Both, and they are two calls rather than one:
`SYS_TIME` is monotonic and means nothing outside this boot, `SYS_WALLTIME` is
the date and can jump. A caller timing something must use the first and a caller
stamping a file must use the second — collapsing them into one call is how a
duration comes out negative.

---

## Memory that will not leak a secret

**Where:** `src/recon_mail.c`, `src/recon_mailwin.c`, `src/recon_crypt.c`.

The Mail window asks for a password every time it connects and stores it
nowhere. That is the honest answer today and it is not the right one for long:
a mail client that cannot remember a password is one somebody stops opening.

The right answer is a keyring -- a key that exists only while somebody is
signed in, derived from their account password at sign-in, used to encrypt
saved secrets and held in memory until they sign out. That much is buildable
here. What is not is the guarantee underneath it, and it is three separate
guarantees that arrive at different times.

### 1. Memory that cannot be paged out

A secret written to swap outlives the session, on a disk, and nothing above the
kernel can prevent it. The page allocator needs to be able to pin a page.

Worth having **before** paging exists rather than after. Nothing is paged yet,
so the discipline costs nothing now; retrofitting it means retrofitting it onto
a system that has already leaked, and there is no way to find out what it
leaked or to whom.

### 2. Memory another process cannot read

The address space boundary. There is nothing to say about it until there are
address spaces.

### 3. Memory that is actually erased

The one that looks solved and is not, and the one that had already bitten this
side of the project before anybody went looking. See BG-090.

`memset(p, 0, n); free(p);` is the obvious way to erase a secret. Those stores
are never read afterwards, so the standard permits a compiler to delete the
call as dead, and at higher optimisation levels compilers do. The source says
the password was erased; the binary leaves it in the heap. CWE-14.

It is the same class of fault as the kernel's `&&label` block being deleted for
being unreachable: **a compiler removing something because nothing observable
depends on it, where the thing that depended on it was not expressible in the
language.** Both were found by looking at the compiled output rather than
reasoning about the source, and in this case the answer was the less obvious
one -- the `memset` had *not* been removed at this project's current
optimisation level, so the property held by accident of flags and would have
stopped holding silently.

The fix here is a volatile-pointer loop, which the standard does not permit to
be elided. That is enough for a userspace erase and it is not enough for a
kernel one: a kernel that hands a freed page to another process without
clearing it has leaked the secret regardless of how carefully the previous
owner erased its own copy.

**What would replace all three:** memory the kernel will not page out, will not
hand to another process without clearing, and will not let another process
read -- plus an erase primitive the optimiser cannot remove, provided rather
than reinvented by every caller who needs one.

Named here rather than in a comment because it is a kernel feature that a
desktop feature is waiting on, which is what this file is for.

---

## Running another operating system inside this one

**Where:** nowhere yet. Asked for on 2026-09-05 as a thing for later, with the
question "is that a kernel thing or a system thing".

**It is a kernel thing, and almost entirely.** Worth writing down now precisely
because it is far off: the parts of it that constrain earlier work are the
parts that get made impossible by accident.

A hypervisor needs three things, and a desktop can supply none of them:

- **The processor's virtualization extensions.** VT-x on Intel, AMD-V on AMD,
  EL2 on aarch64. Entering them is privileged, so it happens in the kernel or
  it does not happen. Checkpoint 3 already reads what the processor can do;
  whether these are present is one more question to ask it, and asking early
  costs nothing.
- **Second-level address translation** — EPT, NPT, stage-2. A guest builds its
  own page tables believing it owns physical memory, and something underneath
  has to translate again. That is the existing page-table work with another
  level under it, which is much easier to design for now than to retrofit onto
  a memory manager that assumed one translation.
- **Trapping and emulating.** A guest touching a device traps to the host, and
  the host has to answer as the device would. That needs the fault path
  (checkpoint 7) and the device model, and it is the part that is genuinely
  large: a hypervisor is mostly device emulation by volume.

What the desktop side owes it is small by comparison -- a window showing a
guest's framebuffer, a list of virtual machines, a way to start and stop one.
That is an application, and it is the last thing to build rather than the
first.

**The one thing worth deciding early:** whether the memory manager keeps the
*option* of a second translation level. Not building it, and not closing the
door on it. Everything else here can wait until there is a kernel to put it in.
