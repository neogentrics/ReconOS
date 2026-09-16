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
#   **One missing thing each: the rest.** realpath (recon_fs.c), pid_t
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
#                              wrong thing. Three more files build because of
#                              it. recon_stb.c itself still does not: it wants
#                              a hosted <math.h> as well.
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

	# -ffreestanding says there is no hosted library, which also stops the
	# compiler turning a byte loop into a call to memcpy -- the same reason
	# CMakeLists passes -fno-builtin to libc/ itself.
	if ! "$CC" -c "$f" -o "$out/$(basename "$f").o" \
		-std=c11 -ffreestanding -fno-builtin -fno-math-errno \
		-nostdinc \
		-isystem "$BUILTIN" \
		-I userland/include \
		-I include \
		-I third_party \
		-DRECONOS_VERSION='"0.0.0"' \
		-Wall -Wextra -Werror \
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
