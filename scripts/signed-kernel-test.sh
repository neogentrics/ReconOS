#!/bin/sh
# Does the loader actually refuse a kernel that is not ours?
#
# The signature is made by `openssl`, which shares no code with the verifier.
# That is the whole point: a verifier checked against signatures produced by its
# own arithmetic is checked against its own misunderstandings.
#
# And then it is shown faults, because **a verifier that has never been seen to
# refuse anything is not a verifier**. Four of them, each a different way for a
# kernel to be wrong:
#
#   unsigned        the signature file is simply absent
#   truncated       it is there and is not 256 bytes
#   tampered kernel one byte of the kernel changed after signing
#   wrong key       a valid signature, by somebody else's key
#
# The last is the one that matters. The first three are damage; only the fourth
# is an attacker, and a verifier that catches corruption while accepting any
# well-formed signature is worth nothing at all.
#
# Usage: scripts/signed-kernel-test.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OVMF=/usr/share/ovmf/OVMF.fd
KERNEL=kernel/build/x86_64/reconos-kernel.elf

[ -f "$OVMF" ] || { echo "OVMF is not installed"; exit 2; }
for t in openssl sgdisk mkfs.vfat mcopy mmd; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done
# Built, not merely looked for. A script that only checks a binary exists
# will happily test one compiled before the change it is meant to prove --
# which is how a security check came to be silently absent (KF-133).
make -C kernel ARCH="${ARCH:-x86_64}" >/dev/null 2>&1 || true
[ -f "$KERNEL" ] || { echo "the kernel did not build" >&2; exit 1; }

W=$(mktemp -d)

# The key this test generates goes into the *source tree* -- the loader compiles
# the public modulus in, so there is nowhere else for it to go. That makes this
# script one that changes the repository, and a test that changes the repository
# changes every test run after it (KF-139): with a key left behind, every
# harness that boots an unsigned kernel is correctly refused, and paths fail for
# a reason that has nothing to do with them.
#
# So the previous state is put back, whatever happens. The loader is rebuilt
# afterwards by whoever needs it, because the header is a tracked dependency --
# which it was not until KF-133.
KEYHDR=boot/src/signing_key.h
if [ -f "$KEYHDR" ]; then
	cp "$KEYHDR" "$W.keyhdr"
	restore_key() { mv "$W.keyhdr" "$KEYHDR"; }
else
	restore_key() { rm -f "$KEYHDR"; }
fi

trap 'restore_key; rm -rf "$W"' EXIT INT TERM

pass=0
fail=0
say() { printf '%-46s' "  $1"; }

# --- a key, and a loader built to trust it ----------------------------------

./scripts/make-signing-key.sh "$W/keys" >/dev/null

# The loader must be *rebuilt* against the key just generated. Header
# dependencies now make that automatic, but this test's whole subject is a
# loader that trusts a specific key, so it does not rely on that: a stale
# binary here would announce "not checked" and run an unsigned kernel, and
# every refusal below would fail for a reason that is not the reason.
rm -f boot/build/x86_64/main.o boot/build/x86_64/BOOTX64.EFI
make -C boot ARCH=x86_64 >/dev/null 2>&1

LOADER=boot/build/x86_64/BOOTX64.EFI
[ -f "$LOADER" ] || { echo "the bootloader did not build"; exit 1; }

# Somebody else's key, for the case that matters.
openssl genrsa -out "$W/other.pem" 2048 2>/dev/null

sign() { openssl dgst -sha256 -sign "$1" -out "$3" "$2"; }

sign "$W/keys/reconos-signing.pem" "$KERNEL" "$W/good.sig"
sign "$W/other.pem"                "$KERNEL" "$W/other.sig"

cp "$KERNEL" "$W/tampered.elf"
# One byte, in the middle, well past any header -- the change an attacker would
# make is not at the front.
python3 -c "
import sys
f=open(sys.argv[1],'r+b'); f.seek(200000); b=f.read(1)
f.seek(200000); f.write(bytes([b[0]^0x01])); f.close()
" "$W/tampered.elf"

# --- a medium, rebuilt for each case ----------------------------------------

medium() {
	kern=$1
	sig=$2

	rm -f "$W/m.img" "$W/e.part"
	truncate -s 512M "$W/m.img"
	sgdisk -n 1:2048:0 -t 1:ef00 "$W/m.img" >/dev/null
	dd if=/dev/zero of="$W/e.part" bs=1M count=500 status=none
	mkfs.vfat -F 32 -n RECONOS "$W/e.part" >/dev/null
	mmd   -i "$W/e.part" ::/EFI ::/EFI/BOOT ::/reconos
	mcopy -i "$W/e.part" "$LOADER" ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i "$W/e.part" "$kern" ::/reconos/kernel-x86_64.elf
	[ -n "$sig" ] && mcopy -i "$W/e.part" "$sig" ::/reconos/kernel-x86_64.elf.sig
	dd if="$W/e.part" of="$W/m.img" bs=512 seek=2048 conv=notrunc status=none
}

boot() {
	timeout 45 qemu-system-x86_64 -bios "$OVMF" -m 512M -nographic \
		-drive "file=$W/m.img,format=raw,if=none,id=m0" \
		-device nvme,serial=m,drive=m0 2>&1 | tr -d '\r'
}

# --- the claim --------------------------------------------------------------

say "a properly signed kernel starts"
medium "$KERNEL" "$W/good.sig"
out=$(boot)
if echo "$out" | grep -q 'signature    : good' && echo "$out" | grep -q 'ReconOS kernel'; then
	echo "signature good, kernel started"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$out" | grep -iE 'signature|reconboot|ReconOS kernel' | sed 's/^/      /' | head -6
	fail=$((fail + 1))
fi

# --- and what makes the claim mean anything ---------------------------------

refuses() {
	name=$1
	kern=$2
	sig=$3
	want=$4

	say "refuses: $name"
	medium "$kern" "$sig"
	out=$(boot)

	if echo "$out" | grep -q 'ReconOS kernel'; then
		echo "STARTED ANYWAY -- it ran a kernel it should not have"
		fail=$((fail + 1))
	elif echo "$out" | grep -q "$want"; then
		echo "refused, and said why"
		pass=$((pass + 1))
	else
		echo "stopped, but not for the stated reason"
		echo "$out" | grep -i 'reconboot' | sed 's/^/      /' | head -4
		fail=$((fail + 1))
	fi
}

head -c 100 "$W/good.sig" > "$W/short.sig"

refuses "no signature at all"              "$KERNEL"          ""             "not signed"
refuses "a signature of the wrong length"  "$KERNEL"          "$W/short.sig" "not 256 bytes"
refuses "a kernel changed after signing" "$W/tampered.elf" "$W/good.sig" "does not match"
refuses "a valid signature by another key" "$KERNEL" "$W/other.sig" "does not match"

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: ran the signed kernel, refused four that were not"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
