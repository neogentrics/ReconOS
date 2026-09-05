# The kernel — plan and checkpoints

Phase 2 of the [roadmap](ROADMAP.md), begun early. The code lives in
[kernel/](../kernel/README.md) and builds separately from everything else in
this repository, because it targets a different machine: no libc, no wlroots,
no Linux underneath it.

Starting it now, while the compositor is still the real system, is the point.
The compositor is where the requirements come from — every time it asks Linux
for something, that is a note about what this kernel will one day have to
provide. [KERNEL-WANTS.md](KERNEL-WANTS.md) is that list, written from hitting
the seam rather than from imagining it, and it is the authority on what the
desktop needs. This file is the kernel's side.

## Version

`0.0.4`. The number says what works. What works: it boots four ways across two
architectures, knows which firmware is underneath it, knows what the processor
can do, knows what memory exists, and can hand pages of it out.

## What "finished" means

A machine with no operating system on it, or with Windows or Linux or macOS
already on it, boots from ReconOS media, is told where to install, and comes up
into the ReconOS desktop on its own kernel — on either architecture, under
either firmware, without GRUB, without Linux, and without destroying whatever
was already on the disk unless asked to.

Every checkpoint below is a step on that path. None of them is a refactor.

## The rule about other people's code

ReconOS boots itself. The bootloader is not an implementation detail to be
delegated — it is the first thing that runs, and the project's claim is that it
was built rather than assembled. Open-source components are used where there is
genuinely no alternative, and are named in [THIRD_PARTY.md](../THIRD_PARTY.md)
when they are.

GRUB is in the *test* path and only there. It boots the kernel today so the
Multiboot2 and firmware-detection code has something real to run against, and
checkpoint 4 removes it. Nothing depends on GRUB existing; the Multiboot2
header is forty bytes and comes out with it.

## Checkpoints

| # | Checkpoint | Status |
|---|---|---|
| 0 | Builds and boots on x86_64 and aarch64, prints its identity over serial | **Done** |
| 1 | Knows what firmware booted it and what memory exists | **Done** |
| 2 | A physical page allocator | **Done** |
| 3 | Reads what this processor can actually do | **Done** |
| 4 | Its own UEFI bootloader, both architectures — GRUB comes out of the tree | |
| 5 | Its own page tables, the higher half, and large pages where the CPU has them | |
| 6 | A kernel heap | |
| 7 | Interrupts and exceptions, and a fault that reports itself instead of resetting the machine | |
| 8 | A timer, a tick, and time | |
| 9 | Threads, a scheduler, and every core in use | |
| 10 | User mode, and the first system call | |
| 11 | Block devices — storage the kernel can read and write | |
| 12 | Partition tables — GPT and MBR, and every layout it will meet | |
| 13 | ReconFS — a filesystem of its own | |
| 14 | Reads foreign filesystems well enough to install beside them | |
| 15 | The installer | |
| 16 | Its own BIOS bootloader on x86_64 — a machine with no UEFI at all | |
| 17 | Boots on real hardware: both architectures, both firmwares | |

Seventeen is a lot to hold in your head, so they group into four stages: **the
kernel exists** (0–3, done), **the kernel owns the machine** (4–10),
**the kernel owns the disk** (11–15), and **it runs on real iron** (16–17).

### Checkpoints 0 to 3 — done

**0 — two architectures, one portable core.** `arch.h` is six functions long
and `make check-portable` fails the build if machine-specific code appears in
`core/`. That discipline is why a third architecture is four files rather than
a rewrite.

**1 — firmware and memory.** Four boot paths, each actually booted, because
"the code handles UEFI" is a claim and a memory map printed under OVMF is
evidence:

| Path | Firmware reported | Regions |
|---|---|---|
| PVH, QEMU direct | paravirtual (no firmware) | 8 |
| Multiboot2 via GRUB, SeaBIOS | BIOS | 8 |
| Multiboot2 via GRUB, OVMF | UEFI | 18 |
| Image header + device tree, aarch64 | device tree | 5 |

