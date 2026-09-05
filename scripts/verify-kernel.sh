#!/usr/bin/env bash
#
# Boot the kernel every way it can be booted, and report every self-test.
#
# The reason this script exists rather than a paragraph in a commit message:
# "it works" is a claim about one boot path, and this kernel has eight. A change
# to the memory map can be right under GRUB and wrong under our own loader; a
# change to the exception vectors can be right on one processor and wrong on
# four. The only way to know is to run them all, every time, and read the
# counts rather than the absence of a crash.
#
#   scripts/verify-kernel.sh              every path
#   scripts/verify-kernel.sh x86_64       just one architecture's paths
#
# Needs: qemu, grub-mkrescue, xorriso, mtools, dosfstools, clang, lld, OVMF,
# AAVMF. Every one of them is packaged; none of them is linked into the kernel.

set -u

cd "$(dirname "$0")/.."
ROOT=$PWD
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ONLY=${1:-all}
TIMEOUT=${TIMEOUT:-45}

OVMF_X64=/usr/share/ovmf/OVMF.fd
OVMF_ARM=/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd

passes=0
failures=0
skipped=0
declare -a FAILED_PATHS=()

# Runs QEMU, then reads the log rather than trusting the exit status. A kernel
# that hangs before printing anything and a kernel that panics both "exit"; only
# the self-test lines say which happened.
# EXPECT, when set before a call, is a string the log must also contain.
#
# It exists because of a specific way this harness could lie. The block
# self-test reports "pass" on a machine with no disk attached, which is correct
# -- a diskless machine must still boot -- but it means a driver that silently
# stopped finding disks would go on reporting eleven passes for ever. So a run
# that was *given* a disk is required to say it found one, and the counts alone
# are not enough.
check() {
	local name=$1; shift
	local log="$WORK/$(echo "$name" | tr ' /' '__').log"
	local want=${EXPECT:-}

	EXPECT=

	printf '%-46s' "$name"
	timeout "$TIMEOUT" "$@" >"$log" 2>&1

	# No end-of-line anchors. QEMU's serial console ends every line with a
	# carriage return as well as a newline, so a pattern anchored with $
	# matches nothing at all -- which reads exactly like a kernel that never
	# booted, and cost one confusing run to notice.
	local ran fail
	ran=$(grep -c ': \(pass\|FAIL\)' "$log")
	fail=$(grep -c ': FAIL' "$log")

	if [ "$ran" -eq 0 ]; then
		echo "NO OUTPUT -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
		return
	fi

	if [ "$fail" -ne 0 ]; then
		echo "$fail of $ran FAILED -- $log"
		grep ': FAIL' "$log" | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
		return
	fi

	# A panic can follow a clean run of the tests -- the idle loop is where
	# a bad exception vector shows up -- so the log is checked past them.
	if grep -q 'kernel fault\|PANIC' "$log"; then
		echo "$ran passed, then it panicked -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
		return
	fi

	if [ -n "$want" ] && ! grep -q "$want" "$log"; then
		echo "$ran passed, but never said '$want' -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("$name")
		return
	fi

	echo "$ran self-tests, all pass"
	passes=$((passes + ran))
}

# The same, for a run that was given a disk: the first argument is the name the
# kernel must report having found.
#
# A function rather than a variable assignment in front of the call, because
# bash restores a prefix assignment after a *function* returns -- so the flag
# would survive into the next check and fail the diskless run for not finding a
# disk it was never offered.
check_for() {
	EXPECT=$1
	shift
	check "$@"
}

skip() {
	printf '%-46s%s\n' "$1" "skipped: $2"
	skipped=$((skipped + 1))
}

# --- Build ------------------------------------------------------------------

echo "Building."
make -C kernel ARCH=x86_64  >/dev/null || { echo "x86_64 kernel build FAILED"; exit 1; }
make -C kernel ARCH=aarch64 >/dev/null || { echo "aarch64 kernel build FAILED"; exit 1; }
make -C kernel check-portable >/dev/null || { echo "core/ is no longer portable"; exit 1; }

X64_ELF=$ROOT/kernel/build/x86_64/reconos-kernel.elf
ARM_IMG=$ROOT/kernel/build/aarch64/reconos-kernel.img

# A disk to attach, so the block layer is tested against something rather than
# reporting that it found nothing and calling that a pass.
#
# A fresh one per run. The block self-test restores every byte it borrows, so
# reusing an image would work -- and a test whose correctness depends on the
# previous run having tidied up is a test that hides the first failure to do so.
DISK=$WORK/disk.img
dd if=/dev/zero of="$DISK" bs=1M count=64 status=none

# force-legacy=false asks QEMU for virtio 1.0 on its memory-mapped bus, which
# still defaults to the pre-1.0 draft. That draft is a different protocol
# wearing the same name -- guest-endian configuration space, a queue set up by
# page number -- and this kernel implements 1.0 and refuses the other with a
# message rather than half-supporting both.
ARM_DISK=(-global virtio-mmio.force-legacy=false
          -drive "file=$DISK,format=raw,if=none,id=d0"
          -device virtio-blk-device,drive=d0)

# The same disk on the other architecture, over PCI. No flag needed: QEMU offers
# a *transitional* device here -- one that can speak either protocol -- and the
# driver decides by whether the 1.0 capability structures are published rather
# than by the identifier, which for a transitional device looks legacy.
X64_DISK=(-drive "file=$DISK,format=raw,if=none,id=d0"
          -device virtio-blk-pci,drive=d0)

