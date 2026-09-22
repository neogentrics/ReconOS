#!/usr/bin/env bash
#
# Does the log port actually hand somebody the boot log?
#
# Boots the kernel with a network and a host port forwarded to it, connects
# from outside the guest, and checks that what comes back is this boot's report
# rather than nothing, an error, or a repeated tail.
#
# **The four things asserted, and why each one can fail on its own:**
#
#   1. Something arrives at all. A port that accepts and sends nothing is
#      indistinguishable from a machine with nothing to say.
#   2. It is *this boot's* log -- matched on a line only this kernel prints.
#      A test that accepts any bytes passes against an echo server.
#   3. It is not the same chunk repeated. The first version of logport.c read
#      the ring in 1 KB pieces, which returns the most recent kilobyte every
#      time; that loop terminates and sends plausible output and is entirely
#      wrong, so the shape of that bug gets its own assertion.
#   4. **Nothing sent to the port changes anything.** A line is written at it
#      and the connection still yields only the log. That is the property that
#      makes this a file being read aloud rather than a console.
#
# And the negative: a boot *without* `logport` on the command line must refuse
# the connection. A port that is open when nobody asked for it is the fault
# this whole design is arranged to prevent, so it is tested for directly.
set -u

cd "$(dirname "$0")/.."
ROOT=$PWD
ARCH=${1:-x86_64}
KERNEL=kernel/build/$ARCH/reconos-kernel.elf
# A free port, asked for rather than assumed.
#
# It was a fixed 14919. Two of these at once -- two worktrees, or this wired
# into a run that something else is already running -- both bind the same host
# port, and **the second one fails in a way that reads like the kernel did not
# listen**: "the guest never printed a log port line", with QEMU's "could not
# set up host forwarding rule" one line lower and nothing connecting them.
#
# Taken from the network session, who hit it and fixed it there first. It
# reached this branch on 21 September, in the minutes before a matrix that
# would have raced another session's -- their warning arrived while that run
# was eight lines in, and it was stopped to take this rather than to find out.
#
# Overridable, because a fixed port is what you want when debugging by hand and
# a free one is what you want when something else chose the moment to run.
free_port() {
	python3 - <<-'PY' 2>/dev/null || echo 14919
	import socket
	s = socket.socket()
	s.bind(('127.0.0.1', 0))
	print(s.getsockname()[1])
	s.close()
	PY
}

HOSTPORT=${HOSTPORT:-$(free_port)}
GUESTPORT=4919

# Which card carries it. Defaults to virtio-net, so an ordinary run is exactly
# the run this test was written against.
#
# **It is a variable because the machine this feature exists for does not have
# a virtio-net in it.** The log port's case is the boot where the medium cannot
# record the fault -- which on the server means USB, and the network there is
# two Realtek 8168s. A feature proved over one emulated card is proved over the
# hypervisor, not over the network stack: virtio-net has no descriptor
# ownership to get wrong, no FCS in its lengths, and no cable to lose. Those
# are precisely the things a real card driver can get wrong underneath a
# working TCP stack.
#
#   NIC=e1000 scripts/logport-test.sh        the Intel 8254x driver
#   NIC=virtio-net scripts/logport-test.sh   the default
#
# No Realtek gigabit part is emulated by QEMU, so r8169 still cannot be run
# this way. e1000 is the closest available proxy: a real descriptor ring, a
# real link register, driven by a driver this branch wrote.
NIC=${NIC:-virtio-net}

