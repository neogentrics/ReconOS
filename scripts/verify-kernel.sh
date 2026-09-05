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
check() {
	local name=$1; shift
	local log="$WORK/$(echo "$name" | tr ' /' '__').log"

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

	echo "$ran self-tests, all pass"
	passes=$((passes + ran))
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

check "  PVH, direct kernel load" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF"

check "  PVH, -cpu max" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -cpu max -kernel "$X64_ELF"

if [ "$ONLY" = all ] || [ "$ONLY" = x86_64 ]; then
	if command -v grub-mkrescue >/dev/null && make_iso; then
		check "  Multiboot2 via GRUB, BIOS" \
			qemu-system-x86_64 -m 512M -nographic -no-reboot \
				-cdrom "$WORK/reconos.iso"
		if [ -f "$OVMF_X64" ]; then
			check "  Multiboot2 via GRUB, UEFI" \
				qemu-system-x86_64 -m 512M -nographic -no-reboot \
					-bios "$OVMF_X64" -cdrom "$WORK/reconos.iso"
		else
			skip "  Multiboot2 via GRUB, UEFI" "no OVMF"
		fi
	else
		skip "  Multiboot2 via GRUB, BIOS" "no grub-mkrescue"
		skip "  Multiboot2 via GRUB, UEFI" "no grub-mkrescue"
	fi

	if [ -f "$OVMF_X64" ] && make -C boot ARCH=x86_64 esp >/dev/null 2>&1; then
		check "  reconboot, UEFI" \
			qemu-system-x86_64 -m 512M -nographic -no-reboot \
				-bios "$OVMF_X64" \
				-drive format=raw,file="$ROOT/boot/build/x86_64/esp.img"
	else
		skip "  reconboot, UEFI" "the loader did not build"
	fi
fi

echo
echo "aarch64"

check "  device tree, cortex-a72" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel "$ARM_IMG"

check "  device tree, -cpu max" \
	qemu-system-aarch64 -M virt -cpu max -m 512M -nographic -kernel "$ARM_IMG"

for n in 2 4 8; do
	check "  device tree, $n processors" \
		qemu-system-aarch64 -M virt -cpu cortex-a72 -smp "$n" -m 512M \
			-nographic -kernel "$ARM_IMG"
done

if [ "$ONLY" = all ] || [ "$ONLY" = aarch64 ]; then
	if [ -f "$OVMF_ARM" ] && make -C boot ARCH=aarch64 esp >/dev/null 2>&1; then
		check "  reconboot, UEFI" \
			qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
				-bios "$OVMF_ARM" \
				-drive format=raw,file="$ROOT/boot/build/aarch64/esp.img"
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
