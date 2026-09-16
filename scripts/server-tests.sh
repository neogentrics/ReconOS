#!/bin/sh
#
# Every server suite, built and run.
#
# --- Why this reads CMakeLists.txt instead of holding its own list ---
#
# The suites were run by hand for thirteen versions: a `gcc` line typed out per
# suite, per session. That works exactly as long as somebody remembers every
# suite, and the failure when they do not is silent -- the suites that were run
# pass, the report says the suites passed, and the one that was skipped is the
# one that would have failed.
#
# A list here would be a second list. `http_reason` and its suite each kept
# their own list of statuses once and the two drifted twice, which is why the
# status table is now generated from one X-macro. Same problem, same answer:
# the targets live in `CMakeLists.txt`, and this parses them out.
#
# So a suite added to the build is run by this the same day, and a suite added
# only here does not exist.
#
# --- What it does not do ---
#
# It does not replace `ctest`. Configuring the project builds the compositor
# and its wlroots dependencies, which is minutes of work to run a few hundred
# microseconds of checks -- so this compiles the suites directly and skips all
# of it. When the full build is being run anyway, `ctest -R server_` is the
# same checks through the real target.

set -eu

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
cd "$root"

out=${TMPDIR:-/tmp}/recon-server-tests.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT

CC=${CC:-gcc}
#
# `gnu11` because `CMakeLists.txt` sets `CMAKE_C_EXTENSIONS ON`, which is
# `-std=gnu11`. The first version of this script said `-std=c11` and two suites
# stopped building: strict ISO defines `__STRICT_ANSI__`, glibc hides `kill`
# behind `__USE_POSIX`, and the socket suites -- which fork a child and signal
# it -- got an implicit declaration each.
#
# The suites were right and the script was wrong. Matching the dialect the
# project actually builds with is the whole claim this script makes; a runner
# that compiles differently from the build is a runner that passes things the
# build would reject, and rejects things the build would pass.
#
# -Werror on purpose. A warning in a suite is a suite that is about to measure
# something other than what it says it measures.
CFLAGS=${CFLAGS:--std=gnu11 -Wall -Wextra -Werror -O1}

targets=$(awk '
	/^add_executable\(recon_server_/ { name = $0; sub(/^add_executable\(/, "", name); collecting = 1; srcs = ""; next }
	collecting {
		line = $0
		sub(/\)[[:space:]]*$/, "", line)
		gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
		if (line != "") srcs = srcs " " line
		if ($0 ~ /\)[[:space:]]*$/) { print name "|" srcs; collecting = 0 }
	}
' CMakeLists.txt)

if [ -z "$targets" ]; then
	echo "no server test targets found in CMakeLists.txt -- has the naming changed?" >&2
	exit 2
fi

total=0
failed=0
suites=0

printf '%s\n' "--- server suites ---"

for entry in $(printf '%s\n' "$targets" | tr ' ' '\001'); do
	entry=$(printf '%s' "$entry" | tr '\001' ' ')
	name=${entry%%|*}
	srcs=${entry#*|}

	suites=$((suites + 1))

	# shellcheck disable=SC2086
	if ! $CC $CFLAGS -o "$out/$name" $srcs 2>"$out/$name.cc"; then
		failed=$((failed + 1))
		printf '\n%s: DID NOT BUILD\n' "$name"
		cat "$out/$name.cc"
		continue
	fi

	# A suite that hangs is a real result and has happened here: the
	# streaming suite waited for ever against a build carrying the fault it
	# was written to catch. Bounded, so a hang is reported rather than sat
	# through.
	printf '\n'
	if timeout 120 "$out/$name" >"$out/$name.log" 2>&1; then
		:
	else
		rc=$?
		failed=$((failed + 1))
		if [ "$rc" -eq 124 ]; then
			printf '%s: TIMED OUT after 120s\n' "$name"
		fi
	fi
	cat "$out/$name.log"

	n=$(sed -n 's/^  \([0-9]*\) checks, .*/\1/p' "$out/$name.log" | tail -1)
	[ -n "$n" ] && total=$((total + n))
done

printf '\n--- %d suites, %d checks, %d suites failed ---\n' \
	"$suites" "$total" "$failed"

[ "$failed" -eq 0 ] || exit 1
