#!/bin/bash
#
# Turn docs/HELP.md and docs/CHANGELOG.md into the pages the Help application
# shows.
#
# One source for each, so a change is written down once. The alternative --
# help text living in a C file next to the code it describes -- means the
# person writing a feature has to remember to update prose in a language they
# are not currently writing, which is the arrangement under which help gets
# out of date.
#
# The output is a folder of plain text files plus an index naming them in
# order. Plain text because the Help window draws text: giving it Markdown
# would mean writing a Markdown renderer to show a document that has no
# formatting in it beyond headings and lists.
#
# Run from the repository root. Rerun after editing either source.

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$REPO_DIR/assets/help"

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*.txt "$OUT_DIR/index.txt"

# Split a Markdown file on "## " headings. Everything before the first one is
# preamble about the file itself and is dropped -- it is a note to whoever
# edits the source, not to whoever reads the help.
split_topics() {
    local source="$1"
    local prefix="$2"

    awk -v out="$OUT_DIR" -v prefix="$prefix" '
        /^## / {
            title = substr($0, 4)
            n++
            # A file name from the number, so order is kept without the
            # index having to carry it, and so a renamed topic does not
            # orphan a file.
            file = sprintf("%s-%02d.txt", prefix, n)
            path = out "/" file
            printf "" > path
            started = 0
            print file "\t" title >> (out "/index.txt")
            next
        }
        n > 0 {
            # Drop the emphasis markers. The window draws one weight of text,
            # so a ** left in is two asterisks in the middle of a sentence
            # rather than a word standing out.
            line = $0
            gsub(/\*\*/, "", line)
            gsub(/`/, "", line)

            # No blank line at the very top of a topic: the heading is why
            # that blank line was there, and the heading has gone.
            # (No apostrophes in here -- the awk program is single-quoted,
            # and one closes it.)
            if (started == 0 && line ~ /^[ 	]*$/) {
                next
            }
            started = 1
            print line >> path
        }
    ' "$source"
}

split_topics "$REPO_DIR/docs/HELP.md" "help"
split_topics "$REPO_DIR/docs/CHANGELOG.md" "changes"

# Trim the blank lines that collect at the end of each topic, so a page does
# not scroll past its own ending.
for file in "$OUT_DIR"/*.txt; do
    [ "$(basename "$file")" = "index.txt" ] && continue
    # shellcheck disable=SC2016
    trimmed="$(awk 'NF {last = NR} {lines[NR] = $0} END {for (i = 1; i <= last; i++) print lines[i]}' "$file")"
    printf '%s\n' "$trimmed" > "$file"
done

echo "Wrote $(grep -c . "$OUT_DIR/index.txt") topics to assets/help"
