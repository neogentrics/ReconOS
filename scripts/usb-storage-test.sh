#!/bin/sh
# Can the kernel actually use a USB stick, or only find one?
#
# medium-boot-test.sh already asserts that the kernel reads the partition table
# off the medium it booted from. That is the important half and it is only the
# read half: a driver whose WRITE(10) does nothing at all passes it completely.
#
# So this boots with *nothing but* a USB disk attached, which is what makes the
# block layer's own self-test pick it -- the self-test takes the first blank
# whole device, and with a virtio disk present that is never the stick. The
# self-test writes a non-repeating pattern, reads it back byte for byte, puts
# the original contents back and flushes.
#
# The last assertion is the one that took the longest to get right. The
# self-test restores what it borrowed, so a passing run leaves the image on
# disk *identical* to a run in which no write was ever issued -- and the first
# version of this test "confirmed" the write path by checking the image was
# unchanged, which is exactly what a driver that writes nothing produces. The
# only way to tell those apart is to ask the emulator what crossed the wire, so
# that is what is asked: QEMU is told to trace parsed SCSI requests, and the
# transcript must contain an actual WRITE(10).
#
# Usage: scripts/usb-storage-test.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

KERNEL=kernel/build/x86_64/reconos-kernel.elf
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL -- make -C kernel first" >&2; exit 1; }

command -v qemu-system-x86_64 >/dev/null 2>&1 || exit 2

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

IMG=$WORK/stick.img
TRACE=$WORK/scsi.log
OUT=$WORK/serial.log

# Sixty-four megabytes of zeroes, and blank on purpose: a device with a
# partition table on it is not the one the self-test picks.
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none

pass=0
fail=0
say() { printf '%-46s' "  $1"; }

timeout 120 qemu-system-x86_64 -m 512M -display none -no-reboot \
	-serial file:"$OUT" \
	--trace 'scsi_req_parsed' -D "$TRACE" \
	-kernel "$KERNEL" \
	-device qemu-xhci,id=xhci \
	-drive if=none,id=stick,format=raw,file="$IMG" \
	-device usb-storage,bus=xhci.0,drive=stick >/dev/null 2>&1 || true

serial=$(tr -d '\r' < "$OUT" 2>/dev/null || true)

# --- it is there, and it is the right size ----------------------------------

say "a USB stick registers as a block device"
if echo "$serial" | grep -qaE '^ *usb0 +: .*131072 blocks of 512 bytes'; then
	echo "$(echo "$serial" | grep -oaE 'usb0 +: [0-9.]+ MB.*' | head -1)"
	pass=$((pass + 1))
else
	echo "FAILED -- no usb0, or the wrong geometry"
	echo "$serial" | grep -aE 'usb|xhci' | head -6 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# --- the round trip ---------------------------------------------------------

say "and survives the block layer self-test"
if echo "$serial" | grep -qaE '^ *block devices +: pass'; then
	echo "pattern written, read back, restored"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$serial" | grep -aE 'block' | head -8 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# --- what actually crossed the wire -----------------------------------------
#
# 0x2A is WRITE(10). Without this the previous assertion cannot tell a working
# write path from an absent one.

say "having really issued a WRITE(10)"
writes=$(grep -caE 'command 42 ' "$TRACE" 2>/dev/null || true)
[ -n "$writes" ] || writes=0
if [ "$writes" -gt 0 ]; then
	echo "$writes on the wire"
	pass=$((pass + 1))
else
	echo "FAILED -- nothing was written to the device"
	grep -oaE 'command [0-9]+ ' "$TRACE" 2>/dev/null | sort | uniq -c |
		head -8 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# 0x35 is SYNCHRONIZE CACHE. block.h promises that a flush on a device
# reporting flush_is_durable reaches the medium; this driver sets that flag, so
# the command has to be real. A flag set without the command behind it is the
# exact failure block.h's own comment warns about.
say "with a real SYNCHRONIZE CACHE behind flush"
if grep -qaE 'command 53 ' "$TRACE" 2>/dev/null; then
	echo "flush is not a no-op"
	pass=$((pass + 1))
else
	echo "FAILED -- flush_is_durable is set but nothing is sent"
	fail=$((fail + 1))
fi

# --- the question block.h asks for ------------------------------------------
#
# KF-127: seek_is_free must come from the SCSI Block Device Characteristics
# page, not from the bus. QEMU's emulated stick does not implement the page, so
# what is asserted is that the kernel *asked* -- the answer being "no page
# here" is a measured default rather than a guess, and only asking makes it so.

say "and asks whether the medium rotates"
if grep -qaE 'command 18 .* length 64' "$TRACE" 2>/dev/null; then
	echo "vital product data page B1 requested"
	pass=$((pass + 1))
else
	echo "FAILED -- seek_is_free would be a guess"
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: the kernel reads and writes a USB stick"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
