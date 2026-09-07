#!/bin/sh
# Checkpoint 16: does a machine with no UEFI at all run our code?
#
# SeaBIOS, an IDE disk, and nothing else. No OVMF, no EFI partition, no
# firmware that knows what a filesystem is -- the interface is "read the first
# sector to 0x7C00 and jump", and everything after that is ours.
#
# What it insists on, and why each one:
#
#   the firmware runs our 440 bytes      the claim
#   stage 2 is read and jumped to        the claim that matters: 440 bytes
#                                        cannot hold a bootloader, so the whole
#                                        design rests on this hop working
#   the boot drive survives the hop      DL is the only way to learn which disk
#                                        the firmware picked, and it is handed
#                                        across by hand
#   no stage 2 says so                   a deliberate fault. A loader that
#                                        jumps into an unread sector does
#                                        something unpredictable instead of
#                                        saying what went wrong
#   a wrong stage 2 says so              a deliberate fault, and the one that
#                                        proves the magic check is connected at
#                                        all -- the LBA is patched into stage 1
#                                        by a program that has finished running
#                                        by the time it matters
#
# The last two are the point. The first three would all pass with the magic
# check deleted.
#
# Usage: scripts/bios-boot-test.sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "need qemu" >&2; exit 2; }

# Built, not merely looked for (BG-134).
make -C boot/bios >/dev/null 2>&1 || true
[ -f boot/bios/build/stage1.bin ] || { echo "stage 1 did not build" >&2; exit 1; }
[ -f boot/bios/build/stage2.bin ] || { echo "stage 2 did not build" >&2; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM

pass=0
fail=0
say() { printf '%-46s' "  $1"; }

# A disk laid out the way stage 1 expects: our boot code in sector 0, stage 2 at
# LBA 2048, which is where the BIOS Boot Partition starts on a real install.
make_disk() {
	dd if=/dev/zero of="$1" bs=1M count=8 status=none
	dd if=boot/bios/build/stage1.bin of="$1" conv=notrunc status=none
	printf '\125\252' | dd of="$1" bs=1 seek=510 conv=notrunc status=none
	if [ "${2:-yes}" = yes ]; then
		dd if=boot/bios/build/stage2.bin of="$1" bs=512 seek=2048 \
			conv=notrunc status=none
	fi
}

# `-display none -serial stdio`, deliberately, and not `-nographic`.
#
# Under -nographic QEMU mirrors the VGA text console to stdout, so INT 10h
# output appears here and a loader that never touches a serial port looks like
# one that does. That mirror also drops characters, which read as truncation and
# sent an evening after a register-clobbering bug that did not exist. With the
# mirror off, the first version of this loader produced nothing at all -- which
# was the true state of it.
#
# So the harness reads the same port a headless machine would, and nothing but
# what the loader actually wrote can reach it.
boot() {
	boot_with 128M "$1"
}

boot_with() {
	timeout 20 qemu-system-x86_64 -m "$1" -display none -serial stdio \
		-no-reboot -drive "file=$2,format=raw,if=ide" 2>&1 | tr -d '\r'
}

# --- before booting anything, is stage 1 going to read all of stage 2? ------
#
# Checked directly rather than inferred from a boot failure. When stage 2
# outgrew the four sectors stage 1 used to read (BG-138), the symptom was a
# machine that printed "ReconOS" and stopped -- and the magic check passed,
# because the first four bytes had arrived. This asks the question the magic
# number cannot: did all of it arrive?

say "stage 1 reads the whole of stage 2"
s2_size=$(stat -c%s boot/bios/build/stage2.bin)
s2_sectors=$(( (s2_size + 511) / 512 ))
count_off=$(( $(nm boot/bios/build/stage1.elf |
	awk '$3 == "stage2_count" { print "0x" $1 }') - 0x7C00 ))
count=$(od -An -tu2 -j "$count_off" -N2 boot/bios/build/stage1.bin | tr -d ' ')

if [ "$count" = "$s2_sectors" ]; then
	echo "$count sectors, $s2_size bytes"
	pass=$((pass + 1))
else
	echo "FAILED -- reads $count sectors, stage 2 needs $s2_sectors"
	fail=$((fail + 1))
fi

# --- the machine boots what we wrote ---------------------------------------

make_disk "$W/good.img"
out=$(boot "$W/good.img")

say "SeaBIOS runs our 440 bytes"
if echo "$out" | grep -q 'ReconOS'; then
	echo "no UEFI in the machine"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$out" | sed -n '/Booting from/,$p' | head -6 | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "stage 1 reads stage 2 and jumps to it"
if echo "$out" | grep -q 'stage2 ok'; then
	echo "440 bytes handed off"
	pass=$((pass + 1))
else
	echo "FAILED"
	echo "$out" | sed -n '/Booting from/,$p' | head -6 | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "the boot drive survives the hop"
if echo "$out" | grep -q 'drive 0x80'; then
	echo "0x80, from the firmware"
	pass=$((pass + 1))
else
	echo "FAILED -- $(echo "$out" | grep -o 'drive 0x..' | head -1)"
	fail=$((fail + 1))
fi

# --- the memory map --------------------------------------------------------

say "reads a memory map from the BIOS"
regions=$(echo "$out" | sed -n 's/^e820: \([0-9]*\) regions.*/\1/p')
if [ -n "$regions" ] && [ "$regions" -ge 4 ]; then
	echo "${regions} regions"
	pass=$((pass + 1))
else
	echo "FAILED -- '$(echo "$out" | grep e820 || echo 'no e820 line')'"
	fail=$((fail + 1))
fi

# The one that a constant cannot pass.
#
# "It printed a number" and "it read the machine" look identical in a test run
# on one machine size. Two sizes, and the number has to follow -- which is the
# same argument the boot-path evidence rests on, and the reason the kernel is
# booted at several processor counts rather than one.
say "and the total follows the machine, not a constant"
small=$(boot_with 128M "$W/good.img" | sed -n 's/^e820: [0-9]* regions, \([0-9]*\) MB.*/\1/p')
large=$(boot_with 512M "$W/good.img" | sed -n 's/^e820: [0-9]* regions, \([0-9]*\) MB.*/\1/p')

if [ -n "$small" ] && [ -n "$large" ] &&
   [ "$small" -ge 100 ] && [ "$small" -le 128 ] &&
   [ "$large" -ge 480 ] && [ "$large" -le 512 ]; then
	echo "${small} MB at 128M, ${large} MB at 512M"
	pass=$((pass + 1))
else
	echo "FAILED -- ${small:-none} MB at 128M, ${large:-none} MB at 512M"
	fail=$((fail + 1))
fi

# --- a disk laid out the way an install will lay one out --------------------
#
# GPT, a BIOS Boot Partition at LBA 2048 holding stage 2, and an EFI System
# Partition after it. Built with sgdisk, which shares no code with this and is
# what actually partitions the disks this will meet.
#
# Our 440 bytes go into the protective MBR's boot code area, which is exactly
# what that space is for: the protective partition entry at offset 446 is left
# untouched, so a tool that reads this disk still sees a GPT disk that is
# entirely claimed.

if command -v sgdisk >/dev/null 2>&1 && command -v mkfs.vfat >/dev/null 2>&1; then
	dd if=/dev/zero of="$W/gpt.img" bs=1M count=64 status=none
	sgdisk -n 1:2048:+1M -t 1:ef02 "$W/gpt.img" >/dev/null 2>&1
	sgdisk -n 2:0:+32M   -t 2:ef00 "$W/gpt.img" >/dev/null 2>&1

	esp_start=$(sgdisk -i 2 "$W/gpt.img" 2>/dev/null |
		sed -n 's/^First sector: \([0-9]*\).*/\1/p')

	dd if=/dev/zero of="$W/esp.part" bs=1M count=32 status=none
	mkfs.vfat -F 32 -n RECONOS "$W/esp.part" >/dev/null 2>&1

	# The real kernel, under the real name, put there by mtools -- which
	# shares no code with the reader that has to find it again. The name
	# needs a long-name entry, and finding it by its 8.3 alias instead is
	# what BG-130 was about.
	make -C kernel ARCH=x86_64 >/dev/null 2>&1 || true
	KELF=kernel/build/x86_64/reconos-kernel.elf
	if [ -f "$KELF" ] && command -v mcopy >/dev/null 2>&1; then
		mmd   -i "$W/esp.part" ::/reconos >/dev/null 2>&1
		mcopy -i "$W/esp.part" "$KELF" ::/reconos/kernel-x86_64.elf
		kernel_size=$(stat -c%s "$KELF")
	else
		kernel_size=
	fi

	dd if="$W/esp.part" of="$W/gpt.img" bs=512 seek="$esp_start" \
		conv=notrunc status=none

	dd if=boot/bios/build/stage1.bin of="$W/gpt.img" bs=1 count=440 \
		conv=notrunc status=none
	dd if=boot/bios/build/stage2.bin of="$W/gpt.img" bs=512 seek=2048 \
		conv=notrunc status=none

	gpt=$(boot "$W/gpt.img")

	say "finds the EFI partition in a real GPT"
	found=$(echo "$gpt" | sed -n 's/^esp: block \([0-9]*\).*/\1/p')
	if [ -n "$found" ] && [ "$found" = "$esp_start" ]; then
		echo "block $found, which is where sgdisk put it"
		pass=$((pass + 1))
	else
		echo "FAILED -- said '${found:-nothing}', sgdisk says $esp_start"
		echo "$gpt" | grep -a 'esp:' | sed 's/^/      /'
		fail=$((fail + 1))
	fi

	# The protective MBR is what stops another tool deciding this disk is
	# unpartitioned and free to take. Writing our boot code into the same
	# sector must not disturb it.
	say "and the protective MBR still says the disk is taken"
	if sgdisk -p "$W/gpt.img" 2>/dev/null | grep -q 'EF02'; then
		echo "sgdisk still reads the table"
		pass=$((pass + 1))
	else
		echo "FAILED -- our 440 bytes damaged the partition table"
		fail=$((fail + 1))
	fi

	# The size is the assertion, not the finding.
	#
	# A reader that walked into the wrong directory entry would still report
	# *a* cluster and *a* size; matching the byte count mtools wrote means
	# it found this file and not a neighbour.
	if [ -n "$kernel_size" ]; then
		say "finds the kernel by its long name"
		got=$(echo "$gpt" | sed -n 's/^kernel: [^,]*, \([0-9]*\) bytes.*/\1/p')
		if [ "$got" = "$kernel_size" ]; then
			echo "$got bytes, as mtools wrote it"
			pass=$((pass + 1))
		else
			echo "FAILED -- said '${got:-nothing}', file is $kernel_size"
			echo "$gpt" | grep -a -E 'esp:|kernel:' | sed 's/^/      /'
			fail=$((fail + 1))
		fi

		# Real mode addresses one megabyte and the kernel belongs above
		# it, so every byte crosses through INT 15h AH=87h -- whose
		# count is in words, which is exactly the kind of detail that
		# transfers half an image and leaves something that looks
		# nearly right. The loader reads the destination back, because
		# a copy that silently did nothing leaves zeroes, and on a
		# machine just powered on so does a failed read.
		say "and puts it above the first megabyte"
		if echo "$gpt" | grep -q 'and it is an ELF'; then
			echo "$(echo "$gpt" | sed -n 's/^kernel: \([0-9]*\) bytes .*ELF.*/\1/p' | head -1) bytes across, ELF header intact"
			pass=$((pass + 1))
		else
			echo "FAILED"
			echo "$gpt" | grep -a 'kernel:' | sed 's/^/      /'
			fail=$((fail + 1))
		fi

		# --- and the whole point of the checkpoint ---------------------
		#
		# Everything above is the loader talking about itself. This is
		# the kernel talking, which is the only evidence that the switch
		# to long mode and the handoff were right rather than merely
		# uncrashing.
		say "the kernel starts, on a machine with no UEFI"
		if echo "$gpt" | grep -q 'ReconOS kernel'; then
			echo "$(echo "$gpt" | grep -oaE 'ReconOS kernel [0-9.]+' | head -1)"
			pass=$((pass + 1))
		else
			echo "FAILED -- it never reached the kernel"
			echo "$gpt" | sed -n '/handing over/,$p' | head -8 | sed 's/^/      /'
			fail=$((fail + 1))
		fi

		# The pairing is the design rule: the BIOS path had to join the
		# set of loaders the kernel cannot tell apart, rather than
		# become a second shape of "how the machine was started". The
		# kernel used to write UEFI in whenever it saw a ReconBoot
		# handoff, because only one loader produced one.
		say "and knows it was a BIOS that started it"
		if echo "$gpt" | grep -qE 'firmware +: BIOS' &&
		   echo "$gpt" | grep -qE 'protocol +: ReconBoot'; then
			echo "BIOS, over ReconBoot"
			pass=$((pass + 1))
		else
			echo "FAILED"
			echo "$gpt" | sed -n '/^Boot$/,+5p' | sed 's/^/      /'
			fail=$((fail + 1))
		fi
	fi
else
	say "finds the EFI partition in a real GPT"
	echo "skipped, sgdisk or mkfs.vfat is missing"
fi

# --- and now the two faults ------------------------------------------------
#
# A check that has never been seen to fire is not known to be connected.

say "a disk with no stage 2 says which step failed"
make_disk "$W/nostage2.img" no
# Truncate so the read of LBA 2048 cannot succeed at all, rather than reading
# zeroes -- this is the "the disk is failing" case, not the "wrong number" case.
dd if=/dev/zero of="$W/nostage2.img" bs=512 count=64 status=none
dd if=boot/bios/build/stage1.bin of="$W/nostage2.img" conv=notrunc status=none
printf '\125\252' | dd of="$W/nostage2.img" bs=1 seek=510 conv=notrunc status=none
bad=$(boot "$W/nostage2.img")

if echo "$bad" | grep -q 'E:RD'; then
	echo "E:RD, and it halted"
	pass=$((pass + 1))
else
	echo "FAILED -- no message, so it jumped somewhere"
	echo "$bad" | sed -n '/Booting from/,$p' | head -6 | sed 's/^/      /'
	fail=$((fail + 1))
fi

say "a stage 2 that is not ours is refused"
make_disk "$W/wrong.img"
# One byte of the magic, changed. Everything else about the disk is correct, so
# the only thing standing between the firmware and a jump into this sector is
# the check being real.
printf 'X' | dd of="$W/wrong.img" bs=1 seek=$(( 2048 * 512 )) conv=notrunc status=none
wrong=$(boot "$W/wrong.img")

if echo "$wrong" | grep -q 'E:S2'; then
	echo "E:S2 -- one byte was enough"
	pass=$((pass + 1))
else
	echo "FAILED -- it jumped into a sector it had not identified"
	echo "$wrong" | sed -n '/Booting from/,$p' | head -6 | sed 's/^/      /'
	fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "  $pass of $pass: a machine with no UEFI runs ReconOS's own loader"
	exit 0
fi

echo "  $fail of $((pass + fail)) failed"
exit 1
