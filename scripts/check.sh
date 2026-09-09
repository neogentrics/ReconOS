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
# --- And then again, optimised ---
#
# Every build this project makes for itself is Debug: build.sh, this script,
# analyze.sh and coverage.sh all pass -DCMAKE_BUILD_TYPE=Debug. The one that
# is not is scripts/package.sh, which builds Release -- so the thing that
# actually ships is the one configuration nothing tests.
#
# That is not theoretical. A containment check added to recon_fs passed the
# sanitizers, the analyzer and every suite, and **aborted on the first path it
# resolved in an optimised build**: realpath's second argument must be at
# least PATH_MAX, and with _FORTIFY_SOURCE on -- which is any optimised build
# on this distribution -- glibc checks the size and kills the process. Debug
# is not fortified, so nothing here could see it.
#
# So the suites are run twice: once sanitized, once the way a release is
# built. The second pass is fast, because it is the same sources.
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
# A finding in the sanitized pass stops here: the optimised one would report
# the same thing again, and two copies of one finding is a longer list rather
# than a fuller one.
if [ "$found" != "0" ]; then
    exit "$found"
fi

# --- The same suites, built the way a release is ---
#
# Optimisation changes what the compiler may assume and what glibc checks, and
# the two together find a class the sanitizers cannot: fortified calls whose
# size rules are only enforced when the optimiser can see the size.
RELEASE_DIR="${RECONOS_RELEASE_BUILD:-$REPO_DIR/build-release-check}"

cmake -S "$REPO_DIR" -B "$RELEASE_DIR" \
    -DCMAKE_BUILD_TYPE=Release >/dev/null

release_targets=()
for t in "$REPO_DIR"/tests/test_*.c; do
    name="recon_$(basename "$t" .c | sed 's/^test_//')_tests"
    release_targets+=("$name")
done

cmake --build "$RELEASE_DIR" -j"$(nproc)" --target "${release_targets[@]}" \
    >/dev/null 2>&1 || {
    echo "Some targets did not build optimised; running what did." >&2
}

release_ran=0
release_found=0
for t in "$RELEASE_DIR"/*_tests; do
    [ -x "$t" ] || continue
    release_ran=$((release_ran + 1))
    if ! out=$("$t" 2>&1); then
        echo "== $(basename "$t") failed when built optimised"
        printf '%s\n' "$out" | tail -5
        release_found=1
    fi
done

echo "$release_ran suites built the way a release is"
if [ "$release_found" = "1" ]; then
    exit 1
fi

# --- And a class of bug no sanitizer can see ---
#
# `memset` on a secret that nothing reads afterwards is a dead store, and an
# optimising compiler deletes it. Measured, not assumed: at -O2 a function that
# fetches a password, copies it out and memsets its buffer compiles to the two
# calls and a return, with no zeroing at all -- while the volatile loop in
# recon_secure_erase survives untouched.
#
# The sanitizers cannot find this. There is no invalid access, no leak and no
# undefined behaviour: the program is correct, it simply does not do the thing
# the line was written to do. It passed every suite, both sanitizers and the
# analyzer for as long as it existed, and it would have shipped -- package.sh
# builds Release.
#
# So it is checked here, in the source, which is the only place the difference
# between "memset" and "recon_secure_erase" is visible.
echo
echo "Checking that secrets are erased with something the compiler keeps"

erase_found=0
while IFS= read -r hit; do
    # The name of the thing being cleared, not the whole line.
    case "$hit" in
        *recon_secure_erase*) continue ;;
    esac
    echo "== $hit"
    erase_found=1
done < <(grep -rn 'memset(' "$REPO_DIR/src" \
    | grep -iE 'secret|password|passphrase|private_key|plaintext' \
    | grep -v 'recon_secure_erase')

if [ "$erase_found" = "1" ]; then
    echo
    echo "Those clear a secret with memset. Nothing reads the buffer"
    echo "afterwards, so the compiler is entitled to delete the store and"
    echo "does. Use recon_secure_erase, which writes through a volatile"
    echo "pointer and survives -O2."
    exit 1
fi
echo "secrets are erased with recon_secure_erase"

# --- The generated files, against what generates them ---
#
# docs/ERRORS.md and the Help application's pages are both written by scripts
# from a source that is compiled -- include/recon_errors.def and
# docs/CHANGELOG.md. The rule has always been "run the script in the same
# commit", and a rule of that shape holds until somebody is busy.
#
# It did not hold. The Help pages were last regenerated fourteen versions
# before this check was added, so the change log inside the running system
# stopped at v0.4.5 and said nothing about it -- which is precisely the fault
# recon_html.h cites as the reason a truncated page must announce itself.
#
# Checked by regenerating into a copy and comparing. A generated file that has
# drifted from its source is not a style problem: it is the system telling
# somebody something that stopped being true.
echo
echo "Checking that the generated files match what generates them"

GENERATED_TMP="$(mktemp -d)"
trap 'rm -rf "$GENERATED_TMP"' EXIT

cp -r "$REPO_DIR/assets/help" "$GENERATED_TMP/help-was"
cp "$REPO_DIR/docs/ERRORS.md" "$GENERATED_TMP/errors-was.md"

"$REPO_DIR/scripts/make-help.sh" >/dev/null
"$REPO_DIR/scripts/make-errors.sh" >/dev/null

generated_drift=0
if ! diff -r -q "$GENERATED_TMP/help-was" "$REPO_DIR/assets/help" >/dev/null; then
    echo "== assets/help is not what docs/CHANGELOG.md generates"
    diff -r -q "$GENERATED_TMP/help-was" "$REPO_DIR/assets/help" | head -10
    generated_drift=1
fi
if ! diff -q "$GENERATED_TMP/errors-was.md" "$REPO_DIR/docs/ERRORS.md" >/dev/null; then
    echo "== docs/ERRORS.md is not what include/recon_errors.def generates"
    generated_drift=1
fi

# Put the tree back exactly as it was found, whichever way the comparison went.
#
# The first version left the regenerated files in place, on the reasoning that
# it was doing somebody a favour. It is not: a check that repairs the thing it
# is checking passes the second time it is run, so a build that ran it twice
# would report clean on a tree that had drifted. A check exists to say what is
# true, and changing what is true so that it can say something nicer is the
# one thing it must not do.
rm -rf "$REPO_DIR/assets/help"
cp -r "$GENERATED_TMP/help-was" "$REPO_DIR/assets/help"
cp "$GENERATED_TMP/errors-was.md" "$REPO_DIR/docs/ERRORS.md"

if [ "$generated_drift" = "1" ]; then
    echo
    echo "The generated files have drifted from their sources. Run"
    echo "scripts/make-help.sh and scripts/make-errors.sh and commit what"
    echo "changes -- the running system has been telling somebody something"
    echo "that stopped being true."
    exit 1
fi
echo "the generated files match their sources"

echo "clean"
