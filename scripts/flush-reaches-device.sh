#!/usr/bin/env bash
#
# Does a flush leave the machine?
#
# --- Why this exists, and what it replaces ---
#
# The question a filesystem needs answered is not "did the kernel's flush
# function return success" -- virtio-blk used to return success having issued
# nothing -- but "did a durability command actually reach the device".
#
# scripts/crash-test.sh cannot answer it. That harness kills QEMU and reads what
# survived, and killing a process does not lose the data it has already written:
# those bytes are in the host's page cache and are written out by the host
# regardless. Run it with `nocache`, which makes QEMU discard guest flushes
# entirely, and it returns the same clean result. A test that passes identically
# whether or not flushes are honoured is not testing flushes.
#
# So this asks the emulated controller instead. QEMU's trace points fire when a
# device model receives a command, which is the far side of the boundary the
# kernel is responsible for: a flush that was never issued cannot appear here,
# and a flush that was issued cannot fail to.
#
# --- What it still cannot tell you ---
#
# That the host, or a physical disk, honours the flush once it has it. That is
# below this boundary and no test run on this machine reaches it.

set -u
cd "$(dirname "$0")/.."

ARCH=${1:-x86_64}
OUT=kernel/build/flush
mkdir -p "$OUT"

IMG="$OUT/flush.img"
LOG="$OUT/trace.log"
CONSOLE="$OUT/console.log"

fail=0

# Each driver, the device that carries it, and the trace point that fires when
# the emulated controller is handed a durability command.
#
# AHCI is counted by ATA opcode: QEMU traces every command on the bus, and
# FLUSH CACHE EXT is 0xea. Matching the opcode rather than the word "flush" is
# deliberate -- it is the number the driver actually puts on the wire.
run_one() {
	name=$1 devargs=$2 tracept=$3 pattern=$4

	rm -f "$IMG" "$LOG" "$CONSOLE"
	dd if=/dev/zero of="$IMG" bs=1M count=8 status=none

	if [ "$ARCH" = aarch64 ]; then
		qemu=(qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic
		      -kernel kernel/build/aarch64/reconos-kernel.img)
	else
		qemu=(qemu-system-x86_64 -m 512M -nographic -no-reboot
		      -kernel kernel/build/x86_64/reconos-kernel.elf)
	fi

	# The workload idles when it is finished, so it is always killed; that is
	# expected, and the message the shell prints about it is not a failure.
	# In a subshell, so the shell reports the expected kill to nowhere rather
	# than to this script's stderr, where it reads like a failure.
	# shellcheck disable=SC2086
	# `exec 2>/dev/null` first: the shell that waits on a process killed by a
	# signal is the one that reports it, so the message has to be silenced in
	# the waiting shell rather than on the command.
	( exec 2>/dev/null; timeout -s KILL 40 "${qemu[@]}" \
		-append "durability=$name" \
		-drive "file=$IMG,format=raw,if=none,id=d0" \
		$devargs \
		--trace "enable=$tracept" --trace "file=$LOG" \
		>"$CONSOLE" 2>&1 ) || true

	markers=$(python3 scripts/check-markers.py "$IMG" 2>&1)
	count=$(echo "$markers" | awk '{print $2}')
	flushes=$(grep -c -- "$pattern" "$LOG" 2>/dev/null) || flushes=0

	case "$markers" in
	ok*) ;;
	*)
		echo "  $name: the workload did not write markers: $markers"
		fail=$((fail + 1))
		return
		;;
	esac

	case "$count$flushes" in
	*[!0-9]*|"")
		echo "  $name: could not count markers or flushes"
		echo "      (markers='$count' flushes='$flushes')"
		fail=$((fail + 1))
		return
		;;
	esac

	# One durability command per marker, at least. More is fine -- a driver
	# may flush once on the way in -- but fewer means a marker was reported
	# durable without a flush having left the machine for it.
	if [ "$flushes" -lt "$count" ]; then
		echo "  $name: $count markers written, but only $flushes flush"
		echo "      commands reached the controller. A flush the kernel"
		echo "      believes it issued did not arrive."
		fail=$((fail + 1))
	else
		echo "  $name: $count markers, $flushes flush commands at the controller"
	fi
}

echo "Flush commands arriving at the emulated controller, $ARCH:"
echo

run_one nvme0n1 "-device nvme,serial=recon0,drive=d0" \
	pci_nvme_flush_ns pci_nvme_flush_ns

if [ "$ARCH" != aarch64 ]; then
	run_one sata0 "-device ahci,id=ahci0 -device ide-hd,drive=d0,bus=ahci0.0" \
		ide_bus_exec_cmd "cmd 0xea"
fi

echo

if [ "$fail" -eq 0 ]; then
	echo "Every marker the kernel called durable had a durability command"
	echo "issued for it, and that command reached the device model."
	exit 0
fi

echo "$fail driver(s) reported durability they did not ask the device for."
exit 1
