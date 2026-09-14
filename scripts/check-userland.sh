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
# Most of `src/` cannot be built this way yet and the reason is one word:
# `free` appears in 56 of the 79 files, `calloc` in 37, `malloc` in 20. There
# is no allocator on this kernel (docs/KERNEL-WANTS.md, first entry), so those
# files have nothing to link against.
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
#   src/recon_color.c        include/recon_ui.h reaches <xkbcommon/xkbcommon.h>
#   src/recon_titlebar.c       -- the keyboard layout library. That is a real
#   src/recon_widget.c         Wayland dependency of the compositor, and a
#   src/recon_widget_state.c   program on the ReconOS kernel will not have it;
#                              these files need their drawing half separated
#                              from their compositor half before they can move.
#
#   src/recon_access.c       also reaches include/recon_ui.h, and so wants
#                              xkbcommon as well -- it is in the group above.
#                              It was the reason userland/include/time.h got
#                              written, though, and src/recon_url.c came with
#                              it: both were blocked only on <time.h> arriving
#                              through recon_fs.h and recon_cookie.h, and
#                              recon_fs.h turned out to be pulling in
#                              <sys/types.h> it does not use at all.
#
#   src/recon_stb.c          the vendored stb shim, which wants third_party/
#                              on the include path and a hosted <math.h>.
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

# Nine of seventy-nine, and every one of them verified by this script rather
# than by a sweep. Seven more are one header away; see above.
FILES="
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
		-std=c11 -ffreestanding -fno-builtin \
		-nostdinc \
		-isystem "$BUILTIN" \
		-I userland/include \
		-I include \
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
