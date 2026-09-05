# The ReconOS kernel

This is phase 2 work, started early and deliberately kept apart from the
compositor above it. Today it boots on two architectures, says what machine it
is on, and stops. That is the whole of it, and the version number says so.

Everything the [roadmap](../docs/ROADMAP.md) lists as "Linux" today — processes,
memory, files, system calls, signals, drivers — is what this directory is
eventually for.

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

No memory allocator, no interrupt handling, no timer, no MMU management of its
own beyond the identity map the boot code needs, no processes, no system calls.
Each of those is a checkpoint in [docs/KERNEL.md](../docs/KERNEL.md), and each
gets written when the thing above it needs it — not before, because an
interface invented ahead of its first caller gets fixed in place before it is
understood. That rule already cost `recon_net` a socket API it did not need.
