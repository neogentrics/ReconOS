#!/bin/sh
# Installs from a real install medium onto a blank disk, and then boots the disk.
#
# This is the only test in the project that checks the thing the whole project
# is for. Everything else proves a component works; this proves that a machine
# with nothing on it, given ReconOS media, ends up starting ReconOS from its own
# disk with the media removed.
#
# The medium is built the way one really is: an ESP with the removable-media
# path, both architectures' loaders, and both kernels. The target is empty. Then
# the target is booted **on its own**, with no medium attached, which is the
# part that cannot be faked -- if anything the installer wrote is missing or
# wrong, the firmware simply does not find a system.
#
# Usage: scripts/install-then-boot-test.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

KERNEL=kernel/build/x86_64/reconos-kernel.elf
LOADER=boot/build/x86_64/BOOTX64.EFI
OVMF=/usr/share/ovmf/OVMF.fd

# Built, not merely looked for. A script that only checks a binary exists
# will happily test one compiled before the change it is meant to prove --
# which is how a security check came to be silently absent (KF-133).
make -C kernel ARCH="${ARCH:-x86_64}" >/dev/null 2>&1 || true
[ -f "$KERNEL" ] || { echo "the kernel did not build" >&2; exit 1; }
[ -f "$OVMF" ] || { echo "OVMF is not installed"; exit 2; }
for t in sgdisk mkfs.vfat mcopy mmd mdir; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done

