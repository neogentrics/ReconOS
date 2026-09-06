#!/usr/bin/env bash
#
# Cuts the power inside a rename and asks what survived.
#
# --- What this is for ---
#
# docs/RECONFS.md lists this first: *cut the power inside a rename and assert
# the target is old-complete or new-complete.* It is the guarantee the desktop's
# registry writer is waiting on -- write a temporary file, rename it over the
# real one, and know that a machine losing power mid-save leaves the old
# settings rather than an empty file.
#
# The machine runs `reconfs-crash=<device>`, which replaces one file by rename
# over and over and never stops. This kills it at a swept moment and then reads
# the image with scripts/reconfs-check.py -- a second implementation of the
# format, in another language, written from the header rather than from the
# kernel's own reader. That distinction is the whole value: the kernel checking
# its own image proves the reader and the writer share their assumptions.
#
# It has already earned that. The first run of this workload found BG-088: every
# commit was writing its superblock into dead space, so the second copy still
# held the empty volume the format left. Nothing inside the kernel could see it
# -- mounting reads the second copy from the right place and finds a valid,
# older superblock, which is what a healthy volume looks like.
#
# --- What must be true after a cut ---
#
#   the image mounts               a superblock validates
#   `settings` resolves            to exactly one thing, and that thing reads
#   the two derivations agree      the tree and the owner table, block for block
#
# What is *allowed* is a leftover `settings.tmp`: the workload creates it in one
# commit and renames it in the next, so a cut between them leaves it behind.
# That is a temporary file, not a fault, and the guarantee never promised
# otherwise.

set -u

cd "$(dirname "$0")/.."

ROUNDS=${1:-12}
ARCH=${2:-x86_64}
OUT=kernel/build/rename-crash
mkdir -p "$OUT"

qemu_pid=

# An interrupted run must not leave emulators holding disk images open. The
# absence of this is how a pid bug stayed invisible in the sibling harness --
# the orphans were the evidence.
cleanup() {
	[ -n "$qemu_pid" ] && kill -9 "$qemu_pid" 2>/dev/null
	return 0
}
trap cleanup EXIT INT TERM

checked=0
bad=0
survivors=0

# --- Before any round is believed, the checker is shown two faults --------
#
# A checker that has only ever been run on good images has never been observed
# to do anything. This makes one image, breaks it two ways, and requires the
# checker to report each -- if it does not, the run stops here rather than
# producing a page of reassuring dots.
#
# It is also how the first version of this control was caught being useless: it
# reached into superblock A while B was live, damaged a block nothing pointed
# at, and the checker correctly called the volume healthy.
echo "  proving the checker first:"
PROVE="$OUT/prove.img"
rm -f "$PROVE"
dd if=/dev/zero of="$PROVE" bs=1M count=16 status=none

if [ "$ARCH" = aarch64 ]; then
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel kernel/build/aarch64/reconos-kernel.img \
		-append "reconfs-crash=nvme0n1 reconfs-rounds=8" \
		-drive "file=$PROVE,format=raw,if=none,id=d0" \
		-device nvme,serial=recon0,drive=d0 >"$OUT/prove.log" 2>&1 &
else
	qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-kernel kernel/build/x86_64/reconos-kernel.elf \
		-append "reconfs-crash=nvme0n1 reconfs-rounds=8" \
		-drive "file=$PROVE,format=raw,if=none,id=d0" \
		-device nvme,serial=recon0,drive=d0 >"$OUT/prove.log" 2>&1 &
fi

qemu_pid=$!
sleep 6
kill -9 "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
while kill -0 "$qemu_pid" 2>/dev/null; do sleep 0.05; done
qemu_pid=

if ! python3 scripts/reconfs-check.py "$PROVE" settings >/dev/null 2>&1; then
	echo "      the image the control needs was not consistent to begin with"
	rm -f "$PROVE"
	exit 1
fi

