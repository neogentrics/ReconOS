#!/usr/bin/env bash
#
# Cuts the power at an arbitrary moment and asks what survived.
#
# --- What this is for ---
#
# Every crash-consistency scheme a filesystem can use rests on one property:
# that a write which has been flushed is on the medium before a write issued
# afterwards. Journalling rests on it. Copy-on-write rests on it. Careful write
# ordering *is* it.
#
# That property is normally assumed, and ReconFS is about to be designed on top
# of whichever version of it this kernel actually has. So it is measured first.
#
# The kernel writes numbered markers, one per block, flushing after each. This
# kills QEMU -- SIGKILL, no shutdown, no flush of anything the host was holding
# -- at a random moment, then reads the image and checks:
#
#   1. the markers present form an unbroken run from zero
#      A gap means a later write reached the medium before an earlier one, and
#      the kernel cannot promise ordering.
#
#   2. no marker is torn
#      Each block carries its own number at both ends. A block whose two ends
#      disagree was half-written, which decides whether a design may put two
#      facts in one block and rely on them agreeing.
#
# --- What it cannot tell you ---
#
# QEMU is not a disk. Killing the process models losing power to the *machine*
# with a device that honours flush perfectly; it does not model a drive with a
# volatile cache that lies, which is the case real hardware gets wrong. A pass
# here means the kernel and the driver are ordering correctly. It does not mean
# a cheap SSD will.
#
# Said out loud because a green result here could otherwise be read as a
# guarantee about hardware this has never run on.

set -u

cd "$(dirname "$0")/.."

ROUNDS=${1:-20}
ARCH=${2:-x86_64}
OUT=kernel/build/crash
mkdir -p "$OUT"

# A fresh image per round, never reused.
#
# The first version of this reused one file and recreated it with dd at the top
# of each round, and it reported the kernel writing markers out of order --
# a suffix surviving, with the first hundred and fifty missing. That is not a
# shape an ordering failure can produce, which is what made it worth chasing
# rather than believing.
#
# The cause was here. `kill -9` returns as soon as the signal is *sent*; the
# process is still alive for a moment afterwards, and its in-flight writes were
# landing in the file the next round had just recreated. Two rounds sharing one
# file, with the loser's writes arriving after the winner's dd.
#
# So: one file per round, and the process is waited for until it is genuinely
# gone. Recorded rather than quietly fixed, because a harness bug that presents
# as a kernel bug is the most expensive kind -- the conclusion it invites is
# "this kernel cannot order writes", and that conclusion would have been drawn
# on the eve of designing a filesystem that depends on the opposite.
gaps=0
torn=0
empty=0

for round in $(seq 1 "$ROUNDS"); do
	IMG="$OUT/round-$round.img"
	rm -f "$IMG"
	dd if=/dev/zero of="$IMG" bs=1M count=8 status=none

	# Somewhere between a moment after the disk is found and a moment before
	# the marker run could finish. Swept rather than fixed, so the cut lands
	# in a different place each time -- a single fixed delay tests one
	# instant, and the instant that matters is the one nobody chose.
	ms=$(( 900 + (round * 137) % 2200 ))

	if [ "$ARCH" = aarch64 ]; then
		timeout -s KILL 30 qemu-system-aarch64 -M virt -cpu cortex-a72 \
			-m 512M -nographic \
			-kernel kernel/build/aarch64/reconos-kernel.img \
			-append "durability=nvme0n1" \
			-drive "file=$IMG,format=raw,if=none,id=d0" \
			-device nvme,serial=recon0,drive=d0 >"$OUT/run.log" 2>&1 &
	else
		timeout -s KILL 30 qemu-system-x86_64 -m 512M -nographic -no-reboot \
			-kernel kernel/build/x86_64/reconos-kernel.elf \
			-append "durability=nvme0n1" \
			-drive "file=$IMG,format=raw,if=none,id=d0" \
			-device nvme,serial=recon0,drive=d0 >"$OUT/run.log" 2>&1 &
	fi

	qemu_pid=$!

	# The cut. SIGKILL rather than SIGTERM: a shutdown would let QEMU flush,
	# which is exactly the thing being tested and exactly what a power cut
	# does not do.
	sleep "$(awk "BEGIN{print $ms/1000}")"
	kill -9 "$qemu_pid" 2>/dev/null

	# Until it is actually gone. `kill` returns when the signal is sent, not
	# when the process has stopped writing, and the difference is what made
	# the first version of this harness accuse the kernel.
	wait "$qemu_pid" 2>/dev/null
	while kill -0 "$qemu_pid" 2>/dev/null; do
		sleep 0.05
	done

	result=$(python3 scripts/check-markers.py "$IMG")
	rm -f "$IMG"
	status=${result%% *}

	case "$status" in
	ok)      ;;
	empty)   empty=$((empty + 1)) ;;
	gap)     gaps=$((gaps + 1)); echo "  round $round (cut at ${ms}ms): $result" ;;
	torn)    torn=$((torn + 1)); echo "  round $round (cut at ${ms}ms): $result" ;;
	*)       echo "  round $round (cut at ${ms}ms): $result" ;;
	esac

	printf '.'
done

echo
echo

if [ "$empty" -eq "$ROUNDS" ]; then
	echo "every round wrote nothing -- the cut is landing before the disk is"
	echo "found, so this measured nothing. Raise the delay."
	exit 1
fi

echo "$ROUNDS cuts on $ARCH: $gaps out of order, $torn torn, $empty too early."

if [ "$gaps" -eq 0 ] && [ "$torn" -eq 0 ]; then
	echo
	echo "Every surviving run of markers was an unbroken prefix, and no block"
	echo "was half-written. On this kernel, through this driver, under this"
	echo "emulator, a flush orders writes and a block does not tear."
	exit 0
fi

exit 1
