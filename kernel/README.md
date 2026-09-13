# The ReconOS kernel

This is phase 2 work, started early and deliberately kept apart from the
compositor above it. It is now the larger half of the project.

**Version 0.2.1.** The middle digit moved on 12 September 2026 because every row
in sections 1.1 to 1.9 of the blueprint audit is Built -- a fact about those
tables rather than a judgement about how large the change felt. What is left in
the audit is section 2, the bootloader, which was never in that gate.

It boots on **x86_64 and aarch64**, under **BIOS and UEFI**, from its own
bootloader and from GRUB, and on its own filesystem. Above that it has physical
and virtual memory, per-process address spaces, threads and a scheduler
with per-processor timers and real preemption -- built to hold 256 and
deliberately untested above 32, which is what `smp.h` says about itself --
interrupts and timers, system calls, a VFS
with ReconFS, FAT32, ramfs, devfs, procfs and ext2 behind one interface,
signals, block and USB storage, input, and an installer that can put itself
on a disk beside an operating system that is already there.

**What it still does not have is a network.** That is the one large thing left,
and it is deliberately last: a machine with no network still boots, installs
and runs. A machine with no input does not.

Everything the [roadmap](../docs/ROADMAP.md) still lists as "Linux" is what this
directory is for. The list of what is built, line by line against the original
architecture checklist, is [docs/KERNEL.md](../docs/KERNEL.md), and the version
history is [docs/KERNEL-CHANGELOG.md](../docs/KERNEL-CHANGELOG.md).

**The numbers this README quotes are checked by a verification run**, not by
being written down: `scripts/verify-kernel.sh` boots the kernel twenty-five
times on every change and runs the same self-tests on each. A claim here that stops being true is
supposed to be caught there, and this file is evidence that it is not caught
automatically — prose is not a test.

## Building

Needs a cross toolchain and QEMU:

```bash
sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu qemu-system-x86 qemu-system-arm
```

x86_64 uses the host's own gcc — a Linux x86-64 compiler produces freestanding
64-bit objects perfectly well; it is the linking, not the compiling, that
differs.

```bash
make ARCH=x86_64          # or aarch64
make all-arches           # both, which is how portability is actually checked
make run ARCH=aarch64     # boot under QEMU, serial on this terminal
make check-portable       # fail if core/ has learned about a machine
```

`make run` leaves you inside QEMU; `Ctrl-A` then `x` exits.

## How the tree is laid out

```
include/recon/kernel/    the contract, arch.h chief among it
core/                    written once, runs everywhere
arch/x86_64/             written for one machine
arch/aarch64/
```

**`core/` may not know what machine it is on.** No inline assembly, no
hardware addresses, no `#ifdef __x86_64__`. When portable code needs something
machine-specific, the answer is a new function in `arch.h` and an
implementation in each `arch/` directory — never a conditional in `core/`.
`make check-portable` enforces this by grep, and it runs before every build.

This costs almost nothing today and is the entire reason a third architecture
is a directory rather than a rewrite. Adding `riscv64` means four files —
`boot.S`, `linker.ld`, `arch.c`, and a stanza in the Makefile — and no change
to a single line under `core/`.

## Which architecture, and when

Worth being precise about, because it is easy to picture wrongly: **the kernel
does not detect its architecture.** It cannot. By the time any kernel code
runs, the machine has already fetched and decoded instructions that only exist
on one architecture. The firmware chose which binary to load, and it chose
correctly, because a boot ISO carries one image per architecture and UEFI
picks by filename.

What *is* discovered at run time is which CPU within that architecture, and
what it can do — `arch_cpu_identify()` is the first of that, reading the CPUID
brand string on x86_64 and `MIDR_EL1` on aarch64. Feature detection (which
vector extensions, how many address bits, which page sizes) comes later, and
that is the part that genuinely varies machine to machine.

So "install on whatever it finds" is an installer and boot-media problem, not
a kernel one. The kernel's job is to exist for each architecture, which is
what this layout is for.

## The two boot paths on x86_64

x86 is the awkward one. A CPU there starts in 32-bit protected mode and the
kernel has to build page tables and switch itself into 64-bit long mode before
any C runs — `arch/x86_64/boot.S` is mostly that. aarch64 needs none of it,
which is why its boot file is a quarter the length.

The image carries two entry points into the same trampoline:

- a **Multiboot2** header, which is how GRUB loads it, and therefore how it
  will boot from a disc or from real hardware;
- a **PVH ELF note**, which QEMU's `-kernel` reads directly, so a build can be
  booted in about a second without producing a disc image first.

Both arrive in 32-bit protected mode with a flat address space, so the only
difference is which register holds the boot information.

## What is deliberately absent

Every item this section used to list — the allocator, interrupts, the timer,
MMU management, processes, system calls — is built. What remains absent, and
why:

- **The whole network stack.** No NIC driver, no packet buffers, no Ethernet,
  ARP, IP, ICMP, UDP or TCP, and no socket layer. The largest single body of
  work left, and last on purpose.
- **EHCI, and USB hot-plug.** xHCI works and hubs are enumerated; older
  controllers are not, and ports are read once at boot.
- **A driver for legacy IDE.** Which is why a kernel that boots over BIOS from
  an IDE disk cannot then read it (BG-192). It belongs in `arch/x86_64/`
  rather than `core/`, because compatibility-mode IDE is at fixed I/O ports and
  `in` and `out` are x86 instructions — the reason the other three block
  drivers can be portable and this one cannot.
- **ext4.** `core/ext2.c` reads ext2 and refuses EXTENTS, RECOVER and 64BIT
  *by name*. Reading a filesystem through the wrong assumption about where its
  blocks are is worse than refusing to mount it.
- **A dentry cache, and `mount()`.** Two opens of one path produce two
  independent files today.

Each is a checkpoint in [docs/KERNEL.md](../docs/KERNEL.md), and each gets
written when the thing above it needs it — not before, because an interface
invented ahead of its first caller gets fixed in place before it is understood.
That rule already cost `recon_net` a socket API it did not need.
