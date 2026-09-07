#!/bin/sh
# Reads a FAT32 volume built by somebody else's tools, then breaks it four ways
# and requires each break to be caught.
#
# The first half is the claim: the kernel can read a volume mkfs.vfat laid out
# and mcopy filled. The second half is what makes the first half mean anything.
# This project has shipped three checks that reported success while doing
# nothing at all, and the rule that came out of it is that **a check must be
# shown a fault and watched to fail before any pass it reports is believed.**
#
# The four damages target four *different* refusals, so passing them all means
# the reader distinguishes them rather than having one catch-all:
#
#   signature -> not a FAT volume
#   size      -> not a FAT volume        (a boot sector that is not this device's)
#   fats      -> the tables disagree
#   chain     -> a chain that cannot be true
#
# Usage: scripts/fat32-reads-foreign.sh [x86_64|aarch64]
set -eu

ARCH=${1:-x86_64}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

if [ "$ARCH" = aarch64 ]; then
	QEMU="qemu-system-aarch64 -M virt -cpu cortex-a72"
	KERNEL=kernel/build/aarch64/reconos-kernel.img
else
	QEMU=qemu-system-x86_64
	KERNEL=kernel/build/x86_64/reconos-kernel.elf
fi

# Built, not merely looked for. A script that only checks a binary exists
# will happily test one compiled before the change it is meant to prove --
# which is how a security check came to be silently absent (BG-133).
make -C kernel ARCH="$ARCH" >/dev/null 2>&1 || true
[ -f "$KERNEL" ] || { echo "the kernel did not build" >&2; exit 1; }

IMG=$(mktemp)
trap 'rm -f "$IMG"' EXIT INT TERM

run() {
	# SIGTERM, not SIGKILL: the kernel idles forever after its self-tests, so
	# every run ends at its timeout, and a killed child makes the shell print
	# "Killed" into the middle of the results. QEMU exits cleanly on TERM.
	# 45 seconds, not 90. The kernel idles forever after its self-tests, so
	# every run costs its whole timeout whatever it found -- five runs times
	# two architectures is fifteen minutes of the verification rig at 90, and
	# the work itself takes seconds. A timeout is a bound on the hang, not a
	# budget for the test.
	timeout 45 $QEMU -m 512M -nographic -no-reboot \
		-kernel "$KERNEL" -append "fat32=nvme0n1" \
		-drive "file=$1,format=raw,if=none,id=d0" \
		-device nvme,serial=fat0,drive=d0 2>&1 | tr -d '\r'
}

fresh() {
	./scripts/make-fat-fixture.sh "$IMG" 64 >/dev/null
}

pass=0
fail=0

# --- the claim -------------------------------------------------------------

printf '%-44s' "  reads a volume mkfs.vfat wrote"
fresh
out=$(run "$IMG")
if echo "$out" | grep -q '4 of 4 checks passed'; then
	echo "$(echo "$out" | grep -oE 'mount .*: pass .*' | head -1)"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$out" | sed -n '/^fat32:/,$p' | head -10 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# --- and what makes it mean something --------------------------------------
#
# Each damage names the phrase the reader must produce. Matching the *specific*
# refusal rather than "some failure" is deliberate: a reader that returned
# FAT32_ERR_IO for everything would pass a test that only asked whether it
# complained.

check_damage() {
	mode=$1
	want=$2

	printf '%-44s' "  refuses: $mode"
	fresh
	python3 scripts/fat32-damage.py "$IMG" --damage "$mode" >/dev/null
	out=$(run "$IMG")

	if echo "$out" | grep -q "$want"; then
		echo "caught -- $want"
		pass=$((pass + 1))
	elif echo "$out" | grep -q '4 of 4 checks passed'; then
		echo "NOT CAUGHT -- it read the damaged volume as if it were fine"
		fail=$((fail + 1))
	else
		echo "caught, but not the way it should have been"
		echo "$out" | sed -n '/^fat32:/,$p' | head -8 | sed 's/^/      /'
		fail=$((fail + 1))
	fi
}

check_damage signature "this is not a FAT volume"
check_damage size      "this is not a FAT volume"
check_damage fats      "the two allocation tables disagree"
check_damage chain     "a cluster chain that cannot be true"

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: read a foreign volume, and refused four broken ones"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
