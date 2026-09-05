# reconboot — the ReconOS bootloader

The first thing that runs. It is here because the project's claim is that
ReconOS was built rather than assembled, and a system that needs somebody
else's bootloader to start has delegated the most fundamental thing it does.

Today it boots the kernel on x86_64 and aarch64 under UEFI. The BIOS loader,
for machines that have no UEFI at all, is checkpoint 16.

## Building and running

```bash
make ARCH=x86_64          # BOOTX64.EFI
make ARCH=aarch64         # BOOTAA64.EFI
make esp                  # a FAT image with the loader and the kernel on it
make run                  # boot that image under real UEFI firmware in QEMU
```

Needs `clang`, `lld`, `mtools`, `dosfstools`, and OVMF or AAVMF for QEMU.

## Why clang, when the kernel uses gcc

A UEFI application is a **PE binary**, not an ELF — UEFI inherited its
executable format from Windows. `clang` can target `x86_64-unknown-windows` and
`aarch64-unknown-windows` from one installation; gcc would need a separate
mingw cross-compiler per architecture, and there is no packaged aarch64 one.

There is no gnu-efi here. [include/efi.h](include/efi.h) declares what the
loader calls, written from the specification, for the same reason the kernel has
no libc.

**The one thing to be careful about in that file:** UEFI tables are an ABI. The
firmware fills in a structure and we index into it. A missing field does not
fail to compile — it shifts every field after it, and the call lands on the
wrong function pointer. So every table lists its full set of members in order,
with unused ones typed as `void *` placeholders. Deleting a placeholder to tidy
up would be a subtle and total disaster.

## What it does

1. Says hello on whatever console the firmware provides.
2. Finds a framebuffer through the Graphics Output Protocol, if there is one.
3. Reads the ACPI root pointer and the device tree out of the configuration
   table.
4. Opens the volume **it was itself loaded from** — asked, not searched, so a
   machine with several installations boots the one it was told to.
5. Reads `\reconos\kernel.elf`, claims the pages the ELF asks for, places each
   loadable segment, and zeroes the rest.
6. Collects the memory map, leaves UEFI, and jumps.

## The two things that are genuinely delicate

**Exiting.** `ExitBootServices` only succeeds if the map key it is given still
matches the current state of the memory map, and anything that allocates —
including `GetMemoryMap` itself — invalidates it. So the sequence is: allocate
everything first, get the map, exit immediately with the key it returned, with
nothing in between. If it fails anyway, get the map again and retry, because
real firmware does change things underneath you. After it returns there is no
console, so the memory map is *translated* before the attempt rather than after.

**The calling convention.** The loader is compiled for the Microsoft ABI,
because UEFI requires it. The kernel is compiled by gcc for System V. On x86_64
those disagree about where the first argument goes — RCX against RDI — so the
handoff is declared System V, because the kernel is the thing that has to live
with it. On aarch64 both pass it in `x0` and there is nothing to reconcile.

## The handoff

[include/reconboot.h](include/reconboot.h) — one file, included by two
compilers producing two different ABIs, so every field is explicitly sized and
there is not a function pointer or a bitfield in it.

The kernel is entered at `reconboot_entry`, **not** at its ELF entry point:
that belongs to the 32-bit trampoline the other boot protocols arrive through,
and UEFI has already put the machine in 64-bit mode. The kernel carries a small
header naming its 64-bit entry, and the loader finds it by scanning the loaded
image for the magic. That is what keeps one kernel binary bootable by every
protocol it supports — which is the property that makes "BIOS, UEFI, or both" a
build with one output rather than three.

## Where the loader goes on the disk

`EFI/BOOT/BOOTX64.EFI`, or `BOOTAA64.EFI` on ARM. That is the **removable media
path**: the one filename firmware will run without a boot entry having been
registered in NVRAM. It is what makes a USB stick bootable on a machine that
has never seen it, and therefore what install media uses.
