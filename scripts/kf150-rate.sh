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
#   ./scripts/kf150-rate.sh <before-commit> [boots-per-side]
#
# It builds both kernels once, copies the two binaries aside, and alternates.
# It does not rebuild between boots, because a rebuild between samples is a
# third variable.
set -u
cd "$(dirname "$0")/.."

BEFORE=${1:?give the commit to compare against, e.g. HEAD~1}
N=${2:-60}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The lock, taken for the same reason verify-kernel.sh takes it: this builds the
# tree, and a matrix running beside it would be reporting on a mixture.
LOCKFILE=${RECON_TREE_LOCK:-/tmp/reconos-kernel-tree.lock}
exec 9>"$LOCKFILE"
if ! flock -n 9; then
	echo "Something else is building or booting this tree. Refusing to start."
	exit 2
fi

build_into() {
	local out=$1
	make -C kernel ARCH=x86_64 >/dev/null 2>&1 || return 1
	cp kernel/build/x86_64/reconos-kernel.elf "$out"
}

echo "building the current tree"
build_into "$WORK/after.elf" || { echo "the current tree does not build"; exit 2; }

echo "building $BEFORE"
git stash list >/dev/null    # nothing; noted so the next reader knows we do not stash
if ! git worktree add --detach "$WORK/before" "$BEFORE" >/dev/null 2>&1; then
	echo "could not make a worktree at $BEFORE"
	exit 2
fi
( cd "$WORK/before" && make -C kernel ARCH=x86_64 >/dev/null 2>&1 ) \
	|| { echo "$BEFORE does not build"; exit 2; }
cp "$WORK/before/kernel/build/x86_64/reconos-kernel.elf" "$WORK/before.elf"
git worktree remove --force "$WORK/before" >/dev/null 2>&1

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
	case $(one_boot "$WORK/before.elf" "$WORK/b$i.log") in
	stall) before_stall=$((before_stall + 1)); cp "$WORK/b$i.log" "$PWD/kf150-before-$i.log" ;;
	ok)    before_ok=$((before_ok + 1)) ;;
	*)     before_other=$((before_other + 1)) ;;
	esac
	case $(one_boot "$WORK/after.elf" "$WORK/a$i.log") in
	stall) after_stall=$((after_stall + 1)); cp "$WORK/a$i.log" "$PWD/kf150-after-$i.log" ;;
	ok)    after_ok=$((after_ok + 1)) ;;
	*)     after_other=$((after_other + 1)) ;;
	esac
	printf '\r  %3d/%d   before %d stalled   after %d stalled  ' \
		"$i" "$N" "$before_stall" "$after_stall"
done
echo
echo
printf '  %-8s %3d stalled, %3d finished, %3d neither\n' "$BEFORE" \
	"$before_stall" "$before_ok" "$before_other"
printf '  %-8s %3d stalled, %3d finished, %3d neither\n' "current" \
	"$after_stall" "$after_ok" "$after_other"
echo
echo "  A log is kept for every stall, as kf150-{before,after}-N.log, because"
echo "  the thread state printed in it is what distinguishes the two candidates"
echo "  and a rate with no log behind it cannot be diagnosed later."
