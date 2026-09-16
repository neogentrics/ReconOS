#!/bin/bash
#
# Read what an Intel display engine is actually doing, on a machine that is
# running Linux, so that a driver for it can be written against measured values
# rather than remembered ones.
#
# --- Why this exists ---------------------------------------------------------
#
# `core/intel_display.c` identifies the adapter and keeps the mode firmware set.
# The next step is reading the mode out of the hardware instead of out of the
# boot handoff, and every register that needs is one nobody here has seen on a
# running machine.
#
# QEMU emulates no Intel display engine, so none of it can be tried in the
# verification matrix, and the one machine in this project that has the hardware
# -- the Gateway GWTC116-2BL, Celeron N4020, UHD Graphics 600 -- **has no serial
# port**. Everything the rig normally reads over a wire has to be read off the
# panel with a camera there (KF-214). Writing register code and finding out on
# that machine is the most expensive possible way to be wrong.
#
# So: boot Linux on it, run this, and bring the output back. Then the driver is
# written against numbers, and -- this is the part that matters -- the
# self-tests can assert *specific expected values* instead of merely that
# something was read.
#
# --- Running it --------------------------------------------------------------
#
#   sudo bash scripts/read-intel-display.sh > intel-gen9.txt
#
# It only reads. Nothing here writes a register, loads a module, or changes a
# mode. The one package it wants and may not find is intel-gpu-tools, for
# `intel_reg`; without it everything else still works and the register dump is
# skipped rather than guessed.
#
#   apt install intel-gpu-tools    # Debian, Ubuntu, Kali
#
set -u

say() { printf '\n===== %s =====\n' "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }

say "when and what"
date -u '+%Y-%m-%dT%H:%M:%SZ'
uname -a
[ -r /sys/class/dmi/id/sys_vendor ] && \
	printf 'machine: %s %s\n' \
		"$(cat /sys/class/dmi/id/sys_vendor 2>/dev/null)" \
		"$(cat /sys/class/dmi/id/product_name 2>/dev/null)"
[ -r /sys/class/dmi/id/bios_version ] && \
	printf 'firmware: %s %s\n' \
		"$(cat /sys/class/dmi/id/bios_vendor 2>/dev/null)" \
		"$(cat /sys/class/dmi/id/bios_version 2>/dev/null)"

# --- the identity, which is what the driver's table is matched against -------
#
# The two numbers `intel_display_identify` decides on, and the class and
# subclass it insists on first. If the laptop's id is not 3185 the table in
# core/intel_display.c is wrong about this machine and that is the single most
# useful thing this script can report.
say "the display device, as PCI reports it"
if have lspci; then
	lspci -nn | grep -iE 'vga|display|3d controller'
	echo
	lspci -nnvv -d 8086: 2>/dev/null | \
		awk '/VGA compatible|Display controller/,/^$/'
else
	echo "no lspci; reading sysfs instead"
	for d in /sys/bus/pci/devices/*; do
		c=$(cat "$d/class" 2>/dev/null)
		case "$c" in
		0x0300*)
			printf '%s vendor=%s device=%s class=%s\n' \
				"${d##*/}" \
				"$(cat "$d/vendor")" "$(cat "$d/device")" "$c"
			;;
		esac
	done
fi

# --- the base address registers ----------------------------------------------
#
# The driver refuses a device whose first BAR is not sixteen megabytes, on the
# grounds that a known id with the wrong shape is not the device the table
# describes. That rule is written from the specification and has never been
# checked against a machine. This is the check.
say "base address registers, sizes and flags"
for d in /sys/bus/pci/devices/*; do
	c=$(cat "$d/class" 2>/dev/null)
	case "$c" in
	0x0300*)
		echo "${d##*/}:"
		if [ -r "$d/resource" ]; then
			awk 'NR<=6 {
				start = strtonum($1); end = strtonum($2);
				size = (end > start) ? end - start + 1 : 0;
				printf "  BAR%d  start=%s  size=%d (%d MB)  flags=%s\n",
				       NR-1, $1, size, size/1048576, $3
			}' "$d/resource"
		fi
		;;
	esac
