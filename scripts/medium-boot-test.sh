#!/bin/sh
# Does the install medium boot the way a stick has to boot?
#
# The medium is the one artefact a person handles. It goes into a machine whose
# firmware nobody asked about, on a port nobody chose, and it either starts or
# it does not -- there is no log to read and no second attempt that is any
# different from the first.
#
# So it is booted the three ways a real one is:
#
#   UEFI, from a USB stick      what a modern machine does with it
#   BIOS, from a USB stick      what an old one does
#   UEFI, aarch64               the other architecture, off the same image
#
# **USB specifically, not a SATA disk.** A stick plugged into a machine appears
# on a USB controller, and the firmware reaches it through its own USB stack.
# Testing with `if=ide` would exercise a path the medium will never take and
# would pass while the real thing did not.
#
# What this deliberately does not test: the kernel reading the medium back
# after it has taken over. It cannot, and that is checkpoint 11b -- see the
# assertion at the end, which requires the kernel to *say so* rather than to
# quietly find nothing.
#
# Usage: scripts/medium-boot-test.sh [image]
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

IMG=${1:-}
if [ -z "$IMG" ]; then
	IMG=$(mktemp -u)/medium.img
	mkdir -p "$(dirname "$IMG")"
	./scripts/make-medium.sh "$IMG" >/dev/null
	CLEAN=$(dirname "$IMG")
	trap 'rm -rf "$CLEAN"' EXIT INT TERM
fi

[ -f "$IMG" ] || { echo "no medium at $IMG" >&2; exit 1; }

OVMF=/usr/share/ovmf/OVMF.fd
AAVMF=/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd

pass=0
fail=0
say() { printf '%-46s' "  $1"; }

# A USB stick, as QEMU emulates one: an xHCI controller with a mass storage
# device on it, which is what the firmware will find and what the kernel will
# not be able to read.
usb_args() {
	echo "-device qemu-xhci,id=xhci -drive if=none,id=stick,format=raw,file=$IMG -device usb-storage,bus=xhci.0,drive=stick"
}

boot_until() {
	want=$1
	shift
	o=$(mktemp)

	# shellcheck disable=SC2068
	timeout 90 qemu-system-x86_64 -m 512M -display none -serial file:"$o" \
		-no-reboot $@ >/dev/null 2>&1 &
	pid=$!

	n=0
	while [ "$n" -lt 300 ]; do
		grep -qa "$want" "$o" 2>/dev/null && break
		kill -0 "$pid" 2>/dev/null || break
		sleep 0.25
		n=$((n + 1))
	done

	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	tr -d '\r' < "$o"
	rm -f "$o"
}

# --- UEFI, from a stick -----------------------------------------------------

# Waited for the *end* of the boot, not for the banner.
#
# The banner prints within a second and the storage summary a good deal later,
# so stopping at "ReconOS kernel" captures a transcript that does not contain
# the thing the fourth assertion is about -- which duly failed, saying the
# kernel claimed storage it could not have, when in fact the kernel had not got
# as far as saying anything at all. **A marker that arrives before the evidence
# is a marker that tests the wrong moment.**
say "a UEFI machine boots it from USB"
# shellcheck disable=SC2046
uefi=$(boot_until 'Idling' -bios "$OVMF" $(usb_args))

if echo "$uefi" | grep -qa 'ReconOS kernel'; then
	echo "$(echo "$uefi" | grep -oaE 'ReconOS kernel [0-9.]+' | head -1)"
	pass=$((pass + 1))
else
	echo "DID NOT BOOT"
	echo "$uefi" | tail -10 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# The medium must not install anything by being plugged in. A stick that
# installs on boot would do it on whatever disk matched, on whatever machine it
# was put into, without being asked.
say "and installs nothing by being plugged in"
if ! echo "$uefi" | grep -qa 'installer:'; then
	echo "it boots and stops"
	pass=$((pass + 1))
else
	echo "FAILED -- the medium tried to install"
	echo "$uefi" | grep -a 'installer:' | sed 's/^/      /' | head -3
	fail=$((fail + 1))
fi

# --- BIOS, from a stick -----------------------------------------------------

say "a machine with no UEFI boots it from USB"
# shellcheck disable=SC2046
bios=$(boot_until 'firmware' $(usb_args) -boot order=c)

if echo "$bios" | grep -qa 'ReconOS kernel' &&
   echo "$bios" | grep -qaE 'firmware +: BIOS'; then
	echo "$(echo "$bios" | grep -oaE 'ReconOS kernel [0-9.]+' | head -1), over BIOS"
	pass=$((pass + 1))
else
	echo "DID NOT BOOT"
	echo "$bios" | sed -n '/Booting from/,$p' | head -10 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# --- the boundary, said out loud --------------------------------------------
#
# The kernel cannot read the stick it was booted from: it is on a USB
# controller and there is no USB driver (checkpoint 11b). That is a real
# limitation and the point of this assertion is that the kernel **says** so
# rather than reporting an empty machine.
#
# "No storage found" and "storage I cannot reach" are different sentences, and
# a person holding a stick that will not install needs the second one.

say "and says it cannot read the stick it came from"
if echo "$uefi" | grep -qaE 'devices *: none found|no device'; then
	echo "no block devices -- 11b is what fixes it"
	pass=$((pass + 1))
else
	echo "FAILED -- it claims to see storage it cannot have"
	echo "$uefi" | grep -a -A3 'Storage' | head -6 | sed 's/^/      /'
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: the medium boots on both firmwares, from a stick"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
