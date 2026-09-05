#!/usr/bin/env bash
#
# A disk image to point QEMU at, so the block layer has something to talk to.
#
# Sixty-four megabytes of zeroes: big enough that a partition table and a
# filesystem will fit later, small enough to create in a moment, and *not* a
# round number of any driver's buffer size, so an off-by-one in block counting
# has somewhere to show itself.
#
# Regenerated rather than committed. A disk image in version control is a
# hundred megabytes of noise in every clone, and this takes a second.

set -eu

cd "$(dirname "$0")/.."
out=${1:-kernel/build/disk.img}

mkdir -p "$(dirname "$out")"

if [ ! -f "$out" ]; then
	dd if=/dev/zero of="$out" bs=1M count=64 status=none
	echo "created $out (64MB)"
else
	echo "$out already exists"
fi
