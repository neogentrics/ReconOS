#!/bin/bash
#
# Turn include/recon_errors.def into docs/ERRORS.md.
#
# The codes live in one place -- the .def file the system compiles -- so a code
# cannot say one thing on screen and another in the documentation. This script
# is the only thing allowed to write docs/ERRORS.md; editing that file by hand
# is editing a copy.
#
# Run it after adding a code, in the same commit.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$REPO_DIR/include/recon_errors.def"
OUT="$REPO_DIR/docs/ERRORS.md"

if [ ! -f "$SOURCE" ]; then
    echo "make-errors: $SOURCE is not there" >&2
    exit 1
fi

{
    cat <<'HEADER'
# ReconOS error codes

Every code ReconOS can report, what it means, and how bad it is.

**This file is generated.** The codes live in `include/recon_errors.def`, which
is what the system compiles and what the `errors` command reads. Run
`scripts/make-errors.sh` after adding one; editing this file by hand edits a
copy.

## How a code is put together

```
VT-A001
^^ ^ ^^^
|  | +--- which fault, 001 upward within that area
|  +----- which area of the system
+-------- Void Tower
```

**The letter is the area, not the severity.** The same area produces faults of
every kind — the filesystem can fail to read a file and fail to open at all —
so severity in the letter would scatter one subsystem across the alphabet.
Worse, a code would have to change if a fault were ever reclassified, and a
code that changes is a code nobody can look up. Severity is a property of the
entry instead, listed below.

**I and O are not area letters.** This is a code somebody reads off a screen
and types into a search box, and in that setting `I` is `1` and `O` is `0`.

**A hundred codes per letter.** When an area fills, it continues at a second
letter reserved for it rather than renumbering: the old codes are already
written down somewhere. Twenty-four letters at a hundred each is two thousand
four hundred.

**A number is never reused**, even after the fault it named stops existing.

## What the severities mean

| | |
| --- | --- |
| **STOP** | The system cannot continue. The screen that says so shows the code, and the code and time are written to `/System/Logs`. |
| **fault** | Something failed and the system carried on without it. Reported where it happened. |
| **note** | Recorded, and nothing broke. There because a run of them is worth seeing even though one is not a problem. |

## Looking one up

On the machine that showed it:

```
errors VT-A001
```

`errors` on its own lists them all, and `errors log` is what has actually
happened on that machine.

HEADER

    awk '
        function flush_entry() {
            if (code == "") return

            if (area != last_area) {
                printf "\n## %s — %s\n\n", area, area_name(area)
                printf "| Code | Severity | What it means |\n"
                printf "| --- | --- | --- |\n"
                last_area = area
            }

            printf "| **%s** | %s | **%s** \xe2\x80\x94 %s |\n", code,
                level_word(level), summary, detail
            code = ""
        }

        function level_word(l) {
            if (l == "STOP")  return "STOP"
            if (l == "FAULT") return "fault"
            return "note"
        }

        function area_name(a) {
            if (a == "A") return "Startup and shutdown"
            if (a == "B") return "Storage and the filesystem"
            if (a == "C") return "Accounts and signing in"
            if (a == "D") return "Display, windows and drawing"
            if (a == "E") return "Programs, modules and packages"
            if (a == "F") return "Network"
            if (a == "G") return "Firewall and remote access"
            if (a == "H") return "Settings and the registry"
            if (a == "J") return "Applications"
            if (a == "K") return "Input"
            if (a == "L") return "Skins, icons and wallpapers"
            if (a == "M") return "Help and documentation"
            return "(unnamed area)"
        }

        # The head of an entry names the area, the number and the severity.
        /^RECON_ERROR\(/ {
            flush_entry()

            head = $0
            sub(/^RECON_ERROR\(/, "", head)
            gsub(/[ \t]/, "", head)

            split(head, parts, ",")
            area = parts[1]
            number = parts[2]
            level = parts[3]

            code = "VT-" area number
            summary = ""
            detail = ""
            piece = 0
            next
        }

        # Everything after it, until the closing bracket, is string literals:
        # the first group is the summary and the rest is the detail. Adjacent
        # literals are joined the way the compiler joins them.
        code != "" {
            line = $0
            closing = (line ~ /\)[ \t]*$/)

            # Keep only what is between quotes.
            text = ""
            inside = 0
            for (i = 1; i <= length(line); i++) {
                c = substr(line, i, 1)
                if (c == "\"") { inside = !inside; continue }
                if (inside) text = text c
            }

            if (text == "") { if (closing) flush_entry(); next }

            if (piece == 0) {
                summary = text
                piece = 1
            } else {
                # A space between joined pieces only where the previous one
                # did not already end in one -- the .def wraps mid-sentence.
                if (detail != "" && substr(detail, length(detail), 1) != " ")
                    detail = detail " "
                detail = detail text
            }

            if (closing) flush_entry()
            next
        }

        END { flush_entry() }
    ' "$SOURCE"

    printf "\n"
} > "$OUT"

count=$(grep -c '^| \*\*VT-' "$OUT" || true)
echo "Wrote $count codes to docs/ERRORS.md"