Firmware detection on x86_64 is by *evidence*: no register says "UEFI started
you", but GRUB emits Multiboot2 EFI tags only when EFI is what started it. The
UEFI run reporting eighteen regions with ACPI NVS ranges where BIOS reports
eight is what makes it evidence rather than a constant.

aarch64 needed a real arm64 Image header before any of it worked. Reading `x0`
for a device tree pointer got zero, because a loader handed a bare ELF has no
handoff convention to follow — it clears the registers and tells the kernel
nothing. Diagnosed by printing the pointer rather than theorising about it.

Overlapping regions are resolved, not reported: firmware describes memory in
layers and ownership is subtracted from availability, always in that direction,
because unused free memory is a waste and reused owned memory is corruption.

**2 — a physical page allocator.** A bitmap, one bit per page. A free list
allocates faster but stores its links *inside* the free pages, so a stale
pointer corrupts the allocator itself and surfaces somewhere else entirely.
Based at the lowest usable address rather than at zero — indexing from zero is
invisible on x86 and describes 262,433 pages of nothing on ARM, where RAM
starts at 1GB. Self-test runs at boot on the real machine and passes on all
four paths.

**3 — what the processor can do.** Not which architecture — the firmware
settled that — but what this chip offers within it, because a 2008 Core 2 and a
2024 Zen 5 are both x86_64 and differ in nearly everything that matters for
going fast. The same binary, on two CPUs:

```
-cpu qemu64   sse2 nx                                    1 core   2MB pages
-cpu max      sse2 sse4.2 avx aes-ni rdrand avx2 rdseed  4 cores  1GB pages
              sha-ni smep smap nx 1g-pages
```

Every field is there because something will branch on it. Large pages decide
whether a gigabyte costs one page table entry or two hundred thousand. Hardware
random decides whether the entropy pool has a real source. Physical address
bits bound the page tables. Core count bounds the scheduler.

## Using the machine you are on

The project's central claim is that a 4GB machine should not need 16GB to feel
responsive. That is a design commitment, and these are the specific things it
means at kernel level. They are listed here rather than left implicit because
each one is a decision some checkpoint has to actually take.

### Memory

**The kernel never sees DDR generations, and this is worth stating plainly
because it is easy to expect otherwise.** Working out DIMM timings — the thing
that genuinely differs between DDR1 and DDR5 — is memory *training*, and it
happens in firmware before a single kernel instruction runs. By the time the
kernel exists, RAM is a memory map. There is nothing to support and nothing to
get wrong, on any generation.

What does make memory use efficient, on every generation equally:

- **Large pages where the CPU has them** (checkpoint 5). Mapping a gigabyte
  with 4KB pages costs 262,144 page table entries and a TLB miss every time the
  working set moves. With 1GB pages it costs one entry. This is the single
  largest lever, it is free once the CPU has been asked, and checkpoint 3
  already asks.
- **Compressing cold pages when memory is scarce and the CPU is not**
  (after checkpoint 9). This is exactly the trade of spending cycles to avoid
  needing RAM — on a machine with a decent processor and little memory it is
  strictly better than swapping to disk, and on a machine with plenty of RAM it
  never triggers. It is adaptive by construction rather than by a setting.
- **Not keeping what nothing asked for.** The anti-bloat principle applies to
  the kernel first: no caches that exist because caches are traditional, no
  daemons, no background work without a wakeup that justifies it.
- **Reclaiming the bootloader.** Everything marked `MEM_BOOTLOADER` in the
  memory map is memory somebody else was using, and after checkpoint 5 nobody
  is. Already recorded so it can be handed back.

### Processor

**Use what the chip has, not what the oldest chip would have had.** Checkpoint
3 reads the capabilities; the checkpoints that follow spend them:

- **Every core** (checkpoint 9). A scheduler that leaves cores parked is
  wasting the most expensive thing the machine has. Secondary cores are
  currently parked in `boot.S` on both architectures, waiting for an SMP story
  to wake them into.
