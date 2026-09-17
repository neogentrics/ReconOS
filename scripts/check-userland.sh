#!/bin/bash
# Compile the desktop's own sources with no glibc underneath them.
#
# `userland/libc/` is checked against the host's library by a differential
# test, which proves the functions answer the same thing. It proves nothing at
# all about whether the *headers* are usable -- whether `recon_url.c` can say
# `#include <string.h>` and get ours, whether the declarations match what the
# desktop's call sites expect, whether something is missing that nothing
# noticed because glibc was quietly supplying it.
#
# The only way to find that out is to take glibc away and try.
#
# So: `-nostdinc`, which removes every system header directory, plus the
# compiler's own (stdarg.h, stddef.h, stdint.h and the rest -- those are the
# compiler's to provide in a freestanding environment, not the library's), plus
# `userland/include` and the project's own `include/`. If a file compiles under
# that, it compiles on ReconOS.
#
# --- Why only some files ---
#
# That sentence used to read "the reason is one word: there is no allocator on
# this kernel". **It stopped being true on 15 September** -- the allocator
# landed in v0.4.28 and the kernel answered SYS_MAP in v0.4.33, so malloc has
# worked on ReconOS for two days while this header went on naming it as the
# blocker. The list is what is measured; the paragraph explaining it was not.
#
# What is left is three groups, and only one of them is large:
#
#   **The compositor: fifteen files.** wayland-server-core.h and wlr/. This is
#   the real remaining work and it is the board's `display-boundary` row, not
#   a library gap -- a program on the ReconOS kernel draws on the framebuffer
#   device, and does not speak Wayland to itself.
#
#   **Vendored headers: eight files** want stb_image.h or stb_truetype.h,
#   which need third_party/ on the include path and a hosted <math.h>.
#
#   **One missing thing each: the rest.** realpath (recon_fs.c -- closed in
#   v0.4.53; it was written and declared in no header), pid_t
#   (recon_cmd.c, recon_control_panel.c), signal.h, sys/time.h, dlfcn.h,
#   ifaddrs.h, zlib.h, mbedtls, libdrm. Each names one thing and is worth one
#   piece of work.
#
# **The list is checked rather than trusted.** A file that grows a `malloc` is
# removed from it deliberately, with the reason; a file that loses its last one
# should be added. The list is not computed here on purpose -- a script that
# decides its own input can always pass, by shrinking it.
#
# --- What this found the first time it ran ---
#
# Seven of the sixteen files a call-counting sweep said were ready did not
# build. That is the whole point of the script: the sweep counted the functions
# it already knew to look for, so it could not see `strtok_r`, which is on 44
# call sites in `src/` and was simply absent from the library. `strncat` and
# `strtoull` came out of the same run. All three are written now.
#
# The rest were not library gaps at all, and each one names something the port
# still needs:
#
#   src/recon_color.c        include/recon_ui.h reached
#   src/recon_titlebar.c       <xkbcommon/xkbcommon.h> -- and it turned out to
#   src/recon_widget.c         want exactly one thing from it, the typedef
#   src/recon_widget_state.c   `xkb_keysym_t`, which is a uint32_t. **That one
#   src/recon_access.c         include held twenty-four files here.** They did
#                              not need their drawing half separated from
#                              their compositor half after all; they needed to
#                              be able to name a key without a Linux keyboard
#                              library. include/recon_key.h is ReconOS's own
#                              names for the same numbers, v0.4.42, and all
#                              five of these are on the list now.
#
#   src/recon_stb.c          the vendored stb shim, which wanted third_party/
#                              on the include path. It has it now -- those
#                              headers ship in this repository and travel to
#                              ReconOS with everything else, exactly like the
#                              icons, so leaving them off was measuring the
#                              wrong thing.
#
#                              And then it wanted <limits.h>, which this
#                              library did not have: the compiler's own chains
#                              to the system's, and with -nostdinc there is
#                              nothing to chain to. That is not the vendored
#                              code being awkward -- **a C library provides
#                              <limits.h>** and this one did not.
#                              userland/include/limits.h, v0.4.48.
#
# --- Two files that are left off on purpose ---
#
# Both would build if handed a header, and neither should be. **Compiling is
# not the point.** This check asks whether a file could run on ReconOS, and a
# file that compiles because it was given a declaration for something this
# system answers with a refusal has a worse answer than one that does not
# build: it looks ready.
#
#   src/recon_control.c   wants <sys/time.h> for `struct timeval`, which it
#                         uses with setsockopt(SO_RCVTIMEO) -- and
#                         userland/include/sys/socket.h refuses every
#                         setsockopt option with ENOPROTOOPT. The timeout is
#                         there to stop a peer that connects and says nothing
#                         from freezing the thread drawing the desktop. Give it
#                         the header and it compiles, gets no timeout, and the
#                         freeze it was written to prevent comes back with
#                         nothing saying so.
#
#   src/recon_procinfo.c  wants `sysconf(_SC_CLK_TCK)`, and it wants it to
#                         divide numbers it read out of /proc. There is no
#                         /proc on ReconOS and this file is a Linux scraper
#                         from top to bottom -- it is on the KERNEL-WANTS list
#                         as "Processes", not on this one. `signal.h` answered
#                         the other half of what it wanted, which is why it is
#                         mentioned here at all.
#
# And one thing this cannot find yet: `strerror`, on 43 call sites in three
# files. The kernel returns negative error numbers (`SYS_ENOENT` and the rest
# in userland/include/recon.h), so the table is ours to write -- but all three
# of those files are blocked on the allocator anyway, so it waits.
#
#   ./scripts/check-userland.sh
#
# Silence is the result. Anything printed is a real finding.

