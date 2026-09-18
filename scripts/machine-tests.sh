#!/bin/sh
#
# Boot a real machine and check what it does.
#
# --- Why there is a second runner ---
#
# `scripts/server-tests.sh` runs every suite in `CMakeLists.txt` in about a
# second, on a host. Those suites are worth having and they are exhaustive
# about what they can reach.
#
# **They were all green on a server that answered twelve requests and then went
# silent for the rest of the boot.** That is VF-034, and the way it was found
# is the reason this file exists: a measurement for an unrelated feature
# happened to need a thirteenth connection. Nothing in the repository would
# otherwise have asked for one, and the fault had been there for at least a
# version and probably many.
#
# A host has thousands of descriptors, a real TCP stack, and a `recv` that
# blocks properly. The target has sixteen connections, a `recv` that answers 0
# with nothing buffered, and a volume. Everything in the gap between those two
# lists is invisible to the suites and this is where it gets checked.
#
# --- What it costs ---
#
# About a minute: a kernel build if anything changed, a boot, and half a minute
# of checks. That is why it is a separate command rather than part of the suite
# runner -- the suites run on every edit, and this runs before a version is
# called done.
#
# Usage:
#
#     scripts/machine-tests.sh            # with the volume, if it exists
#     scripts/machine-tests.sh --diskless # without, for the checks that do not
#                                         # need one
#
set -eu

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
cd "$root"

kernel=kernel/build/x86_64/reconos-kernel.elf
volume=kernel/build/server-volume.img
log=kernel/build/machine-tests.log
diskless=0

for arg in "$@"; do
	case "$arg" in
	--diskless) diskless=1 ;;
	*)
		#
		# An unknown argument is a question, not consent.
		#
		# The network session's own register carries this rule --
		# NW-009, a tool whose default is the side-effecting mode that
		# treated a misspelt flag as agreement. This one only boots a
		# virtual machine, and the rule is free to follow.
		#
		echo "usage: $0 [--diskless]" >&2
		exit 2
		;;
	esac
done

#
# A port nobody else is on.
#
# The other sessions on this machine run QEMU too, and two of them forwarding
# the same host port means one silently answers the other's checks. Walked
# rather than fixed, and the number that is finally used is printed, because a
# run that quietly used a different port than it reported is a run nobody can
# repeat.
#
port=18400
while [ "$port" -lt 18500 ]; do
	if command -v ss >/dev/null 2>&1; then
		ss -ltn 2>/dev/null | grep -q ":$port " || break
	else
		break
	fi
	port=$((port + 1))
done

echo "building the server role"
make -C kernel ARCH=x86_64 ROLE=server >/dev/null

[ -f "$kernel" ] || { echo "the kernel did not build" >&2; exit 1; }

drive=""
if [ "$diskless" -eq 0 ] && [ -f "$volume" ]; then
	drive="-drive file=$volume,format=raw,if=none,id=d0 -device virtio-blk-pci,drive=d0"
	echo "booting with $volume"
else
	echo "booting with no volume (the file checks will be skipped)"
fi

rm -f "$log"

# shellcheck disable=SC2086
qemu-system-x86_64 -m 1024 -smp 2 -kernel "$kernel" $drive \
	-netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$port-:80" \
	-device virtio-net-pci,netdev=n0 \
	-display none -serial "file:$log" >/dev/null 2>&1 &
qemu=$!

#
# Wait for the line that says it is listening, rather than for a number of
# seconds.
#
# A fixed sleep is a race that passes on a quiet machine and fails on a busy
# one, and this repository is shared with three other sessions running their
# own virtual machines. The bound is generous and the failure says which of the
# two things went wrong.
#
waited=0
while [ "$waited" -lt 90 ]; do
	if [ -f "$log" ] && tr -d '\r' < "$log" | grep -q "listening on"; then
		break
	fi
	if ! kill -0 "$qemu" 2>/dev/null; then
		echo "the machine stopped before it listened:" >&2
		tr -d '\r' < "$log" | tail -20 >&2
		exit 1
	fi
	sleep 1
	waited=$((waited + 1))
done

if [ "$waited" -ge 90 ]; then
	echo "the machine never said it was listening:" >&2
	tr -d '\r' < "$log" | tail -20 >&2
	kill "$qemu" 2>/dev/null || true
	exit 1
fi

#
# The boot token, read off the console the machine printed it on.
#
# Not guessed and not configured: it is new on every boot by design, and a test
# that needed it to be fixed would be a test asking for the guard to be
# weakened.
#
token=$(tr -d '\r' < "$log" | sed -n 's/.*Bearer \([0-9a-f]*\).*/\1/p' | head -1)

echo "listening on 127.0.0.1:$port after ${waited}s, token ${token:-none}"
echo

set +e
python3 "$here/machine-checks.py" "$port" "$token"
verdict=$?
set -e

echo
echo "--- what the machine said ---"
tr -d '\r' < "$log" | grep -E "^  (the |sockets)" | tail -12

kill "$qemu" 2>/dev/null || true
wait "$qemu" 2>/dev/null || true

exit "$verdict"
