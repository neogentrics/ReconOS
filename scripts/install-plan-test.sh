#!/bin/sh
# Plans an install against disks laid out like real ones, and checks what it
# decided. Writes nothing to any of them -- that is the whole point of the plan
# being a separate half.
#
# An installer is the one piece of this system that runs once, on a stranger's
# machine, with their data on it. There is no second attempt. So the deciding is
# separated from the doing, and the deciding is tested exhaustively against
# layouts built by sgdisk -- which shares no code with the kernel, and which is
# what actually partitioned the disks this will meet.
#
# The layouts, and what each one is for:
#
#   blank        no table at all -- the easy case, and the one where taking the
#                whole disk is safe *because the reader looked and found nothing*
#   windows      an ESP with room, plus a big NTFS-shaped partition and free
#                space after it. The case the whole project exists for.
#   full         partitions with no gap left. Must refuse, not squeeze.
#   tiny-gap     a gap far too small for a system partition. Must refuse.
#   no-esp       a GPT disk with data and no EFI partition, with room. Must make
#                one rather than give up.
#   esp-full     an ESP with no free space in it. Must refuse and say which,
#                rather than write a bootloader nobody can boot.
#   torn         a GPT header with a nonsense entry count. Must refuse: a disk
#                whose layout cannot be read is the one disk never to write to.
#
# Usage: scripts/install-plan-test.sh [x86_64|aarch64]
set -eu

ARCH=${1:-x86_64}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

if [ "$ARCH" = aarch64 ]; then
	QEMU="qemu-system-aarch64 -M virt -cpu cortex-a72"
	KERNEL=kernel/build/aarch64/reconos-kernel.img
else
	QEMU=qemu-system-x86_64
	KERNEL=kernel/build/x86_64/reconos-kernel.elf
fi

# Built, not merely looked for. A script that only checks a binary exists
# will happily test one compiled before the change it is meant to prove --
# which is how a security check came to be silently absent (BG-133).
make -C kernel ARCH="$ARCH" >/dev/null 2>&1 || true
[ -f "$KERNEL" ] || { echo "the kernel did not build" >&2; exit 1; }
for t in sgdisk mkfs.vfat mcopy python3; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

plan_for() {
	timeout 45 $QEMU -m 512M -nographic -no-reboot \
		-kernel "$KERNEL" -append "install-plan" \
		-drive "file=$1,format=raw,if=none,id=d0" \
		-device nvme,serial=p0,drive=d0 2>&1 |
		tr -d '\r' | sed -n '/^installer:/,/nothing was written/p'
}

pass=0
fail=0

check() {
	name=$1
	img=$2
	want=$3

	printf '%-30s' "  $name"
	out=$(plan_for "$img")

	if echo "$out" | grep -q "$want"; then
		echo "$(echo "$out" | grep -oE '(yes|no)$|^      [a-z].*' | head -1 | cut -c1-42)"
		pass=$((pass + 1))
	else
		echo "NOT AS PLANNED"
		echo "$out" | sed 's/^/      /'
		echo "      wanted to see: $want"
		fail=$((fail + 1))
	fi
}

# --- the layouts ------------------------------------------------------------

mk() { rm -f "$WORK/$1.img"; truncate -s "$2" "$WORK/$1.img"; }

mk blank 8G

mk windows 8G
sgdisk -n 1:2048:+100M -t 1:ef00 "$WORK/windows.img" >/dev/null
sgdisk -n 2:0:+3G      -t 2:0700 "$WORK/windows.img" >/dev/null
# A real ESP, with a real FAT32 filesystem in it and room to spare.
dd if=/dev/zero of="$WORK/esp.part" bs=1M count=100 status=none
mkfs.vfat -F 32 -n SYSTEM "$WORK/esp.part" >/dev/null 2>&1 || true
dd if="$WORK/esp.part" of="$WORK/windows.img" bs=512 seek=2048 conv=notrunc status=none

mk full 4G
sgdisk -n 1:2048:+100M -t 1:ef00 "$WORK/full.img" >/dev/null
sgdisk -n 2:0:0        -t 2:0700 "$WORK/full.img" >/dev/null
dd if="$WORK/esp.part" of="$WORK/full.img" bs=512 seek=2048 conv=notrunc status=none

mk tiny-gap 4G
sgdisk -n 1:2048:+100M   -t 1:ef00 "$WORK/tiny-gap.img" >/dev/null
sgdisk -n 2:206848:+3G   -t 2:0700 "$WORK/tiny-gap.img" >/dev/null
dd if="$WORK/esp.part" of="$WORK/tiny-gap.img" bs=512 seek=2048 conv=notrunc status=none

mk no-esp 8G
sgdisk -n 1:2048:+2G -t 1:0700 "$WORK/no-esp.img" >/dev/null

mk esp-full 8G
sgdisk -n 1:2048:+100M -t 1:ef00 "$WORK/esp-full.img" >/dev/null
sgdisk -n 2:0:+2G      -t 2:0700 "$WORK/esp-full.img" >/dev/null
# The same ESP, filled until nothing useful fits.
cp "$WORK/esp.part" "$WORK/espfull.part"
python3 -c "
import sys
open(sys.argv[1],'wb').write(bytes((i*7)&0xFF for i in range(95*1024*1024)))
" "$WORK/filler"
mcopy -i "$WORK/espfull.part" "$WORK/filler" ::/filler.bin 2>/dev/null || true
dd if="$WORK/espfull.part" of="$WORK/esp-full.img" bs=512 seek=2048 conv=notrunc status=none

cp "$WORK/windows.img" "$WORK/torn.img"
# A GPT header whose entry count is impossible. Nothing else touched, so the
# disk still looks entirely plausible until the number is read.
python3 -c "
import struct, sys
f = open(sys.argv[1], 'r+b')
f.seek(512 + 80)
f.write(struct.pack('<I', 0xFFFFFFFF))
f.close()
" "$WORK/torn.img"

# --- what each must decide --------------------------------------------------

check "a blank disk"          "$WORK/blank.img"    "the disk is blank"
check "beside Windows"        "$WORK/windows.img"  "is reused"
check "a disk with no gaps"   "$WORK/full.img"     "not enough unpartitioned space"
check "a gap too small"       "$WORK/tiny-gap.img" "not enough unpartitioned space"
check "GPT with no EFI part"  "$WORK/no-esp.img"   "a new EFI partition"
check "an ESP with no room"   "$WORK/esp-full.img" "no room"
check "a table that is torn"  "$WORK/torn.img"     "cannot be read"

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass layouts planned or refused correctly, nothing written"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
