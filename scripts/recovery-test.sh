#!/bin/sh
# Does the recovery environment say what is actually wrong?
#
# A recovery environment that has never been seen to report damage is not a
# recovery environment -- it is a screen that says "sound", which is what a
# broken one says too.
#
# So: install onto a disk, break one of its two ReconFS volumes with
# `reconfs-check.py` -- a second implementation written from the specification
# rather than from the kernel -- and require recovery to
#
#   * name the damaged volume and the block,
#   * still call the *other* volume sound, so it is discriminating rather than
#     alarming,
#   * and change nothing, which is checked by hashing the disk.
#
# The near-miss worth remembering: the first attempt passed the damage tool its
# arguments the wrong way round. It damaged nothing, and recovery reported
# "sound" -- exactly what a correct checker says about a volume nobody broke. A
# green result for a test that never ran. The tool now refuses that invocation,
# and this script checks that the damage took before it believes anything after.
#
# Usage: scripts/recovery-test.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

KERNEL=kernel/build/x86_64/reconos-kernel.elf
[ -f "$KERNEL" ] || { echo "build the kernel first"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "need python3" >&2; exit 2; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM

pass=0
fail=0
say() { printf '%-46s' "  $1"; }

run() {
	timeout 120 qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-kernel "$KERNEL" -append "$1" \
		-drive "file=$W/d.img,format=raw,if=none,id=d0" \
		-device nvme,serial=d,drive=d0 2>&1 | tr -d '\r'
}

truncate -s 8G "$W/d.img"
run "install-onto=nvme0n1" > "$W/install.log" 2>&1

if ! grep -q 'installed: table' "$W/install.log"; then
	echo "  could not install a disk to recover; nothing to test" >&2
	sed -n '/^installer:/,$p' "$W/install.log" | head -10 >&2
	exit 1
fi

say "reports a healthy machine as healthy"
before=$(run recovery)
if [ "$(echo "$before" | grep -c 'checked: sound')" = "2" ]; then
	echo "both ReconOS volumes sound"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$before" | sed -n '/Volumes:/,/Repair is/p' | sed 's/^/      /'
	fail=$((fail + 1))
fi

# --- break one volume, with a tool that shares no code with the kernel ------

START=$(( 2048 + 256 * 2048 ))
dd if="$W/d.img" of="$W/p2.img" bs=512 skip="$START" count=4194304 status=none

say "the damage tool actually damaged something"
python3 scripts/reconfs-check.py --damage unallocated "$W/p2.img" > "$W/dmg.log" 2>&1 || true
if grep -q '^damaged ' "$W/dmg.log" &&
   python3 scripts/reconfs-check.py "$W/p2.img" 2>&1 | grep -q 'inconsistent'; then
	echo "$(python3 scripts/reconfs-check.py "$W/p2.img" 2>&1 | tail -1 | sed 's/^ *//')"
	pass=$((pass + 1))
else
	echo "FAILED -- nothing was broken, so nothing below means anything"
	cat "$W/dmg.log" | sed 's/^/      /' | head -4
	fail=$((fail + 1))
fi

dd if="$W/p2.img" of="$W/d.img" bs=512 seek="$START" conv=notrunc status=none
hash_before=$(sha256sum < "$W/d.img")

say "recovery names the damaged volume"
after=$(run recovery)
if echo "$after" | grep -q 'checked: DAMAGED'; then
	echo "$(echo "$after" | grep -oE 'DAMAGED -- .*' | head -1 | cut -c1-46)"
	pass=$((pass + 1))
else
	echo "FAILED -- it did not notice"
	echo "$after" | sed -n '/Volumes:/,/Repair is/p' | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "and still calls the other volume sound"
if [ "$(echo "$after" | grep -c 'checked: sound')" = "1" ]; then
	echo "discriminating, not alarming"
	pass=$((pass + 1))
else
	echo "FAILED -- $(echo "$after" | grep -c 'checked: sound') sound volumes"
	echo "$after" | sed -n '/Volumes:/,/Repair is/p' | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "recovery wrote nothing"
if [ "$hash_before" = "$(sha256sum < "$W/d.img")" ]; then
	echo "the disk is byte for byte"
	pass=$((pass + 1))
else
	echo "CHANGED -- recovery is supposed to look, not touch"
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: found the damage, kept its hands off the disk"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
