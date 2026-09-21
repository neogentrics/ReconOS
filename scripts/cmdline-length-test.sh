#!/bin/bash
#
# Does the UEFI loader say anything when the cmdline file is longer than it can
# hold? Measured rather than argued.
#
# read_cmdline uses cmdline_buf[128] and asks EFI for sizeof-1 = 127 bytes. The
# handoff field is char cmdline[128]. The claim under test is that a longer file
# is read short, with no error and no report -- which would mean a switch past
# byte 127 silently does not happen.
#
# Two boots, both UEFI, same medium, differing only in the length of the file:
#
#   A. "logport" alone           -- the control. The port must listen.
#   B. padding + " logport"      -- the same switch, pushed past the buffer.
#
# If B listens, the claim is wrong and the buffer is bigger than it looks. If B
# is silent AND nothing in the report mentions a truncated or unreadable command
# line, the claim holds: silent short read.
#
# The control is not optional. "No log port in B" is equally well explained by a
# medium that was never built right, and that explanation fails in the same
# direction as the guess.

set -u
cd "$(dirname "$0")/.."

OVMF=${OVMF:-/usr/share/ovmf/OVMF.fd}
[ -f "$OVMF" ] || { echo "no OVMF -- the control cannot run, refusing"; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
IMG=$WORK/medium.img

scripts/make-medium.sh "$IMG" 512M >"$WORK/mk.log" 2>&1 || {
	echo "could not build the medium"; tail -5 "$WORK/mk.log"; exit 2; }

ESP=$(sgdisk -i 2 "$IMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
OFF=$((ESP * 512))

run() {
	local label=$1 content=$2 out=$WORK/$1.log
	printf '%s' "$content" > "$WORK/cmdline"
	mcopy -o -i "$IMG@@$OFF" "$WORK/cmdline" ::/reconos/cmdline 2>/dev/null || {
		echo "could not write the cmdline file"; exit 2; }

	echo
	echo "=== $label (${#content} bytes) ==="
	timeout 180 qemu-system-x86_64 -m 1024M -display none -no-reboot \
		-netdev user,id=n0 -device e1000,netdev=n0 \
		-drive if=none,id=stick,format=raw,file="$IMG" \
		-device usb-ehci,id=ehci \
		-device usb-storage,bus=ehci.0,drive=stick,bootindex=0 \
		-bios "$OVMF" -serial file:"$out" >/dev/null 2>&1

	echo -n "  command line : "; grep -a "command line" "$out" | head -1 | sed 's/.*command line *: *//' | cut -c1-60
	echo -n "  log port     : "; grep -aq "log port" "$out" && echo "LISTENING" || echo "absent"
	echo -n "  any complaint about the command line? "
	grep -aiE "truncat|too long|cmdline.*(fail|error)" "$out" | head -1 || echo "none"
}

# A: the control.
run control "logport"

# B: the same switch, pushed past byte 127.
PAD=$(printf 'x%.0s' $(seq 1 130))
run overlong "$PAD logport"

echo
echo "=== verdict ==="
c=$(grep -ac "log port" "$WORK/control.log" 2>/dev/null || true); c=${c:-0}
o=$(grep -ac "log port" "$WORK/overlong.log" 2>/dev/null || true); o=${o:-0}
if [ "$c" -eq 0 ]; then
	echo "  CONTROL FAILED -- the short cmdline did not work either, so this"
	echo "  run says nothing about length. Do not record a result."
	exit 2
elif [ "$o" -eq 0 ]; then
	echo "  CONFIRMED: a switch past byte 127 is dropped, and the report does"
	echo "  not say the command line was cut. Silent short read."
	exit 1
else
	echo "  REFUTED: the overlong cmdline still carried the switch."
	exit 0
fi
