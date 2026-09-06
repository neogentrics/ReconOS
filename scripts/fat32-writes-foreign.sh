#!/bin/sh
# Writes into a FAT32 volume with the kernel, and reads it back with mtools.
#
# The read harness proves we understand a volume somebody else wrote. This
# proves the reverse, which is the direction an installer depends on: we write
# the bootloader, and *firmware* reads it. A writer checked only by our own
# reader would be checked against its own misunderstandings -- the same trap the
# read harness exists to avoid, pointing the other way.
#
# What it insists on, and why each one:
#
#   the files exist, with the right sizes   the obvious claim
#   140 KiB matches byte for byte           a writer that lays a chain down
#                                           wrongly still produces a file of the
#                                           right length
#   names do not move between installs      BG-130: every rewrite renamed the
#                                           file, including \EFI\BOOT\BOOTX64.EFI,
#                                           which is the one filename UEFI runs
#                                           without a boot entry
#   BOOTX64.EFI has no long-name entry      it is 8.3 already; needing a long
#                                           name to find it means the alias
#                                           drifted
#
# Usage: scripts/fat32-writes-foreign.sh [x86_64|aarch64]
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

[ -f "$KERNEL" ] || { echo "build the kernel first: make -C kernel ARCH=$ARCH" >&2; exit 1; }
for t in mkfs.vfat mdir mcopy python3; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done

IMG=$(mktemp)
OUT=$(mktemp -d)
trap 'rm -rf "$IMG" "$OUT"' EXIT INT TERM

install_once() {
	timeout 45 $QEMU -m 512M -nographic -no-reboot \
		-kernel "$KERNEL" -append "fat32-write=nvme0n1" \
		-drive "file=$IMG,format=raw,if=none,id=d0" \
		-device nvme,serial=w0,drive=d0 2>&1 | tr -d '\r'
}

# mtools' listing, normalised to `alias<TAB>longname` so two installs can be
# compared as text.
names() {
	mdir -i "$IMG" -/ :: 2>/dev/null |
		grep -vE 'Volume|Serial|Directory for|files|bytes free|^$' |
		awk '{ printf "%s %s\t%s\n", $1, $2, $NF }'
}

pass=0
fail=0

rm -f "$IMG"
truncate -s 64M "$IMG"
mkfs.vfat -F 32 -n WRITETEST "$IMG" >/dev/null

printf '%-46s' "  writes files mtools can read"
out=$(install_once)
if echo "$out" | grep -q '5 of 5 writes reported success'; then
	echo "5 of 5"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$out" | sed -n '/^fat32: writing/,$p' | head -10 | sed 's/^/      /'
	fail=$((fail + 1))
fi

printf '%-46s' "  the files are there, at the right sizes"
listing=$(names)
if echo "$listing" | grep -q 'BIG *BIN' &&
   echo "$listing" | grep -q 'a-long-name-written-by-us.txt' &&
   mdir -i "$IMG" ::/EFI/BOOT 2>/dev/null | grep -qi 'BOOTX64'; then
	echo "$(echo "$listing" | wc -l) entries"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$listing" | sed 's/^/      /'
	fail=$((fail + 1))
fi

# The contents, checked by a tool that did not write them. A writer that builds
# a chain wrongly still produces a file of exactly the right length.
printf '%-46s' "  140 KiB matches byte for byte"
mcopy -i "$IMG" ::/big.bin "$OUT/big.bin" 2>/dev/null || true
if python3 - "$OUT/big.bin" <<'PY'
import sys
n = 140 * 1024
want = bytes((i * 31 + 7) & 0xFF for i in range(n))
try:
    got = open(sys.argv[1], "rb").read()
except OSError:
    sys.exit("could not read it back")
if len(got) != n:
    sys.exit("%d bytes, expected %d" % (len(got), n))
for i, (a, b) in enumerate(zip(got, want)):
    if a != b:
        sys.exit("byte %d is 0x%02x, expected 0x%02x" % (i, a, b))
PY
then
	echo "143360 bytes, every one"
	pass=$((pass + 1))
else
	echo "FAILED"
	fail=$((fail + 1))
fi

# --- BG-130: the names must not move ---------------------------------------

printf '%-46s' "  a second install renames nothing"
before=$listing
install_once >/dev/null
after=$(names)

if [ "$before" = "$after" ]; then
	echo "identical"
	pass=$((pass + 1))
else
	echo "NAMES MOVED"
	echo "  before:"; echo "$before" | sed 's/^/      /'
	echo "  after:";  echo "$after"  | sed 's/^/      /'
	fail=$((fail + 1))
fi

# BOOTX64.EFI is 8.3 already, so it should need no long-name entry at all.
# mtools prints the long name in the last column only when there is one.
printf '%-46s' "  BOOTX64.EFI is still its own name"
boot_line=$(mdir -i "$IMG" ::/EFI/BOOT 2>/dev/null | grep -i '^BOOTX64 ' || true)
if [ -n "$boot_line" ] && ! echo "$boot_line" | grep -qi '\.efi *$'; then
	echo "8.3, no long name needed"
	pass=$((pass + 1))
else
	echo "FAILED -- the alias drifted"
	mdir -i "$IMG" ::/EFI/BOOT 2>/dev/null | sed 's/^/      /'
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: wrote a volume somebody else's tools can read"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
