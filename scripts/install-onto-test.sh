#!/bin/sh
# Installs onto disks that already have data, and checks what survived.
#
# The planner is tested separately and writes nothing. This is the other half:
# it writes a partition table, makes a FAT32 filesystem and lays ReconFS on two
# volumes -- on a disk that, in the case that matters, already has somebody
# else's operating system on it.
#
# The claim being tested is not "it installed". It is:
#
#   **every partition that was already there is byte for byte what it was**,
#
# and that is checked by hashing the neighbour's contents before and after, and
# by asking sgdisk -- which shares no code with this -- whether the table it
# ends up with is one it would have written.
#
# Two layouts:
#
#   blank      no table at all. Everything is created.
#   beside     an ESP with a filesystem in it, a data partition with known
#              contents, and free space after. The ESP must be *reused*, the
#              data partition must be untouched, and ReconOS must fit in the
#              gap.
#
# Usage: scripts/install-onto-test.sh [x86_64|aarch64]
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
for t in sgdisk mkfs.vfat mcopy mdir python3; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM

install_onto() {
	timeout 90 $QEMU -m 512M -nographic -no-reboot \
		-kernel "$KERNEL" -append "install-onto=nvme0n1" \
		-drive "file=$1,format=raw,if=none,id=d0" \
		-device nvme,serial=i0,drive=d0 2>&1 |
		tr -d '\r' | sed -n '/^installer:/,/^$/p'
}

pass=0
fail=0

say() { printf '%-46s' "  $1"; }

# --- a blank disk -----------------------------------------------------------

truncate -s 16G "$W/blank.img"

say "installs onto a blank disk"
out=$(install_onto "$W/blank.img")
if echo "$out" | grep -q 'installed: table'; then
	echo "table, EFI filesystem, two volumes"
	pass=$((pass + 1))
else
	echo "FAILED"; echo "$out" | sed 's/^/      /'; fail=$((fail + 1))
fi

# The count is *required*, not reported. The first version printed it, and it
# printed 2 when the plan had three partitions -- the writer was leaving the
# programs volume out of the table, so the executor formatted a filesystem that
# no entry pointed at. A number in the output is not a check.
# Named, not counted by position.
#
# This asked for three and got four the moment a BIOS boot partition was added,
# which is a test failing for a change that was correct. Checking that each
# expected partition is *there* says the same thing about the writer leaving one
# out -- which is what this exists for -- without breaking every time the layout
# legitimately gains one.
say "sgdisk agrees, and every partition is there"
layout=$(sgdisk -p "$W/blank.img" 2>/dev/null)
missing=
for want in "BIOS boot" "ReconOS Boot" "ReconOS System" "ReconOS Programs"; do
	echo "$layout" | grep -q "$want" || missing="$missing '$want'"
done

if sgdisk -v "$W/blank.img" 2>&1 | grep -q 'No problems found' &&
   [ -z "$missing" ]; then
	echo "$(echo "$layout" | grep -c '^ *[0-9]') partitions, all named"
	pass=$((pass + 1))
else
	echo "FAILED -- missing:${missing:- none, but sgdisk objects}"
	echo "$layout" | sed 's/^/      /' | tail -8
	sgdisk -v "$W/blank.img" 2>&1 | sed 's/^/      /' | head -6
	fail=$((fail + 1))
fi

# Where the EFI partition is, asked of the disk. It was the first partition
# until it was not.
esp_at=$(( $(echo "$layout" | awk '$6 == "EF00" { print $2; exit }') * 512 ))

say "the EFI partition is a FAT32 mtools can read"
if mdir -i "$W/blank.img@@$esp_at" :: >/dev/null 2>&1; then
	echo "mounted, at block $(( esp_at / 512 ))"
	pass=$((pass + 1))
else
	echo "FAILED"
	mdir -i "$W/blank.img@@$esp_at" :: 2>&1 | sed 's/^/      /' | head -4
	fail=$((fail + 1))
fi

# --- beside somebody else ---------------------------------------------------

truncate -s 16G "$W/beside.img"
sgdisk -n 1:2048:+100M -t 1:ef00 -c 1:"EFI system partition" "$W/beside.img" >/dev/null
sgdisk -n 2:0:+4G      -t 2:0700 -c 2:"Somebody else"        "$W/beside.img" >/dev/null

# A real filesystem in the ESP, so it can be reused rather than refused.
dd if=/dev/zero of="$W/esp.part" bs=1M count=100 status=none
mkfs.vfat -F 32 -n SYSTEM "$W/esp.part" >/dev/null
printf 'this is somebody else bootloader' > "$W/theirs"
mcopy -i "$W/esp.part" "$W/theirs" ::/theirs.efi
dd if="$W/esp.part" of="$W/beside.img" bs=512 seek=2048 conv=notrunc status=none

