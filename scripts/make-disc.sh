#!/bin/sh
# Builds the install disc: an ISO to burn, or to boot a virtual machine from.
#
# The same software as the stick, on the other kind of medium. A person who
# cannot write a USB stick -- an old machine, a locked-down one, a virtual
# machine whose host hands it an ISO and nothing else -- gets the same ReconOS
# by the same two firmware paths.
#
# --- the ESP is taken from the stick, not built again ------------------------
#
# scripts/make-medium.sh decides what an EFI system partition contains: which
# loaders, which kernels, what they are called, and that there is no cmdline
# file. Writing that out a second time here would mean two authors for one
# fact, and the day they disagree the disc quietly carries something the stick
# does not. So a medium is built and its partition 2 is lifted out whole.
#
# It is asked for a smaller one. The partition is appended to the ISO entire,
# zeroes included, so the stick's 512 MiB would make a half-gigabyte disc out of
# four megabytes of software. Only the size differs; the contents are still the
# other script's business.
#
# --- why the BIOS boot image carries stage 2 ---------------------------------
#
# On a disk stage 2 sits at LBA 2048 because the installer put it there, and
# stage 1 is told to read it from that block. **On a disc there is no such
# place**: xorriso decides where the El Torito boot image lands, so there is no
# block number stage 1 could be given.
#
# El Torito loads more than one sector, though. So the boot image is stage 1, a
# sector of padding, and stage 2 -- loaded together to 0x7C00, which puts stage
# 2 at 0x8000, exactly where stage 1 expects to find it.
#
# Stage 1 is then told to read **zero sectors**, through the same patchable
# field the installer writes. Its magic check still runs and still has to pass,
# so a disc whose stage 2 failed to load is refused rather than jumped into --
# the check is not skipped, only the read that would have preceded it.
#
# --- and why the ESP is a file in the ISO, with no partition table ----------
#
# It was an appended GPT partition first, so that stage 2 could find it the way
# it finds one on a stick -- by walking the GPT for the EFI type GUID -- and so
# that the firmware and stage 2 would read one copy of the same bytes. **The
# firmware would not follow it.** OVMF answered "failed to load ... Not Found"
# and fell through to PXE.
#
# Three discs differing in one thing each said which thing:
#
#   the ESP as a file in the ISO tree, El Torito length 0        boots
#   the same ESP, reached by --interval:appended_partition_2     does not
#
# So the zero length was innocent. That field is sixteen bits counting 512-byte
# units, which caps at 32 MiB, and a FAT32 volume cannot be smaller than about
# 33 MiB -- so *every* FAT32 EFI boot image ever made has a zero there. What
# this firmware will not follow is a boot entry pointing into an appended
# partition.
#
# The ESP is therefore a plain file in the ISO, which the firmware boots. Stage
# 2 reads its location out of the El Torito boot catalogue instead of a GPT --
# the same entry the firmware follows -- so there is still one copy and both
# firmwares still read the same bytes. See find_eltorito() in
# boot/bios/stage2.c.
#
# Usage: scripts/make-disc.sh [output.iso]
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OUT=${1:-reconos.iso}

for t in xorriso sgdisk nm; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM

./scripts/make-medium.sh "$W/medium.img" 64M >/dev/null

ESP_LBA=$(sgdisk -i 2 "$W/medium.img" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
ESP_END=$(sgdisk -i 2 "$W/medium.img" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
[ -n "$ESP_LBA" ] && [ -n "$ESP_END" ] || { echo "no EFI partition on the medium" >&2; exit 1; }

dd if="$W/medium.img" of="$W/efi.img" bs=512 \
   skip="$ESP_LBA" count=$(( ESP_END - ESP_LBA + 1 )) status=none

# --- the BIOS boot image ----------------------------------------------------
S1=boot/bios/build/stage1.bin
S2=boot/bios/build/stage2.bin
s2_bytes=$(stat -c%s "$S2")
s2_sectors=$(( (s2_bytes + 511) / 512 ))
total=$(( 2 + s2_sectors ))		# stage 1, a pad sector, then stage 2

dd if=/dev/zero of="$W/bios.img" bs=512 count="$total" status=none
dd if="$S1" of="$W/bios.img" conv=notrunc status=none
printf '\125\252' | dd of="$W/bios.img" bs=1 seek=510 conv=notrunc status=none
dd if="$S2" of="$W/bios.img" bs=512 seek=2 conv=notrunc status=none

# Read no sectors: stage 2 came with us.
#
# The offset is read out of stage 1's own symbol table rather than written down
# here, for the reason stage1.S gives where it describes the patch fields -- an
# offset kept in a second place is wrong the first time that file is edited,
# silently, on a stranger's disc.
count_off=$(( $(nm boot/bios/build/stage1.elf |
	awk '$3 == "stage2_count" { print "0x" $1 }') - 0x7C00 ))
printf '\0\0' | dd of="$W/bios.img" bs=1 seek="$count_off" conv=notrunc status=none

# The disc's two boot images, and nothing else.
#
# An earlier version also unpacked the loaders and kernels into the ISO tree so
# the disc could be browsed. That is a second copy of every file efi.img already
# holds, and a second copy is a second answer: the day they differ, the one a
# person reads is not the one the machine boots. efi.img is a FAT volume any
# system can mount, so nothing is hidden by leaving it packed.
mkdir -p "$W/iso"
cp "$W/bios.img" "$W/iso/bios.img"
cp "$W/efi.img"  "$W/iso/efi.img"

xorriso -as mkisofs -quiet -o "$OUT" -V RECONOS \
	-b bios.img -no-emul-boot -boot-load-size "$total" \
	-eltorito-alt-boot \
	-e efi.img -no-emul-boot \
	"$W/iso" 2>/dev/null

[ -s "$OUT" ] || { echo "xorriso produced nothing" >&2; exit 1; }

echo "  built $OUT"
echo "    UEFI and BIOS, $(du -h "$OUT" | cut -f1)"
echo "    the BIOS boot image is $total sectors: stage 1, a pad, and $s2_sectors of stage 2"
echo "    the EFI volume is the stick's, built by the same script"
