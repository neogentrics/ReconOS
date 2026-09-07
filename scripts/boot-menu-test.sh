#!/bin/sh
# Does the bootloader find the other operating systems on the machine?
#
# ReconOS installs beside whatever was already there. That promise is worthless
# if the machine then only boots ReconOS: installing beside Windows and then
# being unable to reach Windows is not a partial success, it is a machine
# somebody has lost the use of.
#
# The fixture is shaped like a real dual-boot PC rather than like the code:
# **Windows and Linux on one EFI partition**, which is how they actually live.
# The first version of the discovery code passed a fixture with one system per
# disk and failed this one -- it stopped searching a volume after the first
# match, so it found Windows and never offered the Linux beside it.
#
# What it insists on:
#
#   both systems are found            the dual-boot case, on one ESP
#   the medium is not offered         booting the stick you booted from is a
#                                     loop, not a choice
#   one entry per system              Ubuntu ships a shim *and* a GRUB and both
#                                     open; listing it twice teaches somebody
#                                     the menu is unreliable
#   nothing is written                discovery reads; it must not touch the
#                                     other system's partition at all
#
# Usage: scripts/boot-menu-test.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

LOADER=boot/build/x86_64/BOOTX64.EFI
KERNEL=kernel/build/x86_64/reconos-kernel.elf
OVMF=/usr/share/ovmf/OVMF.fd

[ -f "$OVMF" ] || { echo "OVMF is not installed"; exit 2; }
for t in sgdisk mkfs.vfat mcopy mmd; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done

make -C boot ARCH=x86_64 >/dev/null 2>&1 || true
[ -f "$LOADER" ] || { echo "the bootloader is not built"; exit 2; }
[ -f "$KERNEL" ] || { echo "build the kernel first"; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM

pass=0
fail=0
say() { printf '%-46s' "  $1"; }

# --- our medium -------------------------------------------------------------

truncate -s 512M "$W/medium.img"
sgdisk -n 1:2048:0 -t 1:ef00 "$W/medium.img" >/dev/null
dd if=/dev/zero of="$W/e.part" bs=1M count=500 status=none
mkfs.vfat -F 32 -n RECONOS "$W/e.part" >/dev/null
mmd   -i "$W/e.part" ::/EFI ::/EFI/BOOT ::/reconos
mcopy -i "$W/e.part" "$LOADER" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$W/e.part" "$KERNEL" ::/reconos/kernel-x86_64.elf
dd if="$W/e.part" of="$W/medium.img" bs=512 seek=2048 conv=notrunc status=none

# --- a machine that already has two systems, on one EFI partition -----------

truncate -s 512M "$W/other.img"
sgdisk -n 1:2048:0 -t 1:ef00 "$W/other.img" >/dev/null
dd if=/dev/zero of="$W/o.part" bs=1M count=500 status=none
mkfs.vfat -F 32 -n SYSTEM "$W/o.part" >/dev/null
mmd -i "$W/o.part" ::/EFI ::/EFI/Microsoft ::/EFI/Microsoft/Boot ::/EFI/ubuntu
printf 'not really a bootloader' > "$W/f"
mcopy -i "$W/o.part" "$W/f" ::/EFI/Microsoft/Boot/bootmgfw.efi
# Both of Ubuntu's, because Ubuntu really does ship both.
mcopy -i "$W/o.part" "$W/f" ::/EFI/ubuntu/shimx64.efi
mcopy -i "$W/o.part" "$W/f" ::/EFI/ubuntu/grubx64.efi
dd if="$W/o.part" of="$W/other.img" bs=512 seek=2048 conv=notrunc status=none

before=$(sha256sum < "$W/other.img")

out=$(timeout 60 qemu-system-x86_64 -bios "$OVMF" -m 512M -nographic \
	-drive "file=$W/medium.img,format=raw,if=none,id=m0" \
	-device nvme,serial=m,drive=m0 \
	-drive "file=$W/other.img,format=raw,if=none,id=o0" \
	-device nvme,serial=o,drive=o0 2>&1 | tr -d '\r')

listing=$(echo "$out" | sed -n '/Other systems/,/^$/p')

say "finds Windows beside Linux on one disk"
if echo "$listing" | grep -q 'Windows' && echo "$listing" | grep -q 'Ubuntu'; then
	echo "both"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$listing" | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "lists Ubuntu once, not once per loader"
if [ "$(echo "$listing" | grep -c 'Ubuntu')" = "1" ]; then
	echo "one entry"
	pass=$((pass + 1))
else
	echo "FAILED -- $(echo "$listing" | grep -c 'Ubuntu') entries"
	echo "$listing" | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "does not offer the medium it booted from"
if ! echo "$listing" | grep -q 'ReconOS'; then
	echo "excluded"
	pass=$((pass + 1))
else
	echo "FAILED -- it offered itself"
	echo "$listing" | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "the other system's disk is untouched"
if [ "$before" = "$(sha256sum < "$W/other.img")" ]; then
	echo "byte for byte"
	pass=$((pass + 1))
else
	echo "CHANGED"
	fail=$((fail + 1))
fi

# --- and the countdown is really a countdown ---------------------------------
#
# The unattended test above proves the machine *proceeds*. It does not prove it
# waited: a bounded wait that is accidentally zero, and one that is accidentally
# a minute, look identical in a test that only checks the machine booted. So the
# wait is measured.
#
# Timing it as `qemu | grep -q marker` does not work and looks like it does --
# grep exits on the first match but the shell waits for the whole pipeline, and
# QEMU runs on because the kernel idles for ever. Both cases then measure the
# timeout, to the millisecond, which is the only reason the mistake was caught.

time_to_kernel() {
	o=$(mktemp)
	qemu-system-x86_64 -bios "$OVMF" -m 512M -nographic "$@" > "$o" 2>&1 &
	pid=$!
	start=$(date +%s%N)
	ms=0
	while [ "$ms" -lt 40000 ]; do
		grep -q 'ReconOS kernel' "$o" 2>/dev/null && break
		kill -0 "$pid" 2>/dev/null || break
		sleep 0.1
		ms=$(( ( $(date +%s%N) - start ) / 1000000 ))
	done
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	rm -f "$o"
	echo "$ms"
}

say "waits about five seconds, then starts anyway"
with=$(time_to_kernel 	-drive "file=$W/medium.img,format=raw,if=none,id=m0" -device nvme,serial=m,drive=m0 	-drive "file=$W/other.img,format=raw,if=none,id=o0" -device nvme,serial=o,drive=o0)
alone=$(time_to_kernel 	-drive "file=$W/medium.img,format=raw,if=none,id=m0" -device nvme,serial=m,drive=m0)
diff=$(( with - alone ))

if [ "$diff" -ge 3500 ] && [ "$diff" -le 8000 ]; then
	echo "${diff} ms longer with a menu"
	pass=$((pass + 1))
else
	echo "FAILED -- ${diff} ms (${with} with a menu, ${alone} without)"
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: found the other systems, and touched none of them"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