set -u

cd "$(dirname "$0")/.." || exit 1

CC="${CC:-gcc}"
BUILTIN="$("$CC" -print-file-name=include)"

if [ ! -d "$BUILTIN" ]; then
	echo "cannot find the compiler's own headers: $BUILTIN"
	exit 1
fi

# Eleven of seventy-nine, and every one of them verified by this script rather
# than by a sweep.
#
# src/recon_expr.c is the newest and the one worth naming: the calculator holds
# seventeen maths functions in a table of function pointers, so it could not
# build until userland/libc/math.c existed. It is also the largest file on this
# list at 459 lines, and it evaluates an expression grammar -- which is to say
# ReconOS can now do arithmetic with no glibc underneath it.

# Added 14 September 2026, after the allocator, errno and the descriptor layer.
# Offered to the compiler rather than assumed: every file in src/ was tried and
# these eight built where they had not.
#
# Almost exactly the browser's half of the desktop -- the HTML parser, the CSS
# parser, forms, HTTP -- which is not a coincidence. A parser is strings and
# allocation and very little else, and that is what arrived.
#
# `src/recon_cookie.c` came off this list for an hour and is back. It needed
# `sscanf`, which arrived in v0.4.31 -- and the probe that first suggested it
# compiled without `-Werror`, so an implicit declaration was a warning there
# and an error here. A predictor with looser rules than the thing it predicts
# is worth less than no predictor; it now uses these same flags.
# Added 15 September 2026, when include/recon_key.h took xkbcommon out of
# include/recon_ui.h. Offered to the compiler rather than assumed -- every file
# in src/ was tried and these twenty-two built where they had not.
#
# It is most of the desktop's own furniture: the widget layer and the thing
# that remembers what a widget is set to, the theme engine, the title bar, the
# wallpaper, the icon generator, the avatar, the file dialog, Notepad and the
# Terminal. None of them was ever a library gap.
# And three that only ever wanted third_party/ on the include path.
# Added 15 September 2026, when five applications stopped including the whole
# compositor for one field and the check stopped being stricter than the build.
# Added 15 September 2026, with userland/include/signal.h -- the kernel has had
# SYS_KILL, SYS_SIGACTION, SYS_SIGMASK and SYS_SIGRETURN since before the
# desktop could compile for it at all.
# Added 15 September 2026 with userland/include/limits.h, and with third_party
# told to the compiler as vendored rather than as ours.
# Added 17 September 2026 with include/recon_clients.h: recon_shell.c,
# recon_cmd.c and recon_apps.c. They were the three largest sources in the
# desktop that did not build freestanding and held 11,236 lines between them --
# recon_shell.c alone being 7,231 with not one mention of wayland in it. What
# held all three was a single include of recon_server.h, for a handful of calls
# and two pointers they never looked inside. recon_cmd.c wanted one thing more:
# two button codes out of Linux's input-event-codes.h, which recon_inject.h now
# writes down itself and main.c holds a _Static_assert against.
#
# Note for whoever adds the next one: this list is split on whitespace, so a
# "#" inside the string below is not a comment and every word on the line
# becomes a file the check looks for. Notes go here, above it.
FILES="
src/recon_expr.c
src/recon_url.c
src/recon_version.c
src/recon_data.c
src/recon_image.c
src/recon_service.c
src/recon_video.c
src/recon_crypt.c
src/recon_sniff.c
src/recon_smtp_message.c
src/recon_ocr.c
src/recon_clip.c
src/recon_cookie.c
src/recon_css.c
src/recon_firewall.c
src/recon_fonts.c
src/recon_appwin.c
src/recon_fs.c
src/recon_clock.c
src/recon_desktop.c
src/recon_photos.c
src/recon_player.c
src/recon_session.c
src/recon_taskmgr.c
src/recon_media.c
src/recon_form.c
src/recon_html.c
src/recon_http.c
src/recon_registry.c

