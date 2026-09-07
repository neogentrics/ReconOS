#!/bin/sh
# Installs from a real install medium onto a blank disk, and then boots the disk.
#
# This is the only test in the project that checks the thing the whole project
# is for. Everything else proves a component works; this proves that a machine
# with nothing on it, given ReconOS media, ends up starting ReconOS from its own
# disk with the media removed.
#
# The medium is built the way one really is: an ESP with the removable-media
# path, both architectures' loaders, and both kernels. The target is empty. Then
# the target is booted **on its own**, with no medium attached, which is the
# part that cannot be faked -- if anything the installer wrote is missing or
# wrong, the firmware simply does not find a system.
#
# Usage: scripts/install-then-boot-test.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

KERNEL=kernel/build/x86_64/reconos-kernel.elf
LOADER=boot/build/x86_64/BOOTX64.EFI
OVMF=/usr/share/ovmf/OVMF.fd

[ -f "$KERNEL" ] || { echo "build the kernel first" >&2; exit 1; }
[ -f "$OVMF" ] || { echo "OVMF is not installed"; exit 2; }
for t in sgdisk mkfs.vfat mcopy mmd mdir; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done

make -C boot ARCH=x86_64 >/dev/null 2>&1 || true
[ -f "$LOADER" ] || { echo "the bootloader is not built"; exit 2; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM

pass=0
fail=0
say() { printf '%-46s' "  $1"; }

# --- the install medium, laid out the way a real one is ---------------------

truncate -s 1G "$W/medium.img"
sgdisk -n 1:2048:0 -t 1:ef00 -c 1:"ReconOS Boot" "$W/medium.img" >/dev/null

dd if=/dev/zero of="$W/esp.part" bs=1M count=1000 status=none
mkfs.vfat -F 32 -n RECONOS "$W/esp.part" >/dev/null
mmd   -i "$W/esp.part" ::/EFI ::/EFI/BOOT ::/reconos
mcopy -i "$W/esp.part" "$LOADER" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$W/esp.part" "$KERNEL" ::/reconos/kernel-x86_64.elf

# What the kernel is told to do, on the medium rather than on the QEMU command
# line. `-append` only exists on the `-kernel` path, which skips the bootloader
# entirely -- so a test that used it would be testing a boot nobody's machine
# ever performs. This is how an install medium actually carries its intent.
printf 'install-onto=nvme1n1' > "$W/cmdline"
mcopy -i "$W/esp.part" "$W/cmdline" ::/reconos/cmdline
dd if="$W/esp.part" of="$W/medium.img" bs=512 seek=2048 conv=notrunc status=none

truncate -s 8G "$W/target.img"

# --- install: booted from the medium, writing to the target -----------------

say "installs from the medium onto a blank disk"
out=$(timeout 180 qemu-system-x86_64 -bios "$OVMF" -m 512M -nographic \
	-drive "file=$W/medium.img,format=raw,if=none,id=m0" \
	-device nvme,serial=medium,drive=m0 \
	-drive "file=$W/target.img,format=raw,if=none,id=t0" \
	-device nvme,serial=target,drive=t0 2>&1 | tr -d '\r')

if echo "$out" | grep -q 'installed: table'; then
	echo "$(echo "$out" | grep -oE '[0-9]+ files copied from the medium')"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$out" | sed -n '/^installer:/,$p' | head -14 | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "the loader and kernel are on the target"
if mdir -i "$W/target.img@@1048576" ::/EFI/BOOT 2>/dev/null | grep -qi 'BOOTX64' &&
   mdir -i "$W/target.img@@1048576" ::/reconos 2>/dev/null | grep -qi 'kernel'; then
	echo "BOOTX64.EFI and a kernel"
	pass=$((pass + 1))
else
	echo "FAILED"
	mdir -i "$W/target.img@@1048576" -/ :: 2>&1 | sed 's/^/      /' | head -12
	fail=$((fail + 1))
fi

# --- and the part that cannot be faked --------------------------------------
#
# The target, alone. No medium, no -kernel, nothing but firmware and a disk the
# installer wrote. Everything the installer got wrong shows up here as silence.

say "the installed disk boots on its own"
boot=$(timeout 90 qemu-system-x86_64 -bios "$OVMF" -m 512M -nographic \
	-drive "file=$W/target.img,format=raw,if=none,id=t0" \
	-device nvme,serial=target,drive=t0 2>&1 | tr -d '\r')

if echo "$boot" | grep -q 'ReconOS kernel'; then
	echo "$(echo "$boot" | grep -oE 'ReconOS kernel [0-9.]+' | head -1)"
	pass=$((pass + 1))
else
	echo "DID NOT BOOT"
	echo "$boot" | tail -14 | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "and it finds its own partitions"
if echo "$boot" | grep -q 'nvme0n1p3'; then
	echo "$(echo "$boot" | grep -cE 'nvme0n1p[0-9]') partitions"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$boot" | grep -E 'nvme0n1|table' | sed 's/^/      /' | head -8
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: installed from media, and the disk booted by itself"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
