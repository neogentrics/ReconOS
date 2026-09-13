#!/bin/sh
# Builds the install medium: an image to write to a USB stick.
#
# Laid out the way an installed disk is, and for the same reason -- the machine
# it is put into may have UEFI, may have a BIOS, and may have both, and which
# one it has is not something a stick can ask before it is plugged in:
#
#   protective MBR, with our 440 bytes in its boot code area
#   1  BIOS boot partition   stage 2, for a machine with no UEFI
#   2  EFI system partition  BOOTX64.EFI, BOOTAA64.EFI, the kernels, the stages
#
# A machine with UEFI reads partition 2 and never looks at the first sector. A
# machine with a BIOS reads the first sector and never looks at partition 2.
# Neither is told about the other.
#
# Both architectures on one medium, which KF-128 is about: the two loaders have
# different filenames (BOOTX64.EFI and BOOTAA64.EFI) and so do the two kernels,
# because the first attempt gave them the same name and one medium could carry
# only one architecture.
#
# Usage: scripts/make-medium.sh [output.img]
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OUT=${1:-reconos-medium.img}

for t in sgdisk mkfs.vfat mcopy mmd; do
	command -v "$t" >/dev/null 2>&1 || { echo "need $t" >&2; exit 2; }
done

# Built, not merely looked for (KF-134).
make -C kernel ARCH=x86_64  >/dev/null 2>&1 || true
make -C kernel ARCH=aarch64 >/dev/null 2>&1 || true
make -C boot   ARCH=x86_64  >/dev/null 2>&1 || true
make -C boot   ARCH=aarch64 >/dev/null 2>&1 || true
make -C boot/bios           >/dev/null 2>&1 || true

K64=kernel/build/x86_64/reconos-kernel.elf
KARM=kernel/build/aarch64/reconos-kernel.elf
L64=boot/build/x86_64/BOOTX64.EFI
LARM=boot/build/aarch64/BOOTAA64.EFI
S1=boot/bios/build/stage1.bin
S2=boot/bios/build/stage2.bin

missing=
for f in "$K64" "$L64" "$S1" "$S2"; do
	[ -f "$f" ] || missing="$missing $f"
done
[ -z "$missing" ] || { echo "did not build:$missing" >&2; exit 1; }

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM

# 512 MiB: room for both architectures with space to spare, and small enough to
# write to a stick quickly.
rm -f "$OUT"
truncate -s 512M "$OUT"

sgdisk -n 1:2048:+1M   -t 1:ef02 -c 1:"BIOS boot"    "$OUT" >/dev/null
sgdisk -n 2:0:0        -t 2:ef00 -c 2:"ReconOS Boot" "$OUT" >/dev/null

ESP_LBA=$(sgdisk -i 2 "$OUT" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
ESP_END=$(sgdisk -i 2 "$OUT" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
ESP_MB=$(( (ESP_END - ESP_LBA + 1) / 2048 ))

truncate -s "${ESP_MB}M" "$W/esp.part"
mkfs.vfat -F 32 -n RECONOS "$W/esp.part" >/dev/null

mmd -i "$W/esp.part" ::/EFI ::/EFI/BOOT ::/reconos

mcopy -i "$W/esp.part" "$L64" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$W/esp.part" "$K64" ::/reconos/kernel-x86_64.elf

# aarch64 if it built. A medium that carries one architecture is still a useful
# medium; one that silently carries the wrong one is not, which is why each
# file is named for the architecture it is.
if [ -f "$LARM" ] && [ -f "$KARM" ]; then
	mcopy -i "$W/esp.part" "$LARM" ::/EFI/BOOT/BOOTAA64.EFI
	mcopy -i "$W/esp.part" "$KARM" ::/reconos/kernel-aarch64.elf
	arches="x86_64 and aarch64"
else
	arches="x86_64 only"
fi

# The BIOS stages, for the installer to write onto whatever it installs to.
mcopy -i "$W/esp.part" "$S1" ::/reconos/stage1.bin
mcopy -i "$W/esp.part" "$S2" ::/reconos/stage2.bin

# No cmdline file.
#
# An install medium that carries `install-onto=` installs the moment it is
# booted, on whatever disk that name happens to match, without asking. That is
# right for the test harness, which built the disk it is about to overwrite, and
# it is the single most destructive thing this project could hand somebody on a
# USB stick.
#
# So the medium boots to the kernel and stops. Installing is a thing a person
# asks for, on a machine they are looking at.

dd if="$W/esp.part" of="$OUT" bs=512 seek="$ESP_LBA" conv=notrunc status=none

# Our boot code into the protective MBR, and stage 2 into the partition made
# for it. patch-stage1.sh has already written the sector count; the LBA is the
# default 2048, which is where sgdisk was told to put the partition.
dd if="$S1" of="$OUT" bs=1 count=440 conv=notrunc status=none
dd if="$S2" of="$OUT" bs=512 seek=2048 conv=notrunc status=none

sig=$(od -An -tx1 -j510 -N2 "$OUT" | tr -d ' ')
[ "$sig" = "55aa" ] || { echo "boot signature is $sig, not 55aa" >&2; exit 1; }

echo "  built $OUT"
echo "    $arches, UEFI and BIOS, $(du -h "$OUT" | cut -f1)"
echo "    EFI partition at block $ESP_LBA, ${ESP_MB} MiB"
