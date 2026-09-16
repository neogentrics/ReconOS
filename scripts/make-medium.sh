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
# Usage: scripts/make-medium.sh [output.img] [size]
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OUT=${1:-reconos-medium.img}

# 512 MiB by default: room for both architectures with space to spare, and
# small enough to write to a stick quickly.
#
# Asked for rather than fixed, because scripts/make-disc.sh appends this whole
# partition to an ISO and 512 MiB of mostly-zero FAT makes a half-gigabyte disc
# out of four megabytes of software. **The size is the only thing it varies**:
# what goes in, what it is called, and that there is no cmdline file are still
# decided here and nowhere else.
SIZE=${2:-512M}

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

rm -f "$OUT"
truncate -s "$SIZE" "$OUT"

sgdisk -n 1:2048:+1M   -t 1:ef02 -c 1:"BIOS boot"    "$OUT" >/dev/null
sgdisk -n 2:0:0        -t 2:ef00 -c 2:"ReconOS Boot" "$OUT" >/dev/null

ESP_LBA=$(sgdisk -i 2 "$OUT" | sed -n 's/^First sector: \([0-9]*\).*/\1/p')
ESP_END=$(sgdisk -i 2 "$OUT" | sed -n 's/^Last sector: \([0-9]*\).*/\1/p')
ESP_MB=$(( (ESP_END - ESP_LBA + 1) / 2048 ))

truncate -s "${ESP_MB}M" "$W/esp.part"
mkfs.vfat -F 32 -n RECONOS "$W/esp.part" >/dev/null

mmd -i "$W/esp.part" ::/EFI ::/EFI/BOOT ::/reconos ::/reconos/fonts

mcopy -i "$W/esp.part" "$L64" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$W/esp.part" "$K64" ::/reconos/kernel-x86_64.elf

# The system itself -- the first file on a ReconOS medium that is neither a
# loader nor a kernel. The installer writes it onto the System volume as
# /System/init.elf, and the machine that boots afterwards runs *that* rather
# than the copy compiled into the kernel.
#
# Absent is tolerated: a medium made without it still installs, and the disk
# still boots on the built-in copy and says so.
INIT=kernel/build/x86_64/user/recon_init.elf
if [ -f "$INIT" ]; then
	mcopy -i "$W/esp.part" "$INIT" ::/reconos/init.elf
fi

# --- and the fonts ---------------------------------------------------------
#
# A machine running its own kernel has no /usr/share, and a desktop with no
# font draws nothing anybody can read. So the medium carries two, and the
# installer writes them to /System/Fonts -- borrowed once here, owned by the
# machine afterwards, which is what the trusted roots and the icons already do.
#
# **Taken from the host that builds the medium**, which is why the licence is
# in THIRD_PARTY.md: a file that ships is a file whose terms ship with it. Two
# rather than three: the Terminal wants a fixed pitch and everything else wants
# a proportional one, and bold falls back to the regular face rather than
# costing another 700 KiB on every stick.
#
# Absent is tolerated, the same as the system above. A medium made on a machine
# with no DejaVu still installs; the desktop then finds no font of its own and
# says so, which is a legible failure rather than a blank screen.
FONT_SANS=""
FONT_MONO=""
for d in /usr/share/fonts/truetype/dejavu /usr/share/fonts/dejavu \
	 /usr/share/fonts/TTF; do
	[ -z "$FONT_SANS" ] && [ -f "$d/DejaVuSans.ttf" ] && \
		FONT_SANS="$d/DejaVuSans.ttf"
	[ -z "$FONT_MONO" ] && [ -f "$d/DejaVuSansMono.ttf" ] && \
		FONT_MONO="$d/DejaVuSansMono.ttf"
done

if [ -n "$FONT_SANS" ]; then
	mcopy -i "$W/esp.part" "$FONT_SANS" ::/reconos/fonts/Sans.ttf
fi
if [ -n "$FONT_MONO" ]; then
	mcopy -i "$W/esp.part" "$FONT_MONO" ::/reconos/fonts/Mono.ttf
fi

# The notice, because the licence says a copy carries it.
#
# *"The above copyright and trademark notices and this permission notice shall
# be included in all copies of one or more of the Font Software typefaces."* A
# medium with the font on it is a copy. THIRD_PARTY.md is not enough on its
# own: that file travels with the repository and the font travels with the
# stick.
#
# Written from the host's own copy of the terms where there is one, so the
# words are the licence's rather than a paraphrase of it.
for c in /usr/share/doc/fonts-dejavu-core/copyright \
	 /usr/share/doc/fonts-dejavu/copyright \
	 /usr/share/licenses/dejavu-fonts/LICENSE; do
	if [ -n "$FONT_SANS" ] && [ -f "$c" ]; then
		mcopy -i "$W/esp.part" "$c" ::/reconos/fonts/COPYRIGHT
		break
	fi
done

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
