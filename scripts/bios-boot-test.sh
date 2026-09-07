#!/bin/sh
# Checkpoint 16: does a machine with no UEFI at all run our code?
#
# SeaBIOS, an IDE disk, and nothing else. No OVMF, no EFI partition, no
# firmware that knows what a filesystem is -- the interface is "read the first
# sector to 0x7C00 and jump", and everything after that is ours.
#
# What it insists on, and why each one:
#
#   the firmware runs our 440 bytes      the claim
#   stage 2 is read and jumped to        the claim that matters: 440 bytes
#                                        cannot hold a bootloader, so the whole
#                                        design rests on this hop working
#   the boot drive survives the hop      DL is the only way to learn which disk
#                                        the firmware picked, and it is handed
#                                        across by hand
#   no stage 2 says so                   a deliberate fault. A loader that
#                                        jumps into an unread sector does
#                                        something unpredictable instead of
#                                        saying what went wrong
#   a wrong stage 2 says so              a deliberate fault, and the one that
#                                        proves the magic check is connected at
#                                        all -- the LBA is patched into stage 1
#                                        by a program that has finished running
#                                        by the time it matters
#
# The last two are the point. The first three would all pass with the magic
# check deleted.
#
# Usage: scripts/bios-boot-test.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "need qemu" >&2; exit 2; }

# Built, not merely looked for (BG-134).
make -C boot/bios >/dev/null 2>&1 || true
[ -f boot/bios/build/stage1.bin ] || { echo "stage 1 did not build" >&2; exit 1; }
[ -f boot/bios/build/stage2.bin ] || { echo "stage 2 did not build" >&2; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM

pass=0
fail=0
say() { printf '%-46s' "  $1"; }

# A disk laid out the way stage 1 expects: our boot code in sector 0, stage 2 at
# LBA 2048, which is where the BIOS Boot Partition starts on a real install.
make_disk() {
	dd if=/dev/zero of="$1" bs=1M count=8 status=none
	dd if=boot/bios/build/stage1.bin of="$1" conv=notrunc status=none
	printf '\125\252' | dd of="$1" bs=1 seek=510 conv=notrunc status=none
	if [ "${2:-yes}" = yes ]; then
		dd if=boot/bios/build/stage2.bin of="$1" bs=512 seek=2048 \
			conv=notrunc status=none
	fi
}

# `-display none -serial stdio`, deliberately, and not `-nographic`.
#
# Under -nographic QEMU mirrors the VGA text console to stdout, so INT 10h
# output appears here and a loader that never touches a serial port looks like
# one that does. That mirror also drops characters, which read as truncation and
# sent an evening after a register-clobbering bug that did not exist. With the
# mirror off, the first version of this loader produced nothing at all -- which
# was the true state of it.
#
# So the harness reads the same port a headless machine would, and nothing but
# what the loader actually wrote can reach it.
boot() {
	timeout 20 qemu-system-x86_64 -m 128M -display none -serial stdio \
		-no-reboot -drive "file=$1,format=raw,if=ide" 2>&1 | tr -d '\r'
}

# --- the machine boots what we wrote ---------------------------------------

make_disk "$W/good.img"
out=$(boot "$W/good.img")

say "SeaBIOS runs our 440 bytes"
if echo "$out" | grep -q 'ReconOS'; then
	echo "no UEFI in the machine"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$out" | sed -n '/Booting from/,$p' | head -6 | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "stage 1 reads stage 2 and jumps to it"
if echo "$out" | grep -q 'stage2 ok'; then
	echo "440 bytes handed off"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$out" | sed -n '/Booting from/,$p' | head -6 | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "the boot drive survives the hop"
if echo "$out" | grep -q 'drive 0x80'; then
	echo "0x80, from the firmware"
	pass=$((pass + 1))
else
	echo "FAILED -- $(echo "$out" | grep -o 'drive 0x..' | head -1)"
	fail=$((fail + 1))
fi

# --- and now the two faults ------------------------------------------------
#
# A check that has never been seen to fire is not known to be connected.

say "a disk with no stage 2 says which step failed"
make_disk "$W/nostage2.img" no
# Truncate so the read of LBA 2048 cannot succeed at all, rather than reading
# zeroes -- this is the "the disk is failing" case, not the "wrong number" case.
dd if=/dev/zero of="$W/nostage2.img" bs=512 count=64 status=none
dd if=boot/bios/build/stage1.bin of="$W/nostage2.img" conv=notrunc status=none
printf '\125\252' | dd of="$W/nostage2.img" bs=1 seek=510 conv=notrunc status=none
bad=$(boot "$W/nostage2.img")

if echo "$bad" | grep -q 'E:RD'; then
	echo "E:RD, and it halted"
	pass=$((pass + 1))
else
	echo "FAILED -- no message, so it jumped somewhere"
	echo "$bad" | sed -n '/Booting from/,$p' | head -6 | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "a stage 2 that is not ours is refused"
make_disk "$W/wrong.img"
# One byte of the magic, changed. Everything else about the disk is correct, so
# the only thing standing between the firmware and a jump into this sector is
# the check being real.
printf 'X' | dd of="$W/wrong.img" bs=1 seek=$(( 2048 * 512 )) conv=notrunc status=none
wrong=$(boot "$W/wrong.img")

if echo "$wrong" | grep -q 'E:S2'; then
	echo "E:S2 -- one byte was enough"
	pass=$((pass + 1))
else
	echo "FAILED -- it jumped into a sector it had not identified"
	echo "$wrong" | sed -n '/Booting from/,$p' | head -6 | sed 's/^/      /'
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: a machine with no UEFI runs ReconOS's own loader"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
