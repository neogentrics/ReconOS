#!/usr/bin/env bash
#
# Boot the kernel every way it can be booted, and report every self-test.
#
# The reason this script exists rather than a paragraph in a commit message:
# "it works" is a claim about one boot path, and this kernel has eight. A change
# to the memory map can be right under GRUB and wrong under our own loader; a
# change to the exception vectors can be right on one processor and wrong on
# four. The only way to know is to run them all, every time, and read the
# counts rather than the absence of a crash.
#
#   scripts/verify-kernel.sh              every path
#   scripts/verify-kernel.sh x86_64       just one architecture's paths
#
# Needs: qemu, grub-mkrescue, xorriso, mtools, dosfstools, clang, lld, OVMF,
# AAVMF. Every one of them is packaged; none of them is linked into the kernel.

set -u

cd "$(dirname "$0")/.."
ROOT=$PWD
# One thing builds this tree at a time. See scripts/quick-check.sh for what
# happens without this: a run that reports on a mixture of two binaries.
LOCKFILE=${RECON_TREE_LOCK:-/tmp/reconos-kernel-tree.lock}
exec 9>"$LOCKFILE"
if ! flock -n 9; then
	echo "Something else is building or booting this tree. Refusing to start:"
	echo "  a run that shares the tree reports on no kernel in particular."
	exit 2
fi

WORK=$(mktemp -d)

# --- running the slow tests at the same time ----------------------------------
#
# Most of the hour this takes is seventeen sub-scripts, each of which boots
# several machines, formats disk images and cuts the power. They are independent
# processes that share nothing but the built kernel, so they can overlap.
#
# TWO THINGS THIS DOES NOT DO, both of which are the same mistake in different
# clothes -- a check that cannot report a failure.
#
# It does not put `check` in the background. `failures=$((failures + 1))` inside
# a background job runs in a *subshell*, so the increment is discarded when that
# subshell exits and the run reports green however many paths failed. The work
# overlaps; the verdicts stay in this shell.
#
# And it does not share disk images between jobs. Two boots writing one image
# produce a result about a machine that never existed. Every sub-script here
# either makes its own temporary image or writes to a directory of its own --
# flush-reaches-device.sh did not until today, and was the one that would have
# collided.
#
# Verdicts are consumed in the order they were launched, so the output is the
# same every run whatever order things finish in. A rig whose output moves is a
# rig whose diffs are noise.
JOBS=${JOBS:-4}
mkdir -p "$WORK/sub"

sub_launch() {
	local tag=$1; shift

	# Wait for a slot. Capped rather than unbounded: thirteen QEMUs at
	# 512MB each is six gigabytes of guest, and a machine that starts
	# swapping measures the swap rather than the kernel.
	while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n 2>/dev/null || break; done

	(
		"$@" >"$WORK/sub/$tag.out" 2>&1
		echo $? >"$WORK/sub/$tag.rc"
	) &
}

# Prints what the job printed and exits with what the job exited with, so a
# caller reads exactly as it did when this was a plain command substitution.
sub_out() {
	local tag=$1

	while [ ! -s "$WORK/sub/$tag.rc" ]; do
		sleep 0.2
	done

	cat "$WORK/sub/$tag.out"
	return "$(cat "$WORK/sub/$tag.rc")"
}

# --- showing why a path failed, without showing all of it ---------------------
#
# Every failure report here used to be `head -N`: the first dozen or twenty
# lines of a sub-script's output, indented. That is the right idea and it drops
# the answer whenever the failure is further down than N.
#
# **KF-250 is what that costs.** An installed-disk boot reported
# `the network stack : FAIL` and nothing else. `net_self_test` runs eight
# sub-tests and each prints its own diagnostic; every one of them was below the
# cut. So the register carries an entry saying the network stack failed, with no
# way to know which part, and fifteen further boots have not reproduced it. The
# one occurrence that could have answered it was truncated.
#
# So: the first N lines for context, **and then every line below that names a
# failure**, which is the half that was being thrown away. Still bounded -- a
# broken guest that prints failures for ever is capped -- but bounded on the
# lines that matter rather than on position.
#
#   show_failure "<output>" <context lines>
#
show_failure() {
	local out=$1 n=${2:-16}

	echo "$out" | head -"$n" | sed 's/^/      /'

	# Below the cut, the verdicts **and the lines above each one**.
	#
	# The first version of this took only lines matching FAIL and friends,
	# and on a worked example it surfaced `the network stack : FAIL` while
	# still dropping `net: udp checksum mismatch, wanted ... saw ...` on the
	# line under it -- so it recovered the summary that was already known
	# and lost the diagnostic that was the entire point.
	#
	# The kernel prints a sub-test's reason *before* the line that reports
	# the verdict, so context-before is the half that carries the answer.
	# `tail -n +N` starts after what was already shown, so nothing repeats.
	local rest
	rest=$(echo "$out" | tail -n +"$((n + 1))" |
	       grep -a -B 6 -E 'FAIL|FAILED|panic|refused|could not|did not' |
	       head -40)

	if [ -n "$rest" ]; then
		echo "      ... and below the first $n lines:"
		echo "$rest" | sed 's/^/      /'
	fi
}
# --- what a failing run leaves behind -----------------------------------------
#
# The work directory used to be deleted unconditionally, and with it every
# guest's serial output. What survived was the summary line -- "1 self-test(s)
# failed" -- which says that something went wrong and not *what*.
#
# That cost a real diagnosis: KF-164 turned this run red on the eight-processor
# path, and the only way to find out which check had failed was to reproduce the
# load by hand afterwards and hope. The output naming it had been written,
# printed, and thrown away.
#
# So a run that fails keeps its logs and says where they are. A run that passes
# still cleans up, because a green run's output is of no interest to anybody and
# leaving a directory per run behind is its own kind of mess.
KEEP=${KEEP:-$HOME/reconos-verify-failures}

keep_the_evidence() {
	rc=$?

	if [ "$rc" != 0 ] && [ -d "$WORK" ]; then
		stamp=$(date +%Y%m%d-%H%M%S)
		mkdir -p "$KEEP"

		if cp -r "$WORK" "$KEEP/$stamp" 2>/dev/null; then
			echo
			echo "The output of every guest in this run is in"
			echo "  $KEEP/$stamp"
			echo "which is kept because the run failed. A passing run"
			echo "deletes it."
		fi
	fi

	rm -rf "$WORK"
	exit "$rc"
}

trap keep_the_evidence EXIT

ONLY=${1:-all}
TIMEOUT=${TIMEOUT:-45}

OVMF_X64=/usr/share/ovmf/OVMF.fd
OVMF_ARM=/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd

passes=0
failures=0
skipped=0
declare -a FAILED_PATHS=()

# Runs QEMU, then reads the log rather than trusting the exit status. A kernel
# that hangs before printing anything and a kernel that panics both "exit"; only
# the self-test lines say which happened.
# EXPECT, when set before a call, is a string the log must also contain.
#
# It exists because of a specific way this harness could lie. The block
# self-test reports "pass" on a machine with no disk attached, which is correct
# -- a diskless machine must still boot -- but it means a driver that silently
# stopped finding disks would go on reporting eleven passes for ever. So a run
# that was *given* a disk is required to say it found one, and the counts alone
# are not enough.
check() {
	local name=$1; shift
	local log="$WORK/$(echo "$name" | tr ' /' '__').log"
	local want=${EXPECT:-}

	EXPECT=

	printf '%-46s' "$name"
	timeout "$TIMEOUT" "$@" >"$log" 2>&1

	# No end-of-line anchors. QEMU's serial console ends every line with a
	# carriage return as well as a newline, so a pattern anchored with $
	# matches nothing at all -- which reads exactly like a kernel that never
	# booted, and cost one confusing run to notice.
	local ran fail
	ran=$(grep -c ': \(pass\|FAIL\)' "$log")
	fail=$(grep -c ': FAIL' "$log")

	if [ "$ran" -eq 0 ]; then
		echo "NO OUTPUT -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
		return
	fi

	if [ "$fail" -ne 0 ]; then
		echo "$fail of $ran FAILED -- $log"
		grep ': FAIL' "$log" | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
		return
	fi

	# A panic can follow a clean run of the tests -- the idle loop is where
	# a bad exception vector shows up -- so the log is checked past them.
	if grep -q 'kernel fault\|PANIC' "$log"; then
		echo "$ran passed, then it panicked -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
		return
	fi

	if [ -n "$want" ] && ! grep -q "$want" "$log"; then
		echo "$ran passed, but never said '$want' -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
		return
	fi

	# And it has to have *finished*.
	#
	# Everything above is satisfied by a boot that stopped half way: some
	# passes, no failures, no panic, and the disk it was asked about already
	# named -- because the storage summary prints before the self-tests do.
	# A run that hung after five of fourteen tests reported "5 self-tests,
	# all pass", and there is no count to compare it against, because the
	# count legitimately differs between paths.
	#
	# Found when a lost increment made the scheduler test hang on one boot
	# in three, and every processor-count check in this file went on passing
	# through it. (KF-143)
	if ! grep -q 'Idling\.' "$log"; then
		echo "$ran passed, then it stopped before the end -- $log"
		tr -d "\r" < "$log" | tail -3 | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
		return
	fi

	echo "$ran self-tests, all pass"
	passes=$((passes + ran))
}

# The same, for a run that was given a disk: the first argument is the name the
# kernel must report having found.
#
# A function rather than a variable assignment in front of the call, because
# bash restores a prefix assignment after a *function* returns -- so the flag
# would survive into the next check and fail the diskless run for not finding a
# disk it was never offered.
check_for() {
	EXPECT=$1
	shift
	check "$@"
}