src/recon_access.c
src/recon_appicon.c
src/recon_avatar.c
src/recon_color.c
src/recon_filedlg.c
src/recon_icon_gen.c
src/recon_key.c
src/recon_mail.c
src/recon_manifest.c
src/recon_movie.c
src/recon_mp4.c
src/recon_notepad.c
src/recon_package.c
src/recon_props.c
src/recon_smtp.c
src/recon_terminal.c
src/recon_theme.c
src/recon_titlebar.c
src/recon_users.c
src/recon_wallpaper.c
src/recon_widget.c
src/recon_widget_state.c

src/recon_ico.c
src/recon_icons.c
src/recon_web.c
src/recon_ui.c

src/recon_calendar.c
src/recon_control_panel.c
src/recon_explorer.c
src/recon_help.c
src/recon_mailwin.c
src/recon_ui_fb.c

src/recon_error.c

src/recon_ocr_match.c
src/recon_stb.c

src/recon_shell.c
src/recon_cmd.c
src/recon_apps.c
"

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

failed=0
built=0

for f in $FILES; do
	if [ ! -f "$f" ]; then
		echo "$f: in the list and not in the tree"
		failed=$((failed + 1))
		continue
	fi

	# --- the flags, and why each unobvious one is there ---
	#
	# **No comments inside the command below.** A `#` after a line
	# continuation ends the command at that point: the shell joins the
	# lines, tokenises, and everything after the comment runs as a command
	# of its own. That is not hypothetical -- it happened here, and this
	# check spent several versions compiling without `-Werror`, without
	# `-DRECONOS_VERSION` and without third_party on the path, while
	# reporting that it had used all three.
	#
	# -isystem third_party, not -I: these are vendored and THIRD_PARTY.md
	# says so. With -I the compiler treats them as ours, and -Werror then
	# refuses src/recon_ocr_match.c because stb_truetype defines two
	# functions that file does not call -- not a portability fault, and not
	# our code to fix.
	#
	# -Wno-unused-parameter because CMakeLists.txt sets it. Without it this
	# check is *stricter* than the build and refuses files that would
	# build, which is the rule in this file's header wearing the other
	# shoe: a predictor that does not match the thing it predicts is worth
	# less than none, in either direction.
	#
	# -ffreestanding says there is no hosted library, which also stops the
	# compiler turning a byte loop into a call to memcpy -- the same reason
	# CMakeLists passes -fno-builtin to libc/ itself.
	if ! "$CC" -c "$f" -o "$out/$(basename "$f").o" \
		-std=c11 -ffreestanding -fno-builtin -fno-math-errno \
		-nostdinc \
		-isystem "$BUILTIN" \
		-I userland/include \
		-I include \
		-isystem third_party \
		-DRECONOS_VERSION='"0.0.0"' \
		-Wall -Wextra -Wno-unused-parameter -Werror \
		2> "$out/err"; then
		echo "--- $f does not build without glibc"
		head -20 "$out/err"
		failed=$((failed + 1))
	else
		built=$((built + 1))
	fi
done

echo "$built of $((built + failed)) desktop sources compile with no libc under them"

if [ "$failed" -ne 0 ]; then
	exit 1
fi
