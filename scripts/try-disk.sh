#!/usr/bin/env bash
# A single boot with a disk attached, for the fast loop while debugging.
set -u
cd "$(dirname "$0")/.."

bash scripts/make-test-disk.sh >/dev/null

ARCH=${1:-aarch64}

if [ "$ARCH" = aarch64 ]; then
	# force-legacy=false is not a workaround, it is asking for the protocol
	# this kernel implements. QEMU's memory-mapped virtio bus still defaults
	# to the pre-1.0 draft, in which configuration space is in the guest's
	# byte order and a queue is set up by handing the device a page number.
	# That is a different protocol wearing the same name, and supporting both
	# would mean two paths of which only one is ever exercised. Every real
	# user of virtio-mmio -- and QEMU itself, when asked -- speaks 1.0.
	timeout 40 qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-global virtio-mmio.force-legacy=false \
		-kernel kernel/build/aarch64/reconos-kernel.img \
		-drive file=kernel/build/disk.img,format=raw,if=none,id=d0 \
		-device virtio-blk-device,drive=d0
else
	timeout 40 qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-kernel kernel/build/x86_64/reconos-kernel.elf \
		-drive file=kernel/build/disk.img,format=raw,if=none,id=d0 \
		-device virtio-blk-pci,drive=d0
fi