done

# --- what the kernel driver made of it ---------------------------------------
#
# i915's own view. `i915_display_info` is the one that matters: it names the
# active pipes, the plane on each, its size, its stride and its framebuffer
# offset -- which is exactly what a driver that inherits the firmware's mode
# has to be able to read for itself.
say "i915's view of the display"
if [ -d /sys/kernel/debug/dri ]; then
	for card in /sys/kernel/debug/dri/*; do
		[ -r "$card/i915_display_info" ] || continue
		echo "--- $card/i915_display_info ---"
		cat "$card/i915_display_info" 2>/dev/null
	done
else
	echo "debugfs is not mounted; try: mount -t debugfs none /sys/kernel/debug"
fi

say "the connectors, and which are connected"
for s in /sys/class/drm/card*/status; do
	[ -r "$s" ] || continue
	c=${s%/status}
	printf '%-28s %s\n' "${c##*/}" "$(cat "$s")"
done

say "modes each connected output offers"
for m in /sys/class/drm/card*/modes; do
	[ -r "$m" ] || continue
	c=${m%/modes}
	[ -s "$m" ] || continue
	echo "--- ${c##*/} ---"
	cat "$m"
done

# --- the panel's own description ---------------------------------------------
#
# EDID is how a display says what it is, and reading it is how
# `preferred_mode` would be answered honestly on this backend instead of being
# left null. Dumped as hex so it survives being pasted into a terminal.
say "EDID, as hex"
for e in /sys/class/drm/card*/edid; do
	[ -s "$e" ] || continue
	c=${e%/edid}
	echo "--- ${c##*/} ---"
	if have xxd; then xxd -p "$e"; else od -An -tx1 -v "$e" | tr -d ' \n'; echo; fi
done

# --- the framebuffer Linux ended up with -------------------------------------
say "the framebuffer, as Linux describes it"
for f in /sys/class/graphics/fb*; do
	[ -d "$f" ] || continue
	echo "--- ${f##*/} ---"
	for k in name virtual_size stride bits_per_pixel; do
		[ -r "$f/$k" ] && printf '  %-16s %s\n' "$k" "$(cat "$f/$k")"
	done
done

# --- the registers -----------------------------------------------------------
#
# **The ones the next increment is written against.** Read by name so that the
# output says what each is rather than being a column of addresses, and so that
# a name intel_reg does not know fails visibly instead of printing a plausible
# number from the wrong offset.
#
# PIPE_SRCSZ is the pipe's source size and is the closest thing to "what mode is
# this display in" that can be read in one access. PLANE_STRIDE is the pitch in
# units the hardware chooses, which is the number a framebuffer cannot be drawn
# into without. PLANE_SURF is where the pixels are, as a graphics address rather
# than a physical one -- the difference between those two is the GTT, and is the
# reason this step is being measured before it is coded.
say "display registers, by name"
if have intel_reg; then
	for r in \
		PIPE_SRCSZ_A PIPE_SRCSZ_B PIPE_SRCSZ_C \
		PIPECONF_A PIPECONF_B PIPECONF_C \
		PLANE_CTL_1_A PLANE_STRIDE_1_A PLANE_SURF_1_A PLANE_SIZE_1_A \
		PLANE_CTL_1_B PLANE_STRIDE_1_B PLANE_SURF_1_B PLANE_SIZE_1_B \
		TRANS_CONF_A TRANS_CONF_B \
		HTOTAL_A HBLANK_A HSYNC_A VTOTAL_A VBLANK_A VSYNC_A
	do
		printf '%-18s ' "$r"
		intel_reg read "$r" 2>&1 | tail -n 1
	done
else
	echo "intel_reg not installed, so no register dump."
	echo "  apt install intel-gpu-tools"
	echo
	echo "This is the section the next increment needs most. Everything above"
	echo "describes the mode; only this says which registers it was read from."
fi

say "done"
echo "Bring this whole file back. The driver is written against it, and the"
echo "self-tests assert the values in it by name rather than asserting that"
echo "something was read."
