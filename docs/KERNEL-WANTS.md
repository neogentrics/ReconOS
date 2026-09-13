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
