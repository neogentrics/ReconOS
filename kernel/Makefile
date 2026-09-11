# ReconOS kernel build.
#
# Deliberately a Makefile and not CMake, and deliberately separate from the
# CMake build at the top of this repository. Those two builds target different
# machines: the compositor is a Linux program that links against wlroots, and
# the kernel is a freestanding image that links against nothing at all. Making
# one build system serve both means every kernel flag needs a guard saying it
# does not apply to userland, and vice versa.
#
#   make ARCH=x86_64          build (the default architecture)
#   make ARCH=aarch64
#   make all-arches           build every architecture, which is how a change
#                             is checked for having stayed portable
#   make run ARCH=aarch64     boot it under QEMU, serial on this terminal
#   make check-portable       fail if core/ has learned about a machine
#   make clean

# What works, not what is planned.
#
# **A patch per bug fixed.** BG-187 and BG-188 take this to 0.1.7; the six
# before them -- BG-163, 182, 183, 184, 185, 186 -- are why it is not 0.1.1.
# The number sat at 0.1.0 through all of them and twenty-five commits, not
# because anything forbade a bump but because nothing asked for one. That is
# the same failure as a counter nobody prints: it is only correct while
# somebody remembers.
#
# **0.2.0 is not reached until sections 1.1 to 1.9 of the blueprint audit are
# all built.** Joshua's rule, and a better one than "a minor when it gains a
# capability" -- that version was a judgement call about what counts as big,
# made by whoever happened to be bumping the number. This one is a fact about
# the audit that anybody can check.
#
# Eleven rows stand in the way as of 11 September, plus the four things section
# 1.8 states in prose: IPC (signals, sockets, message queues), the HPET and
# timer wheel, Block I/O finishing verification, ext2/4 and the pseudo
# filesystems, USB host controllers, I2C/SPI, self-relocation, video, embedded
# filesystems, initrd, and the whole of the network stack.
#
# 0.1.0 was raised from 0.0.11 on the older argument -- the kernel had stopped
# fixing things and started gaining them -- and a patch number that only ever
# goes up is a version that has stopped saying anything. That reasoning still
# explains 0.1.0; it is not what decides 0.2.0.
#
# Written here rather than in a document because this is the line that has to
# change, and a rule kept anywhere else is a rule read after the fact.
VERSION := 0.1.7

ARCH ?= x86_64
BUILD := build/$(ARCH)

# --- Per-architecture toolchain and flags ------------------------------------

ifeq ($(ARCH),x86_64)
  CROSS   ?=
  ARCHFLAGS := -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
               -mcmodel=kernel
  QEMU     := qemu-system-x86_64
  # The ELF carries its own boot headers -- Multiboot2 and a PVH note -- so it
  # is what gets booted directly.
  BOOT_IMAGE = $(KERNEL)
  QEMUFLAGS = -kernel $(KERNEL) -m 512M -no-reboot
else ifeq ($(ARCH),aarch64)
  CROSS   ?= aarch64-linux-gnu-
  # -mno-outline-atomics: by default gcc does not emit atomic instructions
  # directly. It emits a *call* to a libgcc helper which checks at run time
  # whether this CPU has the large-system-extension atomics and picks the fast
  # or the slow path. That is a good default for a program and useless for a
  # kernel: there is no libgcc, so the link fails on __aarch64_swp1_acq with no
  # hint as to where it came from. Inline atomics work on every ARMv8, and
  # checkpoint 3 already detects LSE for ourselves.
  ARCHFLAGS := -mgeneral-regs-only -mno-outline-atomics
  QEMU     := qemu-system-aarch64
  # aarch64 loaders read a header at the front of a *flat* image, not an ELF.
  # The ELF is kept for debugging; the .img is what boots.
  BOOT_IMAGE = $(BUILD)/reconos-kernel.img
  QEMUFLAGS = -M virt -cpu cortex-a72 -m 512M -kernel $(BOOT_IMAGE)
else
  $(error Unknown ARCH '$(ARCH)'. Supported: x86_64 aarch64)
endif

CC := $(CROSS)gcc
LD := $(CROSS)ld
OBJCOPY := $(CROSS)objcopy

# -ffreestanding: there is no libc and no runtime under us.
# -fno-stack-protector: the guard would call a libc symbol that does not exist.
# -fno-pic/-no-pie: the image is loaded at a fixed physical address.
CFLAGS := -std=c11 -O2 -g \
          -ffreestanding -fno-stack-protector -fno-pic -fno-builtin \
          -fno-omit-frame-pointer \
          -Wall -Wextra -Werror -Wno-unused-parameter \
          -Iinclude -I../boot/include \
          -DRECONOS_KERNEL_VERSION=\"$(VERSION)\" \
          $(ARCHFLAGS)

ASFLAGS := $(ARCHFLAGS) -fno-pic -Iinclude -I$(BUILD)/user
LDFLAGS := -nostdlib -no-pie -z noexecstack -z max-page-size=4096

# --- Sources -----------------------------------------------------------------

