#!/bin/bash
# Which lines the test suites actually run.
#
# The fourth question for the tools, and the one that answers a different kind
# of doubt from the other three. The warnings, the sanitizers and the analyzer
# all look at code; this looks at the tests, and says which parts of the code
# they have never touched.
#
# Why it earns its place: twice in one night a test passed while testing
# nothing. The MP4 fixture in tests/test_malformed.c was a header and an empty
# `moov`, so the sample walk it was written for never ran -- found by deleting
# a bounds check on purpose and watching five thousand cases report no
# failures. Reading the test would not have shown it. A number saying "this
# function is at 0%" would have.
#
# It is a measurement, not a target. A percentage is a bad goal: chasing one
# produces tests that execute code without asserting anything about it, which
# is worse than no test because it looks like coverage. What this is for is the
# specific question "is anything I believe to be tested not being run at all",
# which has a yes-or-no answer and is worth asking before a release.
#
# --- What the number is, exactly ---
#
# **The most any single suite runs of that file**, not the union of all of
# them. A file linked into fourteen test targets is compiled fourteen times,
# and gcov given all fourteen .gcda files reports the sums -- so recon_fs.c
# came out as "21% of 10920 lines" when it has 780, and the percentage was the
# average across suites rather than how much of it anything reaches. Both
# columns were wrong, and the first version of this script printed them.
#
# So each suite is measured on its own and the best figure for each file is
# kept. That answers the question this exists for exactly -- 0% means nothing
# runs it -- and deliberately does not answer "how much of this file is
# covered in total", which would need merging counters gcov will not merge.
#
#   ./scripts/coverage.sh            what each file under test comes to
#   ./scripts/coverage.sh --zero     only the functions nothing runs
#
# The compositor is not in this. Every test target in this project runs without
# a display, and the parts that need one are not reachable from any of them --
# so listing recon_shell.c at 0% would be reporting the design rather than a
# gap. What is measured is the source that the test targets actually link.

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${RECONOS_COVERAGE_BUILD:-$REPO_DIR/build-coverage}"

ZEROS_ONLY=0
if [ "$1" = "--zero" ]; then
    ZEROS_ONLY=1
fi

# --coverage on both sides: it needs the counters compiled in and the runtime
# linked, and giving it to only one of the two fails at the link with a message
# about __gcov_init that says nothing about coverage.
cmake -S "$REPO_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_FLAGS="--coverage -O0 -g" \
    -DCMAKE_EXE_LINKER_FLAGS="--coverage" >/dev/null

# Only the test targets, for the reason in the header.
targets=()
for t in "$REPO_DIR"/tests/test_*.c; do
    name="recon_$(basename "$t" .c | sed 's/^test_//')_tests"
    targets+=("$name")
done

cmake --build "$BUILD_DIR" -j"$(nproc)" --target "${targets[@]}" >/dev/null 2>&1 || {
    echo "Some test targets did not build; measuring what did." >&2
}

# Counters are cumulative across runs, so a stale one from a previous
# invocation would be reported as coverage this run did not produce.
find "$BUILD_DIR" -name '*.gcda' -delete

ran=0
for t in "$BUILD_DIR"/*_tests; do
    [ -x "$t" ] || continue
    ran=$((ran + 1))
    "$t" >/dev/null 2>&1 || true
done
echo "$ran suites run"
echo

cd "$BUILD_DIR"

# One target at a time. Together, gcov reports the sums; see the note above.
measure() {
    for dir in CMakeFiles/*_tests.dir; do
        [ -d "$dir" ] || continue
        files=$(find "$dir" -name '*.gcda')
        [ -n "$files" ] || continue
        gcov "$@" -n $files 2>/dev/null
    done
}

if [ "$ZEROS_ONLY" = "1" ]; then
    echo "Functions no suite runs:"
    echo
    #
    # The best figure across suites, and then the zeros -- not each suite's
    # zeros.
    #
    # A file is linked into several targets and only one of them exercises it,
    # so every function in it reads as 0% from all the others. Filtering on
    # 0.00% line by line therefore lists most of the system, including things
    # with tests, which is a report nobody can act on. It did, before this.
    #
    measure -f | \
        awk '
            /^Function/ {
                match($0, /\047[^\047]+\047/)
                fn = substr($0, RSTART + 1, RLENGTH - 2)
                want = 1
                next
            }
            /^Lines executed:/ {
                if (!want) { next }
                want = 0
                split($0, parts, ":")
                split(parts[2], bits, "%")
                if (!(fn in best) || bits[1] + 0 > best[fn]) {
                    best[fn] = bits[1] + 0
                }
            }
            END {
                for (f in best) {
                    if (best[f] == 0) { print f }
                }
            }
        ' | sort
    exit 0
fi

printf '%-34s %8s  %s\n' "FILE" "LINES" "MOST RUN"

# gcov quotes the path with apostrophes, not quotation marks, and prints the
# figure twice per file -- once for the file and once as the run's summary.
# Both cost a round of an empty table before anybody looked at what it writes.
measure | \
    awk '
        /^File/ {
            match($0, /\047[^\047]+\047/)
            file = substr($0, RSTART + 1, RLENGTH - 2)
            want = 1
            next
        }
        /^Lines executed:/ {
            if (!want) { next }
            want = 0
            split($0, parts, ":")
            split(parts[2], bits, "% of ")
            if (file !~ /third_party/ && (file ~ /\/src\// || file ~ /\/tests\//)) {
                short = file
                sub(/.*\//, "", short)
                if (!(short in seen) || bits[1] + 0 > seen[short]) {
                    seen[short] = bits[1] + 0
                    total[short] = bits[2]
                }
            }
        }
        END {
            for (f in seen) {
                printf "%-34s %8s  %6.2f%%\n", f, total[f], seen[f]
            }
        }
    ' | sort -k3 -n
