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

VERSION := 0.0.1

ARCH ?= x86_64
BUILD := build/$(ARCH)

# --- Per-architecture toolchain and flags ------------------------------------

ifeq ($(ARCH),x86_64)
  CROSS   ?=
  ARCHFLAGS := -m64 -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mno-80387 \
               -mcmodel=small
  QEMU     := qemu-system-x86_64
  QEMUFLAGS := -kernel $(KERNEL) -no-reboot
else ifeq ($(ARCH),aarch64)
  CROSS   ?= aarch64-linux-gnu-
  ARCHFLAGS := -mgeneral-regs-only
  QEMU     := qemu-system-aarch64
  QEMUFLAGS := -M virt -cpu cortex-a72 -m 256M -kernel $(KERNEL)
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
          -Iinclude \
          -DRECONOS_KERNEL_VERSION=\"$(VERSION)\" \
          $(ARCHFLAGS)

ASFLAGS := $(ARCHFLAGS) -Iinclude
LDFLAGS := -nostdlib -no-pie -z noexecstack -z max-page-size=4096

# --- Sources -----------------------------------------------------------------

CORE_SRCS := $(wildcard core/*.c)
ARCH_SRCS := $(wildcard arch/$(ARCH)/*.c)
ARCH_ASM  := $(wildcard arch/$(ARCH)/*.S)

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRCS) $(ARCH_SRCS)) \
        $(patsubst %.S,$(BUILD)/%.o,$(ARCH_ASM))

LINKER_SCRIPT := arch/$(ARCH)/linker.ld
KERNEL := $(BUILD)/reconos-kernel.elf

# --- Rules -------------------------------------------------------------------

.PHONY: all all-arches run clean check-portable info

all: $(KERNEL)

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

-include $(OBJS:.o=.d)

all-arches:
	$(MAKE) ARCH=x86_64
	$(MAKE) ARCH=aarch64

run: $(KERNEL)
	$(QEMU) $(QEMUFLAGS) -display none -serial mon:stdio

# The rule that keeps arch.h meaningful: nothing under core/ may name a
# machine, embed assembly, or reach a hardware address. When this fails, the
# fix is a new function in include/recon/kernel/arch.h, not an #ifdef.
check-portable:
	@bad=$$(grep -nE '__asm__|asm volatile|x86|aarch64|0x[0-9a-fA-F]{6,}' core/*.c core/*.h 2>/dev/null || true); \
	if [ -n "$$bad" ]; then \
		echo "core/ is not portable any more:"; echo "$$bad"; exit 1; \
	fi; \
	echo "core/ is clean: no machine-specific code outside arch/"

info:
	@echo "ARCH    = $(ARCH)"
	@echo "CC      = $(CC)"
	@echo "KERNEL  = $(KERNEL)"
	@echo "SOURCES = $(CORE_SRCS) $(ARCH_SRCS) $(ARCH_ASM)"

clean:
	rm -rf build
