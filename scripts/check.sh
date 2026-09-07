#!/bin/bash
# Run every test suite under the address and undefined-behaviour sanitizers.
#
# Separate from the ordinary build because the sanitizers make everything two
# to three times slower and change the memory layout, which is exactly why they
# find things -- and exactly why they are not what you want while iterating.
#
# What they catch that the tests do not: reading one byte past an array,
# freeing something twice, using memory after it was freed, shifting by more
# than a word, signed overflow, a misaligned load. Every one of those is a bug
# that passes its test on the machine it was written on and fails somewhere
# else, which is the worst kind to own.
#
# This is worth running before cutting a release, and after anything that
# touches parsing -- a decoder handed a malformed file is where these live.
#
#   ./scripts/check.sh
#
# Silence is the result. Anything printed is a real finding.

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${RECONOS_SAN_BUILD:-$REPO_DIR/build-sanitized}"

cmake -S "$REPO_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" >/dev/null

# The test targets only. The compositor itself needs a display to do anything,
# and a sanitized binary that exits at startup has proved nothing.
targets=()
for t in "$REPO_DIR"/tests/test_*.c; do
    name="recon_$(basename "$t" .c | sed 's/^test_//')_tests"
    targets+=("$name")
done

cmake --build "$BUILD_DIR" -j"$(nproc)" >/dev/null 2>&1 || {
    echo "Some targets did not build; running what did." >&2
}

found=0
ran=0
for t in "$BUILD_DIR"/*_tests; do
    [ -x "$t" ] || continue
    ran=$((ran + 1))

    if ! out=$("$t" 2>&1); then
        # A failing assertion is the ordinary test suite's business, not this
        # script's -- but it is worth saying, because a suite that fails here
        # and passes there means the sanitizers changed the answer.
        echo "== $(basename "$t") failed"
        printf '%s\n' "$out" | tail -5
        found=1
        continue
    fi

    if printf '%s' "$out" | grep -qE 'ERROR: |runtime error'; then
        echo "== $(basename "$t")"
        printf '%s\n' "$out" | grep -E 'ERROR: |runtime error' | head -6
        printf '%s\n' "$out" | grep -E '^ +#[0-9]+ ' | head -6
        found=1
    fi
done

echo "$ran suites under address and undefined-behaviour sanitizers"
[ $found -eq 0 ] && echo "clean"
exit $found
