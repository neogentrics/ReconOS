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

`0.0.6`. The number says what works. What works: it boots four ways across two
architectures, knows which firmware is underneath it, knows what the processor
can do, knows what memory exists, hands pages of it out, runs on page tables it built itself, and is started by a
bootloader we wrote.

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

GRUB is gone from the boot path as of checkpoint 4. `reconboot` starts the
kernel on both architectures under real UEFI firmware. The Multiboot2 header
stays in the image — it is forty bytes, it costs nothing, and it means a machine
that already has GRUB can chain-load ReconOS without an install. It is a
courtesy now rather than a dependency.

## Checkpoints

| # | Checkpoint | Status |
|---|---|---|
| 0 | Builds and boots on x86_64 and aarch64, prints its identity over serial | **Done** |
| 1 | Knows what firmware booted it and what memory exists | **Done** |
| 2 | A physical page allocator | **Done** |
| 3 | Reads what this processor can actually do | **Done** |
| 4 | Its own UEFI bootloader, both architectures — GRUB comes out of the tree | **Done** |
| 5 | Its own page tables, a direct map, and large pages where the CPU has them | **Done** |
| 6 | A kernel heap | |
| 7 | Interrupts and exceptions, and a fault that reports itself instead of resetting the machine | |
| 8 | A timer, a tick, and time | |
| 9 | Threads, a scheduler, and every core in use | |
| 10 | User mode, the first system call, and the kernel moves to the higher half | |
| 11 | Block devices — storage the kernel can read and write | |
| 12 | Partition tables — GPT and MBR, and every layout it will meet | |
| 13 | ReconFS — a filesystem of its own | |
| 14 | Reads foreign filesystems well enough to install beside them | |
| 15 | The installer | |
| 16 | Its own BIOS bootloader on x86_64 — a machine with no UEFI at all | |
| 17 | Boots on real hardware: both architectures, both firmwares | |

Eighteen, numbered 0 to 17, is a lot to hold in your head, so they group into four stages: **the
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

### Checkpoint 4 — done

**reconboot boots the kernel on both architectures under real UEFI firmware.**
Not compiled — booted, against OVMF and AAVMF, off a FAT image laid out as a
real EFI System Partition.

No gnu-efi: `boot/include/efi.h` declares what the loader calls, written from
the specification, for the same reason the kernel has no libc. Built by clang
rather than gcc, because a UEFI application is a PE binary and clang targets
both architectures' PE from one installation.

The x86_64 run against OVMF reports:

```
framebuffer : 1280x800 at 0x0000000080000000, pitch 5120, BGRA
```

which is the thing UEFI gives back that is hardest to obtain any other way, and
most of the argument for writing the loader before the drivers.

**One kernel binary, three ways in.** The kernel is entered at
`reconboot_entry`, not at its ELF entry point — that belongs to the 32-bit
trampoline Multiboot2 and PVH arrive through, and UEFI has already put the
machine in 64-bit mode. The image carries a header naming its 64-bit entry and
the loader scans for it. That is what makes "BIOS, UEFI, or both" a build with
one output rather than three.

**Two bugs, both found by running it, both worth generalising.**

`BOOT_MAX_REGIONS` was 64, with a comment reasoning that "real machines report
well under thirty regions" — true of every machine tested at the time, and
wrong the first time our own loader ran. OVMF reported 86, the kernel silently
dropped 68, and it claimed 487MB usable where the truth was 505MB. It was caught
by a warning added defensively that had never been expected to fire. *Firmware
fragments its memory map as it allocates, so the region count reflects how much
work the firmware did before handing over, not how much memory the machine has.*
A number derived from observing three machines is a guess wearing evidence's
clothes.

The map also printed the same range twice under two names — loader memory and
kernel image, both true, with nothing saying which was operative. The carve-out
only subtracted non-usable from usable. It now has an explicit priority, so the
more specific claim wins: kernel, then bad, then ACPI, then reserved, then
bootloader, then usable.

**A limitation stated rather than discovered.** On aarch64 with virtio-gpu the
firmware offers a Blt-only Graphics Output Protocol: there is no linear
framebuffer and the base address comes back as zero. The loader reports the
layout as not understood and the kernel reports no framebuffer, rather than
handing anything a pointer to address zero. A framebuffer is therefore available
on x86_64 UEFI today; on ARM it needs firmware offering a linear mode, or our
own display driver.

### Why the bootloader was checkpoint 4 and not checkpoint 17

It could reasonably have gone last. It went fourth because of what UEFI gives
back: the memory map, a framebuffer at a known address, and file access on the
boot volume, all before the kernel exists. Each of those is otherwise a driver
the kernel has to write, and two of them cannot be obtained on ARM at all
without firmware help. Writing it early also meant the handoff protocol was
designed by us rather than inherited from GRUB.

### Checkpoint 5 — done

**The kernel stops running on borrowed page tables.** Until now it ran on the
trampoline's identity map, or the firmware's — both map more than they should
and neither can be changed.

Verified on all eight boot paths: PVH, GRUB on BIOS, GRUB on UEFI and reconboot
on UEFI for x86_64; device tree and reconboot on UEFI for aarch64; plus both
architectures with a CPU that has gigabyte pages.

**Large pages, and the evidence they are actually used.** The same binary on
two CPUs, mapping the same 2GB machine:

| CPU | Pages used | Page tables cost |
|---|---|---|
| `-cpu qemu64` (no 1GB pages) | 6,398 × 2MB, 1,036 × 4KB | 23 pages, 92KB |
| `-cpu max` (1GB pages) | **12 × 1GB**, 1,022 × 2MB, 1,036 × 4KB | **12 pages, 48KB** |

Also a direct map of all physical memory at a fixed offset, so allocating a page
and writing to it no longer needs a page table edit; and pages that cannot be
executed, on every CPU that has the bit.

**A span is not a size, again.** The first version mapped one range from zero
to the highest address in the memory map. This machine's map ends with twelve
gigabytes of reserved space at the 1TB mark, so that was 524,288 entries and
4MB of page tables to describe a hole. Region by region it is 29 pages. The same
mistake the page bitmap made at checkpoint 2, wearing different clothes — and
worth noticing that it was made twice, by the same person, three days apart.

**Two bugs, and both are about the moment the map changes.**

*The root table pointer stopped being valid at the instant it was installed.*
Every pointer to a page table was obtained through the map we were handed;
afterwards only the kernel image is identity mapped, and the root table is not
in the kernel image. It presented as a page fault at exactly the root table
address plus the index being looked up — `CR2=0x5800` for a root at `0x5000`
and index 256 — which is a satisfying thing to be able to read off a fault.
Same class as the allocator's bitmap, one level up.

*Supported is not enabled.* Bit 63 of a page table entry is the no-execute bit
only once `EFER.NXE` says so. Until then it is a **reserved** bit, and setting a
reserved bit does not mean "executable" — it means every access through that
entry faults, whatever the entry otherwise says. The two UEFI paths worked and
the two using our own trampoline did not, because firmware enables NXE for
itself and our trampoline never had a reason to. CPUID reports the feature is
*supported*; it does not report that it is *enabled*, and code that reads the
first as the second works only on machines that happened to enable it already.

**The higher half is deferred to checkpoint 10, deliberately.** It was in this
checkpoint's title and it is not done. The kernel still runs identity-mapped.

The reason to move it is to leave the low half of the address space free for
user processes — and there are no user processes until checkpoint 10, so today
it would buy tidiness and nothing else. The direct map, which is the part that
other code will build on, exists now. Moving the kernel image itself is a linker
script change plus one jump, and doing it beside the work that first needs it
means it can be tested by something that actually cares. Recorded here rather
than quietly dropped, because a checkpoint that shrinks to fit what got done is
not a checkpoint.

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

- **Every core, and every thread on it** (checkpoint 9). A scheduler that
  leaves cores parked is wasting the most expensive thing the machine has.
  Secondary cores are currently parked in `boot.S` on both architectures,
  waiting for an SMP story to wake them into.

  Three distinctions the scheduler has to know about, because treating them as
  interchangeable is how a machine gets slower the more work you give it:

  | | What it means | Why it matters |
  | --- | --- | --- |
  | **Cores** | Independent execution units | Real parallelism |
  | **Threads** (Intel Hyper-Threading, AMD SMT) | Two threads sharing one core's execution resources | Two threads on one core are *not* two cores. Filling both before using an idle core is slower than not |
  | **Core types** (Intel P/E-cores, Apple Silicon performance/efficiency) | Cores of different speed and power in one machine | Putting latency-sensitive work on an efficiency core makes an idle machine feel slow |

  Apple Silicon is aarch64 with heterogeneous cores, so it is the same problem
  as Intel's hybrid parts wearing different clothes; both are read from the
  topology rather than from a table of chip names, because a table of names is
  wrong for every chip released after it was written.
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
2. **Understanding the partition layout** (checkpoint 12). Three schemes, not
   two:

   | Scheme | Where it is found |
   | --- | --- |
   | **GPT** | Every modern machine. Windows, Linux, and Intel and Apple Silicon Macs |
   | **MBR** | Old PCs, and removable media formatted by anything old |
   | **APM** (Apple Partition Map) | PowerPC Macs, and some old Mac external drives |

   **This is a data format, not a kernel service**, so it is parsed above the
   block layer — the same line [THIRD_PARTY.md](../THIRD_PARTY.md) already
   draws for PNG and TLS. The desktop owns the parser; the kernel owes it
   sectors, a sector size, a sector count, and whether the device is removable.

   *Worth separating carefully, because the two are easy to conflate:* a
   **partition table** says where the partitions are, and a **filesystem** says
   what is inside one. macOS uses GPT for the first and APFS (or HFS+ on older
   systems) for the second. Reading a Mac's partition table needs GPT, which we
   need anyway. Reading a Mac's *files* needs APFS, which is a separate and much
   larger job — and, importantly, is not required in order to install beside
   macOS without damaging it.
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
   That holds on a Mac too: Apple uses a standard FAT32 EFI System Partition,
   so installing beside macOS needs GPT and FAT32 and nothing Apple-specific.
   APFS, NTFS, ext4 and HFS+ come when somebody actually wants to open a file
   on them.

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

## Where this is tracked, at a glance

A status board carrying the checkpoint list, what works today, and the evidence
behind each completed one:
<https://claude.ai/code/artifact/781c0e1d-e3ed-4d35-848e-9e808e8625dc>

It is regenerated when a checkpoint lands, so it never disagrees with this file
for long. This file is the source; the board is the view.

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
