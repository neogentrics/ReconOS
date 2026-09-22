#!/usr/bin/env bash
#
# Does `\reconos\cmdline` reach the kernel, on BOTH loaders? (NW-013, KF-262)
#
# **The UEFI boot is the control and it is not optional.** A BIOS boot that
# shows no command line is equally well explained by a cmdline file written to
# the wrong path inside the image -- and that explanation fails in the same
# direction as the fault, so without the other loader booting the same medium,
# a red result here means nothing. The network session made that point when
# they found NW-013 and it is the reason this script exists in this shape.
#
# Four states are asserted, not two, because the fourth is the one both loaders
# used to get wrong:
#
#   1  present, and it reaches the kernel        -- on BIOS and on UEFI
#   2  absent                                    -- and neither loader complains
#   3  too long for the loader's buffer          -- both refuse it, loudly,
#                                                   and the kernel gets NOTHING
#   4  ...which is state 3 on the other loader, asserted separately
#
# State 3 is KF-262. `EFI_FILE_PROTOCOL.Read` returns EFI_SUCCESS on a short
# read, so an over-long file used to arrive silently truncated -- and
# `boot_cmdline_has` matches whole tokens, so a `logport` cut to `logpo`
# matches nothing and the switch simply does not happen. It failed closed and
# silently, which is the combination that costs a day.
#
# Refuses rather than reporting an answer it cannot back: exit 2 when the tools
# or OVMF are missing, exit 1 when a loader is wrong.
set -u

cd "$(dirname "$0")/.."
ROOT=$PWD
OVMF=${OVMF:-/usr/share/ovmf/OVMF.fd}

for t in qemu-system-x86_64 mcopy mdir sgdisk; do
	command -v "$t" >/dev/null || { echo "  no $t; cannot answer"; exit 2; }
