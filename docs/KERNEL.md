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

`0.0.1`. Following the same rule as the rest of the project: the number says
what works. What works is that it boots on two architectures and prints who it
is.

## Checkpoints

Each of these is a thing the kernel can *do* afterwards that it could not do
before. None is a refactor.

| # | Checkpoint | Status |
|---|---|---|
| 0 | Builds and boots on x86_64 and aarch64, prints its identity over serial | **Done** |
| 1 | Knows what memory exists — the firmware's memory map, parsed and printed | |
| 2 | A physical page allocator | |
| 3 | Its own page tables; the kernel moves to the higher half | |
| 4 | A kernel heap | |
| 5 | Interrupts and exceptions, and a fault that reports itself instead of resetting the machine | |
| 6 | A timer, and a tick | |
| 7 | Threads and a scheduler | |
| 8 | User mode, and the first system call | |
| 9 | Boots on real hardware, both architectures | |

Nine, like v0.1.0 had nine, and for the same reason: a list short enough to
hold in your head is a list you finish.

### Checkpoint 0 — done

Two architectures, one portable core, and a rule enforced by the build that
they stay separated. `arch.h` is six functions long. It is meant to be.

The measurable outcome: `make all-arches` produces two kernels from the same
`core/`, and `make check-portable` fails if any machine-specific code has crept
into it.

### Why memory comes first

Because everything after it needs to allocate, and because it is the checkpoint
where the two architectures first genuinely diverge — x86_64 learns its memory
map from the bootloader's Multiboot2 tags or UEFI, aarch64 from the device tree
pointer that `boot.S` has already saved but nothing yet reads. That divergence
is the first real test of whether `arch.h` was drawn in the right place.

## Architecture support

| Architecture | Status | Notes |
|---|---|---|
| x86_64 | Boots | Multiboot2 for GRUB, PVH note for direct QEMU boot |
| aarch64 | Boots | QEMU `virt`; PL011 UART, device tree pointer saved |
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
room for at checkpoint 8, when the first system call is written, and it is
expensive to add afterwards.

So: no compatibility work now, one design constraint recorded now. The
constraint is that the system-call layer is a table selected per process, not
a switch statement compiled into the kernel.

## How this is tracked

The same four places as everything else — this file for the plan,
[CHANGELOG.md](CHANGELOG.md) for what shipped, [BUGS.md](BUGS.md) for faults,
GitHub Issues for what is open. Kernel bugs take `BG-` numbers from the same
sequence as the rest of the system, because a bug number is a place in the
project's history and the kernel is part of the same project.
