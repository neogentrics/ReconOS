#!/bin/bash
# Can a ReconOS program that draws a desktop be LINKED yet, and what is missing?
#
# --- why this exists, and what it replaces -------------------------------
#
# `check-userland.sh` proves each source can be handed to a compiler with no
# Linux under it. That is a real thing to know and it is not the question the
# port is actually asking, which is whether the pieces -- put together with the
# ReconOS C library and nothing else -- make a program with no holes in it.
#
# The subject is `userland/desktop/desktop.c` -- the actual program that will run
# from the volume -- rather than a stand-in written to be linkable. A probe
# somebody adjusts whenever it will not link is a probe whose number is about
# the probe.
#
# It exists because the inference it replaces got a published answer wrong.
# On 16 September an earlier probe came back with three unresolved symbols,
# those three were read as "what a desktop needs from recon_fs.c", and a whole
# argument was built on top: walk the call graph from the three, find no
# `stat`, conclude `stat` is not the wall. Every step was sound and the premise
# was not. `recon_theme.c` and `recon_fonts.c` call **twelve** of that file's
# entry points, not three; several of them reach `stat`; and the correction had
# already gone into the change log, the board and a note to the kernel session
# by the time this script said so in one line.
#
# The lesson is not "check harder". It is that a linker answers this question
# directly and a call-graph walk answers it by inference -- so the walk belongs
# downstream of the link, explaining an answer rather than producing one.
#
# --- two flags that are not optimisations --------------------------------
#
# `-ffunction-sections` and `--gc-sections`. A linker pulls a whole object out
# of an archive, so without them every unreached function's undefined symbols
# have to resolve too, and the answer is "everything in recon_fs.c" rather than
# "what this program uses". With them the question is the one worth asking.
#
# Any real ReconOS link should pass them for the same reason.
#
# --- usage ---------------------------------------------------------------
#
#   ./scripts/link-desktop.sh            what is missing
#   STUB=1 ./scripts/link-desktop.sh     supply stat, to see what is BEHIND it
#
# The stub is for measurement only. A `stat` that reports failure would be the
# worst possible real implementation, which is exactly why
# `userland/include/sys/stat.h` declines to ship one.
set -u

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR" || exit 1

CC="${CC:-gcc}"
BUILTIN="$("$CC" -print-file-name=include)"
OUT="${OUT:-$(mktemp -d)}"
STUB="${STUB:-}"
mkdir -p "$OUT"

CFLAGS=(-std=c11 -ffreestanding -fno-builtin -fno-math-errno
        -fno-stack-protector -ffunction-sections -fdata-sections
        -nostdinc -isystem "$BUILTIN"
        -I userland/include -I include -isystem third_party
        -DRECONOS_VERSION='"0.0.0"' -w)

echo "Compiling every desktop source that builds with no libc under it"
n=0
for f in src/*.c; do
    if "$CC" -c "$f" -o "$OUT/$(basename "$f").o" "${CFLAGS[@]}" 2>/dev/null; then
        n=$((n + 1))
    fi
done
echo "  $n objects"

echo "Compiling the ReconOS C library"
m=0
for f in userland/libc/*.c; do
    if "$CC" -c "$f" -o "$OUT/libc-$(basename "$f").o" "${CFLAGS[@]}" \
            -I userland/libc 2>/dev/null; then
        m=$((m + 1))
    fi
done
echo "  $m objects"

if [ -n "$STUB" ]; then
    cat > "$OUT/zz-stat.c" <<'EOF'
#include <sys/stat.h>
int stat(const char *p, struct stat *s) { (void)p; (void)s; return -1; }
int lstat(const char *p, struct stat *s) { (void)p; (void)s; return -1; }
EOF
    "$CC" -c "$OUT/zz-stat.c" -o "$OUT/stat-stub.o" "${CFLAGS[@]}"
    echo "  (stat and lstat stubbed, to see what is behind them)"
fi

ar rcs "$OUT/libdesktop.a" "$OUT"/*.o 2>/dev/null

# From here the objects are named zz-*.o and are deliberately NOT in the
# archive: they are the program. A program inside the library it links against
# is one the linker may or may not pull in depending on what else it needs,
# which is not a thing to leave to chance.

echo "Compiling the desktop program itself"
for f in userland/desktop/*.c scripts/link-desktop-probe.c; do
    if ! "$CC" -c "$f" -o "$OUT/zz-$(basename "$f").o" "${CFLAGS[@]}" \
            -I userland/desktop 2> "$OUT/probe-err"; then
        echo "  $f does not compile, which is a fault in the program:"
        head -12 "$OUT/probe-err"
        exit 1
    fi
done

echo "Linking, with nothing but ReconOS underneath"
if "$CC" -nostdlib -static -Wl,--gc-sections -o "$OUT/desktop" \
        "$OUT"/zz-*.o "$OUT/libdesktop.a" "$OUT/libdesktop.a" \
        2> "$OUT/link-err"; then
    echo "  linked, $(stat -c%s "$OUT/desktop") bytes,"\
         "$(nm -u "$OUT/desktop" | wc -l) undefined symbols"
    exit 0
fi

echo "  did not link. What the kernel still owes it:"
grep -oE "undefined reference to .[A-Za-z0-9_]+." "$OUT/link-err" \
    | sed 's/undefined reference to .//; s/.$//' | sort -u | sed 's/^/    /'
exit 1