# Whether anything is actually on the screen, asked of QEMU rather than of the
# kernel.
#
# Every other display assertion here reads a line the kernel printed, and the
# kernel's own view of its framebuffer is exactly what cannot be trusted on a
# display whose pixels are guest memory: it wrote them, it read them back, they
# were there, and the host had never been told to look (GX-003).
#
# `scripts/screen-has-pixels.py` drives QEMU's monitor and counts non-black
# pixels in a screendump. It adds its own -serial, -monitor and -display, so the
# command passed in must not carry them.
# The same, plus a colour that must be on the screen.
#
# `check_screen` answers "is anything there". This answers "is *this* there",
# which is a different and harder question -- and the only one that can fail on
# a machine whose console already covers the panel. Used for the path that
# proves a program's own pixels, drawn through a mapping and presented with
# SYS_PRESENT, reach the display.
# The same again, plus a colour that must be *absent*.
#
# For the panel-ownership check. "Is the program's picture there" and "is the
# console drawing on top of it" are both true at once when the console writes
# into the middle of a program's screen, so a require-colour check alone passes
# on a kernel that has the fault. The forbidden colour is the console's own
# paper.
check_screen_without() {
	local name=$1 marker=$2 want=$3 forbid=$4; shift 4
	local log="$WORK/$(echo "$name" | tr ' /' '__').screen.log"

	printf '%-46s' "$name"

	if python3 "$ROOT/scripts/screen-has-pixels.py" --marker "$marker" 			--min-pixels 1000 --require-colour "$want" 			--forbid-colour "$forbid" -- "$@" >"$log" 2>&1; then
		echo "ok -- $(tail -n 1 "$log")"
	else
		echo "FAILED -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
	fi
}

check_screen_colour() {
	local name=$1 marker=$2 colour=$3; shift 3
	local log="$WORK/$(echo "$name" | tr ' /' '__').screen.log"

	printf '%-46s' "$name"

	if python3 "$ROOT/scripts/screen-has-pixels.py" --marker "$marker" 			--min-pixels 1000 --require-colour "$colour" 			-- "$@" >"$log" 2>&1; then
		echo "ok -- $(tail -n 1 "$log")"
	else
		echo "FAILED -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
	fi
}

check_screen() {
	local name=$1; shift
	local log="$WORK/$(echo "$name" | tr ' /' '__').screen.log"

	printf '%-46s' "$name"

	if python3 "$ROOT/scripts/screen-has-pixels.py" --marker "first screen" \
			--min-pixels 1000 -- "$@" >"$log" 2>&1; then
		echo "ok -- $(tail -n 1 "$log")"
	else
		echo "FAILED -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
	fi
}

# --- every processor, on both architectures ---------------------------------
#
# Asserted by the *count*, not by the machine booting. A kernel that starts none
# of the other processors boots perfectly and reports one, which is why the
# check that "it still boots with -smp 4" is worth nothing here.
#
# Both numbers are required to match the machine QEMU was told to build: found,
# because discovery can silently stop early, and online, because starting them
# is the part that fails. They are different failures and a check that asks only
# one of them cannot tell which happened.
#
# And ticks, because of what 9b cost on both architectures. On aarch64 the
# secondaries came online with the firmware's exception vectors and took not one
# tick between them; on x86_64 they came online with interrupts still masked
# from the trampoline and did exactly the same thing. Both report as healthy,
# online processors. Nothing but a tick count tells them from working ones.
check_cpus() {
	local label=$1 n=$2
	shift 2
	# The processor count belongs in the name. Without it each sweep
	# overwrote the last, and the log for a failing count survived only if
	# that count happened to be the last one tried (KF-201).
	local log="$WORK/cpus_${label}_$n.log"

	printf '%-46s' "  $label, $n processors"

	timeout "$TIMEOUT" "$@" >"$log" 2>&1

	local found
	found=$(sed -e 's/\r$//' "$log" |
		sed -n 's/^  found *: \([0-9]*\), \([0-9]*\) online.*$/\1 \2/p' |
		head -1)

	if [ "$found" != "$n $n" ]; then
		echo "FAILED -- expected \"$n $n\", got \"${found:-nothing}\""
		sed -e 's/\r$//' "$log" | tail -6 | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("$label $n processors")
		return
	fi

	# Every processor must have been preempted at least once by the end. The
	# summary is printed late, after the self-tests, so an idle processor has
	# had a hundred ticks' worth of opportunity.
	local idle_ticks
	# **Anything may follow the value.** (KF-259)
	#
	# Three expressions in this file read a number out of the boot report and
	# two others were anchored the same way -- the processor count and the
	# shootdown count. All three are loosened together, because the fault was
	# never about the tick line: it is that **a boot report is an interface**,
	# read by a person and parsed by seventeen sub-scripts, and adding a word
	# to a kprintf is an ABI change to every one of them.
	#
	# Nothing in this tree says which lines are load-bearing. Until something
	# does, the cheap defence is to capture what is wanted and stop caring
	# what follows it.
	#
	# This was anchored `ticks$`, which is the same thing as saying "the
	# kernel may never add a word to this line" -- and on 18 September the
	# kernel added `, idle-for-this-cpu` to exactly this line, for a good
	# reason: the summary could not distinguish a thread about to run from
	# one that can never be chosen, which had made a scheduling fault look
	# like a timer fault for a day.
	#
	# Six SMP paths then failed with "not one of them was preempted" about
	# processors that had been preempted perfectly well. The anchor was
	# right about what it wanted and wrong to insist the line end there.
	idle_ticks=$(sed -e 's/\r$//' "$log" |
		sed -n 's/^  thread [0-9]* *: idle-[0-9]*, running, \([0-9]*\) ticks.*$/\1/p' |
		sort -n | head -1)

	if [ "$n" -gt 1 ] && { [ -z "$idle_ticks" ] || [ "$idle_ticks" -eq 0 ]; }; then
		echo "FAILED -- online, and not one of them was preempted"
		sed -e 's/\r$//' "$log" | grep -aE "^  thread|^  cpu " |
			head -8 | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("$label $n processors, no ticks")
		return
	fi

	# And, on x86_64 with more than one processor, that changing a mapping
	# actually told the others.
	#
	# `invlpg` reaches the processor that runs it and no other, and there is
	# no broadcast form -- so every other processor can go on using a
	# translation this one has just replaced. Not a crash: a read of memory
	# that is no longer what the page tables say, on a processor that never
	# faults. Checkpoint 9b created that gap by waking the others.
	#
	# Asserted as *sent and answered*, because the two failures are
	# different: none sent means the mechanism is not running at all, and
	# some unanswered means a processor is still holding a mapping that no
	# longer exists.
	local sent unanswered
	sent=$(tr -d '\r' < "$log" |
		sed -n 's/^  shootdowns *: [0-9]* live invalidations, \([0-9]*\) sent.*/\1/p' | head -1)
	unanswered=$(tr -d '\r' < "$log" |
		sed -n 's/^  shootdowns *: .*, \([0-9]*\) unanswered.*$/\1/p' | head -1)

	if [ -n "$sent" ] && [ "$n" -gt 1 ]; then
		if [ "$sent" -eq 0 ] || [ "${unanswered:-1}" -ne 0 ]; then
			echo "FAILED -- $sent shootdowns sent, ${unanswered:-?} unanswered"
			failures=$((failures + 1))
			FAILED_PATHS+=("$label $n processors, shootdown")
			return
		fi
	fi

	# And that the self-tests passed, which this did not ask until 10
	# September 2026.
	#
	# Every "N self-tests, all pass" row in this rig is a *single-processor*
	# run: check() boots with no -smp at all, and this function is the only
	# thing that ever passes one. It asserted counts, ticks and shootdowns --
	# so a self-test that fails only on more than one processor was invisible
	# to the whole matrix.
	#
	# KF-148 is what that cost. A user program raced its own process
	# attachment and started in the kernel's address space, failing about one
	# boot in eight at two processors, and every path in this rig went on
	# reporting green because no multi-processor run ever looked at a test
	# result. The same shape as KF-143: a check that cannot fail is not a
	# check.
	local failed_tests
	failed_tests=$(tr -d '\r' < "$log" | grep -acE ': +FAIL' || true)

	if [ "${failed_tests:-0}" -ne 0 ]; then
		echo "FAILED -- $n online, and $failed_tests self-test(s) failed"
		# The failures first, and on their own. This was one grep with an
		# alternation and a `head -10`, and the identity block matched the
		# second branch first -- so a real failure printed ten lines of
		# "architecture : x86_64" and never reached the FAIL line it was
		# called to show. KF-201 had to be read out of the raw log.
		tr -d '\r' < "$log" | grep -aE ': +FAIL' |
			head -6 | sed 's/^/      /'
		tr -d '\r' < "$log" | grep -aE '^net: |^  [a-z].*: ' |
			head -6 | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("$label $n processors, self-tests")
		return
	fi

	echo "$n online, idle thread took ${idle_ticks:-0} ticks${sent:+, $sent shootdowns}"
	passes=$((passes + 1))
}

# --- randomness, and both ways a machine can supply it -----------------------
#
# Two assertions, and the second is the one that keeps the first honest.
#
# That the pool seeds itself is the feature. That it seeds itself *from the
# hardware generator where there is one, and from timing where there is not*, is
# what stops the feature being a machine that quietly always uses the weaker
# source -- and the weaker source is the one nothing downstream can detect.
#
# QEMU's default x86_64 processor has no RDRAND and `-cpu max` does, so the two
# runs exercise genuinely different code. Without the second, the hardware path
# would be compiled and never executed on any machine in this rig.
check_random() {
	local label=$1
	shift
	local want=$1
	shift
	local log="$WORK/random_$label.log"

	printf '%-46s' "  entropy $label"

	timeout "$TIMEOUT" "$@" >"$log" 2>&1

	local state
	state=$(tr -d '\r' < "$log" | sed -n 's/^  state *: \(.*\)$/\1/p' | head -1)

	if [ "${state#ready}" = "$state" ]; then
		echo "FAILED -- the pool never seeded: ${state:-no summary at all}"
		failures=$((failures + 1))
		FAILED_PATHS+=("entropy $label")
		return
	fi

	if ! tr -d '\r' < "$log" | grep -q "^  hardware *: $want"; then
		echo "FAILED -- expected hardware '$want'"
		tr -d '\r' < "$log" | grep -aA3 '^Randomness' | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("entropy $label")
		return
	fi

	if ! tr -d '\r' < "$log" | grep -q '^  randomness *: pass'; then
		echo "FAILED -- the generator's own test did not pass"
		failures=$((failures + 1))
		FAILED_PATHS+=("entropy $label")
		return
	fi

	echo "$state"
	passes=$((passes + 1))
}

