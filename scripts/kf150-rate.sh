#!/usr/bin/env bash
#
# How often does a user program fail to finish? (KF-150)
#
# KF-150 is *about one boot in sixty, a user program does not finish, and
# nothing says why*, and its entry names two candidates it could not tell
# apart: a deadline measured in host time under load, or a thread that is
# genuinely never scheduled. It also says exactly how the next occurrence would
# say which:
#
#   > A run that shows *running, 40 ticks, 0 calls served* is a scheduler fault;
#   > one that shows *ready, 0 ticks* is a machine that never got to it.
#
# **KF-258 turned out to be the second of those**, word for word: a thread that
# was READY with zero ticks because both idle loops halted the processor without
# first asking whether anything had become runnable. So there is a prediction to
# test rather than a resemblance to assert.
#
# --- Why this is interleaved, and why it has to be -------------------------
#
# One in sixty is a rate, and a rate measured once is a number with no error
# bar. Running sixty boots of the fixed kernel and finding zero failures is
# consistent with the fix working and equally consistent with an idle machine at
# eleven at night -- the *other* candidate in the entry is load-sensitive, so
# "we ran it when nothing else was happening" is precisely the confound.
#
# So both kernels are booted alternately, in one pass, on one machine, in one
# set of conditions. The comparison is then internal: whatever the conditions
# were, both halves had them. That is how KF-148's original measurement was
# made and this is deliberately the same method.
#
#   ./scripts/kf150-rate.sh <before.elf> <after.elf> [boots-per-side]
#
# It alternates between two already-built kernels and never rebuilds, because a
# rebuild between samples is a third variable.
set -u

# --- Run from a copy of this file, because this file gets edited -----------
#
# **bash does not read a script into memory. It reads it incrementally,
# keeping a byte offset**, and parses each top-level command as it reaches it.
# Edit the file while it is running and every offset past the edit is wrong.
#
# Measured, 21 September 2026, and it cost the tail of a 28-minute run: 17
# lines were added near the top of this file while a 180-a-side measurement was
# at about iteration 40. All 360 boots completed correctly -- `one_boot` and
# the loop had been parsed long before -- and then bash went to read the
# summary block, which it had not reached yet, resumed from a stale offset and
# landed inside a quoted string:
#
#   scripts/kf150-rate.sh: line 223: unexpected EOF while looking for matching `"'
#
# This file is 207 lines. The damage is exactly bounded: **everything already
# parsed ran correctly, everything not yet parsed was destroyed.** The
# measurement survived and the report of it did not, which is the worst
# available split -- the expensive half is the one that lived.
#
# **Four sessions share this repository and edit each other's scripts.** A rule
# saying "do not edit a running script" is a rule nobody can follow, because
# the person editing is usually not the person running. So the script takes a
# copy of itself and runs that, and the original can be edited freely.
#
# The copy is made before anything else happens, so what runs is what was on
# disk at launch -- which is also the honest thing for a measurement to do.
if [ -z "${RECON_SELF_COPY:-}" ]; then
	RECON_SELF_DIR=$(cd "$(dirname "$0")" && pwd) || exit 2
	RECON_SELF_COPY=$(mktemp) || exit 2
	cat "$0" > "$RECON_SELF_COPY" || exit 2
	export RECON_SELF_COPY RECON_SELF_DIR
	exec bash "$RECON_SELF_COPY" "$@"
fi

cd "$RECON_SELF_DIR/.."

BEFORE_ELF=${1:?give the pre-fix kernel ELF}
AFTER_ELF=${2:?give the post-fix kernel ELF}
N=${3:-60}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"; rm -f "$RECON_SELF_COPY"' EXIT

[ -f "$BEFORE_ELF" ] || { echo "no kernel at $BEFORE_ELF"; exit 2; }
[ -f "$AFTER_ELF" ]  || { echo "no kernel at $AFTER_ELF"; exit 2; }

# **Two binaries in, no git.** The first version built them itself, from a
# worktree at a given commit -- and that cannot work here: this tree is a git
# worktree whose `.git` names a Windows path, and git under WSL, which is where
# the boots run, cannot follow it. Every git command fails while the build and
# the boots work perfectly, which is the combination that cost this project
# matrix 54.
#
# Taking two ELFs is better than working around it anyway. The interleaving and
# the counting are this script's job; deciding which two kernels to compare is
# the caller's, and a caller who builds them by hand knows what changed between
# them. Build them however you like and pass them in:
#
#   make -C kernel ARCH=x86_64 && cp kernel/build/x86_64/reconos-kernel.elf /tmp/after.elf
#   <revert the change>       && cp ...                                     /tmp/before.elf
#
# **They must differ only in the thing being measured.** Two kernels built from
# trees that differ in anything else make this a comparison of two trees rather
# than of one change, and the result would not say which.
#
# **The check below does not enforce that, and cannot.** It refuses two
# binaries that are identical, which is the mistake that is cheap to detect.
# The mistake that is expensive to make is the opposite one -- two binaries
# that differ by too much -- and *"differ only in X"* is not computable from
# two ELFs. Nothing here will catch it.
#
# It has already happened once, on 21 September, and it cost a whole run: the
# pre-fix ELF was an older binary that lacked KF-258's sleep probe entirely and
# carried the pre-`timer_sleep_ns` version of `logport.c` -- which is sleeping
# behaviour on the boot path, the very thing under measurement. Recorded as a
# withdrawal in KF-150.
#
# So this is the caller's job and the note is here rather than in the entry,
# because the entry is not what somebody reads before running this. **Build
# both arms from one tree in one sitting, and be able to name the diff in one
# line.** If you cannot, you are measuring two trees.
if cmp -s "$BEFORE_ELF" "$AFTER_ELF"; then
	echo "those are the same binary; this would measure nothing"
	exit 2
