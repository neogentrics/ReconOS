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
cd "$(dirname "$0")/.."

BEFORE_ELF=${1:?give the pre-fix kernel ELF}
AFTER_ELF=${2:?give the post-fix kernel ELF}
N=${3:-60}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

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
	local elf=$1 log=$2
	timeout 60 qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-kernel "$elf" >"$log" 2>&1
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