# --- what the machine says about itself, and being able to switch it off ----
#
# Three assertions, and the third is the one that makes the first two mean
# something. Reading a table and reporting numbers out of it proves the parser
# ran; it does not prove the parser was *right*. Turning the machine off does:
# the value written comes out of the vendor's own bytecode, it is different on
# every chipset, and a wrong one does nothing at all.
check_acpi() {
	local log="$WORK/acpi.log"

	printf '%-46s' "  reads the machine's own description"

	timeout "$TIMEOUT" qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-kernel "$X64_ELF" >"$log" 2>&1

	local aml smbios
	aml=$(tr -d '\r' < "$log" | sed -n 's/^  aml *: \(.*devices.*\)$/\1/p' | head -1)
	smbios=$(tr -d '\r' < "$log" | grep -c '^  machine      : ')

	# A partial namespace says so, and saying so must not be mistaken for
	# success: the parser stops rather than guessing at an opcode it does
	# not know, and a machine where that happens has devices it never saw.
	if tr -d '\r' < "$log" | grep -q 'the namespace is partial'; then
		echo "FAILED -- the AML walk stopped early"
		tr -d '\r' < "$log" | grep -a '  aml ' | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("acpi")
		return
	fi

	if [ -z "$aml" ] || [ "$smbios" -eq 0 ]; then
		echo "FAILED -- ${aml:-no AML line}, $smbios machine line(s)"
		failures=$((failures + 1))
		FAILED_PATHS+=("acpi")
		return
	fi

	echo "$aml"
	passes=$((passes + 1))
}

check_power_off() {
	printf '%-46s' "  and can turn the machine off"

	# The whole assertion is in the exit code. `timeout` returns 124 when it
	# had to kill the guest, and anything else means the guest stopped on
	# its own -- which, for a kernel whose normal end is an idle loop that
	# runs for ever, can only be the power-off path.
	timeout 25 qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-kernel "$X64_ELF" -append poweroff >"$WORK/off.log" 2>&1
	local rc=$?

	if [ "$rc" -eq 124 ]; then
		echo "FAILED -- it was still running when the clock ran out"
		tr -d '\r' < "$WORK/off.log" | tail -3 | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("power off")
		return
	fi

	# And it must have got there by the intended route rather than by
	# falling over: a panic also ends the guest.
	if tr -d '\r' < "$WORK/off.log" | grep -qE 'kernel fault|PANIC'; then
		echo "FAILED -- it stopped, but not by powering off"
		tr -d '\r' < "$WORK/off.log" | tail -4 | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("power off")
		return
	fi

	# **Asked the other way round, and that is the point.**
	#
	# This used to also grep the whole log for `power:`, because the four
	# ways power_off_or_say_why reports failure all begin with it. Then
	# checkpoint 24 added a *passing* self-test that prints
	#
	#   power: 2 wakeup(s) in 197 ms, against 19 a fixed tick would have cost
	#
	# and the check went red on every run, on a machine that had powered off
	# exactly as asked. It had also stopped being able to tell the two apart,
	# which is the half that earned it a number (KF-207).
	#
	# A narrower pattern would have worked today. **Any check that asks
	# whether a string is absent from the whole log is one another subsystem
	# can break by printing.** So this asks the positive question instead: on
	# success the guest dies *inside* power_off(), which makes "Powering off."
	# the last thing in the log; on every failure one of those four lines
	# follows it. Nothing printed earlier can change that.
	local last
	last=$(tr -d '\r' < "$WORK/off.log" | grep -v '^[[:space:]]*$' | tail -1)

	if [ "$last" != "Powering off." ]; then
		echo "FAILED -- it stopped, but not by powering off"
		echo "      the log ends: $last" | sed 's/$//'
		tr -d '\r' < "$WORK/off.log" | tail -4 | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("power off")
		return
	fi

	echo "the guest stopped itself, exit $rc"
	passes=$((passes + 1))
}

# And restarting, which is a different mechanism on every machine.
#
# `power_restart` reaches the architecture first and the FADT's reset register
# second, and the two architectures share none of it: x86_64 writes 0xCF9 and
# pulses the 8042, aarch64 calls PSCI SYSTEM_RESET. So both are asked, because
# a green run on one says nothing about the other -- and until this existed,
# the whole path had one caller (SYS_POWER) that no test can invoke without
# ending the guest, which is a path nobody has run.
#
# `-no-reboot` is what makes it assertable: with it, a guest that resets exits
# instead of coming back round, and the exit is something a script can read.
check_restart() {
	local label=$1
	shift
	printf '%-46s' "  $label"

	timeout 25 "$@" -append restart >"$WORK/restart.log" 2>&1
	local rc=$?

	if [ "$rc" -eq 124 ]; then
		echo "FAILED -- it was still running when the clock ran out"
		tr -d '\r' < "$WORK/restart.log" | tail -3 | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("$label")
		return
	fi

	if tr -d '\r' < "$WORK/restart.log" | grep -qE 'kernel fault|PANIC'; then
		echo "FAILED -- it stopped, but by falling over"
		tr -d '\r' < "$WORK/restart.log" | tail -4 | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("$label")
		return
	fi

	# The positive question, for KF-207's reason. On success the guest dies
	# inside power_restart and "Restarting." is the last thing in the log;
	# on every failure one of the `restart:` lines follows it.
	local last
	last=$(tr -d '\r' < "$WORK/restart.log" | grep -v '^[[:space:]]*$' | tail -1)

	if [ "$last" != "Restarting." ]; then
		echo "FAILED -- it stopped, but not by restarting"
		echo "      the log ends: $last"
		failures=$((failures + 1))
		FAILED_PATHS+=("$label")
		return
	fi

	echo "the guest reset itself, exit $rc"
	passes=$((passes + 1))
}

# Boots against one partition fixture and compares what the kernel read with
# what wrote the disk. A different kind of check from the ones above: those
# count self-tests the kernel ran on itself, and this one holds the kernel's
# answer up against a second opinion from outside it.
check_table() {
	local label=$1 img=$2 expected=$3
	local log="$WORK/table_$label.log"
	local got="$WORK/table_$label.got"

	printf '%-46s' "  reads $label the same as its tool"

	timeout "$TIMEOUT" qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-kernel "$X64_ELF" \
		-drive "file=$img,format=raw,if=none,id=t0" \
		-device nvme,serial=recon0,drive=t0 >"$log" 2>&1

	sed -e 's/\r$//' "$log" \
	  | awk '/^table / { print $3, $4 } /^slice / { print $3, $4, $5 }' >"$got"

	if [ ! -s "$got" ]; then
		echo "read nothing at all -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("table $label")
		return
	fi

	if diff -q "$expected" "$got" >/dev/null 2>&1; then
		echo "matches"
		passes=$((passes + 1))
		return
	fi

	echo "DIFFERS -- $log"
	diff --side-by-side --width=64 "$expected" "$got" | sed 's/^/      /'
	failures=$((failures + 1))
	FAILED_PATHS+=("table $label")
}

skip() {
	printf '%-46s%s\n' "$1" "skipped: $2"
	skipped=$((skipped + 1))
}

# --- Build ------------------------------------------------------------------

echo "Building."
make -C kernel ARCH=x86_64  >/dev/null || { echo "x86_64 kernel build FAILED"; exit 1; }
make -C kernel ARCH=aarch64 >/dev/null || { echo "aarch64 kernel build FAILED"; exit 1; }
make -C kernel check-portable >/dev/null || { echo "core/ is no longer portable"; exit 1; }

# The two system call lists, which are duplicated on purpose and checked here
# because nothing else can check them. A call inserted anywhere but the end of
# the kernel's enumeration shifts every number after it, and a program built
# against the other header then reaches the next call along **and is told it
# succeeded**. Nothing faults and nothing logs; the first symptom is data.
python3 scripts/check-syscall-numbers.py || { echo "the system call numbers disagree"; exit 1; }

# And the loader's promise that recovery is always on the menu, which no boot
# here can test: the paths that used to drop it need firmware that misbehaves,
# and every firmware this script can reach behaves. KF-233.
python3 scripts/check-menu-recovery.py || { echo "recovery is not offered on every path"; exit 1; }

# And the README's badges, which are the first thing anybody sees and were
# wrong by 176 bugs and 33 versions on 15 September. Nobody had been careless;
# nothing was counting. A figure measured once by hand is a sentence, and a
# sentence cannot notice it has stopped being true.
python3 scripts/check-readme-badges.py || { echo "the README's badges do not match the tree"; exit 1; }

X64_ELF=$ROOT/kernel/build/x86_64/reconos-kernel.elf
ARM_IMG=$ROOT/kernel/build/aarch64/reconos-kernel.img

# A disk to attach, so the block layer is tested against something rather than
# reporting that it found nothing and calling that a pass.
#
# A fresh one per run. The block self-test restores every byte it borrows, so
# reusing an image would work -- and a test whose correctness depends on the
# previous run having tidied up is a test that hides the first failure to do so.
DISK=$WORK/disk.img
dd if=/dev/zero of="$DISK" bs=1M count=64 status=none

# A second one, for the path that attaches two disks at once. Its own file, not
# the same one twice: two devices backed by one image is a test of QEMU's
# locking rather than of this kernel.
DISK2=$WORK/disk2.img
dd if=/dev/zero of="$DISK2" bs=1M count=64 status=none

# force-legacy=false asks QEMU for virtio 1.0 on its memory-mapped bus, which
# still defaults to the pre-1.0 draft. That draft is a different protocol
# wearing the same name -- guest-endian configuration space, a queue set up by
# page number -- and this kernel implements 1.0 and refuses the other with a
# message rather than half-supporting both.
ARM_DISK=(-global virtio-mmio.force-legacy=false
          -drive "file=$DISK,format=raw,if=none,id=d0"
          -device virtio-blk-device,drive=d0)

# The same disk on the other architecture, over PCI. No flag needed: QEMU offers
# a *transitional* device here -- one that can speak either protocol -- and the
# driver decides by whether the 1.0 capability structures are published rather
# than by the identifier, which for a transitional device looks legacy.
X64_DISK=(-drive "file=$DISK,format=raw,if=none,id=d0"
          -device virtio-blk-pci,drive=d0)

# A GRUB rescue ISO. The same ISO boots on BIOS and on UEFI -- grub-mkrescue
# writes both an El Torito boot catalogue and an EFI system partition -- which
# is why two of the paths below differ only by whether -bios is passed.
make_iso() {
	mkdir -p "$WORK/iso/boot/grub"
	cp "$X64_ELF" "$WORK/iso/boot/reconos-kernel.elf"
	cat >"$WORK/iso/boot/grub/grub.cfg" <<-EOF
	set timeout=0
	set default=0
	menuentry "ReconOS" {
	    multiboot2 /boot/reconos-kernel.elf
	    boot
	}
	EOF
	grub-mkrescue -o "$WORK/reconos.iso" "$WORK/iso" >/dev/null 2>&1
}

