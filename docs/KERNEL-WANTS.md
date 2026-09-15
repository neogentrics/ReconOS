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

## There is no way for a program to ask for memory

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