# Known contents in the neighbour, so "untouched" is provable rather than
# assumed. Written across the whole partition, not just its start: a bug that
# lands in the middle of somebody's data would pass a check that only looked at
# the first sector.
python3 - "$W/beside.img" <<'PY'
import sys
start = 206848 * 512          # partition 2, from sgdisk
size = 4 * 1024 * 1024 * 1024
f = open(sys.argv[1], "r+b")
for off in (0, size // 2, size - 65536):
    f.seek(start + off)
    f.write(bytes((i * 37 + off) & 0xFF for i in range(65536)))
f.close()
PY

hash_neighbour() {
	python3 - "$1" <<'PY'
import hashlib, sys
start = 206848 * 512
size = 4 * 1024 * 1024 * 1024
h = hashlib.sha256()
f = open(sys.argv[1], "rb")
for off in (0, size // 2, size - 65536):
    f.seek(start + off)
    h.update(f.read(65536))
print(h.hexdigest())
PY
}

before=$(hash_neighbour "$W/beside.img")
before_table=$(sgdisk -p "$W/beside.img" 2>/dev/null | grep '^ *[12] ')

say "installs beside an existing system"
out=$(install_onto "$W/beside.img")
if echo "$out" | grep -q 'installed: table'; then
	echo "$(echo "$out" | grep -oE 'the one already there, kept' || echo 'installed')"
	pass=$((pass + 1))
else
	echo "FAILED"; echo "$out" | sed 's/^/      /'; fail=$((fail + 1))
fi

say "it reused the EFI partition, not a second"
if echo "$out" | grep -q 'the one already there, kept' &&
   [ "$(sgdisk -p "$W/beside.img" 2>/dev/null | grep -ci 'EF00')" = "1" ]; then
	echo "one EFI partition, still theirs"
	pass=$((pass + 1))
else
	echo "FAILED"
	sgdisk -p "$W/beside.img" 2>&1 | sed 's/^/      /' | tail -8
	fail=$((fail + 1))
fi

# 1 MiB is correct here and stays hard-coded on purpose: this test *made* that
# partition, four lines of sgdisk above, and the installer reuses it where it
# is rather than moving it. A constant describing the harness's own setup is a
# fact; a constant describing where the installer puts things is a claim about
# a program somebody is still editing, and three of those went stale today.
say "their bootloader is still on the EFI partition"
if mdir -i "$W/beside.img@@1048576" :: 2>/dev/null | grep -qi 'theirs'; then
	echo "theirs.efi intact"
	pass=$((pass + 1))
else
	echo "FAILED"
	mdir -i "$W/beside.img@@1048576" :: 2>&1 | sed 's/^/      /' | head -6
	fail=$((fail + 1))
fi

say "the neighbour's data is byte for byte"
after=$(hash_neighbour "$W/beside.img")
if [ "$before" = "$after" ]; then
	echo "unchanged"
	pass=$((pass + 1))
else
	echo "CHANGED -- $before -> $after"
	fail=$((fail + 1))
fi

say "their table entries are unchanged"
after_table=$(sgdisk -p "$W/beside.img" 2>/dev/null | grep '^ *[12] ')
if [ "$before_table" = "$after_table" ]; then
	echo "entries 1 and 2 identical"
	pass=$((pass + 1))
else
	echo "CHANGED"
	echo "  before:"; echo "$before_table" | sed 's/^/      /'
	echo "  after:";  echo "$after_table"  | sed 's/^/      /'
	fail=$((fail + 1))
fi

# Their two are still there and ours were added. Counted as "more than before"
# rather than as an exact number, because the exact number is a property of our
# layout and this assertion is about theirs surviving.
say "sgdisk agrees, and their partitions are still there"
bparts=$(sgdisk -p "$W/beside.img" 2>/dev/null | grep -c '^ *[0-9]')
if sgdisk -v "$W/beside.img" 2>&1 | grep -q 'No problems found' &&
   [ "$bparts" -ge 4 ] &&
   sgdisk -p "$W/beside.img" 2>/dev/null | grep -q 'ReconOS System'; then
	echo "their two, plus ours: $bparts in all"
	pass=$((pass + 1))
else
	echo "FAILED"; sgdisk -v "$W/beside.img" 2>&1 | sed 's/^/      /' | head -8
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: installed, and the disk's existing contents survived"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