- **Crypto in silicon.** AES-NI and ARM's AES extension turn encryption from a
  loop into an instruction. Disk encryption without it is a reason not to use
  disk encryption.
- **Hardware entropy** for the randomness service, where RDSEED or RNDR exists.
- **The right idle instruction.** `hlt` and `wfi` are already what the idle
  loop uses, which is why idle costs nothing rather than nearly nothing.
- **Balancing, not maximising.** The goal is the least total resource for the
  work, not the most of any one. A subsystem that can run at a lower duty cycle
  and produce the same output should.

## Firmware: BIOS, UEFI, and machines with both

All three are supported, and the third is not a special case.

| Machine | What it offers | What ReconOS boots with |
|---|---|---|
| Modern, UEFI only | UEFI, GPT, Secure Boot | The UEFI loader (checkpoint 4) |
| Old, BIOS only | 16-bit real mode, MBR | The BIOS loader (checkpoint 16) |
| Transitional, both | UEFI with a Compatibility Support Module | The UEFI loader, because it is better |

A machine with both does not require a third loader. It requires that the
install medium carry both, that each be findable by the firmware that looks for
it — an `EFI/BOOT/BOOTX64.EFI` for UEFI and a boot sector for BIOS, on the same
disk — and that the installer write whichever the machine will actually use.
That is a media-layout problem, solved once, at checkpoint 15.

UEFI is built first because one design covers both architectures and it hands
back a memory map, a framebuffer at a known address, and file access on the
boot volume before the kernel exists. A BIOS loader is 16-bit real-mode x86 and
covers one architecture, so it is worth doing second rather than not at all.

## Storage, partitions and filesystems

The goal that shapes all of this: **install beside whatever is already there.**
A disk with Windows on it is a disk ReconOS has to understand well enough not
to damage.

That splits into four separate problems, and conflating them is how the work
looks impossible:

1. **Reading and writing sectors** (checkpoint 11). Drivers: NVMe, AHCI, USB
   mass storage, virtio. This is the kernel's job and nobody else's.
2. **Understanding the partition layout** (checkpoint 12). GPT and MBR are the
   only two schemes that matter — everything else sits inside them. Windows
   uses GPT on any modern machine and MBR on old ones; Linux the same; macOS
   uses GPT with an Apple partition scheme inside it. **This is a data format,
   not a kernel service**, so it is parsed above the block layer, the same line
   [THIRD_PARTY.md](../THIRD_PARTY.md) already draws for PNG and TLS. The
   desktop owns the parser; the kernel owes it sectors, a sector size, a sector
   count, and whether the device is removable.
3. **A filesystem of our own** (checkpoint 13). ReconFS. Not designed yet, and
   deliberately not designed yet: it should be designed when there is a block
   layer to build it on and real use to shape it, not before. What is already
   known about it is recorded below.
4. **Reading foreign filesystems** (checkpoint 14). NTFS, ext4, APFS, HFS+,
   FAT32. Much bigger than it sounds, and **mostly not needed for the goal.**
   Installing beside Windows requires reading the *partition table* and
   possibly resizing a partition; it does not require reading a single NTFS
   file. FAT32 is the exception and is required, because the UEFI System
   Partition is FAT32 and that is where our own bootloader has to be written.
   So: FAT32 at checkpoint 14, the others when somebody actually wants to open
   a file on them.

### What is already known about ReconFS

Not a design, just the constraints it has to satisfy, collected as they turned
up:

- **Create-with-mode.** The desktop currently writes a TLS private key and
  *then* tightens it with `chmod`, leaving a window where a private key is
  world-readable. Every secret ReconOS writes has that window. The first
  filesystem calls this kernel defines take a mode at creation — decided now,
  because now is when it is free.
- **Recoverable after power loss.** Which means write ordering that survives an
  unexpected reset, and a `flush` that returns when the data is on the medium
  rather than when it was accepted.
- **It has to hold `recon_fs`'s shape.** The desktop already has System,
  Programs and User volumes with a recycle bin each, deliberately written so
  they can sit on real volumes later.
