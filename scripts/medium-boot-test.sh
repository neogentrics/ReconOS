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

# --- reading the stick it came from -----------------------------------------
#
# This assertion used to say the opposite. Until checkpoint 11b the kernel
# could not read the medium it had booted from -- the stick is on a USB
# controller and there was no USB driver -- and what this checked was that the
# kernel *said* so rather than reporting an empty machine.
#
# It now reads it, so the assertion is the other way round, and it is
# deliberately not a check that a device merely appeared. A driver that
# enumerates the controller and registers a device of the wrong size, or one
# whose reads return another page's contents, passes "a disk is present" and
# fails the only thing anybody wants from it. So what is checked is the
# partition table: the kernel must find a GPT on the stick with the two
# partitions make-medium.sh put there, at the offsets it put them at. Those
# bytes are 1 MiB into the device and cannot be produced by accident.

say "and reads the stick it came from"
if echo "$uefi" | grep -qaE '^table usb0 gpt 2'; then
	slices=$(echo "$uefi" | grep -acE '^slice usb0 ')
	echo "gpt, $slices partitions off USB"
	pass=$((pass + 1))
else
	echo "FAILED -- no readable table on the medium"
	echo "$uefi" | grep -aE 'usb|Storage|table ' | head -8 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# The offsets, not just the count. make-medium.sh puts the BIOS boot partition
# at 2048 and the ESP at 4096; a driver with an off-by-one in its block
# addressing still returns two slices, just not these ones.
say "at the offsets the medium was built with"
if echo "$uefi" | grep -qaE '^slice usb0 1 2048 ' &&
   echo "$uefi" | grep -qaE '^slice usb0 2 4096 '; then
	echo "2048 and 4096, as written"
	pass=$((pass + 1))
else
	echo "FAILED -- the partitions are not where they were put"
	echo "$uefi" | grep -aE '^slice usb0' | head -4 | sed 's/^/      /'
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: the medium boots on both firmwares, from a stick"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
