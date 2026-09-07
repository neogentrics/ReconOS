#!/bin/bash
# Run the compiler's static analyzer over everything.
#
# A third question to ask the tools, after the warnings and the sanitizers, and
# it finds a different class from both. The warnings look at one statement; the
# sanitizers look at the paths a test actually takes; this walks paths nothing
# has ever run -- the branch where a malloc fails, the third way into a
# function that clears a struct owning a pointer.
#
# What it found the first time it was run: a memset over a struct that owns a
# pointer in Notepad's undo stack (correct, for a reason two functions away
# that nothing stated at that line), an account slot the code was trusted to
# find and did not check, and three test allocations that would have turned a
# failed malloc into a segfault -- which reports nothing at all, not even which
# check it died on.
#
# It is noisier than the other two. An analyzer that follows every path also
# imagines some, and the ones it imagines here are ownership handed to
# somebody else: a file descriptor given to the Wayland event loop looks like
# a leak, because the loop closes it somewhere this cannot see. Those are
# listed below rather than silenced, so the list stays short enough to read and
# a new entry in it is a new thing.
#
#   ./scripts/analyze.sh
#
# Silence is the result. Anything printed that is not in KNOWN is worth
# looking at.

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${RECONOS_ANALYZE_BUILD:-$REPO_DIR/build-analyzed}"

# Ownership the analyzer cannot follow, one per line, as "file:line: reason".
#
# Both of these are the control socket's listening descriptor. It is handed to
# wl_event_loop_add_fd and closed in recon_control_destroy, neither of which
# this analysis can see from inside recon_control_create. Every failure path
# between the socket() and the add_fd does close it -- checked by reading,
# because that is the only way to check it.
KNOWN='src/recon_control.c:612|src/recon_control.c:656'

cmake -S "$REPO_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fanalyzer" >/dev/null

# Warnings only. The build's own output is not the point and a failed build
# says so loudly enough on its own.
out=$(cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1 | \
    grep -E 'warning:.*-Wanalyzer' | \
    sed "s|$REPO_DIR/||; s|$BUILD_DIR/||" | \
    sort -u || true)

if [ -n "$out" ]; then
    out=$(printf '%s\n' "$out" | grep -Ev "$KNOWN" || true)
fi

if [ -z "$out" ]; then
    echo "static analyzer: clean"
    exit 0
fi

printf '%s\n' "$out"
echo
echo "$(printf '%s\n' "$out" | wc -l) finding(s) the analyzer is not told to expect."
exit 1
