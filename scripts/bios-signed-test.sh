#!/bin/sh
# Does the BIOS loader refuse a kernel that is not ours?
#
# **A BIOS path that does not check the signature is the off-switch the UEFI
# path deliberately does not have.** An attacker who can write to the disk would
# not need to defeat the check in reconboot; they would only need to make the
# machine take this path instead. So every claim in docs/INTEGRITY.md is a claim
# about both loaders or about neither, and this is the half that says so about
# this one.
#
# The signature is made by `openssl`, which shares no code with the verifier --
# a verifier checked against signatures produced by its own arithmetic is
# checked against its own misunderstandings.
#
# And then it is shown faults, because a verifier that has never been seen to
# refuse anything is not a verifier:
#
#   signed          the ordinary case, and the one that must still work
#   unsigned        the signature file is simply absent
#   tampered        one byte of the kernel changed after signing
#   wrong key       a valid signature, by somebody else's key
#
# The last is the one that matters most: it is the only case where everything is
# well-formed, and a verifier that checks the padding and forgets the modulus
# passes it.
#
# Usage: scripts/bios-signed-test.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

for t in openssl sgdisk mkfs.vfat mcopy mmd qemu-system-x86_64; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done

W=$(mktemp -d)

# The key this test generates goes into the *source tree* -- the loader compiles
# the public modulus in, so there is nowhere else for it to go. That makes this
# script one that changes the repository, and a test that changes the repository
# changes every test run after it (KF-139): with a key present, every harness
# that boots an unsigned kernel is correctly refused, and five paths fail for a
# reason that has nothing to do with them.
#
# So the previous state is put back, whatever happens.
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

# A key first, then everything built against it. The order matters: the loader
# compiles the public modulus in, so a build made before the key existed
# announces "not checked" and runs anything (KF-133).
./scripts/make-signing-key.sh "$W/keys" >/dev/null
openssl genrsa -out "$W/other.pem" 2048 2>/dev/null

make -C kernel ARCH=x86_64 >/dev/null 2>&1 || true

# Deleted before building, not merely rebuilt.
#
# Header dependencies do not save this one, and the reason is worth knowing:
# stage2.c reaches the key through `#if __has_include("signing_key.h")`, so on a
# build made *before* the key existed the answer was no and the header never
# entered the .d file. Creating it afterwards therefore triggers nothing. **A
# generated header that does not exist yet cannot be recorded as a dependency.**
#
# The verification rig keeps its build directory between runs, so this was a
# stage 2 compiled with no key announcing "not checked" and running anything,
# while every refusal below failed for a reason that was not the reason. Same
# mechanism as KF-133, third time; signed-kernel-test.sh deletes the UEFI loader
# for exactly this.
rm -f boot/bios/build/stage2_c.o boot/bios/build/stage2.bin \
      boot/bios/build/stage1.bin boot/bios/build/mbr.img
make -C boot/bios >/dev/null 2>&1 || true

KELF=kernel/build/x86_64/reconos-kernel.elf
[ -f "$KELF" ] || { echo "the kernel did not build" >&2; exit 1; }
[ -f boot/bios/build/stage2.bin ] || { echo "stage 2 did not build" >&2; exit 1; }

grep -q RECONOS_KEY_PRESENT boot/src/signing_key.h 2>/dev/null || {
	echo "no signing key was generated; nothing here would mean anything" >&2
	exit 1
}

sign() {
	[ -f "$1" ] || { echo "no private key at $1" >&2; exit 1; }
	openssl dgst -sha256 -sign "$1" -out "$3" "$2"
	[ -s "$3" ] || { echo "openssl produced no signature" >&2; exit 1; }
}

# --- a disk, built the way an install builds one ---------------------------
#
# $1 is the image to write, $2 is the kernel to install, $3 is the signature
# file to install, or empty for none.
make_disk() {
	img=$1
	kern=$2
	sig=$3

	dd if=/dev/zero of="$img" bs=1M count=64 status=none
	sgdisk -n 1:2048:+1M -t 1:ef02 "$img" >/dev/null 2>&1
	sgdisk -n 2:0:+32M   -t 2:ef00 "$img" >/dev/null 2>&1
	start=$(sgdisk -i 2 "$img" 2>/dev/null |
		sed -n 's/^First sector: \([0-9]*\).*/\1/p')

	dd if=/dev/zero of="$W/esp.part" bs=1M count=32 status=none
	mkfs.vfat -F 32 -n RECONOS "$W/esp.part" >/dev/null 2>&1
	mmd   -i "$W/esp.part" ::/reconos >/dev/null 2>&1
	mcopy -i "$W/esp.part" "$kern" ::/reconos/kernel-x86_64.elf
	[ -n "$sig" ] && mcopy -i "$W/esp.part" "$sig" \
		::/reconos/kernel-x86_64.elf.sig

	dd if="$W/esp.part" of="$img" bs=512 seek="$start" conv=notrunc status=none
	dd if=boot/bios/build/stage1.bin of="$img" bs=1 count=440 \
		conv=notrunc status=none
	dd if=boot/bios/build/stage2.bin of="$img" bs=512 seek=2048 \
		conv=notrunc status=none
}

