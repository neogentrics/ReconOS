#!/usr/bin/env bash
#
# Nowhere in the desktop may a release build say a string might be cut.
#
# **Zero, not "no more than before".** `include/ReconOS.h` made the argument
# when it introduced `recon_text_copy`: *"a build with fourteen warnings in it
# is a build where the fifteenth is invisible."* A threshold is a build where
# the fifteenth is invisible with extra steps -- it passes, and the number it
# passes against is one somebody wrote down once.
#
# Every site is therefore one of two things, and says which in the source:
#
#   a cut that is meant       -- through recon_text_copy / recon_text_printf,
#                                or written out where a file cannot include
#                                ReconOS.h, with a comment saying why the cut
#                                is harmless
#   a name something looks up -- refused, because a truncated name is not a
#                                shortened name for the same thing
#
# The first sweep of this found five of the second kind in a list a board row
# had described as *"every one builds a string to display rather than to open"*.
#
# --- What it covers, which is not everything ---
#
# `-Wall` turns on `-Wformat-truncation=1`: the compiler warns where it can
# *prove* a cut is possible from lengths it can see -- a 192-byte field into a
# 128-byte buffer. It says nothing about a `const char *` of unknown length,
# so a new call whose input the optimiser cannot bound passes this check.
#
# Level 2 assumes any argument can be arbitrarily long. The desktop has **147
# sites** at that level, measured, so it is not a bar this tree holds today and
# turning it on would replace one invisible fifteenth warning with a hundred
# and forty-seven.
#
# So the claim here is exactly: *nowhere that the compiler can show a cut is
# reachable.* That is what found the five names being cut instead of refused,
# and it is the strongest bar this tree can currently be held to.
#
# --- Why it compiles rather than greps ---
#
# The warning is the optimiser's, and it depends on what the optimiser can
# prove about the lengths reaching a call. No pattern over the source can tell
# `snprintf` into a buffer that provably fits from one that does not, which is
# the only distinction that matters here.
#
# --- Why it forces a recompile ---
#
# A warning is emitted when a file is compiled. An incremental build compiles
# nothing and reports nothing, so a check reading one would pass on any tree
# whose build directory was warm -- which is every tree after the first run.
# Touching the sources is what makes the answer about the tree rather than
# about the build directory.
set -u
cd "$(dirname "$0")/.."

CC_DIR="${RECONOS_TRUNCATION_BUILD:-build-truncation-check}"

if ! cmake -S . -B "$CC_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1; then
	echo "could not configure a release build to look for truncation in"
	exit 1
fi

touch src/*.c

LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT

# The desktop itself, which is the one target that compiles every file in src/.
if ! cmake --build "$CC_DIR" -j"$(nproc)" --target ReconOS >"$LOG" 2>&1; then
	echo "the desktop does not build optimised"
	grep -E ": (error|warning):" "$LOG" | head -20
	exit 1
fi

# By file:line:column, deduplicated. One warning prints a `warning:` line and
# one or more `note:` lines, and a file compiled into several targets emits the
# same warning once per target -- so a count of matching lines is several times
# too large and moves when GCC changes how much it explains itself.
sites=$(grep -E '^.*/src/[^ ]+\.(c|h):[0-9]+:[0-9]+: warning:.*(truncat|may be truncated)' "$LOG" \
	| sed -E 's|^.*/(src/)|\1|' \
	| sort -u)

if [ -z "$sites" ]; then
	echo "no place in the desktop says a string may be cut"
	exit 0
fi

count=$(printf '%s\n' "$sites" | wc -l)

echo "$count place(s) where a release build says a string may be cut:"
printf '%s\n' "$sites" | sed 's/^/    /'
echo
echo "Each is one of two things. If the cut is meant, say so with"
echo "recon_text_copy or recon_text_printf -- that is what they are for."
echo "If it is a name something is looked up by -- a path, a key, a URL that"
echo "gets followed -- refuse it: a truncated name is not a shortened name"
echo "for the same thing, it is the name of a different one."
exit 1
