#!/bin/bash
#
# Does a kernel command line reach the kernel? Asked of both loaders.
#
# --- Why this exists ---------------------------------------------------------
#
# `boot/bios/stage2.c` writes an empty string into the handoff's cmdline field
# and never reads `\reconos\cmdline`. The UEFI loader reads that file, strips a
# trailing CR or LF, and passes it on. So every switch this kernel has is a
# UEFI switch:
#
#   logport   verbose   noinit   recovery   poweroff   restart
#
# and nothing anywhere says so. That is NW-013, and it is open.
#
# It matters because of where it lands. The server in `docs/BARE-METAL.md` is
# legacy BIOS. `logport.h` says the log port exists because the kernel's own
# medium is USB and USB is the broken thing (KF-256) -- so it exists for the
# machine that cannot record its own failure, and on that machine there is no
# way to ask for it. `noinit` goes with it, and `noinit` is what makes
# `xhci.c` print PORTSC for every port as the controller comes up: the
# diagnostic for a USB fault, unavailable on the machine with the USB fault.
#
# --- Why nothing caught it ---------------------------------------------------
#
# `scripts/logport-test.sh` passes 7 of 7, over virtio-net and over e1000, and
# every one of those runs enables the port with QEMU's `-append` on the
# `-kernel` path -- which is neither loader. `verify-kernel.sh` does the same
# wherever it needs a switch. **The BIOS loader's command line has never been
# exercised by anything in this tree**, so nothing could have failed. A feature
# can be fully tested and completely unreachable, and a green suite will not
# mention it.
#
# --- The shape of the test ---------------------------------------------------
#
# One medium, one cmdline file, booted both ways. **The UEFI boot is the
# control and it is not optional.** Without it, "no log port under BIOS" is
# equally well explained by a cmdline file written to the wrong path or in the
# wrong format -- in which case neither boot shows it, and the conclusion comes
# out wrong in the same direction as the guess that prompted the test. The file
# is also read back off the image before booting, so "the file is there and
# says what it should" is a measurement rather than an assumption.
#
# Two switches are asked for, not one. `logport` announces itself in the boot
# report and `verbose` changes it, so a run that shows neither is not a fact
# about the log port in particular.
#
#   scripts/cmdline-test.sh
#
# Exits 0 when the two loaders agree, 1 when they disagree (the fault is
# present), and 2 when the test could not answer -- which is a separate thing
# from the fault and says so.
#
# Needs: qemu-system-x86_64, OVMF, sgdisk, mtools. All of them are already
# needed by scripts/make-medium.sh or verify-kernel.sh.

set -u

cd "$(dirname "$0")/.."

OVMF=${OVMF:-/usr/share/ovmf/OVMF.fd}

command -v qemu-system-x86_64 >/dev/null || { echo "no qemu"; exit 2; }
command -v sgdisk            >/dev/null || { echo "no sgdisk"; exit 2; }
command -v mcopy             >/dev/null || { echo "no mtools"; exit 2; }

if [ ! -f "$OVMF" ]; then
	echo "no OVMF at $OVMF -- refusing to run."
	echo "  The UEFI boot is this test's control. Without it a BIOS boot"
	echo "  showing nothing proves nothing, because a cmdline file written"
	echo "  wrongly looks exactly like a loader that ignores it."
	exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
IMG=$WORK/medium.img

echo "  the kernel command line, through both loaders"

scripts/make-medium.sh "$IMG" 512M >"$WORK/mk.log" 2>&1 || {
	echo "    could not build the medium:"; tail -5 "$WORK/mk.log"; exit 2; }

ESP_LBA=$(sgdisk -i 2 "$IMG" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
[ -n "$ESP_LBA" ] || { echo "    could not find the EFI partition"; exit 2; }
ESP_OFF=$((ESP_LBA * 512))

WANT='logport verbose'
printf '%s' "$WANT" > "$WORK/cmdline"
mcopy -o -i "$IMG@@$ESP_OFF" "$WORK/cmdline" ::/reconos/cmdline 2>/dev/null || {
	echo "    could not write the cmdline file onto the medium"; exit 2; }

# Read back, so its presence is measured rather than assumed.
GOT=$(mcopy -i "$IMG@@$ESP_OFF" ::/reconos/cmdline - 2>/dev/null)
if [ "$GOT" != "$WANT" ]; then
	echo "    the cmdline file did not read back as written:"
	echo "      wrote [$WANT] read [$GOT]"
	echo "    this test cannot say anything about either loader"
	exit 2
fi

boot() {
	local out=$1; shift
	timeout 180 qemu-system-x86_64 -m 1024M -display none -no-reboot \
		-netdev user,id=n0 -device e1000,netdev=n0 \
		-drive if=none,id=stick,format=raw,file="$IMG" \
		-device usb-ehci,id=ehci \
		-device usb-storage,bus=ehci.0,drive=stick,bootindex=0 \
		"$@" -serial file:"$out" >/dev/null 2>&1
}

boot "$WORK/bios.log"
boot "$WORK/uefi.log" -bios "$OVMF"

# `grep -c` prints 0 *and* exits 1 when it finds nothing, so `|| echo 0` would
# append a second zero and the comparisons below would die on a two-line
# number. `|| true` swallows the status without printing anything; the default
# covers a missing file, where grep prints nothing at all.
bios=$(grep -ac "log port" "$WORK/bios.log" 2>/dev/null || true); bios=${bios:-0}
uefi=$(grep -ac "log port" "$WORK/uefi.log" 2>/dev/null || true); uefi=${uefi:-0}

# Did either boot get far enough to be asked? A kernel that never started says
# nothing about command lines, and must not be read as one that ignored it.
for f in bios uefi; do
	if ! grep -aq "ReconOS kernel" "$WORK/$f.log" 2>/dev/null; then
		echo "    the $f boot never reached the kernel, so this test"
		echo "    cannot answer. Its output:"
		tail -5 "$WORK/$f.log" | sed 's/^/      /'
		exit 2
	fi
done

printf '    %-56s %s\n' "UEFI carries the command line" \
	"$([ "$uefi" -gt 0 ] && echo ok || echo FAILED)"
printf '    %-56s %s\n' "BIOS carries the command line" \
	"$([ "$bios" -gt 0 ] && echo ok || echo "FAILED -- NW-013")"

if [ "$uefi" -eq 0 ]; then
	echo
	echo "    The control failed: UEFI did not carry it either. That is a"
	echo "    fault in this test or in the medium, not evidence about the"
	echo "    BIOS loader. Do not record a result from this run."
	exit 2
fi

if [ "$bios" -eq 0 ]; then
	echo
	echo "    NW-013 is present. The UEFI boot reports:"
	grep -a "command line" "$WORK/uefi.log" | head -1 | sed 's/^/      /'
	echo "    and the BIOS boot reports no command line at all."
	echo "    boot/bios/stage2.c writes an empty string and never reads"
	echo "    \\reconos\\cmdline. dir_find and read_file are already there."
	exit 1
fi

echo "    both loaders carry it -- NW-013 is fixed"
exit 0
