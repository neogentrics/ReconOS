# reconboot-bios -- the ReconOS bootloader for a machine with no UEFI.
#
# Checkpoint 16. Separate from ../Makefile because nothing is shared: that one
# builds a PE application the firmware loads for us, this one builds flat binary
# images that a 1981 interface reads off a disk by block number.
#
# Same toolchain, though -- clang's integrated assembler, ld.lld and objcopy,
# all of which are already required. No NASM: adding an assembler to the
# dependency list to write 440 bytes would be a poor trade.
#
#   make            stage1.bin, exactly 440 bytes
#   make mbr        a full 512-byte MBR with an empty partition table, for tests
#   make check      prove the size limit is enforced rather than hoped for
#   make clean

BUILD := build

CC      := clang
LD      := ld.lld
OBJCOPY := objcopy

# -m32 with a 16-bit code directive in the source: the assembler emits real-mode
# instructions because the .code16 directive says so, and the target only has to
# agree about the instruction set, not the mode.
ASFLAGS := -target i386-unknown-none -ffreestanding -nostdlib \
           -Wall -Wextra -Werror

.PHONY: all mbr disk run check clean

all: $(BUILD)/stage1.bin $(BUILD)/stage2.bin

# Stage 2 is C, compiled with -m16.
#
# clang emits real-mode code from ordinary C, which matters because what is left
# of this checkpoint -- a FAT32 reader, an RSA-2048 check over a SHA-256 hash,
# page tables and a handoff structure -- would otherwise be hundreds of lines of
# 16-bit assembly nobody could review against the UEFI loader that already does
# the same jobs in C.
#
# -Os rather than -O2: this has a megabyte of partition and no reason to trade
# size for speed, and smaller code in the first stages is smaller reading.
CFLAGS16 := -target i386-unknown-none -m16 -ffreestanding -nostdlib -fno-pic \
            -Os -Wall -Wextra -Werror

$(BUILD)/stage2_entry.o: stage2_entry.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/stage2_c.o: stage2.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS16) -MMD -MP -c $< -o $@

$(BUILD)/stage2.elf: $(BUILD)/stage2_entry.o $(BUILD)/stage2_c.o stage2.ld
	$(LD) -T stage2.ld -o $@ $(BUILD)/stage2_entry.o $(BUILD)/stage2_c.o

-include $(BUILD)/stage2_c.d

$(BUILD)/stage2.bin: $(BUILD)/stage2.elf
	$(OBJCOPY) -O binary $< $@
	@magic=$$(dd if=$@ bs=1 count=4 status=none); \
	if [ "$$magic" != "RCB2" ]; then \
		echo "stage2 does not begin with its magic -- stage 1 will refuse it" >&2; \
		exit 1; \
	fi
	@echo "  built $@ ($$(stat -c%s $@) bytes)"

$(BUILD)/stage1.o: stage1.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/stage1.elf: $(BUILD)/stage1.o stage1.ld
	$(LD) -T stage1.ld -o $@ $<

$(BUILD)/stage1.bin: $(BUILD)/stage1.elf
	$(OBJCOPY) -O binary $< $@
	@size=$$(stat -c%s $@); \
	if [ "$$size" -ne 440 ]; then \
		echo "stage1.bin is $$size bytes, and the partition table starts at 440" >&2; \
		exit 1; \
	fi
	@echo "  built $@ (440 bytes, the whole space before the partition table)"

# A whole first sector, for booting under SeaBIOS without an installer.
#
# The partition table is left empty on purpose: this image exists to prove the
# firmware runs our code, and a machine that boots it should not also be
# claiming to have partitions it does not have.
mbr: $(BUILD)/mbr.img

$(BUILD)/mbr.img: $(BUILD)/stage1.bin
	dd if=/dev/zero of=$@ bs=512 count=1 status=none
	dd if=$< of=$@ conv=notrunc status=none
	printf '\125\252' | dd of=$@ bs=1 seek=510 conv=notrunc status=none
	@sig=$$(od -An -tx1 -j510 -N2 $@ | tr -d ' '); \
	if [ "$$sig" != "55aa" ]; then \
		echo "boot signature is $$sig, not 55aa -- the BIOS will not run this" >&2; \
		exit 1; \
	fi
	@echo "  built $@"

# A whole disk: stage 1 in the first sector, stage 2 where stage 1 looks for it.
#
# LBA 2048 is where the BIOS Boot Partition will start on a real install, and
# it is the value stage1.S carries as its default. The installer will patch it;
# until there is an installer for this path, agreeing on the default is what
# makes the two halves meet.
disk: $(BUILD)/disk.img

$(BUILD)/disk.img: $(BUILD)/mbr.img $(BUILD)/stage2.bin
	dd if=/dev/zero of=$@ bs=1M count=8 status=none
	dd if=$(BUILD)/mbr.img of=$@ conv=notrunc status=none
	dd if=$(BUILD)/stage2.bin of=$@ bs=512 seek=2048 conv=notrunc status=none
	@echo "  built $@"

run: $(BUILD)/disk.img
	qemu-system-x86_64 -m 128M -nographic -no-reboot \
	  -drive file=$(BUILD)/disk.img,format=raw,if=ide

# The size limit is the one guard here that matters, so it is shown to fail
# rather than assumed to work.
#
# The first version of this check appended 400 bytes to the *end* of the file,
# past the `.space` that enforces the limit -- so it grew the binary without
# ever making the guard's subtraction negative, and reported that the guard did
# nothing. The guard was fine. The check was asking a different question, which
# is the more usual way this goes wrong: padding has to go *before* the
# `.space`, where real code would go.
check: $(BUILD)/stage1.bin
	@echo "stage1 must not reach the partition table:"
	@awk '/\.space.*440 - / { print "\t.space 400, 0xCC" } { print }' \
		stage1.S > $(BUILD)/oversize.S
	@if $(CC) $(ASFLAGS) -c $(BUILD)/oversize.S -o $(BUILD)/oversize.o \
		2>$(BUILD)/oversize.log; then \
		echo "  FAILED -- 400 bytes of code past the limit assembled cleanly"; \
		rm -f $(BUILD)/oversize.*; exit 1; \
	else \
		echo "  refused, as it must:"; \
		grep -m1 'error:' $(BUILD)/oversize.log | sed 's/^/    /'; \
		rm -f $(BUILD)/oversize.*; \
	fi
	@echo "  and the signature is where the BIOS looks for it:"
	@$(MAKE) --no-print-directory mbr | sed 's/^/  /'

clean:
	rm -rf $(BUILD)
