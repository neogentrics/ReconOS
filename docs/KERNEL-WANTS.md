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

---

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
