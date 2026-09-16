#!/bin/bash
# What is left of the port, and how much each blocker is holding.
#
# --- why this is not check-userland.sh -----------------------------------
#
# That script answers "how many sources build with no Linux under them" and
# lists the ones that do. It is a progress number and it is the right one.
#
# It says nothing about the rest, and the rest is where the decisions are. The
# useful question is not *which* files are stuck but **what is holding them and
# how much it is holding** -- because the answer has twice now been "a very
# small thing is holding a very large one", and neither time did anybody expect
# it:
#
#   v0.4.42  `recon_ui.h` pulled xkbcommon for one typedef. Twenty-four files.
#   v0.4.44  `recon_ui.c` was 3,100 lines of drawing, 28 of which said wlroots.
#   v0.4.46  `recon_server.h` -- a whole compositor -- was included by five
#            applications for one field, and by two of them for nothing at all.
#   v0.4.57  `recon_appwin.c` was 1,629 lines held by **one forwarding
#            function** and a second include used by nothing.
#
# Each time the file's own header had a theory about why it was stuck, and each
# time the theory was wrong. So this measures instead of theorising: it counts
# the lines a blocker holds, which is the number that says whether a seam is
# worth drawing.
#
# --- what the columns mean ------------------------------------------------
#
#   lines     how big the file is -- what would be freed
#   markers   how many lines in it mention a compositor at all
#   blocker   the first thing the compiler could not find
#
# **A small `markers` beside a large `lines` is the shape to look for.** It is
# a file that is about something else and is being held by an accident.
set -u

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR" || exit 1

CC="${CC:-gcc}"
BUILTIN="$("$CC" -print-file-name=include)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

held=0
stuck=0

printf '%-26s %7s %8s  %s\n' "source" "lines" "markers" "what stops it"
printf '%-26s %7s %8s  %s\n' "--------------------------" "-----" "-------" \
    "-------------------------------"

for f in src/*.c; do
    if "$CC" -c "$f" -o "$OUT/x.o" \
        -std=c11 -ffreestanding -fno-builtin -fno-math-errno \
        -nostdinc -isystem "$BUILTIN" \
        -I userland/include -I include -isystem third_party \
        -DRECONOS_VERSION='"0.0.0"' \
        -Wall -Wextra -Wno-unused-parameter -Werror 2> "$OUT/err"
    then
        continue
    fi

    lines=$(wc -l < "$f")
    markers=$(grep -ci 'wlr\|wayland\|xdg_\|drm_' "$f" || true)

    # The first thing it could not find, or the first error if it found
    # everything and disliked something else.
    why=$(grep -m1 -oE "fatal error: [^:]+" "$OUT/err" | sed 's/fatal error: //')
    if [ -z "$why" ]; then
        why=$(grep -m1 -oE "error: .*" "$OUT/err" | cut -c1-44)
    fi

    printf '%-26s %7s %8s  %s\n' "$(basename "$f")" "$lines" "$markers" "$why"
    held=$((held + lines))
    stuck=$((stuck + 1))
done

echo
echo "$stuck sources do not build freestanding, holding $held lines."
echo
echo "Look for a small marker count beside a large line count: that is a file"
echo "about something else, held by an accident rather than by its subject."
