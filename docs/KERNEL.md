# The kernel — plan and checkpoints

Phase 2 of the [roadmap](ROADMAP.md), begun early. The code lives in
[kernel/](../kernel/README.md) and builds separately from everything else in
this repository, because it targets a different machine: no libc, no wlroots,
no Linux underneath it.

Starting it now, while the compositor is still the real system, is the point.
The compositor is where the requirements come from — every time it asks Linux
for something, that is a note about what this kernel will one day have to
provide. Building both at once means the list is written from use rather than
from memory of an operating systems course.

## Version

`0.0.2`. Following the same rule as the rest of the project: the number says
what works. What works is that it boots four ways across two architectures,
knows which firmware is underneath it, and can say what memory exists.

## The rule about other people's code

ReconOS boots itself. The bootloader is not an implementation detail to be
delegated — it is the first thing that runs, and the project's whole claim is
that it was built rather than assembled. Open-source components are used where
there is genuinely no alternative, and are named in
[THIRD_PARTY.md](../THIRD_PARTY.md) when they are.

GRUB is currently in the tree's *test* path, and only there. It boots the
kernel today so that the Multiboot2 and firmware-detection code has something
real to run against, and it is the thing checkpoint 3 removes. Nothing in the
kernel depends on GRUB existing; the Multiboot2 header is forty bytes and comes
out with it.

## Checkpoints

Each of these is a thing the kernel can *do* afterwards that it could not do
before. None is a refactor.

| # | Checkpoint | Status |
|---|---|---|
| 0 | Builds and boots on x86_64 and aarch64, prints its identity over serial | **Done** |
| 1 | Knows what firmware booted it and what memory exists | **Done** |
| 2 | A physical page allocator | |
| 3 | Its own UEFI bootloader, both architectures — GRUB comes out of the tree | |
| 4 | Its own page tables; the kernel moves to the higher half | |
| 5 | A kernel heap | |
| 6 | Interrupts and exceptions, and a fault that reports itself instead of resetting the machine | |
| 7 | A timer, and a tick | |
| 8 | Threads and a scheduler | |
| 9 | User mode, and the first system call | |
| 10 | Its own BIOS bootloader on x86_64 — boots a machine with no UEFI at all | |
| 11 | Boots on real hardware, both architectures | |

### Checkpoint 0 — done

Two architectures, one portable core, and a rule enforced by the build that
they stay separated. `arch.h` is six functions long. It is meant to be.

### Checkpoint 1 — done

The kernel can now answer three questions it could not answer before: what
firmware is underneath, who loaded it, and what memory exists.

Four boot paths were built and each was actually booted, because "the code
handles UEFI" is a claim and a memory map printed under OVMF is evidence:

| Path | Firmware reported | Regions |
|---|---|---|
| PVH, QEMU direct | paravirtual (no firmware) | 8 |
| Multiboot2 via GRUB, SeaBIOS | BIOS | 8 |
| Multiboot2 via GRUB, OVMF | UEFI | 18 |
| Image header + device tree, aarch64 | device tree | 5 |

**Firmware detection, on x86_64, is by evidence rather than by asking.** There
is no register that says "you were booted by UEFI". What there is: GRUB emits
Multiboot2 tags carrying the EFI system table and image handle *only* when EFI
is what started it. Their presence is the answer, and their absence on a
machine that produced a memory map at all means legacy BIOS. That the UEFI run
reports eighteen regions with ACPI NVS ranges and the BIOS run reports eight
is the confirmation that this is reading reality and not a constant.

**aarch64 needed a real Image header before any of it worked.** The first
attempt read `x0` for a device tree pointer and got zero, because QEMU had
loaded a bare ELF — and a loader handed a bare ELF has no handoff convention
to follow, so it jumps to the entry point with the registers cleared and tells
the kernel nothing. Adding the 64-byte arm64 Image header and emitting a flat
binary changes that: the loader now places the image at the offset the header
asks for, puts the device tree below it, and passes the pointer. This is the
convention U-Boot and UEFI stubs implement too, so it is not a QEMU
accommodation — it is the thing real hardware will do.

**Overlapping regions are resolved, not reported.** Firmware describes memory
in layers: a broad "this is RAM" range, and smaller ranges inside it that
something already owns. Both are true. Left alone, the totals count owned
memory as free and the allocator eventually hands out the device tree. So
ownership is subtracted from availability — always in that direction, because
unused free memory is a waste and reused owned memory is corruption.

### Why the bootloader is checkpoint 3 and not checkpoint 11

It could reasonably go last. It is put third because of what UEFI gives back.

