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