echo
echo "x86_64"

# The PVH paths are the ones with no firmware at all, so they are also the only
# ones where the kernel has to place the device's registers itself. Everything
# else on this architecture arrives with the base address registers already
# assigned by SeaBIOS or OVMF.
# --- launch the slow ones now, read their verdicts later ----------------------
# --- the two that rewrite the tree, run alone and first ----------------------
#
# Both signing tests put a public key in `boot/src/signing_key.h` and rebuild
# the bootloader around it, because the key is *compiled in* -- which is the
# whole point of it, and is why they cannot simply be handed a file.
#
# **That makes them the only tests here that change what every other test
# builds from.** The loader they leave behind refuses any kernel that is not
# signed, and the install and menu tests do not sign theirs. Run beside them,
# those tests boot a medium whose loader declines to start the kernel on it.
#
# The symptom is nothing like the cause. The install fails, the target ends up
# empty, and the next check reports `init :: non DOS media` -- mtools being
# asked to read a filesystem that was never written. Three tests fail and none
# of them names a signature.
#
# It went unnoticed because it is a race that usually resolves the other way,
# and because a killed run makes it permanent: the key is removed by an EXIT
# trap, and a SIGKILL does not run traps. A run stopped that way leaves the key
# behind and every run afterwards fails the same three tests until somebody
# deletes it.
#
# So they go first, serially, and the loader is put back to a keyless build
# before anything else is started. (KF-166)
rm -f boot/src/signing_key.h

printf '%-46s' "  the signature tests, run on their own"
sig_out=$(sh scripts/signed-kernel-test.sh 2>&1); sig_rc=$?
bsig_out=$(sh scripts/bios-signed-test.sh 2>&1); bsig_rc=$?
echo "done; their results are reported below"

# Whatever they left, removed -- including after a failure, which does not run
# their traps either. Then the loader is rebuilt without a key, so that every
# test after this point gets the one it expects.
rm -f boot/src/signing_key.h
rm -f boot/build/x86_64/main.o boot/build/x86_64/BOOTX64.EFI
make -C boot ARCH=x86_64 >/dev/null 2>&1 || true

#
# Everything below that boots a machine of its own starts here and is collected
# where its result is printed. Nothing between the two depends on them, and the
# kernel they all use is already built.
sub_launch crash    bash scripts/crash-test.sh 6 x86_64
sub_launch rename   bash scripts/rename-crash-test.sh 6 x86_64
sub_launch beside   bash scripts/beside-others-test.sh x86_64
sub_launch plan     bash scripts/install-plan-test.sh x86_64
sub_launch onto     bash scripts/install-onto-test.sh x86_64
sub_launch e2e      bash scripts/install-then-boot-test.sh
sub_launch menu     bash scripts/boot-menu-test.sh
sub_launch rec      bash scripts/recovery-test.sh
for a in x86_64 aarch64; do
	sub_launch "flush_$a"    bash scripts/flush-reaches-device.sh "$a"
	sub_launch "fatread_$a"  bash scripts/fat32-reads-foreign.sh "$a"
	sub_launch "fatwrite_$a" bash scripts/fat32-writes-foreign.sh "$a"
done

check_for virtio0 "  PVH, direct kernel load" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF" \
		"${X64_DISK[@]}"

check_for virtio0 "  PVH, -cpu max" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -cpu max -kernel "$X64_ELF" \
		"${X64_DISK[@]}"

check "  PVH, no disk attached" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF"

# A display adapter with enough memory to be asked for every size.
#
# The adapter QEMU gives by default has 16 MB, which is 1920x1080 and no more
# -- so on every other path the mode sweep skips 4K, 5K and 8K for want of
# memory and says so. That is honest and it is not coverage: the sizes this
# kernel is meant to drive largest were the ones nothing ever tried.
#
# 256 MB is enough for 7680x4320 at four bytes a pixel with room to spare, so
# this path is the one where every shape in the sweep is actually set and read
# back -- portrait, ultra-wide, 4K and 8K included.
# And it must say it got there. Without this the path asks only the four
# ordinary questions -- did tests run, did any fail, did it panic, did it
# reach the end -- and every one of them is satisfied by a boot whose mode
# sweep skipped all seven shapes for want of memory. A passing run deletes
# the per-boot logs, so there would be nothing left to check afterwards
# either. Seven set and none skipped is the entire claim.
check_for "7 shape(s) set and read back, 0 skipped" \
	"  PVH, an adapter big enough for 8K" \
	qemu-system-x86_64 -m 1024M -nographic -no-reboot \
		-device VGA,vgamem_mb=256 -kernel "$X64_ELF"

# A program mapped the screen and the pixels are on it.
#
# Checked for what it *said*, for the same reason as the sweep above: the
# self-test returns true on a machine with no screen, deliberately, because
# most of this matrix has none. So a boot where /dev/fb0 does not exist, or
# where SYS_MAP quietly stopped working, satisfies every ordinary question --
# tests ran, none failed, it reached the end -- and says nothing.
check_for "both markers are on the screen" \
	"  PVH, a program draws on the screen" \
	qemu-system-x86_64 -m 1024M -nographic -no-reboot \
		-device VGA,vgamem_mb=256 -kernel "$X64_ELF"

# --- the second display backend ---------------------------------------------
#
# virtio-gpu, and these paths exist because one backend cannot show whether an
# interface abstracts anything.
#
# `display_ops` was shaped entirely by the Bochs adapter, whose framebuffer is a
# PCI aperture being scanned out continuously -- so a store is a pixel appearing
# and every consumer in the kernel was written on that assumption. virtio-gpu
# keeps its pixels in guest RAM the host cannot see until it is told, which made
# the assumption visible by breaking it.
#
# `-vga none` matters and is not tidiness: QEMU puts a Bochs adapter on the bus
# by default, so without it the kernel finds that one, drives it perfectly, and
# this path tests the backend it was meant to replace.
check_for "virtio-gpu" \
	"  PVH, virtio-gpu" \
	qemu-system-x86_64 -m 1024M -nographic -no-reboot \
		-vga none -device virtio-gpu-pci -kernel "$X64_ELF"

# That the mode came from the *device* rather than from a ladder of guesses.
#
# The Bochs adapter cannot be asked what its panel is, so `display_init` picks
# the largest size the adapter's memory can hold. That was the only answer
# available until a device could give a better one -- and then it went on being
# used, driving a host reporting 1280x800 at 5120x2880 and spending sixty
# megabytes to do it (GX-005). Asserted on the size, because "a mode was set" is
# satisfied by the wrong one.
check_for "reports its screen is 1280x800, and that is the mode it is in" \
	"  PVH, virtio-gpu takes the host's size" \
	qemu-system-x86_64 -m 1024M -nographic -no-reboot \
		-vga none -device virtio-gpu-pci -kernel "$X64_ELF"

# **And whether any of it is actually on the glass.**
#
# This is the one check in the matrix that the kernel cannot perform on itself,
# and the reason it exists is GX-003: on the first boot that drove a virtio-gpu,
# `a mode of our own`, `a screen to draw on` and `a C program ... its pixels are
# on the screen` all reported pass against a screen that was entirely black.
# Every one of them read the framebuffer back through the kernel's own eyes, and
# on this device those pages are ordinary memory -- so the read-back returned
# what had just been written whether or not the host had ever seen it.
#
# A check that cannot fail looks exactly like one that passes. This one asks
# QEMU instead, and it has been shown to fail: with the driver's flush stubbed
# to report success without sending anything, it reports nought non-black pixels
# on a kernel whose own self-tests all pass.

# **A program draws through its mapping and asks for it to be shown.**
#
# The path SYS_PRESENT exists for, and the one GX-003 left open: a mapping is
# the kernel getting out of the way, and on virtio-gpu getting out of the way
# means the pixels are never seen. The console worked; a program did not.
#
# Two things make this assert something. The screen is **2560x1600**, larger
# than the 1920x1200 the console bounds itself to, so the paint program's fill
# reaches glass the console never touches -- at the host's default 1280x800 the
# console covers the panel and repaints over the program, and the check would
# pass on a kernel where SYS_PRESENT did nothing at all.
#
# And it asserts a **colour**, not a pixel count: #2b3342 is the ground the
# paint program fills with. With the present call taken out of that program the
# screen still reports 2,304,000 non-black pixels, because the console is still
# drawing -- what disappears is the program's own colour, and 0 of it is what
# this check then reports.
# **The console stops drawing while a program owns the screen.**
#
# The second entry in docs/KERNEL-WANTS.md, found on 14 September by
# photographing a panel because the serial line said the program had succeeded
# and it had. A program maps /dev/fb0 and fills it; the kernel prints its next
# line; the console draws characters straight over the picture -- not all of it,
# only the cells it has text in, so what is left is the program's background
# showing round the edges of a block of kernel log.
#
# Asserted both ways round, because one way round is not enough: the first
# screen's own colour must be **present**, and the console's paper must be
# **absent**. With the claim disabled this screen still shows the init panel
# across 64% of the glass -- what appears on top of it is 2,126,664 pixels of
# console paper, and a check that only asked whether the program had drawn
# would pass.
#
# The Bochs adapter rather than virtio-gpu, and at 7680x4320: the console bounds
# itself to 1920x1200, so on a panel this size the two areas are distinguishable
# and the console's intrusion has somewhere to show.
check_screen_without "  PVH, the console leaves a program alone" \
	"the first screen is up" 141821:100000 0c0e10 \
	qemu-system-x86_64 -m 1024M -device VGA,vgamem_mb=256 \
		-kernel "$X64_ELF"

check_screen_colour "  PVH, a program presents what it mapped" \
	"drew on the screen and presented it" 2b3342:100000 \
	qemu-system-x86_64 -m 1024M -vga none \
		-device virtio-gpu-pci,xres=2560,yres=1600 -kernel "$X64_ELF"
check_screen "  PVH, virtio-gpu really shows pixels" \
	qemu-system-x86_64 -m 1024M -vga none -device virtio-gpu-pci \
		-kernel "$X64_ELF"

