#!/bin/sh
# Does the install disc boot the way a disc has to boot?
#
# The stick is tested by scripts/medium-boot-test.sh. This is the other medium,
# and it is not the same test with a different `-drive` line: a disc has
# 2048-byte sectors, no partition table, and a boot catalogue where a disk has a
# GPT. Every one of those is a place the loaders can be wrong while the stick
# still works.
#
# So it is booted the three ways a real disc is:
#
#   UEFI, x86_64     what a modern machine does with it
#   BIOS, x86_64     what an old one does
#   UEFI, aarch64    the other architecture, off the same disc
#
# Usage: scripts/disc-boot-test.sh [image.iso]
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

ISO=${1:-}
if [ -z "$ISO" ]; then
	ISO=$(mktemp -u)/reconos.iso
	mkdir -p "$(dirname "$ISO")"
	./scripts/make-disc.sh "$ISO" >/dev/null
	CLEAN=$(dirname "$ISO")
	trap 'rm -rf "$CLEAN"' EXIT INT TERM
fi

[ -f "$ISO" ] || { echo "no disc at $ISO" >&2; exit 1; }

OVMF=/usr/share/ovmf/OVMF.fd
AAVMF=/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd

pass=0
fail=0
say() { printf '%-46s' "  $1"; }

boot_until() {
	qemu=$1
	want=$2
	shift 2
	o=$(mktemp)
	# shellcheck disable=SC2068
	timeout 150 "$qemu" -m 512M -display none -serial file:"$o" \
		-no-reboot $@ >/dev/null 2>&1 &
	pid=$!

	n=0
	while [ "$n" -lt 500 ]; do
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

# --- UEFI, x86_64 -----------------------------------------------------------

say "a UEFI machine boots the disc"
uefi=$(boot_until qemu-system-x86_64 'Idling' -bios "$OVMF" -cdrom "$ISO")

if echo "$uefi" | grep -qa 'ReconOS kernel'; then
	echo "$(echo "$uefi" | grep -oaE 'ReconOS kernel [0-9.]+' | head -1)"
	pass=$((pass + 1))
else
	echo "DID NOT BOOT"
	echo "$uefi" | tail -10 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# --- BIOS, x86_64 -----------------------------------------------------------

say "a machine with no UEFI boots the disc"
bios=$(boot_until qemu-system-x86_64 'Idling' -cdrom "$ISO" -boot order=d)

if echo "$bios" | grep -qa 'ReconOS kernel' &&
   echo "$bios" | grep -qaE 'firmware +: BIOS'; then
	echo "$(echo "$bios" | grep -oaE 'ReconOS kernel [0-9.]+' | head -1), over BIOS"
	pass=$((pass + 1))
else
	echo "DID NOT BOOT"
	echo "$bios" | sed -n '/stage2/,$p' | head -10 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# The disc was read as a disc, not as a disk that happened to work.
#
# A disc has no GPT. Stage 2 identifies the medium from its ISO 9660 primary
# volume descriptor and then reads the EFI boot image's location out of the El
# Torito catalogue -- the same entry the firmware follows. If that dispatch
# broke and the GPT reader ran instead, this boot would simply stop, so the
# check is that stage 2 *said* which reader it used.
say "and read it as a disc, not as a disk"
if echo "$bios" | grep -qaE '^esp: disc, block [0-9]+' &&
   echo "$bios" | grep -qa 'stage2 ok, drive 0xe0'; then
	echo "$(echo "$bios" | grep -oaE 'esp: disc, block [0-9]+' | head -1)"
	pass=$((pass + 1))
else
	echo "FAILED -- the boot catalogue was not what it read"
	echo "$bios" | grep -aE 'stage2 ok|esp:' | head -4 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# --- UEFI, aarch64 ----------------------------------------------------------
#
# The same disc, and that is the assertion. One image carries both loaders and
# both kernels; a disc that boots only the architecture it was built on is a
# disc somebody has to choose between at download time.
say "and the other architecture boots it too"
arm=$(boot_until qemu-system-aarch64 'Idling' -M virt -cpu cortex-a57 \
	-bios "$AAVMF" -cdrom "$ISO")

if echo "$arm" | grep -qa 'ReconOS kernel' &&
   echo "$arm" | grep -qaE 'architecture +: aarch64'; then
	echo "$(echo "$arm" | grep -oaE 'ReconOS kernel [0-9.]+' | head -1), on aarch64"
	pass=$((pass + 1))
else
	echo "DID NOT BOOT"
	echo "$arm" | tail -10 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# The handoff, on every one of them.
#
# The disc paths go through the same relocation trampoline the stick paths do,
# and the trampoline is the last thing that touches the registers before the
# kernel starts. It got this wrong twice while it was being written -- once per
# architecture -- and both times the kernel's own check is what said so.
say "and each one hands over a clean machine"
clean=0
for t in "$uefi" "$bios" "$arm"; do
	echo "$t" | grep -qaE 'a clean handoff *: pass' && clean=$((clean + 1))
done

if [ "$clean" -eq 3 ]; then
	echo "3 of 3, no register left holding anything"
	pass=$((pass + 1))
else
	echo "FAILED -- $clean of 3 handed over clean"
	for t in "$uefi" "$bios" "$arm"; do
		echo "$t" | grep -aE 'arrived holding|clean handoff' |
			head -2 | sed 's/^/      /'
	done
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: the disc boots on both firmwares and both architectures"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
