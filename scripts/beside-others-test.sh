#!/usr/bin/env bash
#
# Puts a filesystem in one partition and proves nothing else moved.
#
# --- What this is for ---
#
# The whole of phase 2 rests on one promise: ReconOS installs beside an
# operating system that is already there, without destroying it. Every other
# safety rule in the storage stack exists to keep that promise -- the claim
# rule, the refusal to format a device carrying a partition table, reading
# partition tables for geometry only.
#
# None of that is worth anything as a set of intentions. This is the
# measurement: build a disk with two partitions, fill the second one with a
# recognisable pattern, hash the pattern and both copies of the partition table,
# format ReconFS into the *first* partition and run every operation it has, and
# then require all three hashes to be unchanged and the table to still validate
# under the tool that wrote it.
#
# --- Why sgdisk verifies the table afterwards ---
#
# Comparing hashes proves the bytes did not change. It does not prove they were
# ever right, and a test that only compares against itself is the failure mode
# scripts/make-partition-fixtures.sh opens by naming. `sgdisk -v` is a second
# opinion that shares no code with this kernel: it says the table is still a
# valid GPT, not merely the same bytes as before.

set -u

cd "$(dirname "$0")/.."

ARCH=${1:-x86_64}
OUT=kernel/build/beside
mkdir -p "$OUT"

IMG="$OUT/beside.img"

if ! command -v sgdisk >/dev/null 2>&1; then
	echo "sgdisk is not installed; this check needs the tool it compares against"
	exit 2
fi

rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=48 status=none

sgdisk -n 1:2048:+16M -t 1:8300 -c 1:"ReconOS" "$IMG" >/dev/null 2>&1
sgdisk -n 2:0:0      -t 2:8300 -c 2:"SomebodyElse" "$IMG" >/dev/null 2>&1

# Where partition two actually is, asked of the table rather than assumed.
#
# These were literals -- 34816 and 98270, copied from one run of sgdisk. A
# literal here is the quiet way this check becomes worthless: if the layout ever
# differed, it would hash a region that is not the neighbour, find it unchanged,
# and report that nothing was damaged.
NEIGHBOUR=$(sgdisk -i 2 "$IMG" 2>/dev/null |
	    awk '/First sector/ {f=$3} /Last sector/ {l=$3} END {print f" "l}')
set -- $NEIGHBOUR
N_FIRST=${1:-} N_LAST=${2:-}

if [ -z "$N_FIRST" ] || [ -z "$N_LAST" ] || [ "$N_FIRST" = "0" ]; then
	echo "  could not read partition two's extent from the table"
	exit 1
fi

# The neighbour gets a pattern, and everything that must not move is hashed.
before=$(python3 - "$IMG" "$N_FIRST" "$N_LAST" <<'PY'
import hashlib, sys

path = sys.argv[1]
first, last = int(sys.argv[2]), int(sys.argv[3])
d = bytearray(open(path, "rb").read())

start, end = first * 512, (last + 1) * 512
if end > len(d) or start >= end:
    raise SystemExit("partition two's extent is not inside the image")

for i in range(start, end):
    d[i] = (i * 7 + 11) & 0xFF
open(path, "wb").write(d)

print(hashlib.sha256(d[start:end]).hexdigest(),        # the neighbour
      hashlib.sha256(d[:34 * 512]).hexdigest(),        # protective MBR + GPT
      hashlib.sha256(d[-33 * 512:]).hexdigest())       # the backup GPT
PY
)

if [ "$ARCH" = aarch64 ]; then
	qemu=(qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic
	      -kernel kernel/build/aarch64/reconos-kernel.img)
else
	qemu=(qemu-system-x86_64 -m 512M -nographic -no-reboot
	      -kernel kernel/build/x86_64/reconos-kernel.elf)
fi

out=$( ( exec 2>/dev/null; timeout -s KILL 150 "${qemu[@]}" \
	-append "reconfs=nvme0n1p1" \
	-drive "file=$IMG,format=raw,if=none,id=d0" \
	-device nvme,serial=recon0,drive=d0 ) || true )

sizes=$(echo "$out" | grep -c 'the checker caught 5 of 5')

after=$(python3 - "$IMG" "$N_FIRST" "$N_LAST" <<'PY'
import hashlib, sys

d = open(sys.argv[1], "rb").read()
first, last = int(sys.argv[2]), int(sys.argv[3])
start, end = first * 512, (last + 1) * 512
print(hashlib.sha256(d[start:end]).hexdigest(),
      hashlib.sha256(d[:34 * 512]).hexdigest(),
      hashlib.sha256(d[-33 * 512:]).hexdigest())
PY
)

fail=0

if [ "$sizes" != "3" ]; then
	echo "  the filesystem did not run in the partition ($sizes of 3 sizes)"
	echo "$out" | sed -n '/reconfs:/,$p' | head -12 | sed 's/^/      /'
	fail=1
fi

set -- $before
b_neighbour=$1 b_head=$2 b_tail=$3
set -- $after
a_neighbour=$1 a_head=$2 a_tail=$3

[ "$b_neighbour" = "$a_neighbour" ] || { echo "  the neighbouring partition changed"; fail=1; }
[ "$b_head" = "$a_head" ] || { echo "  the partition table changed"; fail=1; }
[ "$b_tail" = "$a_tail" ] || { echo "  the backup partition table changed"; fail=1; }

if ! sgdisk -v "$IMG" 2>&1 | grep -q "No problems found"; then
	echo "  sgdisk no longer considers the table valid:"
	sgdisk -v "$IMG" 2>&1 | tail -4 | sed 's/^/      /'
	fail=1
fi

# The comparison has to be able to notice. Three hashes that match prove nothing
# if the thing computing them would match whatever it was given -- so one byte of
# the neighbour is changed on purpose, and the same comparison must reject it.
proof=$(python3 - "$IMG" "$N_FIRST" "$N_LAST" "$a_neighbour" <<'PY'
import hashlib, sys

path, first, last, expected = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
d = bytearray(open(path, "rb").read())
start, end = first * 512, (last + 1) * 512
d[start] ^= 0xFF
print("caught" if hashlib.sha256(d[start:end]).hexdigest() != expected
      else "MISSED")
PY
)

if [ "$proof" != "caught" ]; then
	echo "  the comparison did not notice a byte changed on purpose,"
	echo "  so nothing above it meant anything"
	fail=1
fi

rm -f "$IMG"

if [ "$fail" -eq 0 ]; then
	echo "a filesystem in partition 1; the neighbour, both copies of the table"
	echo "  and sgdisk's own verdict all unchanged"
	exit 0
fi

exit 1