done
[ -f "$OVMF" ] || { echo "  no OVMF at $OVMF; the control boot cannot run"; exit 2; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

pass=0
fail=0

check() {
	if [ "$1" = yes ]; then
		printf '    %-56s ok\n' "$2"
		pass=$((pass + 1))
	else
		printf '    %-56s FAILED\n' "$2"
		[ -n "${3:-}" ] && printf '      %s\n' "$3"
		fail=$((fail + 1))
	fi
}

echo "  the kernel command line, on both loaders"

bash scripts/make-medium.sh "$W/m.img" >/dev/null 2>&1 || {
	echo "    the medium would not build"; exit 2; }

# Where the EFI partition starts, asked of the image rather than assumed.
esp_lba=$(sgdisk -i 2 "$W/m.img" 2>/dev/null | grep -oE 'First sector: [0-9]+' | grep -oE '[0-9]+')
[ -n "$esp_lba" ] || { echo "    could not find the EFI partition"; exit 2; }
off=$((esp_lba * 512))

# Put a command line on the medium, then READ IT BACK OFF THE IMAGE. A file
# written to the wrong path is the competing explanation for every failure
# below, so it is ruled out here rather than argued about later.
set_cmdline() {
	printf '%s' "$1" > "$W/cmdline"
	mcopy -o -i "$W/m.img@@$off" "$W/cmdline" ::/reconos/cmdline 2>/dev/null || return 1
	mdir -i "$W/m.img@@$off" ::/reconos 2>/dev/null | grep -qi cmdline
}

clear_cmdline() {
	mdel -i "$W/m.img@@$off" ::/reconos/cmdline 2>/dev/null
	! mdir -i "$W/m.img@@$off" ::/reconos 2>/dev/null | grep -qi cmdline
}

# Did the kernel actually run?
#
# **The first version of this script had no such question, and it cost the
# afternoon.** A stray signing key left in the tree made the loader refuse an
# unsigned kernel, so the BIOS guest never started one -- and the assertion
# "bios is silent when there is no cmdline" PASSED, because a machine that
# prints nothing is silent about everything. A check satisfied by the absence
# of the thing it is checking is not a check.
#
# The graphics session's harness says this better than I can: *the guest never
# reached the filesystem, so there is no image to prove the checker against --
# this is not a filesystem fault.*
booted() {
	echo "$1" | grep -qa 'ReconOS kernel'
}

# What the KERNEL says about its command line, which is not the same string as
# what a LOADER says about one.
#
# The first version grepped for `command line`, and the loader's own refusal --
# "the command line is too long for this loader" -- contains it. So the
# assertion that nothing was passed to the kernel was satisfied by the message
# saying nothing was passed. KF-207's shape: a pattern another subsystem can
# satisfy by printing.
kernel_cmdline() {
	echo "$1" | grep -aoE 'command line : .*' | head -1
}

boot() {
	local mode=$1 out=$2 pid n
	if [ "$mode" = uefi ]; then
		timeout 90 qemu-system-x86_64 -m 512M -display none \
			-serial file:"$out" -no-reboot -bios "$OVMF" \
			-device qemu-xhci,id=xhci \
			-drive if=none,id=stick,format=raw,file="$W/m.img" \
			-device usb-storage,bus=xhci.0,drive=stick \
			>/dev/null 2>&1 &
	else
		timeout 90 qemu-system-x86_64 -m 512M -display none \
			-serial file:"$out" -no-reboot \
			-drive if=none,id=d0,format=raw,file="$W/m.img" \
			-device ide-hd,drive=d0 >/dev/null 2>&1 &
	fi
	pid=$!
	n=0
	while [ "$n" -lt 320 ]; do
		grep -qa 'Idling' "$out" 2>/dev/null && break
		kill -0 "$pid" 2>/dev/null || break
		sleep 0.25
		n=$((n + 1))
	done
	kill "$pid" 2>/dev/null || true
	wait "$pid" 2>/dev/null || true
	tr -d '\r' < "$out"
}

# --- 1. present, and it reaches the kernel ---------------------------------

WANT="logport verbose"

if ! set_cmdline "$WANT"; then
	echo "    could not write the cmdline onto the medium"; exit 2
fi

for mode in bios uefi; do
	got=$(boot "$mode" "$W/$mode.log")
	if ! booted "$got"; then
		check no "$mode carries the command line to the kernel" \n			"the kernel never started -- this says nothing about cmdline"
		continue
	fi
	line=$(kernel_cmdline "$got")
	if echo "$line" | grep -qa "$WANT"; then
		check yes "$mode carries the command line to the kernel"
	else
		check no "$mode carries the command line to the kernel" \
			"the kernel said: ${line:-(no command line line at all)}"
	fi
done

# --- 2. absent, and neither loader complains -------------------------------

if clear_cmdline; then
	for mode in bios uefi; do
		got=$(boot "$mode" "$W/${mode}-none.log")
		if ! booted "$got"; then
			check no "$mode is silent when there is no cmdline" \n				"the kernel never started -- silence proves nothing"
			continue
		fi
		if echo "$got" | grep -qaiE 'cmdline:|command line is too long|would not read whole'; then
			check no "$mode is silent when there is no cmdline" \
				"$(echo "$got" | grep -aiE 'cmdline:|too long|read whole' | head -1)"
		else
			check yes "$mode is silent when there is no cmdline"
		fi
	done
else
	echo "    could not remove the cmdline; states 2 and 3 not asked"
	fail=$((fail + 1))
fi

# --- 3. too long: refused loudly, and NOTHING passed -----------------------
#
# 200 bytes against a 128-byte field. The word that matters is at the front, so
# a loader that truncated instead of refusing would still pass "logport" to the
# kernel -- which is why the assertion is on the kernel's line being ABSENT
# rather than on the loader's complaint alone.

LONG="logport $(head -c 200 /dev/zero | tr '\0' 'x')"

if set_cmdline "$LONG"; then
	for mode in bios uefi; do
		got=$(boot "$mode" "$W/${mode}-long.log")
		if ! booted "$got"; then
			check no "$mode refuses an over-long cmdline out loud" \n				"the kernel never started"
			continue
		fi
		said=$(echo "$got" | grep -aiE 'too long|would not read whole' | head -1)
		kern=$(kernel_cmdline "$got")

		if [ -n "$said" ]; then
			check yes "$mode refuses an over-long cmdline out loud"
		else
			check no "$mode refuses an over-long cmdline out loud" \
				"nothing was said about it"
		fi

		if [ -z "$kern" ]; then
			check yes "$mode passes none of it rather than a prefix"
		else
			check no "$mode passes none of it rather than a prefix" \
				"the kernel got: $kern"
		fi
	done
else
	echo "    could not write the over-long cmdline"
	fail=$((fail + 1))
fi

echo
echo "    $pass of $((pass + fail)) checks passed"
[ "$fail" -eq 0 ] || exit 1