# The same assertion for the adapter that was always here, so that the check
# above is known to be measuring the display rather than the device: a rig that
# reports pixels only on the new backend is a rig with something else wrong
# with it.
check_screen "  PVH, the Bochs adapter really shows pixels" \
	qemu-system-x86_64 -m 1024M -device VGA,vgamem_mb=256 \
		-kernel "$X64_ELF"

# The table that recognises real graphics hardware, checked on a machine that
# has none.
#
# `core/intel_display.c` cannot be exercised against a Gen9 here -- QEMU
# emulates no Intel display engine -- so what runs in this matrix is the
# recognition: the ids it must claim, and the ids it must refuse. The refusals
# are the half worth asserting. Among them is the Gemini Lake host bridge, which
# sits in the same package as the graphics and in the same numbering.
#
# **Asserted on the count, not on the word pass.** The self-test returns true
# after checking nothing if the table is empty, exactly as a mode sweep that
# skips every shape reports green (KF-187). Eighteen models is the claim.
check_for "18 model(s) known, recognition and refusal both checked" \
	"  PVH, the graphics it can recognise" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF"

# And the AMD table, which is the one written against hardware in the room.
#
# 1002:73ff and 1002:164e are the two adapters in this project's desktop -- a
# Radeon RX 6600 on the bus and Raphael graphics in the processor package --
# read off that machine rather than recalled. The refusals include the USB
# controllers AMD puts on its own graphics cards, and a case that only the
# vendor rule can catch, which had to be constructed because every real
# identifier collision is refused by the class check first (GX-008).
check_for "13 model(s) known, recognition and refusal both checked" \
	"  PVH, the graphics cards it can recognise" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF"

# The arithmetic a Gen9 modeset would rest on, checked on machines that have
# no Gen9 -- which is every machine here.
#
# Every active and total in that display engine is stored as one *less* than
# the number it describes. A pipe told it is 1921 pixels wide accepts it and
# the panel looks almost right, so nothing catches the omission but a known
# answer. The vectors are computed from the field layouts, and two of them are
# Linux's own 640x480 test pattern, so they can be checked outside this tree.
#
# It also asserts that the two register blocks have not been confused -- the
# transcoder timings live at 0x6xxxx and the pipe and its planes at 0x7xxxx.
# That is not hypothetical tidiness: the first draft of this map put TRANSCONF
# at 0x60008, which is the horizontal sync register, and the pipe-enable bit
# would have gone into it on a laptop with no serial port.
check_for "the register map and its arithmetic are checked" \
	"  PVH, a mode expressed as numbers" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF"

# Two display adapters in one machine, and the report naming both.
#
# QEMU with `-device virtio-gpu-pci` and no `-vga none` gives a Bochs adapter
# *and* a virtio-gpu, which is the arrangement GX-002's parallel private-state
# array would have mis-addressed -- and it is the shape of this project's own
# desktop, which has a Radeon RX 6600 on the bus and Raphael graphics in the
# processor package.
#
# **Asserted on the two disagreeing, not on the machine booting.** A boot with
# one adapter satisfies every other question this rig asks, and a boot with two
# where the second was ignored entirely would satisfy a check that only looked
# for its name. So the line asserted carries the second adapter *and* its flush
# disposition: virtio-gpu is primary and reports its present count, while the
# Bochs adapter beside it reports that it scans itself out. One report, two
# backends, and they say different things about the same question (GX-009).
# One /dev/fbN per display, and they name different screens.
#
# **Asserted on the count, and the self-test asserts the rest.** A kernel where
# fb1 is an alias for fb0 opens it, reads the same pixels from it, and passes
# anything that only asks whether the device is there -- which is exactly what a
# half-finished version of this change looks like. The boot check below is the
# machine having two nodes at all; `a screen each` inside it is the one that
# fails when they are the same display.
check_for "2 framebuffer node(s) for 2 display(s)" \
	"  PVH, a node for each screen" \
	qemu-system-x86_64 -m 1024M -nographic -no-reboot \
		-device virtio-gpu-pci -kernel "$X64_ELF"

check_for "also         : bochs-display, found and not in any mode, scans itself out" \
	"  PVH, two display adapters at once" \
	qemu-system-x86_64 -m 1024M -nographic -no-reboot \
		-device virtio-gpu-pci -kernel "$X64_ELF"

# Two disks of the *same kind*, which no path here had ever attached.
#
# Eighteen boot paths and six storage configurations, and every one of them had
# exactly one disk of each sort -- so KF-163 lived for as long as it did not
# because it was subtle but because nothing ever asked. It presented as a
# machine crawling through its boot at one request every two seconds, on a
# kernel and a device that were both behaving correctly, and the cause was a
# legacy interrupt line nothing acknowledged.
#
# virtio1 rather than virtio0 is the string checked: virtio0 appears whenever
# the first disk attaches, which it did throughout the fault. The second one is
# the one that was never reached.
check_for virtio1 "  PVH, two disks of one kind" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF" \
		-drive "file=$DISK,format=raw,if=none,id=d0" \
		-device virtio-blk-pci,drive=d0 \
		-drive "file=$DISK2,format=raw,if=none,id=d1" \
		-device virtio-blk-pci,drive=d1

# The controller real hardware has, rather than the one a hypervisor offers.
# Worth a path of its own because almost nothing about it is shared with virtio:
# a different queue format, a different way of describing where the data goes,
# and a controller that must be stopped before it can be configured.
check_for nvme0n1 "  PVH, NVMe" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF" \
		-drive "file=$DISK,format=raw,if=none,id=n0" \
		-device nvme,serial=recon0,drive=n0

# The suspend summary is printed, on a machine that has a disk to lose.
#
# Its self-test is counted with the others and would go red if the registry
# broke -- but **the deliverable is the printed line**, the machine saying by
# name what would not survive, and nothing asserted that it appears at all.
# Remove the call to suspend_print_summary and every other path here still
# passes.
#
# The label rather than today's answer: this line is printed whether the machine
# can suspend or cannot, so the assertion does not need rewriting on the day the
# answer changes -- which is the day nobody would think to look at it.
check_for "suspend      :" "  PVH, and it says what cannot suspend" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF" \
		-drive "file=$DISK,format=raw,if=none,id=n0" \
		-device nvme,serial=recon0,drive=n0

# The disk a machine with no drive bay has: an SD host controller, which is what
# an eMMC part is wired to. Every laptop below a certain price has this instead
# of a slot, and the kernel had no driver for it until 3.1.
#
# **Asserted on the partition offsets rather than on a device appearing.** The
# first version of this driver found the controller, identified the card, and
# reported a 64 MB card as 30 GB -- every CSD field read eight bits from where it
# lives, because an R2 response arrives without its low byte. A check for "mmc0
# exists" passes that. A check that says where the partitions are does not.
if command -v sgdisk >/dev/null 2>&1; then
	SDCARD=$(mktemp -u)/card.img
	mkdir -p "$(dirname "$SDCARD")"
	truncate -s 64M "$SDCARD"
	sgdisk -n 1:2048:+8M  -t 1:ef00 -c 1:ESP  "$SDCARD" >/dev/null 2>&1
	sgdisk -n 2:20480:+16M -t 2:8300 -c 2:data "$SDCARD" >/dev/null 2>&1

	check_for "slice mmc0 2 20480" "  PVH, an SD host controller" \
		qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF" \
			-device sdhci-pci,id=sd \
			-drive "file=$SDCARD,format=raw,if=none,id=card" \
			-device sd-card,drive=card,bus=sd-bus

	rm -rf "$(dirname "$SDCARD")"
fi

# And SATA, which is what the machines between the IDE era and the NVMe one
# have -- roughly everything built between 2005 and 2020, which is most of what
# this will actually be installed on for some years yet.
check_for sata0 "  PVH, AHCI" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF" \
		-device ahci,id=ahci0 \
		-drive "file=$DISK,format=raw,if=none,id=s0" \
		-device ide-hd,drive=s0,bus=ahci0.0

# Checkpoint 9b on this architecture: a real-mode trampoline, INIT and two
# startup messages, and a local APIC timer per processor. Every one of those is
# a separate way to end up with a machine that boots and uses one processor.
for n in 2 4 8; do
	check_cpus "PVH" "$n" \
		qemu-system-x86_64 -m 512M -smp "$n" -nographic -no-reboot \
			-kernel "$X64_ELF" "${X64_DISK[@]}"
done

if [ "$ONLY" = all ] || [ "$ONLY" = x86_64 ]; then
	if command -v grub-mkrescue >/dev/null && make_iso; then
		check_for virtio0 "  Multiboot2 via GRUB, BIOS" \
			qemu-system-x86_64 -m 512M -nographic -no-reboot \
				-cdrom "$WORK/reconos.iso" "${X64_DISK[@]}"
		if [ -f "$OVMF_X64" ]; then
			check_for virtio0 "  Multiboot2 via GRUB, UEFI" \
				qemu-system-x86_64 -m 512M -nographic -no-reboot \
					-bios "$OVMF_X64" -cdrom "$WORK/reconos.iso" \
					"${X64_DISK[@]}"
		else
			skip "  Multiboot2 via GRUB, UEFI" "no OVMF"
		fi
	else
		skip "  Multiboot2 via GRUB, BIOS" "no grub-mkrescue"
		skip "  Multiboot2 via GRUB, UEFI" "no grub-mkrescue"
	fi

	if [ -f "$OVMF_X64" ] && make -C boot ARCH=x86_64 esp >/dev/null 2>&1; then
		check_for virtio0 "  reconboot, UEFI" \
			qemu-system-x86_64 -m 512M -nographic -no-reboot \
				-bios "$OVMF_X64" \
				-drive format=raw,file="$ROOT/boot/build/x86_64/esp.img" \
				"${X64_DISK[@]}"
	else
		skip "  reconboot, UEFI" "the loader did not build"
	fi
fi

echo
echo "aarch64"

check_for virtio0 "  device tree, cortex-a72" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel "$ARM_IMG" "${ARM_DISK[@]}"

check_for virtio0 "  device tree, -cpu max" \
	qemu-system-aarch64 -M virt -cpu max -m 512M -nographic -kernel "$ARM_IMG" \
		"${ARM_DISK[@]}"