CORE_SRCS := $(wildcard core/*.c)
ARCH_SRCS := $(wildcard arch/$(ARCH)/*.c)
ARCH_ASM  := $(wildcard arch/$(ARCH)/*.S)

# core/ has one .S, and it holds no instructions -- it carries a file into
# the image with .incbin. check-portable reads these too, so "portable" keeps
# meaning the same thing here as it does for the C.
CORE_ASM  := $(wildcard core/*.S)

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRCS) $(ARCH_SRCS)) \
        $(patsubst %.S,$(BUILD)/%.o,$(ARCH_ASM) $(CORE_ASM))

LINKER_SCRIPT := arch/$(ARCH)/linker.ld
KERNEL := $(BUILD)/reconos-kernel.elf

# --- The program the kernel loads --------------------------------------------
#
# A real ELF, linked by the ordinary cross linker, embedded in the kernel image
# and parsed at run time by the kernel's own loader.
#
# Built rather than hand-written for the same reason the partition fixtures are
# built by sgdisk and the FAT32 ones by mkfs.vfat: a loader tested against a
# structure this project also wrote is tested against its own misunderstandings.
# The linker decides the program headers here, including the ones nobody
# thought to arrange.
#
# It is embedded rather than read from a disk *for now* -- there is no path from
# a file on a volume to a running program yet, and this is the half of it that
# can be finished first. The bytes are identical either way, which is the point:
# when the filesystem half arrives, the loader does not change.
USER_ELF := $(BUILD)/user/hello.elf

# --- Rules -------------------------------------------------------------------

.PHONY: all all-arches run clean check-portable info

all: $(BOOT_IMAGE)

$(BUILD)/reconos-kernel.img: $(KERNEL)
	$(OBJCOPY) -O binary $< $@
	@echo "  built $@"

$(KERNEL): $(OBJS) $(LINKER_SCRIPT)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T $(LINKER_SCRIPT) -o $@ $(OBJS)
	@echo
	@echo "  built $@"
	@size $@ 2>/dev/null || $(CROSS)size $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

# -nostdlib and its own script: no C runtime, no libc, and nothing between
# `_start` and the first instruction.
$(USER_ELF): user/hello.S user/user.ld
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c user/hello.S -o $(BUILD)/user/hello.o
	$(LD) -nostdlib -no-pie -z noexecstack -T user/user.ld -o $@ $(BUILD)/user/hello.o
	@echo "  built $@"

# The blob that carries it into the kernel image. `.incbin` rather than
# objcopy's `-I binary`, because objcopy names its symbols after the path it
# was given and that name changes when the build directory does.
$(BUILD)/core/user_elf.o: core/user_elf.S $(USER_ELF)

-include $(OBJS:.o=.d)

all-arches:
	$(MAKE) ARCH=x86_64
	$(MAKE) ARCH=aarch64

run: $(BOOT_IMAGE)
	$(QEMU) $(QEMUFLAGS) -display none -serial mon:stdio

# The rule that keeps arch.h meaningful: nothing under core/ may name a
# machine, embed assembly, or reach a hardware address. When this fails, the
# fix is a new function in include/recon/kernel/arch.h, not an #ifdef.
#
# Comments are stripped before the check runs. The rule is about what the code
# does, not what the prose is allowed to mention -- a portable file explaining
# *why* it is portable will name the machines it is avoiding, and failing the
# build for that teaches people to write worse comments.
#
# --- The numeric clause was replaced, and this is why ---
#
# It used to flag any hex constant of six digits or more, on the theory that
# such a number is a hardware address. Over checkpoints 11 and 12 that clause
# fired six times and was right none of them: it caught "virt" read out of a
# 32-bit register, every-block-there-could-be, the all-ones written to size a
# base address register, a CRC polynomial, and two published checksum values.
# Each time, correct code was rewritten to appease it. A check that makes people
# rephrase working code is teaching the wrong lesson -- it becomes the author's
# adversary rather than the reader's friend -- and one with no true positives is
# not paying for that.
#
# What replaced it is what a hardware address actually *looks like when it is
# used*: an integer literal cast to a pointer. That is the only way core/ can
# reach a fixed location, because everything else it touches arrives as a
# pointer from arch/ or from a bus. Checked against a file of nine real
# violations and a file of legitimate constants before it was adopted, rather
# than assumed to work.
#
# Port instructions are named too. They exist on exactly one architecture, and a
# portable file containing one is not portable no matter what it looks like.
#
# This is a smoke alarm, not a proof. The real guarantee is that core/ compiles
# unchanged for both architectures and is linked against two different arch/
# directories, and a genuinely machine-specific assumption fails there.
check-portable:
	@bad=""; \
	for f in core/*.c core/*.h core/*.S; do \
		[ -e "$$f" ] || continue; \
		hit=$$(sed -e 's|/\*.*\*/||g' -e '\|/\*|,\|\*/|d' -e 's|//.*||' "$$f" \
		      | grep -nE '__asm__|asm volatile|\bx86|\baarch64|\briscv|\*[[:space:]]*\)[[:space:]]*0x[0-9a-fA-F]{6,}|\bout[bwl]\b|\bin[bwl]\b' \
		      | sed "s|^|$$f:|"); \
		[ -n "$$hit" ] && bad="$$bad$$hit\n"; \
	done; \
	if [ -n "$$bad" ]; then \
		echo "core/ is not portable any more:"; printf "$$bad"; exit 1; \
	fi; \
	echo "core/ is clean: no machine-specific code outside arch/"

# The interface the desktop compiles against, built for the *host* rather than
# for the kernel image: it is userland code that will one day call into a
# kernel, and today it links against stubs that say so. Kept honest by being
# compiled with the same warnings-as-errors as everything else, so a header
# that does not compile is found here rather than in the compositor's build.
API_LIB := build/api/librecon_kernel_stubs.a
HOSTCC  ?= cc

.PHONY: api
api: $(API_LIB)

$(API_LIB): api/stubs.c include/recon_kernel.h
	@mkdir -p build/api
	$(HOSTCC) -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter \
	          -Iinclude -c api/stubs.c -o build/api/stubs.o
	ar rcs $@ build/api/stubs.o
	@echo "  built $@"

info:
	@echo "ARCH    = $(ARCH)"
	@echo "CC      = $(CC)"
	@echo "KERNEL  = $(KERNEL)"
	@echo "SOURCES = $(CORE_SRCS) $(ARCH_SRCS) $(ARCH_ASM)"

clean:
	rm -rf build
