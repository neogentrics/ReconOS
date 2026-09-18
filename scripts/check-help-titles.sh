#!/usr/bin/env bash
#
# Does every help title fit the buffer the searches carry it in?
#
# --- what goes wrong when one does not ---
#
# `recon_help_search` skips a title longer than `RECON_HELP_TITLE_MAX`, and it
# is right to: a title cut to fit is handed to `recon_help_show_topic`, which
# looks pages up by exact text, so a cut title names a page that cannot then be
# opened. The comment beside the skip says so.
#
# **The consequence is that the page disappears.** Not from Help -- it is still
# in the list and still readable -- but from every *search*: the Start menu's,
# `help <word>`, and `errors <code>`. Nothing reports it. A page nobody can
# find is a page that might as well not have been written.
#
# It had happened. The buffer was 64 and the change log had grown four headings
# past it, the longest 71 bytes. It surfaced by accident, when `errors E006`
# reported no pages for a code that is written out in full on one.
#
# So the number is held against the titles that actually exist, and the next
# long heading is this check going red rather than a page quietly going
# missing.
#
#   ./scripts/check-help-titles.sh
#
# Run by scripts/check.sh. Wants assets/help/index.txt, which make-help.sh
# generates.

set -u

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

HEADER=include/recon_help.h
INDEX=assets/help/index.txt

[ -f "$INDEX" ] || { echo "no $INDEX -- run scripts/make-help.sh"; exit 1; }

# The limit, read from the header rather than repeated here. A second copy of
# the number is a second thing to keep in step, and this check exists because
# one number got out of step with the data it was about.
LIMIT=$(sed -n 's/^#define RECON_HELP_TITLE_MAX \([0-9]*\).*/\1/p' "$HEADER")

if [ -z "$LIMIT" ]; then
    echo "could not read RECON_HELP_TITLE_MAX from $HEADER"
    exit 1
fi

# Bytes, not characters, and `LC_ALL=C` is what makes `${#title}` count them.
#
# Without it this measured 69 where `strlen` measures 71: the change log's
# headings use an em dash, which is one character and three bytes. A check for
# a byte limit that counts characters is a check that passes two bytes before
# the thing it is checking for -- which is the same shape as the bug it exists
# to catch, one level up.
#
# `strlen(title) >= LIMIT` is the test in recon_help.c, so a title of exactly
# LIMIT-1 bytes is the longest that fits.
export LC_ALL=C
longest=0
longest_title=""
over=0

while IFS=$(printf '\t') read -r _file title; do
    [ -n "${title:-}" ] || continue
    n=${#title}
    if [ "$n" -gt "$longest" ]; then
        longest=$n
        longest_title=$title
    fi
    if [ "$n" -ge "$LIMIT" ]; then
        if [ "$over" -eq 0 ]; then
            echo "== help titles that no search can return"
            echo "   RECON_HELP_TITLE_MAX is $LIMIT, and recon_help_search"
            echo "   skips anything that does not fit."
            echo
        fi
        printf '   %3d  %s\n' "$n" "$title"
        over=$((over + 1))
    fi
done < "$INDEX"

if [ "$over" -gt 0 ]; then
    echo
    echo "$over of $(wc -l < "$INDEX") titles are too long to be found."
    echo "Raise RECON_HELP_TITLE_MAX in $HEADER, or shorten the headings"
    echo "in docs/CHANGELOG.md that generate them."
    exit 1
fi

printf 'every help title fits (%d titles, longest %d of %d bytes)\n' \
    "$(wc -l < "$INDEX")" "$longest" "$((LIMIT - 1))"