# One run with no disk at all, on purpose. A kernel that only works on a machine
# with storage attached is a kernel that cannot boot a diskless one, and the
# path where nothing is found is otherwise never taken.
check "  device tree, no disk attached" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel "$ARM_IMG"

# NVMe on this architecture too, which exercises a path nothing else does: the
# device is on PCI, and on the device-tree boot the configuration window comes
# from the host bridge node rather than from an ACPI table.
check_for nvme0n1 "  device tree, NVMe" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel "$ARM_IMG" \
		-drive "file=$DISK,format=raw,if=none,id=n0" \
		-device nvme,serial=recon0,drive=n0

# A screen on this architecture, and **this path exists because of KF-209**.
#
# On aarch64 the display adapter's aperture is at 0x11000000 and RAM starts at
# 0x40000000 -- the pixels are *below* the memory the allocator manages. That is
# the only arrangement in this matrix where handing a mapped device back to the
# page allocator is a fault instead of silent corruption, and it is how the bug
# was found at all: x86_64 ran the same code, freed the framebuffer into the
# allocator, and reported pass.
#
# So this is not "aarch64 has a screen too". It is the one machine shape that
# can fail loudly, and without it the rig cannot catch that class again.
check_for "both markers are on the screen" \
	"  device tree, a program draws on the screen" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 1024M -nographic \
		-no-reboot -device bochs-display -kernel "$ARM_IMG"

# virtio-gpu on the other architecture, over PCI.
#
# The same driver, unchanged, on a machine with a different page size story, a
# different interrupt controller and a framebuffer allocated from a different
# part of the map. A display driver that works on one architecture has shown
# that it works on one architecture.
check_for "virtio-gpu" \
	"  device tree, virtio-gpu over PCI" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 1024M -nographic \
		-no-reboot -device virtio-gpu-pci -kernel "$ARM_IMG"

# And over the memory-mapped transport, which is the point of having two.
#
# The virtio transport table exists so that a driver does not know whether its
# device was found on a bus or listed by firmware. virtio-blk and virtio-net
# have always been driven both ways; this is the display saying the same thing.
#
# `force-legacy=false` is required and is not a workaround: QEMU's `virt`
# machine presents memory-mapped virtio as version 1 by default, which is the
# pre-1.0 register layout this kernel deliberately does not implement -- it says
# so by name on every such boot rather than failing quietly. Asking for the
# modern layout is asking for the device this driver actually supports.
check_for "virtio-gpu" \
	"  device tree, virtio-gpu, memory-mapped" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 1024M -nographic \
		-no-reboot -global virtio-mmio.force-legacy=false \
		-device virtio-gpu-device -kernel "$ARM_IMG"

# That the pixels reach the glass on this architecture too.
#
# Worth its own path for the same reason the bochs-display one above it is: the
# aarch64 framebuffer is allocated from a different region, and this is the
# architecture where KF-209 failed loudly while x86_64 reported success doing
# the identical wrong thing.
check_screen "  device tree, virtio-gpu really shows pixels" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 1024M \
		-device virtio-gpu-pci -kernel "$ARM_IMG"

check_for sata0 "  device tree, AHCI" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel "$ARM_IMG" \
		-device ahci,id=ahci0 \
		-drive "file=$DISK,format=raw,if=none,id=s0" \
		-device ide-hd,drive=s0,bus=ahci0.0

# Sixteen is past where QEMU's virt board stops giving a GICv2.
#
# This loop stopped at eight, and eight is exactly the largest machine the older
# interrupt controller supports -- so the kernel panicked at boot on any ARM
# machine with nine or more processors, and nothing here could see it (KF-124).
# A rig built on the principle that some bugs only exist above a certain machine
# size had its own ceiling, one processor below the first machine that would
# have shown this one.
for n in 2 4 8 16; do
	check_for virtio0 "  device tree, $n processors" \
		qemu-system-aarch64 -M virt -cpu cortex-a72 -smp "$n" -m 512M \
			-nographic -kernel "$ARM_IMG" "${ARM_DISK[@]}"
done

# The same machines again, asked how many processors they ended up with rather
# than whether they found a disk. Sixteen is left out on purpose: this kernel
# holds eight, so a sixteen-processor machine is *supposed* to report eight and
# say so, and asserting sixteen there would be asserting a bug.
for n in 2 4 8; do
	check_cpus "device tree" "$n" \
		qemu-system-aarch64 -M virt -cpu cortex-a72 -smp "$n" -m 512M \
			-nographic -kernel "$ARM_IMG" "${ARM_DISK[@]}"
done

if [ "$ONLY" = all ] || [ "$ONLY" = aarch64 ]; then
	if [ -f "$OVMF_ARM" ] && make -C boot ARCH=aarch64 esp >/dev/null 2>&1; then
		# The disk here is on PCI rather than on the memory-mapped bus.
		# This firmware describes the machine with ACPI instead of a
		# device tree, so the configuration window is read out of the
		# MCFG table -- which is the same walk x86_64 will need for its
		# processor list, and the reason it was worth writing here.
		check_for virtio0 "  reconboot, UEFI" \
			qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
				-bios "$OVMF_ARM" \
				-drive format=raw,file="$ROOT/boot/build/aarch64/esp.img" \
				-drive "file=$DISK,format=raw,if=none,id=d1" \
				-device virtio-blk-pci,drive=d1
	else
		skip "  reconboot, UEFI" "the loader did not build"
	fi
fi

# --- Partition tables ------------------------------------------------------
#
# Three disks written by sgdisk and sfdisk, read by the kernel, compared. The
# hybrid is the one that matters: its protective entry covers a fraction of the
# disk and sits beside two entries that describe real partitions correctly, so
# every wrong reader produces a plausible answer on it.

echo

# --- randomness -------------------------------------------------------------

echo
echo "randomness"

check_random "from timing alone (x86_64)" "none on this processor" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF"

check_random "from the processor (x86_64)" "[0-9]* words accepted" \
	qemu-system-x86_64 -m 512M -cpu max -nographic -no-reboot -kernel "$X64_ELF"

check_random "from timing alone (aarch64)" "none on this processor" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel "$ARM_IMG"

check_random "from the processor (aarch64)" "[0-9]* words accepted" \
	qemu-system-aarch64 -M virt -cpu max -m 512M -nographic \
		-kernel "$ARM_IMG"

echo
echo "the machine, described"

check_acpi
check_power_off
check_restart "and can restart itself" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-kernel "$X64_ELF"

# And the other architecture, which shares not one line of the mechanism: a
# PSCI SYSTEM_RESET firmware call against a write to a chipset register.
# `arch_power_off` is already the entry about why a portable path is not enough
# here -- a machine booted from a device tree has no ACPI at all to reach --
# and restarting has exactly the same shape.
check_restart "and so can the other architecture" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-no-reboot -kernel "$ARM_IMG"

echo "partition tables"

if bash scripts/make-partition-fixtures.sh "$WORK/fixtures" >/dev/null 2>&1; then
	check_table gpt        "$WORK/fixtures/gpt.img"    "$WORK/fixtures/gpt.expected"
	check_table mbr        "$WORK/fixtures/mbr.img"    "$WORK/fixtures/mbr.expected"
	check_table hybrid-mbr "$WORK/fixtures/hybrid.img" "$WORK/fixtures/hybrid.expected"
else
	skip "  partition tables" "sgdisk or sfdisk is missing"
fi

# --- Durability -------------------------------------------------------------
#
# That a flushed write is on the medium before the next one is issued is the
# property every crash-consistency scheme rests on, and it is the kind of thing
# that regresses silently: somebody makes a driver faster by returning from
# flush when the command was accepted rather than when it completed, every
# self-test still passes, and the only symptom arrives months later as a
# filesystem that does not survive a power cut.
#
# So it is measured on every run, not once. Six cuts rather than the twenty the
# standalone script does -- enough to catch a flush that stopped waiting, few
# enough not to double the length of this.

echo
echo "durability"

# Two checks, because they answer different questions and only one of them was
# ever being asked.
#
# The cut measures *ordering*: what a reader finds on the medium after the
# machine stops. It cannot measure flushing at all -- killing QEMU does not lose
# the writes QEMU already made, those bytes are in the host's page cache and the
# host writes them out regardless. Run crash-test.sh with `nocache`, which tells
# QEMU to discard guest flushes entirely, and it returns the same clean result.
#
# So the flush is checked where it can be seen: at the emulated controller,
# which is the far side of the boundary the kernel is responsible for.

printf '%-46s' "  a flush orders writes, blocks do not tear"

if crash_out=$(sub_out crash); then
	echo "$(echo "$crash_out" | grep -oE '[0-9]+ cuts.*')"
	passes=$((passes + 1))
else
	echo "FAILED"
	echo "$crash_out" | sed 's/^/      /'
	failures=$((failures + 1))
	FAILED_PATHS+=("durability: ordering")
fi

for a in x86_64 aarch64; do
	printf '%-46s' "  every flush reaches the device ($a)"

	if flush_out=$(sub_out "flush_$a"); then
		echo "$(echo "$flush_out" | grep -c 'flush commands at the controller') driver(s)"
		passes=$((passes + 1))
	else
		echo "FAILED"
		echo "$flush_out" | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("durability: flush reaches device ($a)")
	fi
done

# --- ReconFS ----------------------------------------------------------------
#
# The interesting number here is not that a fresh volume checks out. It is that
# the checker was shown four faults it was built to catch and caught all four.
#
# A checker that has only ever been run on good images has never been observed
# to do anything. Three harness bugs on the day this was written (KF-114,
# KF-115, KF-116) all presented as clean passes, and the filesystem's crash
# suite is about to rest on exactly this checker being honest.

echo
echo "swap"