- **Case sensitivity, and whether drives have letters,** are open questions.
  Windows assigns letters to partitions; Linux mounts them into one tree. The
  decision belongs with the filesystem and the installer together, and is not
  taken yet.

### The installer

Checkpoint 15, and the thing that turns all of the above into an operating
system somebody can have. It needs: the boot medium to carry both loaders and
both architectures; a partitioner that can read an existing layout and shrink a
partition without losing what is in it; somewhere to write the bootloader that
the machine's firmware will actually look at; and a first-run flow, which the
desktop already has.

## Architecture support

| Architecture | Status | Notes |
|---|---|---|
| x86_64 | Boots, BIOS and UEFI both | Multiboot2 for now; own loader at 4 and 16 |
| aarch64 | Boots | arm64 Image header, device tree parsed |
| riscv64 | Not started | Four files and a Makefile stanza when wanted |
| i686 (32-bit x86) | Not planned | See below |

**32-bit x86 is not planned, and this is a decision rather than an oversight.**
A different pointer size means every structure holding an address, every page
table format, and every assumption about how much can be mapped has to work two
ways. It roughly doubles the cost of most checkpoints ahead, for machines that
have not been sold in over a decade.

This is a separate question from *running* 32-bit programs. A 64-bit x86 kernel
can execute 32-bit user code, which is what a Windows 95 application needs.
That capability is unaffected by this decision.

## Running old and foreign applications

Phase 3 and later, recorded now because it constrains a decision taken much
earlier. An application binary needs three things: an instruction set it can
execute (an emulation problem, userspace, independent of the kernel); a loader
for its file format (PE, ELF, Mach-O); and the system calls and libraries it
expects — which is nearly all of the work, and why the plan is to contribute to
Wine rather than reimplement it.

What the kernel owes all three is one small thing: a process created with a
*personality* — a loader and a system-call table chosen per binary rather than
fixed for the system. Linux calls this `binfmt`; Windows NT called them
subsystems. Free to leave room for at checkpoint 10; expensive to retrofit.

## The interface the desktop compiles against

`kernel/include/recon_kernel.h` and `kernel/api/stubs.c`, built by `make api`.
Every call returns "not implemented", and the version does not move for it,
because the version says what works and stubs work at nothing.

Four rules hold across all of it, each paid for by something:

- **Nothing allocates.** The caller owns every buffer. A kernel that allocates
  for a caller must decide what to do when it cannot, halfway through an
  operation the caller has already begun.
- **Every call returns a status, never a bool.** "Failed" is not a sentence a
  settings page can show. Above the boundary the desktop keeps its own
  `bool` + `_last_error()` convention and converts, which is one conversion per
  subsystem and is what a boundary is for.
- **Enumeration is by identity, not position**, with a generation counter.
  Hardware appears and disappears while software is looking at it.
- **No callbacks into the kernel.** Asynchronous things leave a result to be
  collected, the way `recon_net` does with reachability.

Processes are deliberately *not* stubbed. Applications are `dlopen`'d into the
compositor's address space, so there is no boundary to write in advance — the
boundary is an address space and there is not one. The module ABI is settled at
checkpoint 5, where address spaces arrive, and `recon_appwin_impl` is its real
surface: a struct of callbacks holding pointers into the compositor's own
memory, which is the part that cannot survive a process boundary unchanged.

The kernel claims error-code area letter `N` in `recon_errors.def`.

## How this is tracked

The same four places as everything else — this file for the plan,
[CHANGELOG.md](CHANGELOG.md) for what shipped, [BUGS.md](BUGS.md) for faults,
GitHub Issues for what is open. Kernel bugs take `BG-` numbers from the same
sequence as the rest of the system, because a bug number is a place in the
project's history and the kernel is part of the same project. The kernel
session sends the account; the desktop session writes the entry, so that one
register does not become two across a branch.

Developed in a git worktree on the `kernel` branch, merged to `main` when a
checkpoint lands.
