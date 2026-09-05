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

---

## Memory that will not leak a secret

**Where:** `src/recon_mail.c`, `src/recon_mailwin.c`.

The Mail window asks for a password every time it connects and stores it
nowhere. That is the honest answer today and it is not the right one for long:
a mail client that cannot remember a password is a mail client somebody stops
opening.

The right answer is a keyring — a key that exists only while somebody is
signed in, derived from their account password at sign-in, used to encrypt
saved secrets and held in memory until they sign out. That much can be built
on this side. What cannot is the guarantee underneath it.

While the password is in memory, three things can happen to it that nothing
here can prevent:

- it can be written to swap, and swap outlives the process
- it can be read by anything with permission to read this process's memory
- it can be left in a freed page that is handed to something else

The third is the only one ReconOS can do anything about today, and it does:
the buffers are cleared rather than merely freed. The other two are the host's
to give away and the host does give them away.

**What would replace it:** memory the kernel will not page out, will not hand
to another process without clearing, and will not let another process read.
Named here rather than in a comment because it is the thing standing between
this and a mail client somebody would use daily, and because it is a kernel
feature that a desktop feature is waiting on -- which is exactly what this
file is for.