# A GRUB rescue ISO. The same ISO boots on BIOS and on UEFI -- grub-mkrescue
# writes both an El Torito boot catalogue and an EFI system partition -- which
# is why two of the paths below differ only by whether -bios is passed.
make_iso() {
	mkdir -p "$WORK/iso/boot/grub"
	cp "$X64_ELF" "$WORK/iso/boot/reconos-kernel.elf"
	cat >"$WORK/iso/boot/grub/grub.cfg" <<-EOF
	set timeout=0
	set default=0
	menuentry "ReconOS" {
	    multiboot2 /boot/reconos-kernel.elf
	    boot
	}
	EOF
	grub-mkrescue -o "$WORK/reconos.iso" "$WORK/iso" >/dev/null 2>&1
}

echo
echo "x86_64"

# The PVH paths are the ones with no firmware at all, so they are also the only
# ones where the kernel has to place the device's registers itself. Everything
# else on this architecture arrives with the base address registers already
# assigned by SeaBIOS or OVMF.
check_for virtio0 "  PVH, direct kernel load" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF" \
		"${X64_DISK[@]}"

check_for virtio0 "  PVH, -cpu max" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -cpu max -kernel "$X64_ELF" \
		"${X64_DISK[@]}"

check "  PVH, no disk attached" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF"

# The controller real hardware has, rather than the one a hypervisor offers.
# Worth a path of its own because almost nothing about it is shared with virtio:
# a different queue format, a different way of describing where the data goes,
# and a controller that must be stopped before it can be configured.
check_for nvme0n1 "  PVH, NVMe" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF" \
		-drive "file=$DISK,format=raw,if=none,id=n0" \
		-device nvme,serial=recon0,drive=n0

if [ "$ONLY" = all ] || [ "$ONLY" = x86_64 ]; then
	if command -v grub-mkrescue >/dev/null && make_iso; then
		check_for virtio0 "  Multiboot2 via GRUB, BIOS" \
			qemu-system-x86_64 -m 512M -nographic -no-reboot \
				-cdrom "$WORK/reconos.iso" "${X64_DISK[@]}"
		if [ -f "$OVMF_X64" ]; then
			check_for virtio0 "  Multiboot2 via GRUB, UEFI" \
				qemu-system-x86_64 -m 512M -nographic -no-reboot \
					-bios "$OVMF_X64" -cdrom "$WORK/reconos.iso" \
					"${X64_DISK[@]}"
		else
			skip "  Multiboot2 via GRUB, UEFI" "no OVMF"
		fi
	else
		skip "  Multiboot2 via GRUB, BIOS" "no grub-mkrescue"
		skip "  Multiboot2 via GRUB, UEFI" "no grub-mkrescue"
	fi

	if [ -f "$OVMF_X64" ] && make -C boot ARCH=x86_64 esp >/dev/null 2>&1; then
		check_for virtio0 "  reconboot, UEFI" \
			qemu-system-x86_64 -m 512M -nographic -no-reboot \
				-bios "$OVMF_X64" \
				-drive format=raw,file="$ROOT/boot/build/x86_64/esp.img" \
				"${X64_DISK[@]}"
	else
		skip "  reconboot, UEFI" "the loader did not build"
	fi
fi

echo
echo "aarch64"

check_for virtio0 "  device tree, cortex-a72" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel "$ARM_IMG" "${ARM_DISK[@]}"

check_for virtio0 "  device tree, -cpu max" \
	qemu-system-aarch64 -M virt -cpu max -m 512M -nographic -kernel "$ARM_IMG" \
		"${ARM_DISK[@]}"

# One run with no disk at all, on purpose. A kernel that only works on a machine
# with storage attached is a kernel that cannot boot a diskless one, and the
# path where nothing is found is otherwise never taken.
check "  device tree, no disk attached" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel "$ARM_IMG"

# NVMe on this architecture too, which exercises a path nothing else does: the
# device is on PCI, and on the device-tree boot the configuration window comes
# from the host bridge node rather than from an ACPI table.
check_for nvme0n1 "  device tree, NVMe" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel "$ARM_IMG" \
		-drive "file=$DISK,format=raw,if=none,id=n0" \
		-device nvme,serial=recon0,drive=n0

for n in 2 4 8; do
	check_for virtio0 "  device tree, $n processors" \
		qemu-system-aarch64 -M virt -cpu cortex-a72 -smp "$n" -m 512M \
			-nographic -kernel "$ARM_IMG" "${ARM_DISK[@]}"
done

if [ "$ONLY" = all ] || [ "$ONLY" = aarch64 ]; then
	if [ -f "$OVMF_ARM" ] && make -C boot ARCH=aarch64 esp >/dev/null 2>&1; then
		# The disk here is on PCI rather than on the memory-mapped bus.
		# This firmware describes the machine with ACPI instead of a
		# device tree, so the configuration window is read out of the
		# MCFG table -- which is the same walk x86_64 will need for its
		# processor list, and the reason it was worth writing here.
		check_for virtio0 "  reconboot, UEFI" \
			qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
				-bios "$OVMF_ARM" \
				-drive format=raw,file="$ROOT/boot/build/aarch64/esp.img" \
				-drive "file=$DISK,format=raw,if=none,id=d1" \
				-device virtio-blk-pci,drive=d1
	else
		skip "  reconboot, UEFI" "the loader did not build"
	fi
fi

echo
if [ "$failures" -eq 0 ]; then
	echo "$passes self-tests across every path, no failures${skipped:+ ($skipped skipped)}."
	exit 0
fi

echo "$failures path(s) failed:"
printf '  %s\n' "${FAILED_PATHS[@]}"
exit 1
