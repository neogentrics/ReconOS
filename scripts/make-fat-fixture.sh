#!/bin/sh
# Builds a FAT32 volume for the kernel's reader to be tested against.
#
# Nothing here shares a line with the kernel. mkfs.vfat lays out the volume and
# mcopy fills it, which is the whole point: a reader tested against a volume our
# own code wrote would be tested against its own misunderstandings. This is the
# same argument the partition-table fixtures make with sgdisk and sfdisk.
#
# What goes in, and why each one:
#
#   pattern.bin                   140 KiB of a computed pattern. Large enough to
#                                 span several clusters at any sane cluster
#                                 size, so the reader has to follow a chain --
#                                 a file that fits in one cluster never does,
#                                 and following the chain is the part that goes
#                                 wrong. The kernel recomputes the pattern
#                                 rather than storing it, so the two cannot
#                                 drift into agreeing.
#
#   a-long-name-for-testing.txt   A name that cannot be expressed in 8.3, so it
#                                 must be read out of long-name fragments and
#                                 their checksum matched against the alias. Our
#                                 own ESP holds kernel-x86_64.elf, whose alias
#                                 is KERNEL~1.ELF, and an installer that could
#                                 only see aliases could not tell one kernel
#                                 from another.
#
#   deeper/inside.txt             A path two components deep, which is what an
#                                 ESP is: \EFI\BOOT\BOOTX64.EFI.
#
# Usage: scripts/make-fat-fixture.sh <output.img> [size-mb]
set -eu

OUT=${1:?usage: make-fat-fixture.sh <output.img> [size-mb]}
MB=${2:-64}

for t in mkfs.vfat mcopy mmd python3; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 1; }
done

# FAT32 is defined by having more than 65524 data clusters, and mkfs.vfat will
# refuse -F 32 on an image too small to reach that. Said here rather than
# discovered from a confusing error.
if [ "$MB" -lt 40 ]; then
	echo "a FAT32 volume needs more than 65524 clusters; use at least 40 MB" >&2
	exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

python3 - "$WORK/pattern.bin" <<'PY'
import sys
n = 140 * 1024
sys.argv[1]
with open(sys.argv[1], "wb") as f:
    f.write(bytes((i * 31 + 7) & 0xFF for i in range(n)))
PY

printf 'reconos' > "$WORK/long.txt"
printf 'two deep' > "$WORK/inside.txt"

rm -f "$OUT"
truncate -s "${MB}M" "$OUT"
mkfs.vfat -F 32 -n RECONFIX "$OUT" >/dev/null

mmd   -i "$OUT" ::/deeper
mcopy -i "$OUT" "$WORK/pattern.bin" ::/pattern.bin
mcopy -i "$OUT" "$WORK/long.txt"    ::/a-long-name-for-testing.txt
mcopy -i "$OUT" "$WORK/inside.txt"  ::/deeper/inside.txt

# What the volume actually turned out to be, printed rather than assumed --
# mkfs.vfat chooses the cluster size, and the reader has to cope with whatever
# it chose rather than with what this script expected.
echo "  $OUT: $(mdir -i "$OUT" :: | tail -1)"
mdir -i "$OUT" -/ :: | grep -iE 'pattern|long|inside' | sed 's/^/    /'