fi

# One boot. Prints "stall" if the program did not finish, "ok" otherwise.
#
# **Matched on the diagnostic rather than on the absence of something.** A boot
# that panics early also lacks the success line, and counting that as a stall
# would make an unrelated crash look like this fault. `user: it is ` is printed
# only by say_how_far_it_got, which runs only when a program missed its
# deadline.
one_boot() {
	local elf=$1 log=$2 waited=0

	# **Ended when the machine is finished, not when the clock runs out.**
	#
	# This used to be a bare `timeout 60`, and a boot does all of its work in
	# about three and a half seconds -- so every sample spent fifty-six
	# seconds watching a kernel that had nothing left to do. At a hundred and
	# eighty a side that is three hundred and sixty boots, twenty-two minutes
	# of measurement inside six hours of waiting, and the reason a run that
	# should fit in a coffee break did not fit in a night.
	#
	# The marker is the last line the init program prints. Two things make it
	# safe to stop there, and both were read out of the source rather than
	# assumed:
	#
	#   - `the heap: ` comes from userland/init/recon_init.c, which runs at
	#     the very end of the boot;
	#   - `user: it is ` comes from say_how_far_it_got in kernel/core/user.c,
	#     which runs with the kernel self-tests, well before init.
	#
	# The stall diagnostic therefore cannot be cut off by stopping at the
	# finish marker -- it is already in the log by then, or it is not coming.
	# A boot that stalls and never reaches init simply never matches, waits
	# out the full timeout, and is classified exactly as it was before. That
	# is the rare case and it is the one we are counting, so it is the right
	# one to leave slow.
	timeout 60 qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-kernel "$elf" >"$log" 2>&1 &
	local qpid=$!

	while kill -0 "$qpid" 2>/dev/null; do
		if grep -qa "the heap: " "$log" 2>/dev/null; then
			# A breath for any trailing line to land before the
			# machine goes away. Cheap, and it is the difference
			# between reading a log and racing one.
			sleep 0.3
			kill "$qpid" 2>/dev/null
			pkill -f -- "-kernel $elf" 2>/dev/null
			break
		fi
		sleep 0.1
		waited=$((waited + 1))
	done
	wait "$qpid" 2>/dev/null

	if grep -qa "user: it is " "$log"; then
		echo stall
	elif grep -qa "self-test" "$log"; then
		echo ok
	else
		echo other
	fi
}

before_stall=0; before_ok=0; before_other=0
after_stall=0;  after_ok=0;  after_other=0

printf 'alternating %d boots a side\n' "$N"
for i in $(seq 1 "$N"); do
	case $(one_boot "$BEFORE_ELF" "$WORK/b$i.log") in
	stall) before_stall=$((before_stall + 1)); cp "$WORK/b$i.log" "$PWD/kf150-before-$i.log" ;;
	ok)    before_ok=$((before_ok + 1)) ;;
	*)     before_other=$((before_other + 1)) ;;
	esac
	case $(one_boot "$AFTER_ELF" "$WORK/a$i.log") in
	stall) after_stall=$((after_stall + 1)); cp "$WORK/a$i.log" "$PWD/kf150-after-$i.log" ;;
	ok)    after_ok=$((after_ok + 1)) ;;
	*)     after_other=$((after_other + 1)) ;;
	esac
	printf '\r  %3d/%d   before %d stalled   after %d stalled  ' \
		"$i" "$N" "$before_stall" "$after_stall"
done
echo
echo
printf '  %-8s %3d stalled, %3d finished, %3d neither\n' "before" \
	"$before_stall" "$before_ok" "$before_other"
printf '  %-8s %3d stalled, %3d finished, %3d neither\n' "after" \
	"$after_stall" "$after_ok" "$after_other"
echo
echo "  A log is kept for every stall, as kf150-{before,after}-N.log, because"
echo "  the thread state printed in it is what distinguishes the two candidates"
echo "  and a rate with no log behind it cannot be diagnosed later."
echo

# **Did this run have the power to tell you anything?**
#
# The "before" half is the control, and if IT did not stall then the fault was
# never reproduced and the run says nothing about the fix. Zero against zero
# reads like a pass and is not one -- it is the same shape as KF-208, which
# KF-150's own entry cites: the broken kernel passes twelve boots in a row, so
# passing is what a fix and a non-fix both produce.
#
# The arithmetic is cheap and belongs here rather than in somebody's head. At a
# rate of one in sixty, the chance of a clean sweep with the fault fully
# present is (59/60)^N -- 36% at sixty boots, 13% at a hundred and twenty, 4.7%
# at a hundred and eighty. Sixty a side was never enough, and this says so
# rather than letting a green-looking table be quoted.
if [ "$before_stall" -eq 0 ]; then
	chance=$(python3 -c "print(f'{(59/60)**$N*100:.0f}')" 2>/dev/null || echo "?")
	echo "  THIS RUN MEASURED NOTHING, and the control is how you can tell."
	echo
	echo "  The pre-fix kernel did not stall either, so the fault was never"
	echo "  reproduced and nothing here bears on whether the fix works. At a"
	echo "  rate of one in sixty, $N boots come back clean ${chance}% of the time"
	echo "  with the fault entirely present."
	echo
	echo "  To distinguish, the BEFORE half has to stall. Run it again with"
	echo "  180 a side, where a clean control is under 5% likely -- and if the"
	echo "  control is still clean at that, the rate is wrong rather than the"
	echo "  fix being good."
	exit 2
fi
