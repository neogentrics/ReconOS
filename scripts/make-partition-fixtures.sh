#!/usr/bin/env bash
#
# Disk images with real partition tables on them, for the kernel to read.
#
# --- Why these are made by somebody else's tool ---
#
# The obvious way to test a partition-table reader is to write a table with your
# own code and read it back. That tests nothing: a writer and a reader built from
# the same misunderstanding agree perfectly. If I have the CRC seed wrong in both
# halves, the round trip passes and the first real disk fails.
#
# So these are built by sgdisk and sfdisk, which are decades old, are what
# everybody else's disks were partitioned with, and share no code and no
# assumptions with this kernel. What the kernel reads back is then compared
# against what *those* tools say is there -- ground truth from outside.
#
# --- The three layouts, and what each is here to break ---
#
#   gpt.img     an ordinary GPT disk. Three partitions with distinct types and
#               names, and deliberate free space at the end, so a reader that
#               assumes partitions are contiguous or that the last one runs to
#               the end of the disk is caught.
#
#   mbr.img     an MBR disk with an *extended* partition holding two logical
#               ones. Logical partitions are a linked list living inside the
#               extended partition, with offsets relative to different bases
#               depending on which link you are on -- which is the single most
#               misparsed structure in the format, and the reason this image
#               exists rather than a simple four-primary one.
#
#   hybrid.img  a GPT disk whose protective MBR also carries real entries. Apple
#               ships these, and they are how a reader that trusts the MBR first
#               silently reads the wrong table on a Mac. A correct reader sees
#               0xEE and believes the GPT; an incorrect one sees a plausible
#               MBR and believes that.
#
# Each image gets a .expected file listing what the tools say is on it, in the
# same form the kernel prints, and scripts/verify-kernel.sh boots the kernel
# against each image and compares.
#
# That last sentence was false for one commit: this script said the harness
# compared the two and nothing wired it in. A file that describes a check nobody
# runs is worse than no file, because it reads as coverage. Wired in before the
# reader it checks was written, so the harness failed first and had to be made
# to pass.

set -eu

cd "$(dirname "$0")/.."
out=${1:-kernel/build/fixtures}

mkdir -p "$out"

SECTORS_PER_MIB=2048		# at 512 bytes a sector, which these images use

blank() {
	dd if=/dev/zero of="$1" bs=1M count=64 status=none
}

# ---------------------------------------------------------------- GPT

gpt="$out/gpt.img"
blank "$gpt"

sgdisk \
	--new=1:2048:18431      --typecode=1:EF00 --change-name=1:"ESP" \
	--new=2:18432:67583     --typecode=2:8300 --change-name=2:"recon-root" \
	--new=3:67584:100351    --typecode=3:0700 --change-name=3:"data" \
	"$gpt" >/dev/null

# A FAT32 filesystem in the first one. Not needed to read the table, and needed
# at checkpoint 14 -- the UEFI system partition is FAT32 and is where this
# kernel's own bootloader has to be written, which makes it the one foreign
# filesystem that is not optional.
mkfs.vfat -F 32 -n ESP --offset 2048 "$gpt" 16384 >/dev/null 2>&1 || \
	echo "note: could not put a FAT32 filesystem in the ESP; the table is still valid"

sgdisk --print "$gpt" > "$out/gpt.sgdisk.txt"

# ---------------------------------------------------------------- MBR

mbr="$out/mbr.img"
blank "$mbr"

# sfdisk reads a layout from stdin. Written as a here-document because the
# layout is the specification of the fixture and belongs beside it.
sfdisk --no-reread --no-tell-kernel "$mbr" >/dev/null 2>&1 <<-'LAYOUT'
	label: dos
	unit: sectors

	start=2048,   size=32768,  type=0c
	start=34816,  size=32768,  type=83
	start=67584,  size=63488,  type=05
	start=69632,  size=28672,  type=83
	start=100352, size=30720,  type=83
LAYOUT

sfdisk --dump "$mbr" > "$out/mbr.sfdisk.txt" 2>/dev/null

# ------------------------------------------------------------- Hybrid

hybrid="$out/hybrid.img"
cp "$gpt" "$hybrid"

# Put partitions 1 and 2 into the MBR as well, which is what Apple's installer
# does so that a BIOS can boot a GPT disk. The MBR then looks real and is not
# the truth.
sgdisk --hybrid=1:2 "$hybrid" >/dev/null 2>&1 || \
	echo "note: could not build a hybrid MBR; skipping that case"

sgdisk --print "$hybrid" > "$out/hybrid.sgdisk.txt" 2>/dev/null || true

# ---------------------------------------------------------- Expectations
#
# What the kernel should report, one partition per line, in the order it should
# find them. Compared against the kernel's own output by the harness.
#
# Written by hand from the layouts above rather than generated from the tools'
# output: a generator that reformats sgdisk's answer would agree with sgdisk
# about anything sgdisk got wrong, and the point of this file is to be a second
# opinion.

# The kernel prints geometry and nothing else: which slice, where it starts,
# where it ends. Not names, not type codes -- those are what the table *means*,
# and meaning belongs to the caller. So the expectations name no names, and the
# richer columns move to a second file when there is a desktop-side parser to
# check against them.
cat > "$out/gpt.expected" <<-'EOF'
	gpt 3
	1 2048 18431
	2 18432 67583
	3 67584 100351
EOF

# Four, not five. sfdisk's layout has five entries, but one of them -- number
# three, at 67584 -- is the extended *container*, which is not a partition and
# holds no filesystem. It exists to be walked, and what is found inside it is
# numbered from five. A count that included it would be counting the box as one
# of the things in the box.
cat > "$out/mbr.expected" <<-'EOF'
	mbr 4
	1 2048 34815
	2 34816 67583
	5 69632 98303
	6 100352 131071
EOF

# The hybrid must read exactly like the plain GPT disk. That is the whole test:
# its master boot record is not empty and not obviously protective -- the
# protective entry covers 2047 sectors rather than the disk, and sits beside two
# entries that describe real partitions correctly. Every wrong reader produces a
# *plausible* answer here, which is why the expected answer is "ignore all of
# that and read the GPT".
cat > "$out/hybrid.expected" <<-'EOF'
	gpt 3
	1 2048 18431
	2 18432 67583
	3 67584 100351
EOF

echo "fixtures in $out:"
ls -1 "$out"
