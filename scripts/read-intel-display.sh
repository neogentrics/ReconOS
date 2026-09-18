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
# **The ones the next increment is written against.**
#
# Read by *address*, with the name beside it as a label, and that is the second
# version of this section. The first read by name, on the reasoning that a name
# intel_reg does not know would fail visibly rather than print a plausible
# number from the wrong offset.
#
# **It does not fail visibly. It exits 0 and prints nothing** (GX-015), and this
# script then printed the name, no value, and no newline -- so twenty-two failed
# reads came back as one tidy row of register names that looked like a heading.
# The register dump had already been described in a signal as "missing because
# intel-gpu-tools is not installed"; it was missing a second time, on a machine
# where the tool was installed, and looked the same.
#
# The builtin register spec in igt 2.5 knows TRANS_HTOTAL_A and does not know
# PIPE_SRCSZ_A or any PLANE_* -- and it warns, on stderr, that it is using the
# builtin spec at all. So a name is not a portable way to ask this question.
# An address is.
#
# Both are read anyway, because they answer different questions: the address
# says what this hardware holds, and the name -- where the tool knows one --
# makes the tool print the address *it* associates with that name, which is an
# independent check on the map in kernel/core/intel_modeset.c.
#
# PIPE_SRCSZ is the pipe's source size and is the closest thing to "what mode is
# this display in" that can be read in one access. PLANE_STRIDE is the pitch in
# units the hardware chooses, which is the number a framebuffer cannot be drawn
# into without. PLANE_SURF is where the pixels are, as a graphics address rather
# than a physical one -- the difference between those two is the GTT, and is the
# reason this step is being measured before it is coded.
say "display registers, by address"
if have intel_reg; then
	# name:address. The address is this project's map, from
	# kernel/core/intel_modeset.c -- reading it here is what checks it.
	#
	# **Both transcoder blocks**, and that is the whole point of the second
	# version of this list: an eDP panel on Gen9 can run through transcoder
	# EDP at 0x6F000 rather than through pipe A's own at 0x60000, and on
	# this project's laptop it does. Pipe A's six timing registers read
	# zero there, which is not an error anybody would notice -- zero is a
	# mode of 1x1 once the minus-one encoding is undone (GX-013).
	for pair in \
		TRANS_HTOTAL_A:0x60000   TRANS_HBLANK_A:0x60004 \
		TRANS_HSYNC_A:0x60008    TRANS_VTOTAL_A:0x6000C \
		TRANS_VBLANK_A:0x60010   TRANS_VSYNC_A:0x60014 \
		PIPE_SRCSZ_A:0x6001C     TRANSCONF_A:0x70008 \
		TRANS_HTOTAL_EDP:0x6F000 TRANS_HBLANK_EDP:0x6F004 \
		TRANS_HSYNC_EDP:0x6F008  TRANS_VTOTAL_EDP:0x6F00C \
		TRANS_VBLANK_EDP:0x6F010 TRANS_VSYNC_EDP:0x6F014 \
		TRANSCONF_EDP:0x7F008 \
		PIPE_SRCSZ_B:0x6101C     PIPE_SRCSZ_C:0x6201C \
		PLANE_CTL_1_A:0x70180    PLANE_STRIDE_1_A:0x70188 \
		PLANE_POS_1_A:0x7018C    PLANE_SIZE_1_A:0x70190 \
		PLANE_SURF_1_A:0x7019C   PLANE_OFFSET_1_A:0x701A4
	do
		name=${pair%%:*}
		addr=${pair#*:}
		value=$(intel_reg read "$addr" 2>/dev/null | tr -s ' ' | tail -n 1)

		if [ -z "$value" ]; then
			# **Silence is not success.** The whole reason this
			# section was rewritten.
			printf '%-18s %-9s NOTHING -- intel_reg printed no value and exited 0\n' \
				"$name" "$addr"
		else
			printf '%-18s %-9s %s\n' "$name" "$addr" "$value"
		fi
	done

	say "the same registers by name, which does not work on igt 2.5"
	echo "Kept because the result is the finding, not because it is useful."
	echo
	echo "On this build **no name read returns anything at all** -- not a name"
	echo "missing from the register spec, and not a name the tool itself"
	echo "prints: PIPEASRC, which is the label intel_reg dump uses in its own"
	echo "output, returns nothing too. Reading by name is not a mechanism that"
	echo "functions here. Address or nothing."
	echo
	echo "If a line below ever does print a value, that is worth knowing: it"
	echo "would mean a build where names work, and the address it states would"
	echo "then be an independent check on the map above."
	echo
	for r in \
		TRANS_HTOTAL_A TRANS_HBLANK_A TRANS_HSYNC_A \
		TRANS_VTOTAL_A TRANS_VBLANK_A TRANS_VSYNC_A \
		PIPE_SRCSZ_A PIPECONF_A PLANE_CTL_1_A PLANE_STRIDE_1_A \
		PLANE_SIZE_1_A PLANE_SURF_1_A
	do
		value=$(intel_reg read "$r" 2>/dev/null | tr -s ' ' | tail -n 1)
		# **Not "NOT KNOWN", which was a diagnosis this script had no
		# evidence for** and printed into every dump it produced. The
		# first version of this line blamed the register spec; the same
		# run then printed it beside TRANS_HTOTAL_A, which
		# `intel_reg list` on that very machine does know (GX-015).
		printf '%-18s %s\n' "$r" "${value:-no value -- name reads return nothing on this build}"
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
