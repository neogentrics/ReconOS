#!/usr/bin/env bash
#
# Boot the kernel against each partition fixture and compare what it read with
# what the tool that wrote the disk says is on it.
#
# The comparison is the point. The kernel prints `table` and `slice` lines; the
# .expected files were written by hand from the layouts the fixtures were built
# from, and sgdisk and sfdisk agree with them. Three descriptions of the same
# disk, from three directions, and the kernel is the only one of them written
# here.

set -u

cd "$(dirname "$0")/.."

FIX=kernel/build/fixtures
bash scripts/make-partition-fixtures.sh "$FIX" >/dev/null

ARCH=${1:-x86_64}
fail=0

# What the kernel printed about the device it was given, reduced to the same
# shape as the expectation: the scheme and count, then one line per slice.
extract() {
	sed -e 's/\r$//' "$1" \
	  | awk '/^table /  { print $3, $4 }
	         /^slice /  { print $3, $4, $5 }'
}

run_one() {
	local img=$1 expected=$2 label=$3
	local log="$FIX/$label.log"

	if [ "$ARCH" = aarch64 ]; then
		timeout 45 qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
			-nographic -kernel kernel/build/aarch64/reconos-kernel.img \
			-drive "file=$img,format=raw,if=none,id=d0" \
			-device nvme,serial=recon0,drive=d0 >"$log" 2>&1
	else
		timeout 45 qemu-system-x86_64 -m 512M -nographic -no-reboot \
			-kernel kernel/build/x86_64/reconos-kernel.elf \
			-drive "file=$img,format=raw,if=none,id=d0" \
			-device nvme,serial=recon0,drive=d0 >"$log" 2>&1
	fi

	extract "$log" > "$FIX/$label.got"

	printf '%-26s' "$label"

	if diff -q "$expected" "$FIX/$label.got" >/dev/null 2>&1; then
		echo "matches what wrote it"
	else
		echo "DIFFERS"
		echo "    expected            |  the kernel read"
		diff --side-by-side --width=70 "$expected" "$FIX/$label.got" \
			| sed 's/^/    /'
		fail=$((fail + 1))
	fi
}

echo "reading fixtures on $ARCH:"
run_one "$FIX/gpt.img"    "$FIX/gpt.expected"    "gpt"
run_one "$FIX/mbr.img"    "$FIX/mbr.expected"    "mbr"
run_one "$FIX/hybrid.img" "$FIX/hybrid.expected" "hybrid-mbr"

echo
if [ "$fail" -eq 0 ]; then
	echo "all three read the same as the tools that wrote them."
	exit 0
fi

echo "$fail of 3 differ."
exit 1