# A store to evict into, on both architectures.
#
# This is here rather than left to the self-test battery because the battery
# runs on machines with no disk, where the swap test reports that it found
# nothing to test against -- which is honest and is also a pass. A pass for a
# test that did not run is the exact shape of result this rig exists to stop, so
# there is a path that gives it a device and then requires it to have used one.
#
# The device is a whole disk with nothing on it. Swap writes over everything
# from the first eviction, and the kernel deliberately does not decide for
# itself which device that may be -- so the harness says, the same way it says
# which device is the filesystem.
for a in x86_64 aarch64; do
	printf '%-46s' "  a page written somewhere that is not memory ($a)"

	img=$(mktemp)
	dd if=/dev/zero of="$img" bs=1M count=32 status=none

	if [ "$a" = aarch64 ]; then
		sw_out=$(timeout -s KILL 90 qemu-system-aarch64 -M virt \
			-cpu cortex-a72 -m 512M -nographic \
			-kernel kernel/build/aarch64/reconos-kernel.img \
			-append "swap=nvme0n1 poweroff" \
			-drive "file=$img,format=raw,if=none,id=s0" \
			-device nvme,serial=sw0,drive=s0 2>&1) || true
	else
		sw_out=$(timeout -s KILL 90 qemu-system-x86_64 -m 512M \
			-nographic -no-reboot \
			-kernel kernel/build/x86_64/reconos-kernel.elf \
			-append "swap=nvme0n1 poweroff" \
			-drive "file=$img,format=raw,if=none,id=s0" \
			-device nvme,serial=sw0,drive=s0 2>&1) || true
	fi

	rm -f "$img"

	sw_out=$(echo "$sw_out" | sed -e 's/\r$//')

	# Three separate things, and the third is the one that matters.
	#
	# That the test passed is not enough: it passes when it finds no store.
	# That the store was attached is not enough either: it could attach and
	# then write nothing. So the assertion is that pages were actually
	# written to it, read back, and the test still passed.
	sw_pass=$(echo "$sw_out" | grep -c 'somewhere to evict *: pass')
	sw_used=$(echo "$sw_out" | grep -c 'swap: using ')
	sw_wrote=$(echo "$sw_out" | grep -cE 'activity *: [1-9][0-9]* written, [1-9][0-9]* read')

	if [ "$sw_pass" -ge 1 ] && [ "$sw_used" -ge 1 ] && \
	   [ "$sw_wrote" -ge 1 ]; then
		echo "pages written and read back"
		passes=$((passes + 1))
	else
		echo "FAILED -- pass=$sw_pass attached=$sw_used wrote=$sw_wrote"
		echo "$sw_out" | grep -aE 'swap|evict' | head -4 |
			sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("swap ($a)")
	fi
done

echo

echo "reconfs"

for a in x86_64 aarch64; do
	printf '%-46s' "  the checker catches what it is shown ($a)"

	img=$(mktemp)
	dd if=/dev/zero of="$img" bs=1M count=16 status=none

	if [ "$a" = aarch64 ]; then
		fs_out=$(timeout -s KILL 150 qemu-system-aarch64 -M virt \
			-cpu cortex-a72 -m 512M -nographic \
			-kernel kernel/build/aarch64/reconos-kernel.img \
			-append "reconfs=nvme0n1" \
			-drive "file=$img,format=raw,if=none,id=d0" \
			-device nvme,serial=recon0,drive=d0 2>&1) || true
	else
		fs_out=$(timeout -s KILL 150 qemu-system-x86_64 -m 512M \
			-nographic -no-reboot \
			-kernel kernel/build/x86_64/reconos-kernel.elf \
			-append "reconfs=nvme0n1" \
			-drive "file=$img,format=raw,if=none,id=d0" \
			-device nvme,serial=recon0,drive=d0 2>&1) || true
	fi

	rm -f "$img"

	# The whole battery, at every block size, and the commit path with it.
	# Checking only the last line would pass a run where two of the three
	# block sizes failed, which is the shape of pass this work has already
	# produced three of.
	sizes=$(echo "$fs_out" | grep -cE 'the checker caught 5 of 5')
	commits=$(echo "$fs_out" | grep -cE 'a commit that survives a remount : pass')
	verdict=$(echo "$fs_out" | grep -oE '[0-9]+ of [0-9]+ block sizes behaved')

	# The freed-block exclusion can only be checked on a volume small enough
	# to fill in one transaction, so it reports whether it ran rather than
	# failing on a large disk (KF-126). This image is 16 MB, which is small
	# enough at every block size -- so demand all three here. Without this,
	# growing the image would silently stop checking the one rule that keeps
	# a transaction from overwriting live storage, and the run would still
	# be green.
	excl=$(echo "$fs_out" | grep -cE 'the freed-block exclusion was checked at 3 of 3')

	# And create-with-mode, which is asserted *here* because this is the only
	# run that has a volume for it to work on. Every disk in this rig is
	# blank, so on any other path the kernel truthfully reports that there is
	# no filesystem and the test does not run.
	#
	# Checked rather than merely printed. The line appears after the
	# self-test list, so nothing counts it, and a test whose result is
	# printed and never read is the failure this file has already had once
	# (KF-143) -- a boot that stopped half way answering every question a
	# healthy one does.
	mode=$(echo "$fs_out" | grep -cE 'files carry a mode : pass')

	if [ "$verdict" = "3 of 3 block sizes behaved" ] &&
	   [ "$sizes" = "3" ] && [ "$commits" = "3" ] && [ "$excl" = "1" ] &&
	   [ "$mode" = "1" ]; then
		echo "3 block sizes, 15 faults, 3 commits, exclusion 3/3, modes kept"
		passes=$((passes + 1))
	else
		echo "FAILED"
		echo "$fs_out" | grep -aE 'files carry a mode|rootfs:' | sed 's/^/      /'
		# Already a targeted slice rather than a blind cut, but it can
		# still end above the verdict -- so it goes through the same
		# helper as everything else.
		show_failure "$(echo "$fs_out" | sed -n '/reconfs:/,$p')" 20
		failures=$((failures + 1))
		FAILED_PATHS+=("reconfs ($a)")
	fi
done

# The one docs/RECONFS.md lists first: cut the power inside a rename, and let
# something that shares no code with the kernel say what survived.
#
# Fewer cuts than the standalone script does, because each one boots a machine
# and each check reads a whole image -- enough to catch a regression, few enough
# not to double the length of this.

printf '%-46s' "  a rename survives the power going out"

if rn_out=$(sub_out rename); then
	echo "$(echo "$rn_out" | grep -oE '[0-9]+ cuts inside a rename.*')"
	passes=$((passes + 1))
else
	echo "FAILED"
	show_failure "$rn_out" 20
	failures=$((failures + 1))
	FAILED_PATHS+=("reconfs: rename under a power cut")
fi

# The promise the whole of phase 2 rests on: ReconOS installs beside an
# operating system that is already there without destroying it. Every safety
# rule in the storage stack exists to keep it, and none of them is worth
# anything as an intention.

printf '%-46s' "  a filesystem beside somebody else's"

# The status is captured rather than read from `$?` inside an elif, where what
# it refers to depends on how the shell got there.
beside_out=$(sub_out beside)
beside_rc=$?

if [ "$beside_rc" -eq 0 ]; then
	echo "neighbour and both tables unchanged"
	passes=$((passes + 1))
elif [ "$beside_rc" -eq 2 ]; then
	echo "skipped, sgdisk is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$beside_out" 14
	failures=$((failures + 1))
	FAILED_PATHS+=("reconfs beside another partition")
fi

# --- Somebody else's filesystem --------------------------------------------
#
# FAT32 is the one foreign format that is required rather than optional: the
# UEFI System Partition is FAT32 by specification and that is where our own
# bootloader has to be written. A reader tested against a volume our own code
# wrote would be tested against its own misunderstandings, so the fixture is
# built by mkfs.vfat and filled by mcopy -- and then broken four ways, because a
# reader that has never been seen to refuse anything is not a reader yet.

echo
echo "foreign filesystems"

for a in x86_64 aarch64; do
	printf '%-46s' "  reads FAT32, and refuses four broken ones ($a)"

	fat_out=$(sub_out "fatread_$a")
	fat_rc=$?

	if [ "$fat_rc" -eq 0 ]; then
		echo "$(echo "$fat_out" | grep -oE '[0-9]+ of [0-9]+: read a foreign.*' | head -1)"
		passes=$((passes + 1))
	else
		echo "FAILED"
		show_failure "$fat_out" 16
		failures=$((failures + 1))
		FAILED_PATHS+=("fat32 reads ($a)")
	fi
done

# And the other direction, which is the one an installer depends on: we write
# the bootloader and *firmware* reads it. A writer checked only by our own
# reader would be checked against its own misunderstandings.

for a in x86_64 aarch64; do
	printf '%-46s' "  writes FAT32 that mtools can read ($a)"

	fw_out=$(sub_out "fatwrite_$a")
	fw_rc=$?

	if [ "$fw_rc" -eq 0 ]; then
		echo "$(echo "$fw_out" | grep -oE '[0-9]+ of [0-9]+: wrote a volume.*' | head -1)"
		passes=$((passes + 1))
	elif [ "$fw_rc" -eq 2 ]; then
		echo "skipped, mtools is not installed"
		skipped=$((skipped + 1))
	else
		echo "FAILED"
		show_failure "$fw_out" 16
		failures=$((failures + 1))
		FAILED_PATHS+=("fat32 writes ($a)")
	fi
done

# --- Deciding, before there is any doing ------------------------------------
#
# The installer is the one piece here that runs once, on a stranger's machine,
# with their data on it. So the deciding is a separate half that writes nothing,
# and it is checked against layouts sgdisk built -- including four it must
# *refuse*. A planner that always says yes would pass a test that only asked
# whether it produced a plan.

echo
echo "installer"

printf '%-46s' "  plans or refuses seven real layouts"

plan_out=$(sub_out plan)
plan_rc=$?

if [ "$plan_rc" -eq 0 ]; then
	echo "$(echo "$plan_out" | grep -oE '[0-9]+ of [0-9]+ layouts.*' | head -1)"
	passes=$((passes + 1))
elif [ "$plan_rc" -eq 2 ]; then
	echo "skipped, sgdisk or mtools is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$plan_out" 18
	failures=$((failures + 1))
	FAILED_PATHS+=("install planner")
fi

# And the doing. Installs for real -- onto a blank disk, and beside an existing
# system whose data, bootloader and table entries must all survive byte for
# byte. The claim being checked is not "it installed".

printf '%-46s' "  installs, and the disk's contents survive"

onto_out=$(sub_out onto)
onto_rc=$?

if [ "$onto_rc" -eq 0 ]; then
	echo "$(echo "$onto_out" | grep -oE '[0-9]+ of [0-9]+: installed.*' | head -1)"
	passes=$((passes + 1))
elif [ "$onto_rc" -eq 2 ]; then
	echo "skipped, sgdisk or mtools is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$onto_out" 20
	failures=$((failures + 1))
	FAILED_PATHS+=("install onto")
