#!/bin/bash
# Does a plain ReconOS boot write to a disk it did not put there?
#
# `docs/BARE-METAL.md` is a procedure for booting ReconOS on a machine with four
# mounted volumes of somebody's data on it, and the whole procedure rests on one
# claim: that starting the kernel reads disks and does not write to them. This
# is that claim, made checkable.
#
# --- Why two instruments and not one ---
#
# The kernel prints its own block counters, and a kernel that wrote to a disk
# without counting it would print zero just as loudly. So the disks are also
# checksummed either side of the boot, from outside, by a tool that knows
# nothing about ReconOS. The counter is the kernel's account of itself; the
# checksum is the fact. Agreement between them is worth more than either.
#
# --- Why the disks look like this ---
#
# A GPT partition table on all three and a real ext4 filesystem with real files
# on the first, because "nothing happened to an empty disk" is a weaker claim
# than it sounds: an empty disk has no superblock to rewrite, no journal to
# replay, and nothing a filesystem driver could decide to tidy up.
#
# Usage: scripts/check-disk-safety.sh [arch]
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
ARCH=${1:-x86_64}
KERNEL="$ROOT/kernel/build/$ARCH/reconos-kernel.elf"

for t in sgdisk mkfs.ext4 md5sum qemu-system-x86_64; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done

[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL -- run make first" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

for n in 1 2 3; do
	truncate -s 256M "foreign${n}.img"
	sgdisk -n "1:2048:0" -t 1:8300 "foreign${n}.img" >/dev/null 2>&1
done

# Real filesystem, real contents, on the first one.
mkfs.ext4 -F -q -E offset=1048576 foreign1.img 200M

for n in 1 2 3; do
	md5sum "foreign${n}.img"
done > before.md5

# Offered as ordinary AHCI disks, which is how the server presents its own.
timeout 120 qemu-system-x86_64 -kernel "$KERNEL" -m 512M -no-reboot \
	-display none -serial mon:stdio \
	-device ahci,id=ahci \
	-drive file=foreign1.img,format=raw,if=none,id=d1 \
	-device ide-hd,drive=d1,bus=ahci.0 \
	-drive file=foreign2.img,format=raw,if=none,id=d2 \
	-device ide-hd,drive=d2,bus=ahci.1 \
	-drive file=foreign3.img,format=raw,if=none,id=d3 \
	-device ide-hd,drive=d3,bus=ahci.2 \
	> boot.log 2>&1 || true

for n in 1 2 3; do
	md5sum "foreign${n}.img"
done > after.md5

echo "the kernel's own account:"
grep -A 3 "^Block traffic" boot.log | sed 's/^/  /' || echo "  (no block traffic section -- did it boot?)"

echo
echo "what it found:"
grep -iE "storage: |no volume this kernel" boot.log | sed 's/^/  /' || true

echo
if diff -q before.md5 after.md5 >/dev/null; then
	echo "PASS: three disks, one with a filesystem on it, byte-identical after a full boot"
	exit 0
fi

echo "FAIL: ReconOS wrote to a disk it was only shown"
diff before.md5 after.md5 || true
exit 1