[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL"; exit 2; }
command -v qemu-system-x86_64 >/dev/null || { echo "no qemu"; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"; kill %1 2>/dev/null' EXIT

pass=0
fail=0

check() {
	if [ "$1" = "yes" ]; then
		printf '    %-56s ok\n' "$2"
		pass=$((pass + 1))
	else
		printf '    %-56s FAILED\n' "$2"
		fail=$((fail + 1))
	fi
}

# --- the boot that should be listening ---------------------------------------
#
# `-append logport` is the whole switch. The cmdline reaches the kernel through
# the handoff on this path, which is what `\reconos\cmdline` does on a medium.
run_guest() {
	local append=$1 out=$2

	qemu-system-x86_64 -m 1024M -nographic -no-reboot \
		-netdev user,id=n0,hostfwd=tcp::$HOSTPORT-:$GUESTPORT \
		-device "$NIC",netdev=n0 \
		-kernel "$KERNEL" -append "$append" > "$out" 2>&1 &

	# Wait for the kernel to say it is listening, rather than sleeping a
	# guessed number of seconds -- a guess is what makes a test flaky on a
	# loaded machine.
	local waited=0
	while [ "$waited" -lt 60 ]; do
		grep -aq "log port" "$out" && return 0
		sleep 0.5
		waited=$((waited + 1))
	done
	return 1
}

echo "  the log port"

run_guest "logport" "$WORK/on.log"
started=$?

if [ "$started" -ne 0 ]; then
	echo "    the guest never printed a log port line; its output:"
	sed 's/^/      /' "$WORK/on.log" | tail -12
	exit 1
fi

grep -aq "log port     : \*\*listening\*\*" "$WORK/on.log"
check "$([ $? -eq 0 ] && echo yes || echo no)" "it says it is listening, in the report"

# 1 and 2: something arrives, and it is this boot's log.
timeout 20 bash -c "exec 3<>/dev/tcp/127.0.0.1/$HOSTPORT; cat <&3" \
	> "$WORK/got.txt" 2>/dev/null

[ -s "$WORK/got.txt" ]
check "$([ $? -eq 0 ] && echo yes || echo no)" "a reader gets bytes"

grep -aq "ReconOS kernel" "$WORK/got.txt"
check "$([ $? -eq 0 ] && echo yes || echo no)" "and they are this boot's report"

# 3: not one chunk repeated. The kernel's banner appears once in a boot, so
# more than one of it means the same region was sent twice.
banners=$(grep -ac "ReconOS kernel" "$WORK/got.txt")
[ "$banners" -eq 1 ]
check "$([ $? -eq 0 ] && echo yes || echo no)" "the same region is not sent twice ($banners banner)"

# 4: it reads nothing. Say something at it and ask again; the answer must be
# the log, unchanged in kind, and the guest must not have acted on it.
timeout 20 bash -c "exec 3<>/dev/tcp/127.0.0.1/$HOSTPORT; \
	printf 'poweroff\nreboot\n' >&3; cat <&3" > "$WORK/after.txt" 2>/dev/null

grep -aq "ReconOS kernel" "$WORK/after.txt"
check "$([ $? -eq 0 ] && echo yes || echo no)" "a reader that talks still only gets the log"

kill %1 2>/dev/null
wait 2>/dev/null

# --- and the boot that should not be listening -------------------------------
run_guest "" "$WORK/off.log"

sleep 2

# **Asserted on bytes, not on the connect succeeding.**
#
# The first version of this checked that `exec 3<>/dev/tcp/...` failed, and it
# never could: QEMU's hostfwd accepts the host side of the connection
# unconditionally and only then tries to reach the guest. So that check was
# measuring QEMU's socket rather than ReconOS's, and would have passed a kernel
# with the port wide open exactly as readily.
#
# What must be true is that nothing comes back, because nothing is listening.
timeout 8 bash -c "exec 3<>/dev/tcp/127.0.0.1/$HOSTPORT 2>/dev/null; \
	timeout 5 cat <&3" > "$WORK/closed.txt" 2>/dev/null

[ ! -s "$WORK/closed.txt" ]
check "$([ $? -eq 0 ] && echo yes || echo no)" "without the word, the port yields nothing"

grep -aq "log port" "$WORK/off.log"
[ $? -ne 0 ]
check "$([ $? -eq 0 ] && echo yes || echo no)" "and the report does not mention it at all"

kill %1 2>/dev/null
wait 2>/dev/null

echo "    $pass of $((pass + fail)) checks passed"
[ "$fail" -eq 0 ]
