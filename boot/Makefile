# reconboot -- the ReconOS UEFI bootloader.
#
# Built by clang and lld rather than by gcc and ld, and that is the reason the
# loader has its own build: a UEFI application is a PE binary, not an ELF, and
# clang can target `<arch>-unknown-windows` for both architectures from one
# installation. gcc would need a separate mingw cross-compiler per architecture,
# and there is no aarch64 one packaged.
#
# No gnu-efi. The UEFI declarations we use are in include/efi.h, written from
# the specification, for the same reason the kernel has no libc.
#
#   make ARCH=x86_64        build BOOTX64.EFI
#   make ARCH=aarch64       build BOOTAA64.EFI
#   make all-arches
#   make esp                a FAT image with the loader and the kernel on it
#   make run                boot that image under QEMU with real UEFI firmware

ARCH  ?= x86_64
BUILD := build/$(ARCH)

ifeq ($(ARCH),x86_64)
  TRIPLE   := x86_64-unknown-windows
  EFI_NAME := BOOTX64.EFI
  QEMU     := qemu-system-x86_64
  # OVMF is the reference UEFI implementation, and running against it is the
  # only way to know the loader works rather than compiles.
  FIRMWARE := -bios /usr/share/ovmf/OVMF.fd
  MACHINE  :=
else ifeq ($(ARCH),aarch64)
  TRIPLE   := aarch64-unknown-windows
  EFI_NAME := BOOTAA64.EFI
  QEMU     := qemu-system-aarch64
  FIRMWARE := -bios /usr/share/AAVMF/AAVMF_CODE.no-secboot.fd
  MACHINE  := -M virt -cpu cortex-a72
else
  $(error Unknown ARCH '$(ARCH)'. Supported: x86_64 aarch64)
endif

CC := clang
LD := lld-link

# -fshort-wchar: UEFI strings are UTF-16 and CHAR16 must be two bytes.
# -mno-red-zone: on x86_64 the firmware may take an interrupt at any point, and
#                the red zone assumes nobody will.
# -fno-stack-protector: the guard would call a runtime that does not exist here.
CFLAGS := -target $(TRIPLE) \
          -std=c11 -O2 \
          -ffreestanding -fshort-wchar -fno-stack-protector -fno-stack-check \
          -Wall -Wextra -Werror -Wno-unused-parameter \
          -Iinclude

ifeq ($(ARCH),x86_64)
  CFLAGS += -mno-red-zone
endif

# subsystem:efi_application is what makes the firmware willing to run it.
# nodefaultlib because there is no C runtime to link against.
LDFLAGS := -subsystem:efi_application -nodefaultlib -dll -entry:efi_main

SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
EFI  := $(BUILD)/$(EFI_NAME)

KERNEL_ELF := ../kernel/build/$(ARCH)/reconos-kernel.elf
ESP_IMG    := $(BUILD)/esp.img

.PHONY: all all-arches esp run clean

all: $(EFI)

$(EFI): $(OBJS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) $(OBJS) -out:$@
	@echo "  built $@"

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

all-arches:
	$(MAKE) ARCH=x86_64
	$(MAKE) ARCH=aarch64

# A FAT filesystem laid out the way UEFI expects to find one.
#
# EFI/BOOT/BOOT<arch>.EFI is the "removable media path": the one filename the
# firmware will run without a boot entry having been registered in NVRAM. It is
# what makes a USB stick bootable on a machine that has never seen it, and it is
# therefore what install media uses.
$(ESP_IMG): $(EFI) $(KERNEL_ELF)
	@mkdir -p $(dir $@)
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mkfs.vfat -F 32 -n RECONOS $@ >/dev/null
	mmd   -i $@ ::/EFI ::/EFI/BOOT ::/reconos
	mcopy -i $@ $(EFI) ::/EFI/BOOT/$(EFI_NAME)
	mcopy -i $@ $(KERNEL_ELF) ::/reconos/kernel.elf
	@echo "  built $@"

esp: $(ESP_IMG)

$(KERNEL_ELF):
	$(MAKE) -C ../kernel ARCH=$(ARCH)

run: $(ESP_IMG)
	$(QEMU) $(MACHINE) $(FIRMWARE) -m 512M \
	  -drive format=raw,file=$(ESP_IMG) \
	  -display none -serial mon:stdio

clean:
	rm -rf build
