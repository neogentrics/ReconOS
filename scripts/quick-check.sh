#!/bin/bash
#
# The fast loop: does this kernel still boot and pass its own tests?
#
# `verify-kernel.sh` is the thing that decides whether a change is good enough
# to push. It boots about sixty machines, formats real disk images, cuts the
# power twenty times mid-transaction, installs onto a disk and boots the disk it
# made -- and it takes the better part of an hour. That is the right price for a
# push and the wrong price for a question like "did that compile and still
# work", which is most questions.
#
# WHY THIS EXISTS, said plainly, because it is a lesson rather than a
# convenience. Using the hour-long run as the iteration loop does not only waste
# an hour. It pushes you to batch changes between runs -- and on 10 September
# that is exactly how two fixes landed together and the wrong one got the credit
# for fixing a fault, until an interleaved measurement said otherwise. **A slow
# check does not just cost time; it changes how much you are willing to change
# at once.**
#
#   scripts/quick-check.sh              both architectures
#   scripts/quick-check.sh x86_64       one of them
#
# Ten seconds or so. It is not a substitute for the matrix and does not pretend
# to be: no disks, no firmware, no installer, no power cuts. What it does cover
# is the thing that breaks most often, which is the kernel's own self-tests.

set -u

cd "$(dirname "$0")/.." || exit 1

# --- one thing builds this tree at a time -------------------------------------
#
# This script's first act is `make`. Run while verify-kernel.sh is going, that
# rewrites the kernel underneath it -- and the matrix then reports on a mixture
# of two binaries, which describes no kernel that ever existed.
#
# It has happened three times on this project. Twice it was impatience; the
# third time it was *this file*, written to save time, whose first action is a
# build. Good intentions are not a lock, so here is a lock.
#
# Both scripts take it. `flock -n` fails rather than waits, because a fast check
# that silently blocks for fifty minutes is worse than one that says why.
LOCKFILE=${RECON_TREE_LOCK:-/tmp/reconos-kernel-tree.lock}
exec 9>"$LOCKFILE"
if ! flock -n 9; then
	echo "Something else is building or booting this tree -- probably"
	echo "scripts/verify-kernel.sh. Waiting for it rather than racing it:"
	echo "  $(ps -eo pid,etime,cmd | grep -E 'verify-kernel|quick-check' | grep -v grep | head -2)"
	exit 2
fi

ONLY=${1:-all}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

failures=0
FAILED=()

# --- the assertions -----------------------------------------------------------
#
# The same four the matrix makes, and they are four rather than one for reasons
# each of which cost something:
#
#   - that any test ran at all, because a kernel that never booted prints
#     nothing and "no failures" is true of nothing;
#   - that none failed;
#   - that it did not panic *after* the tests, because a bad exception vector
#     shows up in the idle loop, after everything has passed;
#   - that it reached the end, because a boot that stops half way answers every
#     other question the way a good one does. That was KF-143.
#
# The last is free here: the kernel is asked to power itself off, so reaching
# the end means the guest stopped on its own rather than being killed by the
# timeout. A machine that hangs comes back as exit 124 and is a failure.
verdict() {
	local name=$1 log=$2 rc=$3
	local ran fail

	ran=$(grep -ac ': \(pass\|FAIL\)' "$log")
	fail=$(grep -ac ': FAIL' "$log")

	printf '%-40s' "  $name"

	if [ "$ran" -eq 0 ]; then
		echo "NO OUTPUT -- $log"
		failures=$((failures + 1)); FAILED+=("$name"); return
	fi

	if [ "$fail" -ne 0 ]; then
		echo "$fail of $ran FAILED"
		grep -a ': FAIL' "$log" | sed 's/^/        /'
		grep -aE '^  (user|elf|addrspace|sched|vm): ' "$log" |
			head -6 | sed 's/^/        /'
		failures=$((failures + 1)); FAILED+=("$name"); return
	fi

	if grep -aq 'kernel fault\|PANIC\|panic' "$log"; then
		echo "$ran passed, then it panicked"
		grep -aA3 'kernel fault\|PANIC' "$log" | head -6 | sed 's/^/        /'
		failures=$((failures + 1)); FAILED+=("$name"); return
	fi

	if [ "$rc" -eq 124 ]; then
		echo "$ran passed, then it hung -- it never powered off"
		failures=$((failures + 1)); FAILED+=("$name"); return
	fi

	echo "$ran self-tests, all pass"
}

boot() {
	local name=$1; shift
	local log="$WORK/$(echo "$name" | tr ' ,' '__').log"
	local rc=0

	timeout 60 "$@" >"$log" 2>&1 || rc=$?
	tr -d '\r' < "$log" > "$log.clean" && mv "$log.clean" "$log"
	verdict "$name" "$log" "$rc"
}

# --- x86_64 -------------------------------------------------------------------

if [ "$ONLY" = all ] || [ "$ONLY" = x86_64 ]; then
	echo "x86_64"
	make -C kernel ARCH=x86_64 >"$WORK/build64.log" 2>&1 || {
		echo "  build FAILED"; tail -20 "$WORK/build64.log" | sed 's/^/      /'
		exit 1
	}
	K=kernel/build/x86_64/reconos-kernel.elf

	boot "one processor" \
		qemu-system-x86_64 -m 512M -nographic -no-reboot \
			-kernel "$K" -append poweroff

	# Two, because every "all pass" row in the matrix used to be a
	# single-processor run and a fault that only appears with more than one
	# was invisible to all of it. That is how KF-148 survived: a program
	# raced its own process attachment and started in the wrong address
	# space, about one boot in twenty, and nothing in the rig ever looked.
	boot "two processors" \
		qemu-system-x86_64 -m 512M -smp 2 -nographic -no-reboot \
			-kernel "$K" -append poweroff
fi

# --- aarch64 ------------------------------------------------------------------

if [ "$ONLY" = all ] || [ "$ONLY" = aarch64 ]; then
	echo "aarch64"
	make -C kernel ARCH=aarch64 >"$WORK/buildarm.log" 2>&1 || {
		echo "  build FAILED"; tail -20 "$WORK/buildarm.log" | sed 's/^/      /'
		exit 1
	}
	K=kernel/build/aarch64/reconos-kernel.img

	boot "one processor" \
		qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M \
			-nographic -no-reboot -kernel "$K" -append poweroff

	boot "two processors" \
		qemu-system-aarch64 -M virt -cpu cortex-a72 -smp 2 -m 512M \
			-nographic -no-reboot -kernel "$K" -append poweroff
fi

# --- and the rule that keeps arch.h meaningful --------------------------------

echo
if ! make -C kernel check-portable >"$WORK/portable.log" 2>&1; then
	sed -n '2,12p' "$WORK/portable.log" | sed 's/^/  /'
	failures=$((failures + 1)); FAILED+=("core/ is no longer portable")
fi

if [ "$failures" -eq 0 ]; then
	echo "quick check passed. This is not the matrix:"
	echo "  no disks, no firmware, no bootloader, no installer, no power cuts."
	echo "  Run scripts/verify-kernel.sh before pushing."
	exit 0
fi

echo "$failures check(s) failed:"
printf '  %s\n' "${FAILED[@]}"
exit 1
