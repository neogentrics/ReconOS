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

# Boots against one partition fixture and compares what the kernel read with
# what wrote the disk. A different kind of check from the ones above: those
# count self-tests the kernel ran on itself, and this one holds the kernel's
# answer up against a second opinion from outside it.
check_table() {
	local label=$1 img=$2 expected=$3
	local log="$WORK/table_$label.log"
	local got="$WORK/table_$label.got"

	printf '%-46s' "  reads $label the same as its tool"

	timeout "$TIMEOUT" qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-kernel "$X64_ELF" \
		-drive "file=$img,format=raw,if=none,id=t0" \
		-device nvme,serial=recon0,drive=t0 >"$log" 2>&1

	sed -e 's/\r$//' "$log" \
	  | awk '/^table / { print $3, $4 } /^slice / { print $3, $4, $5 }' >"$got"

	if [ ! -s "$got" ]; then
		echo "read nothing at all -- $log"
		failures=$((failures + 1))
		FAILED_PATHS+=("table $label")
		return
	fi

	if diff -q "$expected" "$got" >/dev/null 2>&1; then
		echo "matches"
		passes=$((passes + 1))
		return
	fi

	echo "DIFFERS -- $log"
	diff --side-by-side --width=64 "$expected" "$got" | sed 's/^/      /'
	failures=$((failures + 1))
	FAILED_PATHS+=("table $label")
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

# And SATA, which is what the machines between the IDE era and the NVMe one
# have -- roughly everything built between 2005 and 2020, which is most of what
# this will actually be installed on for some years yet.
check_for sata0 "  PVH, AHCI" \
	qemu-system-x86_64 -m 512M -nographic -no-reboot -kernel "$X64_ELF" \
		-device ahci,id=ahci0 \
		-drive "file=$DISK,format=raw,if=none,id=s0" \
		-device ide-hd,drive=s0,bus=ahci0.0

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

check_for sata0 "  device tree, AHCI" \
	qemu-system-aarch64 -M virt -cpu cortex-a72 -m 512M -nographic \
		-kernel "$ARM_IMG" \
		-device ahci,id=ahci0 \
		-drive "file=$DISK,format=raw,if=none,id=s0" \
		-device ide-hd,drive=s0,bus=ahci0.0

# Sixteen is past where QEMU's virt board stops giving a GICv2.
#
# This loop stopped at eight, and eight is exactly the largest machine the older
# interrupt controller supports -- so the kernel panicked at boot on any ARM
# machine with nine or more processors, and nothing here could see it (BG-124).
# A rig built on the principle that some bugs only exist above a certain machine
# size had its own ceiling, one processor below the first machine that would
# have shown this one.
for n in 2 4 8 16; do
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

# --- Partition tables ------------------------------------------------------
#
# Three disks written by sgdisk and sfdisk, read by the kernel, compared. The
# hybrid is the one that matters: its protective entry covers a fraction of the
# disk and sits beside two entries that describe real partitions correctly, so
# every wrong reader produces a plausible answer on it.

echo
echo "partition tables"

if bash scripts/make-partition-fixtures.sh "$WORK/fixtures" >/dev/null 2>&1; then
	check_table gpt        "$WORK/fixtures/gpt.img"    "$WORK/fixtures/gpt.expected"
	check_table mbr        "$WORK/fixtures/mbr.img"    "$WORK/fixtures/mbr.expected"
	check_table hybrid-mbr "$WORK/fixtures/hybrid.img" "$WORK/fixtures/hybrid.expected"
else
	skip "  partition tables" "sgdisk or sfdisk is missing"
fi

# --- Durability -------------------------------------------------------------
#
# That a flushed write is on the medium before the next one is issued is the
# property every crash-consistency scheme rests on, and it is the kind of thing
# that regresses silently: somebody makes a driver faster by returning from
# flush when the command was accepted rather than when it completed, every
# self-test still passes, and the only symptom arrives months later as a
# filesystem that does not survive a power cut.
#
# So it is measured on every run, not once. Six cuts rather than the twenty the
# standalone script does -- enough to catch a flush that stopped waiting, few
# enough not to double the length of this.

echo
echo "durability"

# Two checks, because they answer different questions and only one of them was
# ever being asked.
#
# The cut measures *ordering*: what a reader finds on the medium after the
# machine stops. It cannot measure flushing at all -- killing QEMU does not lose
# the writes QEMU already made, those bytes are in the host's page cache and the
# host writes them out regardless. Run crash-test.sh with `nocache`, which tells
# QEMU to discard guest flushes entirely, and it returns the same clean result.
#
# So the flush is checked where it can be seen: at the emulated controller,
# which is the far side of the boundary the kernel is responsible for.

printf '%-46s' "  a flush orders writes, blocks do not tear"

if crash_out=$(bash scripts/crash-test.sh 6 x86_64 2>&1); then
	echo "$(echo "$crash_out" | grep -oE '[0-9]+ cuts.*')"
	passes=$((passes + 1))
else
	echo "FAILED"
	echo "$crash_out" | sed 's/^/      /'
	failures=$((failures + 1))
	FAILED_PATHS+=("durability: ordering")
fi

for a in x86_64 aarch64; do
	printf '%-46s' "  every flush reaches the device ($a)"

	if flush_out=$(bash scripts/flush-reaches-device.sh "$a" 2>&1); then
		echo "$(echo "$flush_out" | grep -c 'flush commands at the controller') driver(s)"
		passes=$((passes + 1))
	else
		echo "FAILED"
		echo "$flush_out" | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("durability: flush reaches device ($a)")
	fi
done

# --- ReconFS ----------------------------------------------------------------
#
# The interesting number here is not that a fresh volume checks out. It is that
# the checker was shown four faults it was built to catch and caught all four.
#
# A checker that has only ever been run on good images has never been observed
# to do anything. Three harness bugs on the day this was written (BG-114,
# BG-115, BG-116) all presented as clean passes, and the filesystem's crash
# suite is about to rest on exactly this checker being honest.

echo
echo "reconfs"

for a in x86_64 aarch64; do
	printf '%-46s' "  the checker catches what it is shown ($a)"

	img=$(mktemp)
	dd if=/dev/zero of="$img" bs=1M count=16 status=none

	if [ "$a" = aarch64 ]; then
		fs_out=$(timeout -s KILL 150 qemu-system-aarch64 -M virt \
			-cpu cortex-a72 -m 512M -nographic \
			-kernel kernel/build/aarch64/reconos-kernel.img \
			-append "reconfs=nvme0n1" \
			-drive "file=$img,format=raw,if=none,id=d0" \
			-device nvme,serial=recon0,drive=d0 2>&1) || true
	else
		fs_out=$(timeout -s KILL 150 qemu-system-x86_64 -m 512M \
			-nographic -no-reboot \
			-kernel kernel/build/x86_64/reconos-kernel.elf \
			-append "reconfs=nvme0n1" \
			-drive "file=$img,format=raw,if=none,id=d0" \
			-device nvme,serial=recon0,drive=d0 2>&1) || true
	fi

	rm -f "$img"

	# The whole battery, at every block size, and the commit path with it.
	# Checking only the last line would pass a run where two of the three
	# block sizes failed, which is the shape of pass this work has already
	# produced three of.
	sizes=$(echo "$fs_out" | grep -cE 'the checker caught 5 of 5')
	commits=$(echo "$fs_out" | grep -cE 'a commit that survives a remount : pass')
	verdict=$(echo "$fs_out" | grep -oE '[0-9]+ of [0-9]+ block sizes behaved')

	# The freed-block exclusion can only be checked on a volume small enough
	# to fill in one transaction, so it reports whether it ran rather than
	# failing on a large disk (BG-126). This image is 16 MB, which is small
	# enough at every block size -- so demand all three here. Without this,
	# growing the image would silently stop checking the one rule that keeps
	# a transaction from overwriting live storage, and the run would still
	# be green.
	excl=$(echo "$fs_out" | grep -cE 'the freed-block exclusion was checked at 3 of 3')

	if [ "$verdict" = "3 of 3 block sizes behaved" ] &&
	   [ "$sizes" = "3" ] && [ "$commits" = "3" ] && [ "$excl" = "1" ]; then
		echo "3 block sizes, 15 faults, 3 commits, exclusion 3/3"
		passes=$((passes + 1))
	else
		echo "FAILED"
		echo "$fs_out" | sed -n '/reconfs:/,$p' | head -20 | sed 's/^/      /'
		failures=$((failures + 1))
		FAILED_PATHS+=("reconfs ($a)")
	fi
done

# The one docs/RECONFS.md lists first: cut the power inside a rename, and let
# something that shares no code with the kernel say what survived.
#
# Fewer cuts than the standalone script does, because each one boots a machine
# and each check reads a whole image -- enough to catch a regression, few enough
# not to double the length of this.

printf '%-46s' "  a rename survives the power going out"

if rn_out=$(bash scripts/rename-crash-test.sh 6 x86_64 2>&1); then
	echo "$(echo "$rn_out" | grep -oE '[0-9]+ cuts inside a rename.*')"
	passes=$((passes + 1))
else
	echo "FAILED"
	echo "$rn_out" | sed 's/^/      /' | head -20
	failures=$((failures + 1))
	FAILED_PATHS+=("reconfs: rename under a power cut")
fi

# The promise the whole of phase 2 rests on: ReconOS installs beside an
# operating system that is already there without destroying it. Every safety
# rule in the storage stack exists to keep it, and none of them is worth
# anything as an intention.

printf '%-46s' "  a filesystem beside somebody else's"

# The status is captured rather than read from `$?` inside an elif, where what
# it refers to depends on how the shell got there.
beside_out=$(bash scripts/beside-others-test.sh x86_64 2>&1)
beside_rc=$?

if [ "$beside_rc" -eq 0 ]; then
	echo "neighbour and both tables unchanged"
	passes=$((passes + 1))
elif [ "$beside_rc" -eq 2 ]; then
	echo "skipped, sgdisk is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	echo "$beside_out" | sed 's/^/      /' | head -14
	failures=$((failures + 1))
	FAILED_PATHS+=("reconfs beside another partition")
fi

# --- Somebody else's filesystem --------------------------------------------
#
# FAT32 is the one foreign format that is required rather than optional: the
# UEFI System Partition is FAT32 by specification and that is where our own
# bootloader has to be written. A reader tested against a volume our own code
# wrote would be tested against its own misunderstandings, so the fixture is
# built by mkfs.vfat and filled by mcopy -- and then broken four ways, because a
# reader that has never been seen to refuse anything is not a reader yet.

echo
echo "foreign filesystems"

for a in x86_64 aarch64; do
	printf '%-46s' "  reads FAT32, and refuses four broken ones ($a)"

	fat_out=$(bash scripts/fat32-reads-foreign.sh "$a" 2>&1)
	fat_rc=$?

	if [ "$fat_rc" -eq 0 ]; then
		echo "$(echo "$fat_out" | grep -oE '[0-9]+ of [0-9]+: read a foreign.*' | head -1)"
		passes=$((passes + 1))
	else
		echo "FAILED"
		echo "$fat_out" | sed 's/^/      /' | head -16
		failures=$((failures + 1))
		FAILED_PATHS+=("fat32 reads ($a)")
	fi
done

# And the other direction, which is the one an installer depends on: we write
# the bootloader and *firmware* reads it. A writer checked only by our own
# reader would be checked against its own misunderstandings.

for a in x86_64 aarch64; do
	printf '%-46s' "  writes FAT32 that mtools can read ($a)"

	fw_out=$(bash scripts/fat32-writes-foreign.sh "$a" 2>&1)
	fw_rc=$?

	if [ "$fw_rc" -eq 0 ]; then
		echo "$(echo "$fw_out" | grep -oE '[0-9]+ of [0-9]+: wrote a volume.*' | head -1)"
		passes=$((passes + 1))
	elif [ "$fw_rc" -eq 2 ]; then
		echo "skipped, mtools is not installed"
		skipped=$((skipped + 1))
	else
		echo "FAILED"
		echo "$fw_out" | sed 's/^/      /' | head -16
		failures=$((failures + 1))
		FAILED_PATHS+=("fat32 writes ($a)")
	fi
done

# --- Deciding, before there is any doing ------------------------------------
#
# The installer is the one piece here that runs once, on a stranger's machine,
# with their data on it. So the deciding is a separate half that writes nothing,
# and it is checked against layouts sgdisk built -- including four it must
# *refuse*. A planner that always says yes would pass a test that only asked
# whether it produced a plan.

echo
echo "installer"

printf '%-46s' "  plans or refuses seven real layouts"

plan_out=$(bash scripts/install-plan-test.sh x86_64 2>&1)
plan_rc=$?

if [ "$plan_rc" -eq 0 ]; then
	echo "$(echo "$plan_out" | grep -oE '[0-9]+ of [0-9]+ layouts.*' | head -1)"
	passes=$((passes + 1))
elif [ "$plan_rc" -eq 2 ]; then
	echo "skipped, sgdisk or mtools is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	echo "$plan_out" | sed 's/^/      /' | head -18
	failures=$((failures + 1))
	FAILED_PATHS+=("install planner")
fi

# And the doing. Installs for real -- onto a blank disk, and beside an existing
# system whose data, bootloader and table entries must all survive byte for
# byte. The claim being checked is not "it installed".

printf '%-46s' "  installs, and the disk's contents survive"

onto_out=$(bash scripts/install-onto-test.sh x86_64 2>&1)
onto_rc=$?

if [ "$onto_rc" -eq 0 ]; then
	echo "$(echo "$onto_out" | grep -oE '[0-9]+ of [0-9]+: installed.*' | head -1)"
	passes=$((passes + 1))
elif [ "$onto_rc" -eq 2 ]; then
	echo "skipped, sgdisk or mtools is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	echo "$onto_out" | sed 's/^/      /' | head -20
	failures=$((failures + 1))
	FAILED_PATHS+=("install onto")
fi

# The one test that checks what the project is for, rather than that a part of
# it works: a bare disk, an install medium, and afterwards the disk boots on its
# own with the medium gone. Everything the installer got wrong shows up as a
# firmware that finds no system.

printf '%-46s' "  installs from media, and the disk boots"

e2e_out=$(bash scripts/install-then-boot-test.sh 2>&1)
e2e_rc=$?

if [ "$e2e_rc" -eq 0 ]; then
	echo "$(echo "$e2e_out" | grep -oE '[0-9]+ of [0-9]+: installed from media.*' | head -1)"
	passes=$((passes + 1))
elif [ "$e2e_rc" -eq 2 ]; then
	echo "skipped, OVMF or mtools is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	echo "$e2e_out" | sed 's/^/      /' | head -20
	failures=$((failures + 1))
	FAILED_PATHS+=("install then boot")
fi

# Installing beside another system is only half the promise. The other half is
# still being able to reach it afterwards, and the fixture is shaped like a real
# dual-boot machine -- Windows and Linux on one EFI partition -- rather than
# like the code, which is what caught the first version listing only one.

printf '%-46s' "  finds the other systems on the machine"

menu_out=$(bash scripts/boot-menu-test.sh 2>&1)
menu_rc=$?

if [ "$menu_rc" -eq 0 ]; then
	echo "$(echo "$menu_out" | grep -oE '[0-9]+ of [0-9]+: found the other.*' | head -1)"
	passes=$((passes + 1))
elif [ "$menu_rc" -eq 2 ]; then
	echo "skipped, OVMF or mtools is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	echo "$menu_out" | sed 's/^/      /' | head -14
	failures=$((failures + 1))
	FAILED_PATHS+=("boot menu")
fi

# The loader refusing a kernel that is not ours. The signature is made by
# openssl, which shares no code with the verifier -- a verifier checked against
# signatures produced by its own arithmetic is checked against its own
# misunderstandings.
#
# Four refusals, and only the last is an attacker: a valid signature by somebody
# else's key. A verifier that catches corruption while accepting any well-formed
# signature is worth nothing at all.

echo
echo "integrity"

printf '%-46s' "  runs a signed kernel, refuses four others"

sig_out=$(bash scripts/signed-kernel-test.sh 2>&1)
sig_rc=$?

if [ "$sig_rc" -eq 0 ]; then
	echo "$(echo "$sig_out" | grep -oE '[0-9]+ of [0-9]+: ran the signed.*' | head -1)"
	passes=$((passes + 1))
elif [ "$sig_rc" -eq 2 ]; then
	echo "skipped, openssl or OVMF is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	echo "$sig_out" | sed 's/^/      /' | head -14
	failures=$((failures + 1))
	FAILED_PATHS+=("signed kernel")
fi

# The recovery environment, shown a volume broken on purpose by a tool that
# shares no code with the kernel. A recovery screen that has never been seen to
# report damage is not a recovery screen -- it is one that says "sound", which
# is what a broken one says too.

printf '%-46s' "  recovery finds damage, and touches nothing"

rec_out=$(bash scripts/recovery-test.sh 2>&1)
rec_rc=$?

if [ "$rec_rc" -eq 0 ]; then
	echo "$(echo "$rec_out" | grep -oE '[0-9]+ of [0-9]+: found the damage.*' | head -1)"
	passes=$((passes + 1))
elif [ "$rec_rc" -eq 2 ]; then
	echo "skipped, python3 is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	echo "$rec_out" | sed 's/^/      /' | head -16
	failures=$((failures + 1))
	FAILED_PATHS+=("recovery")
fi

# --- the menu draws where it can, and falls back where it cannot -------------
#
# Both halves asserted, because only one of them is the interesting one.
#
# That the graphical menu draws on a machine with a framebuffer is the feature.
# That it *does not* on a machine without one is what keeps the feature from
# being a machine that shows nothing: AAVMF provides no framebuffer at all, so
# the aarch64 loader must take the text path on every boot, for ever. An
# unasserted fallback is exactly the branch that rots, and this one is the
# reason the graphical menu was allowed to exist.
#
# Checked by the loader's own report rather than by looking at pixels: it says
# which surface it used, once, on the serial console, and that is a claim the
# rig can hold it to.

printf '%-46s' "  the menu draws, and falls back where it cannot"

menu_x64=; menu_arm=
if [ -f "$OVMF_X64" ] && make -C boot ARCH=x86_64 esp >/dev/null 2>&1; then
	menu_x64=$(timeout 60 qemu-system-x86_64 -m 512M -nographic -no-reboot \
		-bios "$OVMF_X64" \
		-drive format=raw,file="$ROOT/boot/build/x86_64/esp.img" 2>&1 |
		tr -d '\r' | sed -n 's/^  menu  *: \(.*\)/\1/p' | head -1)
fi
if [ -f "$OVMF_ARM" ] && make -C boot ARCH=aarch64 esp >/dev/null 2>&1; then
	menu_arm=$(timeout 60 qemu-system-aarch64 -M virt -cpu cortex-a72 \
		-m 512M -nographic -no-reboot -bios "$OVMF_ARM" \
		-drive format=raw,file="$ROOT/boot/build/aarch64/esp.img" 2>&1 |
		tr -d '\r' | sed -n 's/^  menu  *: \(.*\)/\1/p' | head -1)
fi

if [ -z "$menu_x64" ] && [ -z "$menu_arm" ]; then
	echo "skipped, neither firmware is installed"
	skipped=$((skipped + 1))
elif [ "${menu_x64%% *}" = "drawn" ] && [ "${menu_arm%% *}" = "text" ]; then
	echo "drawn on OVMF, text on AAVMF"
	passes=$((passes + 1))
else
	echo "FAILED"
	echo "      x86_64 (has a framebuffer): ${menu_x64:-nothing}"
	echo "      aarch64 (has none):         ${menu_arm:-nothing}"
	failures=$((failures + 1))
	FAILED_PATHS+=("boot menu surface")
fi

# Checkpoint 16, on a machine with no UEFI in it at all: SeaBIOS, an IDE disk,
# and 440 bytes of ours in the first sector. Includes the two deliberate faults
# -- an unreadable stage 2 and one changed byte of its magic -- because a loader
# that jumps into an unidentified sector and one that checks are indistinguish-
# able on a disk where the sector happens to be right.

printf '%-46s' "  a machine with no UEFI runs our loader"

bios_out=$(sh scripts/bios-boot-test.sh 2>&1)
bios_rc=$?

if [ "$bios_rc" -eq 0 ]; then
	echo "$(echo "$bios_out" | grep -oE '[0-9]+ of [0-9]+: a machine.*' | head -1)"
	passes=$((passes + 1))
elif [ "$bios_rc" -eq 2 ]; then
	echo "skipped, qemu is not installed"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	echo "$bios_out" | sed 's/^/      /' | head -16
	failures=$((failures + 1))
	FAILED_PATHS+=("bios loader")
fi

# And the same machine again, with a key. The BIOS path shares reconboot's
# sha256.c and rsa.c rather than reimplementing them, so this is checking that
# they were *reached* -- a loader that never calls the verifier is exactly what
# an attacker who can write to the disk would arrange.

printf '%-46s' "  the BIOS path refuses an unsigned kernel"

bsig_out=$(sh scripts/bios-signed-test.sh 2>&1)
bsig_rc=$?

if [ "$bsig_rc" -eq 0 ]; then
	echo "$(echo "$bsig_out" | grep -oE '[0-9]+ of [0-9]+: the BIOS path.*' | head -1)"
	passes=$((passes + 1))
elif [ "$bsig_rc" -eq 2 ]; then
	echo "skipped, openssl or a disk tool is missing"
	skipped=$((skipped + 1))
else
	echo "FAILED"
	echo "$bsig_out" | sed 's/^/      /' | head -14
	failures=$((failures + 1))
	FAILED_PATHS+=("bios signature")
fi

echo
if [ "$failures" -eq 0 ]; then
	echo "$passes self-tests across every path, no failures${skipped:+ ($skipped skipped)}."
	exit 0
fi

echo "$failures path(s) failed:"
printf '  %s\n' "${FAILED_PATHS[@]}"
exit 1