# Stopped as soon as the answer is on the wire, rather than waited out.
#
# Three of the four cases end in the loader refusing and halting, so QEMU never
# exits on its own and a fixed timeout is paid in full every time -- four and a
# half minutes of a matrix run spent watching a machine that had already said
# what it was going to say. The line appears within a few seconds; the cap is
# only there for the case where it never appears at all.
boot_until() {
	img=$1
	want=$2
	o=$(mktemp)

	qemu-system-x86_64 -m 256M -display none -serial stdio -no-reboot \
		-drive "file=$img,format=raw,if=ide" > "$o" 2>&1 &
	pid=$!

	n=0
	while [ "$n" -lt 300 ]; do
		grep -qa "$want" "$o" 2>/dev/null && break
		kill -0 "$pid" 2>/dev/null || break
		sleep 0.2
		n=$((n + 1))
	done

	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	tr -d '\r' < "$o"
	rm -f "$o"
}

check() {
	say "$1"
	shift
	want=$1
	shift

	out=$(boot_until "$1" "$want")
	if echo "$out" | grep -q "$want"; then
		echo "$(echo "$out" | grep -oaE 'signature: .*|reconboot: this kernel.*' |
			head -1 | cut -c1-44)"
		pass=$((pass + 1))
	else
		echo "FAILED -- wanted: $want"
		echo "$out" | sed -n '/stage2 ok/,$p' | head -8 | sed 's/^/      /'
		fail=$((fail + 1))
	fi
}

# --- the four cases --------------------------------------------------------

sign "$W/keys/reconos-signing.pem" "$KELF" "$W/good.sig"
make_disk "$W/signed.img" "$KELF" "$W/good.sig"

# Before anything else: is this loader capable of refusing at all?
#
# A stage 2 built without the key announces "not checked" and runs whatever it
# is given. Every case below would then fail, and the reported reason would be
# the case rather than the build. Asked first, and said plainly, because a
# harness that cannot tell "the check said no" from "there was no check" is
# reporting on the wrong thing.
say "the loader under test can check at all"
first=$(boot_until "$W/signed.img" "signature")
if echo "$first" | grep -qa 'not checked'; then
	echo "FAILED -- built without a key, so nothing below means anything"
	echo "$first" | grep -a 'signature' | sed 's/^/      /'
	exit 1
elif echo "$first" | grep -qa 'signature'; then
	echo "the key is compiled in"
	pass=$((pass + 1))
else
	echo "FAILED -- it said nothing about a signature"
	echo "$first" | sed -n '/stage2 ok/,$p' | head -8 | sed 's/^/      /'
	exit 1
fi

check "a properly signed kernel is accepted" "signature: good" "$W/signed.img"

make_disk "$W/unsigned.img" "$KELF" ""
check "refuses: no signature at all" "only runs signed kernels" \
	"$W/unsigned.img"

# One byte, changed after signing, well past the ELF header so that the loader's
# own "is it an ELF" check cannot be what catches it.
cp "$KELF" "$W/tampered.elf"
printf '\101' | dd of="$W/tampered.elf" bs=1 seek=40000 conv=notrunc status=none
make_disk "$W/tampered.img" "$W/tampered.elf" "$W/good.sig"
check "refuses: a kernel changed after signing" "does not match" \
	"$W/tampered.img"

# Everything well-formed, and signed by somebody else. The only difference from
# the first case is the key.
sign "$W/other.pem" "$KELF" "$W/other.sig"
make_disk "$W/otherkey.img" "$KELF" "$W/other.sig"
check "refuses: a valid signature by another key" "does not match" \
	"$W/otherkey.img"

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: the BIOS path verifies too, or there is no path"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