A UEFI application gets the memory map, a framebuffer at a known address, and
the ability to read files off the boot volume — before the kernel exists. Every
one of those is something the kernel would otherwise have to write a driver to
obtain, and two of them are things it cannot obtain at all on ARM without
firmware help. Writing the loader early means the kernel's own handoff protocol
is designed by us rather than inherited from GRUB's, and it means the
Multiboot2 code comes out while it is still forty bytes rather than after
something has come to depend on it.

UEFI before BIOS because one design covers both architectures. A BIOS
bootloader is 16-bit real-mode x86 and covers exactly one of them, so it is
worth doing — the project's aim is to boot both kinds — but it is worth doing
second.

## Architecture support

| Architecture | Status | Notes |
|---|---|---|
| x86_64 | Boots, BIOS and UEFI both | Multiboot2 for now; own loader at checkpoints 3 and 10 |
| aarch64 | Boots | arm64 Image header, device tree parsed for memory and command line |
| riscv64 | Not started | Four files and a Makefile stanza when it is wanted |
| i686 (32-bit x86) | Not planned | See below |

**32-bit x86 is not planned, and this is a decision rather than an oversight.**
Supporting it is not a matter of one more directory: 32-bit means a different
pointer size, which means every structure that holds an address, every page
table format, and every assumption about how much can be mapped at once has to
work two ways. It doubles the cost of most checkpoints ahead. Machines that can
only run 32-bit code have not been sold for well over a decade. If the goal
were reached and 32-bit still mattered, it could be added then — but paying for
it through every checkpoint in between would slow all of them down.

Note that this is a separate question from *running* 32-bit programs. A 64-bit
x86 kernel can execute 32-bit user code, which is what a Windows 95 application
would need. That capability is not affected by this decision.

Adding **riscv64** costs almost nothing by comparison, because it is 64-bit and
the abstraction already exists. It happens when there is a machine to run it on.

## Running old and foreign applications

The long-term ambition — Windows 95-era applications, current Windows
applications, Linux applications, macOS applications — is real but it is
phase 3 and later, and it is worth writing down now *where* it attaches,
because that shapes decisions taken much earlier.

An application binary needs three things from a kernel, and each is a separate
problem:

1. **An instruction set it can execute.** A Win95 program is 32-bit x86 code.
   Running it on an aarch64 machine means emulating or translating those
   instructions — the same problem Apple solved twice, with Rosetta. This is
   independent of the kernel and belongs in userspace.
2. **A loader that understands its file format** — PE for Windows, ELF for
   Linux, Mach-O for macOS.
3. **The system calls and libraries it expects.** This is nearly all of the
   work. Wine is twenty-five thousand hours of exactly this, which is why the
   roadmap says contribute to Wine rather than reimplement it.

What the kernel owes all three is the same small thing: the ability for a
process to be created with a *personality* — a loader and a system-call
handler chosen per binary rather than fixed for the whole system. Linux calls
this `binfmt`; Windows NT called them subsystems. It costs nothing to leave
room for at checkpoint 9, when the first system call is written, and it is
expensive to add afterwards.

So: no compatibility work now, one design constraint recorded now. The
constraint is that the system-call layer is a table selected per process, not
a switch statement compiled into the kernel.

## What userland is waiting on

The compositor side has a list, in the order that unblocks the most work.
Recorded here so the kernel's checkpoints can be checked against it:

| Wanted | Needs | Earliest |
|---|---|---|
| Headers to compile against, stubs included | nothing | any time |
| Device enumeration (Device Manager) | a device model | after checkpoint 6 |
| Block devices and partitions (real volumes, encryption) | drivers, interrupts | after checkpoint 8 |
| Display mode setting | a display driver | not blocked — wlroots can do it today |
| Power states (sleep, hibernate) | ACPI, or firmware calls | after checkpoint 7 |
| Packet filtering | a network stack | far out |
| Recovery before boot | the bootloader | checkpoint 3 |
| Users and permissions enforced beneath ReconOS | checkpoint 9 | checkpoint 9 |

The first row is the one worth acting on early, and it costs almost nothing:
an interface the userland can compile against, even entirely stubbed, lets both
halves proceed instead of taking turns.

## How this is tracked

The same four places as everything else — this file for the plan,
[CHANGELOG.md](CHANGELOG.md) for what shipped, [BUGS.md](BUGS.md) for faults,
GitHub Issues for what is open. Kernel bugs take `BG-` numbers from the same
sequence as the rest of the system, because a bug number is a place in the
project's history and the kernel is part of the same project.

The kernel is developed in a git worktree on the `kernel` branch, so that two
sessions can work the same repository without sharing a checkout.