for how in checksum unallocated torn; do
	cp "$PROVE" "$PROVE.broken"
	python3 scripts/reconfs-check.py --damage "$how" "$PROVE.broken" >/dev/null

	if python3 scripts/reconfs-check.py "$PROVE.broken" settings >/dev/null 2>&1; then
		echo "      the checker passed an image broken on purpose ($how)"
		echo "      -- nothing below this line would have meant anything"
		rm -f "$PROVE" "$PROVE.broken"
		exit 1
	fi

	echo "      $how: caught"
	rm -f "$PROVE.broken"
done

rm -f "$PROVE"
echo

for round in $(seq 1 "$ROUNDS"); do
	IMG="$OUT/round-$round.img"
	rm -f "$IMG"
	dd if=/dev/zero of="$IMG" bs=1M count=16 status=none

	# Swept, so the cut lands somewhere different each time. A single fixed
	# delay tests one instant, and the instant that matters is the one nobody
	# chose.
	ms=$(( 1500 + (round * 211) % 2600 ))

	# QEMU directly, with nothing wrapped around it: `$!` has to be the
	# emulator. Wrapping it in `timeout` makes `$!` the wrapper, and killing
	# a wrapper leaves the emulator running -- which is how the sibling
	# harness reported twenty-eight power cuts having cut nothing.
	if [ "$ARCH" = aarch64 ]; then
		qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
			-kernel kernel/build/aarch64/reconos-kernel.img \
			-append "reconfs-crash=nvme0n1" \
			-drive "file=$IMG,format=raw,if=none,id=d0" \
			-device nvme,serial=recon0,drive=d0 \
			>"$OUT/run.log" 2>&1 &
	else
		qemu-system-x86_64 -m 512M -nographic -no-reboot \
			-kernel kernel/build/x86_64/reconos-kernel.elf \
			-append "reconfs-crash=nvme0n1" \
			-drive "file=$IMG,format=raw,if=none,id=d0" \
			-device nvme,serial=recon0,drive=d0 \
			>"$OUT/run.log" 2>&1 &
	fi

	qemu_pid=$!

	sleep "$(awk "BEGIN{print $ms/1000}")"
	kill -9 "$qemu_pid" 2>/dev/null

	# Until it is actually gone. `kill` returns when the signal is sent, not
	# when the process has stopped writing.
	wait "$qemu_pid" 2>/dev/null
	while kill -0 "$qemu_pid" 2>/dev/null; do
		sleep 0.05
	done
	qemu_pid=

	result=$(python3 scripts/reconfs-check.py "$IMG" settings 2>&1)
	status=${result%% *}
	rm -f "$IMG"

	case "$status" in
	ok)
		checked=$((checked + 1))
		survivors=$((survivors + 1))
		;;
	unreadable|inconsistent)
		# A volume that will not read at all is only acceptable if the
		# cut landed before the first commit ever reached the medium.
		# The workload commits within milliseconds, and the sweep starts
		# well after that, so this is a failure.
		checked=$((checked + 1))
		bad=$((bad + 1))
		echo "  round $round (cut at ${ms}ms):"
		echo "$result" | sed 's/^/      /'
		;;
	*)
		echo "  round $round (cut at ${ms}ms): the check did not run: $result"
		;;
	esac

	printf '.'
done

echo
echo

if [ "$checked" -ne "$ROUNDS" ]; then
	echo "only $checked of $ROUNDS rounds were checked at all."
	echo
	echo "This is not a result. A round whose check did not run says nothing"
	echo "about crash consistency, and a harness that counts it as a pass is"
	echo "worse than one that crashes."
	exit 1
fi

echo "$ROUNDS cuts inside a rename on $ARCH: $bad inconsistent."

if [ "$bad" -eq 0 ]; then
	echo
	echo "Every image mounted, resolved 'settings' to exactly one readable"
	echo "object whose contents were one whole version -- the round number"
	echo "agreeing in three places and the checksum over the whole payload --"
	echo "and had its tree and its owner table agree block for block, judged"
	echo "by a reader that shares no code with the kernel."
	exit 0
fi

exit 1
