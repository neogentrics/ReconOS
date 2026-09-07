#!/bin/sh
# Write stage 2's real size into stage 1, instead of hoping the constant is
# still true.
#
# stage1.S carries the number of sectors to read as a literal, and stage 2 grew
# past it the moment a FAT32 reader went in: 2797 bytes against the 2048 that
# were being read. The magic check at the start of stage 2 passed, because the
# first four bytes had loaded perfectly -- and stage 1 jumped into an image
# whose second half was whatever the disk had there before.
#
# **That is the failure the magic check cannot catch**, and it is silent: no
# error, no message, just a machine that stops. So the number is not maintained
# by hand any more. It is computed from the file and written into the binary,
# and the offset comes from the symbol table rather than from counting bytes in
# a hex dump -- an offset worked out by hand goes wrong silently the first time
# stage1.S is edited.
#
# Usage: patch-stage1.sh <stage1.elf> <stage1.bin> <stage2.bin> [lba]
set -eu

ELF=$1
BIN=$2
STAGE2=$3
LBA=${4:-}

LOAD_ADDRESS=$((0x7C00))

# Many BIOSes refuse more than 127 sectors in a single extended read, whatever
# the packet's 16-bit count field allows. Stage 2 has a megabyte of partition
# and no need to approach this, so exceeding it means something is wrong rather
# than something needs a second read.
MAX_SECTORS=127

symbol_offset() {
	addr=$(nm "$ELF" | awk -v s="$1" '$3 == s { print $1 }')
	[ -n "$addr" ] || { echo "no symbol '$1' in $ELF" >&2; exit 1; }
	echo $(( 0x$addr - LOAD_ADDRESS ))
}

# A 16-bit little-endian value, written with octal escapes because dash's printf
# has no \x -- it writes the six characters "\x55\xaa" instead, which is how the
# boot signature came to be missing once.
poke16() {
	off=$1
	val=$2
	lo=$(( val & 0xFF ))
	hi=$(( (val >> 8) & 0xFF ))
	printf "\\$(printf '%03o' "$lo")\\$(printf '%03o' "$hi")" |
		dd of="$BIN" bs=1 seek="$off" conv=notrunc status=none
}

size=$(stat -c%s "$STAGE2")
sectors=$(( (size + 511) / 512 ))

if [ "$sectors" -gt "$MAX_SECTORS" ]; then
	echo "stage 2 is $size bytes, $sectors sectors -- more than the $MAX_SECTORS" >&2
	echo "a single BIOS extended read can be relied on to fetch" >&2
	exit 1
fi

poke16 "$(symbol_offset stage2_count)" "$sectors"

if [ -n "$LBA" ]; then
	off=$(symbol_offset stage2_lba)
	poke16 "$off" $(( LBA & 0xFFFF ))
	poke16 $(( off + 2 )) $(( (LBA >> 16) & 0xFFFF ))
fi

# Read it back. A patch that silently did nothing would leave the previous
# value, which is exactly the state this script exists to prevent.
got=$(od -An -tu2 -j "$(symbol_offset stage2_count)" -N2 "$BIN" | tr -d ' ')
if [ "$got" != "$sectors" ]; then
	echo "patched $sectors sectors, read back $got" >&2
	exit 1
fi

echo "  stage 1 will read $sectors sectors ($size bytes of stage 2)"
