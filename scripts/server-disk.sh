#!/bin/sh
#
# A disk with ReconOS installed on it, for testing the server role's volume.
#
# --- Why this exists ---
#
# Most of what this role does needs no disk: the web server, the resolver, the
# clock and the guard all run from a diskless boot. Four things do not, and they
# are exactly the four that are easy to leave untested --
#
#   * serving a file off the volume at all;
#   * `/console.css`, without which the console renders unstyled;
#   * an upload landing somewhere, rather than being accepted and dropped;
#   * and whether any of it **survives a reboot**.
#
# That last one had never been tested before 17 September, because the disk it
# needs kept disappearing. `/tmp` is shared with the other sessions on this
# machine and is cleaned without warning -- a 16 GB image and every reference
# boot log went with it twice in one day, and the second time the loss sent a
# diagnosis down the wrong path for four boots.
#
# So the image lives in `kernel/build/`, which is git-ignored and nobody else's,
# and this script makes it again when it is not there.
#
# **Regenerated rather than committed**, for the reason `make-test-disk.sh`
# gives: an image in version control is gigabytes of noise in every clone.
#
# --- Why sixteen gigabytes ---
#
# Not a round number chosen for comfort. At one gigabyte the installer answers
#
#     there is room for ReconOS but not for a separate partition to install
#     programs into
#
# and lays down nothing. Sixteen is what `install-onto-test.sh` uses, so it is
# the size the installer is actually exercised against. The file is sparse where
# the filesystem allows it.
#
# Usage: scripts/server-disk.sh [path]

set -eu

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
cd "$root"

img=${1:-kernel/build/server-volume.img}
kernel=kernel/build/x86_64/reconos-kernel.elf

if [ -f "$img" ]; then
	echo "$img is already there -- delete it to start again"
	exit 0
fi

#
# Built for the server role, immediately before it is used.
#
# `ROLE` is a stamp and **the last build wins**: a `make ROLE=workstation` for
# any reason leaves a workstation kernel in `kernel/build/`, and a workstation
# installs and boots perfectly while never serving anything. That cost four
# boots on 17 September and is VF-023.
#
echo "building the server role"
make -C kernel ARCH=x86_64 ROLE=server >/dev/null

[ -f "$kernel" ] || { echo "the kernel did not build" >&2; exit 1; }

mkdir -p "$(dirname "$img")"
truncate -s 16G "$img"

echo "installing onto $img"
log=$(mktemp)
trap 'rm -f "$log"' EXIT INT TERM

#
# The installer does not power the machine off when it finishes; it carries on
# and boots what it just wrote. So this is bounded and the timeout is the
# ordinary ending rather than a failure -- what decides success is the line the
# installer prints, which is checked below.
#
timeout 200 qemu-system-x86_64 -m 512M -nographic -no-reboot \
	-kernel "$kernel" -append "install-onto=nvme0n1" \
	-drive "file=$img,format=raw,if=none,id=d0" \
	-device nvme,serial=i0,drive=d0 >"$log" 2>&1 || true

if tr -d '\r' < "$log" | grep -q "installed: table, EFI filesystem"; then
	echo "installed:"
	tr -d '\r' < "$log" | grep -iE "^installer:|^  installed:" | sed 's/^/  /'
	echo
	echo "boot it with:"
	echo "  qemu-system-x86_64 -m 1024 -smp 2 \\"
	echo "    -kernel $kernel \\"
	echo "    -drive file=$img,format=raw,if=none,id=d0 \\"
	echo "    -device virtio-blk-pci,drive=d0 \\"
	echo "    -netdev user,id=n0,hostfwd=tcp:127.0.0.1:8080-:80 \\"
	echo "    -device virtio-net-pci,netdev=n0 -display none -serial mon:stdio"
else
	echo "the installer did not report a finished install:" >&2
	tr -d '\r' < "$log" | tail -20 >&2
	rm -f "$img"
	exit 1
fi