make -C boot ARCH=x86_64 >/dev/null 2>&1 || true
[ -f "$LOADER" ] || { echo "the bootloader is not built"; exit 2; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM

pass=0
fail=0
# 52, because three of the labels below are longer than 46 and the
# column of answers had been stepping out to meet them.
say() { printf '%-52s' "  $1"; }

# --- the install medium, laid out the way a real one is ---------------------

truncate -s 1G "$W/medium.img"
sgdisk -n 1:2048:0 -t 1:ef00 -c 1:"ReconOS Boot" "$W/medium.img" >/dev/null

dd if=/dev/zero of="$W/esp.part" bs=1M count=1000 status=none
mkfs.vfat -F 32 -n RECONOS "$W/esp.part" >/dev/null
mmd   -i "$W/esp.part" ::/EFI ::/EFI/BOOT ::/reconos
mcopy -i "$W/esp.part" "$LOADER" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$W/esp.part" "$KERNEL" ::/reconos/kernel-x86_64.elf

# And the system, which is what the disk will actually run. Without this the
# installed machine falls back to the copy inside the kernel -- which is the
# thing this file exists to stop being the only option.
INIT=kernel/build/x86_64/user/recon_init.elf
[ -f "$INIT" ] && mcopy -i "$W/esp.part" "$INIT" ::/reconos/init.elf

# And the BIOS loader, which a real medium carries for the same reason it
# carries BOOTX64.EFI: the machine this is installed on may have no UEFI in it,
# and the installer cannot write a loader it was not given.
make -C boot/bios >/dev/null 2>&1 || true
if [ -f boot/bios/build/stage1.bin ] && [ -f boot/bios/build/stage2.bin ]; then
	mcopy -i "$W/esp.part" boot/bios/build/stage1.bin ::/reconos/stage1.bin
	mcopy -i "$W/esp.part" boot/bios/build/stage2.bin ::/reconos/stage2.bin
fi

# What the kernel is told to do, on the medium rather than on the QEMU command
# line. `-append` only exists on the `-kernel` path, which skips the bootloader
# entirely -- so a test that used it would be testing a boot nobody's machine
# ever performs. This is how an install medium actually carries its intent.
printf 'install-onto=nvme1n1' > "$W/cmdline"
mcopy -i "$W/esp.part" "$W/cmdline" ::/reconos/cmdline
dd if="$W/esp.part" of="$W/medium.img" bs=512 seek=2048 conv=notrunc status=none

truncate -s 8G "$W/target.img"

# --- install: booted from the medium, writing to the target -----------------

say "installs from the medium onto a blank disk"
out=$(timeout 180 qemu-system-x86_64 -bios "$OVMF" -m 512M -nographic \
	-drive "file=$W/medium.img,format=raw,if=none,id=m0" \
	-device nvme,serial=medium,drive=m0 \
	-drive "file=$W/target.img,format=raw,if=none,id=t0" \
	-device nvme,serial=target,drive=t0 2>&1 | tr -d '\r')

if echo "$out" | grep -q 'installed: table'; then
	echo "$(echo "$out" | grep -oE '[0-9]+ files copied from the medium')"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$out" | sed -n '/^installer:/,$p' | head -14 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# Where the EFI partition is, asked of the disk rather than assumed.
#
# This used to be a literal 1 MiB, which was true while the ESP was the first
# partition and stopped being true the moment a BIOS boot partition went in
# front of it. The failure read as "non DOS media" -- mtools looking at the
# BIOS partition and correctly saying it is not a filesystem -- which is a
# confusing way to be told that a constant went stale.
esp_lba=$(sgdisk -p "$W/target.img" 2>/dev/null |
	awk '$6 == "EF00" { print $2; exit }')

# **Refused rather than defaulted.** (KF-260)
#
# This was `${esp_lba:-2048}`, and that default is the same stale constant the
# paragraph above is about: 2048 blocks is the 1 MiB that stopped being right
# the moment a BIOS boot partition went in front of the ESP. So when sgdisk
# found nothing -- no EF00, a damaged table, sgdisk missing -- the fallback
# quietly rebuilt the exact wrong offset whose symptom the comment calls a
# confusing way to be told a constant went stale.
#
# An empty capture becoming a plausible number is the shape that produced a
# false "0 sectors written" here on 18 September: `$((b - a))` over two empty
# strings is 0, and `$(( ${x:-2048} * 512 ))` is 1048576. Both are answers
# nobody measured.
#
# Measured, so the three cases are on the record rather than assumed: a bare
# empty variable is 0 silently, a `:-` default is its default silently, and an
# empty command substitution with no default is a loud syntax error. Only the
# last one tells you anything.
if [ -z "$esp_lba" ]; then
	echo "FAILED -- sgdisk found no EF00 partition on the target image."
	echo "  Not defaulting: the old default was 2048, which is the stale"
	echo "  offset this lookup exists to replace, and using it here would"
	echo "  report a filesystem fault about a partition nobody located."
	exit 1
fi

esp_at=$(( esp_lba * 512 ))

say "the loader and kernel are on the target"
if mdir -i "$W/target.img@@$esp_at" ::/EFI/BOOT 2>/dev/null | grep -qi 'BOOTX64' &&
   mdir -i "$W/target.img@@$esp_at" ::/reconos 2>/dev/null | grep -qi 'kernel'; then
	echo "BOOTX64.EFI and a kernel, at block $esp_lba"
	pass=$((pass + 1))
else
	echo "FAILED"
	mdir -i "$W/target.img@@$esp_at" -/ :: 2>&1 | sed 's/^/      /' | head -12
	fail=$((fail + 1))
fi

# Whether a boot's own self-tests passed.
#
# Worth its own function because it is asked of two boots, and because the
# interesting case is the second one: both are the same installed disk, so the
# second boot is the only place in the whole matrix where a ReconFS volume is
# mounted that something has already written to. Every other path attaches
# sixty-four megabytes of zeroes, on which the five tests that need a volume
# print "no volume on this machine" and are counted as having run.
#
# Reports what failed rather than that something did. A test name is enough to
# find it; "self-tests failed" is not.
self_tests_passed() {
	local out=$1 label=$2 bad

	bad=$(printf '%s\n' "$out" | grep -E ': +FAIL' || true)

	if [ -n "$bad" ]; then
		echo "FAILED"
		printf '%s\n' "$bad" | sed 's/^/      /' | head -8
		return 1
	fi

	# A boot that printed no self-tests at all is not a pass. It is a boot
	# that stopped before them, and the grep above cannot tell the two
	# apart -- which is the failure this whole function exists to stop
	# being invisible.
	if ! printf '%s\n' "$out" | grep -qE ': +pass'; then
		echo "NO SELF-TESTS RAN"
		return 1
	fi

	echo "$(printf '%s\n' "$out" | grep -cE ': +pass') passed, $label"
	return 0
}

# --- and the part that cannot be faked --------------------------------------
#
# The target, alone. No medium, no -kernel, nothing but firmware and a disk the
# installer wrote. Everything the installer got wrong shows up here as silence.

say "the installed disk boots on its own"
boot=$(timeout 90 qemu-system-x86_64 -bios "$OVMF" -m 512M -nographic \
	-drive "file=$W/target.img,format=raw,if=none,id=t0" \
	-device nvme,serial=target,drive=t0 2>&1 | tr -d '\r')

# A harness that will not show its working is one whose result has to be taken
# on trust, and the status board quotes these transcripts as evidence. Setting
# KEEP_LOG keeps the serial output; nothing else changes.
[ -n "${KEEP_LOG:-}" ] && printf '%s\n' "$boot" > "$KEEP_LOG"

if echo "$boot" | grep -q 'ReconOS kernel'; then
	echo "$(echo "$boot" | grep -oE 'ReconOS kernel [0-9.]+' | head -1)"
	pass=$((pass + 1))
else
	echo "DID NOT BOOT"
	echo "$boot" | tail -14 | sed 's/^/      /'
	fail=$((fail + 1))
fi

# The first boot on a volume that has one. Five tests run here and nowhere else
# in the matrix, and until now nothing asked what they said.
say "and its own tests pass, with a volume under them"
if self_tests_passed "$boot" "first boot on this volume"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
fi

# --- and the part that makes it a system rather than a kernel ---------------
#
# The kernel carries a copy of the first-boot program inside its own image and
# falls back to it when the volume has none -- deliberately, because a machine
# that has not been installed onto has nowhere else to get one. That fallback
# is why this has to be asserted rather than observed: without it, an installer
# that quietly stopped writing `/System/init.elf` would still produce a machine
# that boots and draws a screen, and this file would still print 7 of 7.
#
# `from the volume` is the kernel saying it loaded somebody else's program off
# somebody else's disk, which is the whole difference between a kernel that
# *contains* a system and one that *starts* one.
say "and the system it runs came off the volume"
if echo "$boot" | grep -qa 'the system: /System/init.elf, from the volume'; then
	echo "/System/init.elf, not the copy in the kernel"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$boot" | grep -a 'the system:' | sed 's/^/      /' | head -2
	fail=$((fail + 1))
fi

# --- and the program it started could ask for memory ------------------------
#
# `SYS_MAP` with a descriptor of -1 is what an allocator calls, and it answered
# EBADF until v0.4.33 -- so `malloc` worked on the host and returned NULL here,
# and every part of the path between the two had been built and none of it had
# ever been run. `recon_init` allocates sixty-four blocks, frees half, takes
# them again, takes one large enough to need a second range from the kernel,
# reads every one of them back and audits the heap.
#
# **Present and clean, and absence is a failure.** The version of this that
# only checked for a bad line passed on the kernel that handed the same address
# out twice: the allocator put a region onto itself, walked the loop, and the
# program never printed anything at all. So the match is the good line, not the
# absence of a bad one.
say "and the program it ran could ask for memory"
if echo "$boot" | grep -qa 'the heap: [0-9]* blocks and [0-9]* KiB written, read back and freed'; then
	echo "$boot" | grep -a 'the heap:' | sed 's/^ *the heap: //' | head -1
	pass=$((pass + 1))
else
	echo "FAILED"
	if echo "$boot" | grep -qa 'the heap:'; then
		echo "$boot" | grep -a 'the heap:' | sed 's/^/      /' | head -2
	else
		echo "      it never said -- the program did not get that far"
	fi
	fail=$((fail + 1))
fi

# --- and it could hold a socket ---------------------------------------------
#
# The five socket calls landed with no caller, and the kernel session said what
# that meant: their self-test cannot hold a descriptor, because a kernel thread
# has no process and fd_install fails there for sockets exactly as it does for
# pipes. So "a socket is a file" was argued from the design and never run.
#
# A program in ring 3 is the only thing that can answer it, and closing twice is
# the sharp half: a socket that was never really installed closes as many times
# as it likes.
say "and it could hold a socket, and let go of it once"
if echo "$boot" | grep -qa 'a socket: a descriptor, closed once, refused twice'; then
	echo "$boot" | grep -a 'a socket:' | sed 's/^ *a socket: //' | head -1
	pass=$((pass + 1))
else
	echo "FAILED"
	if echo "$boot" | grep -qa 'a socket:'; then
		echo "$boot" | grep -a 'a socket:' | sed 's/^/      /' | head -2
	else
		echo "      it never said -- the program did not get that far"
	fi
	fail=$((fail + 1))
fi

# --- and the same disk again, on a machine with no UEFI in it ---------------
#
# The disk above booted under OVMF, through the EFI partition. This is the same
# disk with SeaBIOS underneath it instead: no firmware that knows what a
# filesystem is, nothing but "read the first sector and jump".
#
# It is the same image, not a second install. If the installer wrote one path
# and broke the other, exactly one of these two assertions fails -- which is the
# whole reason to boot it twice rather than once.

# The BIOS boot's serial output, kept when asked for. The UEFI boot above has
# had KEEP_LOG since it was written; this one had nothing, which is part of why
# two of its self-tests could fail unnoticed.
keep_bios() { [ -n "${KEEP_BIOS_LOG:-}" ] && printf '%s
' "$1" > "$KEEP_BIOS_LOG"; return 0; }

say "and boots the same disk with no UEFI at all"
bios_boot=$(timeout 90 qemu-system-x86_64 -m 512M -display none -serial stdio \
	-no-reboot -drive "file=$W/target.img,format=raw,if=ide" 2>&1 |
	tr -d '\r')

if echo "$bios_boot" | grep -qa 'ReconOS kernel' &&
   echo "$bios_boot" | grep -qaE 'firmware +: BIOS'; then
	echo "$(echo "$bios_boot" | grep -oaE 'ReconOS kernel [0-9.]+' | head -1), over BIOS"
	pass=$((pass + 1))
else
	echo "DID NOT BOOT"
	echo "$bios_boot" | sed -n '/Booting from/,$p' | head -12 | sed 's/^/      /'
	fail=$((fail + 1))
fi

keep_bios "$bios_boot"

say "and it finds its own partitions"
if echo "$boot" | grep -q 'nvme0n1p3'; then
	echo "$(echo "$boot" | grep -cE 'nvme0n1p[0-9]') partitions"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$boot" | grep -E 'nvme0n1|table' | sed 's/^/      /' | head -8
	fail=$((fail + 1))
fi

# The second boot of the same volume, and the only one anywhere in the matrix.
#
# This is where a test that leaves a file behind shows itself: the first boot
# created it and this one finds it already there. The block layer's own test has
# said since it was written that "a test that leaves the disk modified is a test
# that can only be run once" -- and puts back every byte it borrows. Nothing was
# checking whether the tests above it did the same.
#
# --- Why this is its own boot and not the BIOS one ---
#
# It used to be asked of `$bios_boot`, and could not have failed. Over BIOS the
# kernel does not find the disk it booted from -- KF-192, open -- so the boot
# report reads `root : none found; file calls will say so`, the five tests that
# need a volume say so, and a boot that never touched a volume was counted as
# "passed, second boot on this volume".
#
# The fault it was written to catch was sitting there the whole time: five
# self-tests create a file with a fixed name and none cleared it first, so
# every second boot of an installed disk reported five FAILs (KF-229). It took
# booting the installed disk twice by hand to see it.
#
# Hence the guard below. A check that quietly becomes vacuous is worse than one
# that is missing, so this one says so rather than passing.
say "and they pass again, on a volume already written to"
again=$(timeout 90 qemu-system-x86_64 -bios "$OVMF" -m 512M -nographic \
	-drive "file=$W/target.img,format=raw,if=none,id=t0" \
	-device nvme,serial=target,drive=t0 2>&1 | tr -d '\r')

if ! echo "$again" | grep -qaE 'root +: +nvme'; then
	echo "NO VOLUME UNDER IT"
	echo "$again" | grep -aE 'root +:' | sed 's/^/      /' | head -3
	fail=$((fail + 1))
elif self_tests_passed "$again" "second boot on this volume"; then
	pass=$((pass + 1))
else
	fail=$((fail + 1))
fi


echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: installed from media, and the disk booted by itself"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