fi

# The one test that checks what the project is for, rather than that a part of
# it works: a bare disk, an install medium, and afterwards the disk boots on its
# own with the medium gone. Everything the installer got wrong shows up as a
# firmware that finds no system.

printf '%-46s' "  installs from media, and the disk boots"

e2e_out=$(sub_out e2e)
e2e_rc=$?

if [ "$e2e_rc" -eq 0 ]; then
	echo "$(echo "$e2e_out" | grep -oE '[0-9]+ of [0-9]+: installed from media.*' | head -1)"
	passes=$((passes + 1))
elif [ "$e2e_rc" -eq 2 ]; then
	echo "skipped, OVMF or mtools is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$e2e_out" 20
	failures=$((failures + 1))
	FAILED_PATHS+=("install then boot")
fi

# Installing beside another system is only half the promise. The other half is
# still being able to reach it afterwards, and the fixture is shaped like a real
# dual-boot machine -- Windows and Linux on one EFI partition -- rather than
# like the code, which is what caught the first version listing only one.

printf '%-46s' "  finds the other systems on the machine"

menu_out=$(sub_out menu)
menu_rc=$?

if [ "$menu_rc" -eq 0 ]; then
	echo "$(echo "$menu_out" | grep -oE '[0-9]+ of [0-9]+: found the other.*' | head -1)"
	passes=$((passes + 1))
elif [ "$menu_rc" -eq 2 ]; then
	echo "skipped, OVMF or mtools is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$menu_out" 14
	failures=$((failures + 1))
	FAILED_PATHS+=("boot menu")
fi

# The loader refusing a kernel that is not ours. The signature is made by
# openssl, which shares no code with the verifier -- a verifier checked against
# signatures produced by its own arithmetic is checked against its own
# misunderstandings.
#
# Four refusals, and only the last is an attacker: a valid signature by somebody
# else's key. A verifier that catches corruption while accepting any well-formed
# signature is worth nothing at all.

echo
echo "integrity"

printf '%-46s' "  runs a signed kernel, refuses four others"

# Captured before the batch started; see the note up there for why.
if [ "$sig_rc" -eq 0 ]; then
	echo "$(echo "$sig_out" | grep -oE '[0-9]+ of [0-9]+: ran the signed.*' | head -1)"
	passes=$((passes + 1))
elif [ "$sig_rc" -eq 2 ]; then
	echo "skipped, openssl or OVMF is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$sig_out" 14
	failures=$((failures + 1))
	FAILED_PATHS+=("signed kernel")
fi

# The recovery environment, shown a volume broken on purpose by a tool that
# shares no code with the kernel. A recovery screen that has never been seen to
# report damage is not a recovery screen -- it is one that says "sound", which
# is what a broken one says too.

printf '%-46s' "  recovery finds damage, and touches nothing"

rec_out=$(sub_out rec)
rec_rc=$?

if [ "$rec_rc" -eq 0 ]; then
	echo "$(echo "$rec_out" | grep -oE '[0-9]+ of [0-9]+: found the damage.*' | head -1)"
	passes=$((passes + 1))
elif [ "$rec_rc" -eq 2 ]; then
	echo "skipped, python3 is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$rec_out" 16
	failures=$((failures + 1))
	FAILED_PATHS+=("recovery")
fi

# --- the medium, as a stick rather than as a disk ---------------------------
#
# Checkpoint 17's ground floor: the one artefact a person handles, booted the
# way they will boot it. On an emulated xHCI controller specifically -- a stick
# appears on USB and the firmware reaches it through its own stack, so testing
# with an IDE drive would exercise a path the medium never takes.

printf '%-46s' "  the install medium boots from a USB stick"

med_out=$(sh scripts/medium-boot-test.sh 2>&1)
med_rc=$?

if [ "$med_rc" -eq 0 ]; then
	echo "$(echo "$med_out" | grep -oE '[0-9]+ of [0-9]+: the medium.*' | head -1)"
	passes=$((passes + 1))
elif [ "$med_rc" -eq 2 ]; then
	echo "skipped, a disk tool is missing"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$med_out" 12
	failures=$((failures + 1))
	FAILED_PATHS+=("install medium")
fi

# --- the same software, on the other kind of medium -------------------------
#
# A disc is not a stick with a different cable. Its sectors are 2048 bytes, it
# has no partition table, and where a disk keeps a GPT it keeps an El Torito
# boot catalogue -- three places the loaders can be wrong while every stick
# test above still passes. Both firmwares and both architectures, off one ISO.

printf '%-46s' "  the install disc boots on both firmwares"

disc_out=$(sh scripts/disc-boot-test.sh 2>&1)
disc_rc=$?

if [ "$disc_rc" -eq 0 ]; then
	echo "$(echo "$disc_out" | grep -oE '[0-9]+ of [0-9]+: the disc.*' | head -1)"
	passes=$((passes + 1))
elif [ "$disc_rc" -eq 2 ]; then
	echo "skipped, xorriso is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$disc_out" 14
	failures=$((failures + 1))
	FAILED_PATHS+=("install disc")
fi

# --- the stick, read and written --------------------------------------------
#
# Checkpoint 11b. The medium test above proves the kernel can read the stick it
# booted from, which is the half that matters for installing; this proves the
# other half, and proves it from the wire rather than from the driver's own
# report. See the script: a restoring self-test leaves an image that looks
# exactly like one no write ever touched, so the WRITE(10) is asserted in
# QEMU's SCSI trace.

printf '%-46s' "  the kernel reads and writes a USB stick"

usb_out=$(sh scripts/usb-storage-test.sh 2>&1)
usb_rc=$?

if [ "$usb_rc" -eq 0 ]; then
	echo "$(echo "$usb_out" | grep -oE '[0-9]+ of [0-9]+: the kernel.*' | head -1)"
	passes=$((passes + 1))
elif [ "$usb_rc" -eq 2 ]; then
	echo "skipped, qemu is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$usb_out" 12
	failures=$((failures + 1))
	FAILED_PATHS+=("usb storage")
fi

# --- the menu draws where it can, and falls back where it cannot -------------
#
# Both halves asserted, because only one of them is the interesting one.
#
# That the graphical menu draws on a machine with a framebuffer is the feature.
# That it *does not* on a machine without one is what keeps the feature from
# being a machine that shows nothing: AAVMF provides no framebuffer at all, so
# the aarch64 loader must take the text path on every boot, for ever. An
# unasserted fallback is exactly the branch that rots, and this one is the
# reason the graphical menu was allowed to exist.
#
# Checked by the loader's own report rather than by looking at pixels: it says
# which surface it used, once, on the serial console, and that is a claim the
# rig can hold it to.

printf '%-46s' "  the menu draws, and falls back where it cannot"

menu_x64=; menu_arm=
if [ -f "$OVMF_X64" ] && make -C boot ARCH=x86_64 esp >/dev/null 2>&1; then
	menu_x64=$(timeout 60 qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-bios "$OVMF_X64" \
		-drive format=raw,file="$ROOT/boot/build/x86_64/esp.img" 2>&1 |
		tr -d '\r' | sed -n 's/^  menu  *: \(.*\)/\1/p' | head -1)
fi
if [ -f "$OVMF_ARM" ] && make -C boot ARCH=aarch64 esp >/dev/null 2>&1; then
	menu_arm=$(timeout 60 qemu-system-aarch64 -M virt -cpu cortex-a72 \
		-m 512M -nographic -no-reboot -bios "$OVMF_ARM" \
		-drive format=raw,file="$ROOT/boot/build/aarch64/esp.img" 2>&1 |
		tr -d '\r' | sed -n 's/^  menu  *: \(.*\)/\1/p' | head -1)
fi

if [ -z "$menu_x64" ] && [ -z "$menu_arm" ]; then
	echo "skipped, neither firmware is installed"
	skipped=$((skipped + 1))
elif [ "${menu_x64%% *}" = "drawn" ] && [ "${menu_arm%% *}" = "text" ]; then
	echo "drawn on OVMF, text on AAVMF"
	passes=$((passes + 1))
else
	echo "FAILED"
	echo "      x86_64 (has a framebuffer): ${menu_x64:-nothing}"
	echo "      aarch64 (has none):         ${menu_arm:-nothing}"
	failures=$((failures + 1))
	FAILED_PATHS+=("boot menu surface")
fi

# Checkpoint 16, on a machine with no UEFI in it at all: SeaBIOS, an IDE disk,
# and 440 bytes of ours in the first sector. Includes the two deliberate faults
# -- an unreadable stage 2 and one changed byte of its magic -- because a loader
# that jumps into an unidentified sector and one that checks are indistinguish-
# able on a disk where the sector happens to be right.

printf '%-46s' "  a machine with no UEFI runs our loader"

bios_out=$(sh scripts/bios-boot-test.sh 2>&1)
bios_rc=$?

if [ "$bios_rc" -eq 0 ]; then
	echo "$(echo "$bios_out" | grep -oE '[0-9]+ of [0-9]+: a machine.*' | head -1)"
	passes=$((passes + 1))
elif [ "$bios_rc" -eq 2 ]; then
	echo "skipped, qemu is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$bios_out" 16
	failures=$((failures + 1))
	FAILED_PATHS+=("bios loader")
fi

# And the same machine again, with a key. The BIOS path shares reconboot's
# sha256.c and rsa.c rather than reimplementing them, so this is checking that
# they were *reached* -- a loader that never calls the verifier is exactly what
# an attacker who can write to the disk would arrange.

printf '%-46s' "  the BIOS path refuses an unsigned kernel"

# Captured before the batch started, for the same reason.
if [ "$bsig_rc" -eq 0 ]; then
	echo "$(echo "$bsig_out" | grep -oE '[0-9]+ of [0-9]+: the BIOS path.*' | head -1)"
	passes=$((passes + 1))
elif [ "$bsig_rc" -eq 2 ]; then
	echo "skipped, openssl or a disk tool is missing"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	show_failure "$bsig_out" 14
	failures=$((failures + 1))
	FAILED_PATHS+=("bios signature")
fi

echo
if [ "$failures" -eq 0 ]; then
	echo "$passes self-tests across every path, no failures${skipped:+ ($skipped skipped)}."
	exit 0
fi

echo "$failures path(s) failed:"
printf '  %s\n' "${FAILED_PATHS[@]}"
exit 1
