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
	#
	# The carriage return goes first, on every line.
	#
	# `CMakeLists.txt` is checked out with CRLF endings on this machine, so
	# without this every target name carries a trailing \r. That did not
	# matter while the name was only ever used as a filename -- it became
	# visible the day the name was also compared against the README, where
	# `server_http_tests\r` did not match the pattern that strips `_tests`
	# and the summary came out as two lines per suite.
	#
	# A latent fault that waited for a second reader. Stripped at the point
	# the file is read rather than at each use, so there is one place to be
	# right.
	#
	{ gsub(/\r/, "") }
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

#
# Before anything is built: are the string literals intact?
#
# The compiler finds this too, but only in files some suite happens to build,
# and only as `missing terminating " character` -- which has been mistaken for
# a typo three times and for a mangled patch script none. See the header of
# `check-c-literals.py`, including the part where the checker itself was broken
# by the very fault it exists to catch.
#
# It costs milliseconds and it runs first, because a broken literal makes every
# suite below it fail for a reason that has nothing to do with the code.
if command -v python3 >/dev/null 2>&1; then
	if ! python3 "$here/check-c-literals.py"; then
		echo "refusing to run the suites against sources that will not compile" >&2
		exit 2
	fi
else
	echo "literals: python3 not found, check skipped" >&2
fi

#
# And is every `struct http_site` built with a designated initializer?
#
# Not a style check. A site assembled field by field sets what somebody
# remembered and leaves the rest holding whatever the stack contained -- and
# the field this file's own `http_site` gained most recently is the *host name
# the server dispatches on*. The init program shipped that way for one version
# and answered every request correctly on the boot it was measured on, which is
# the worst way for a fault like this to behave. VF-029.
#
# Here rather than in a suite because no suite can see the init program's
# stack: this is a shape in the source, and the only thing that can check a
# shape is something that reads the source.
#
if command -v python3 >/dev/null 2>&1; then
	if ! python3 "$here/check-site-init.py"; then
		echo "refusing to run the suites against a site that will not be fully initialised" >&2
		exit 2
	fi
fi

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
	if [ -n "$n" ]; then
		total=$((total + n))
		# `recon_server_http_tests` is `server_http` to CMake's add_test
		# and to the README's table. One mechanical mapping rather than
		# a fourth list to keep.
		#
		# The name CMake registers the test under, which is what the
		# README's table uses. Looked up rather than derived: the first
		# version of this turned `recon_server_serve_tests` into
		# `server_serve` while the table -- correctly -- says
		# `server_http_serve`. An invented mapping is a fourth list
		# with extra steps.
		#
		short=$(awk -v t="$name" '
			{ gsub(/\r/, "") }
			$0 ~ ("^add_test\\(NAME .* COMMAND " t "\\)$") {
				n = $0
				sub(/^add_test\(NAME[[:space:]]+/, "", n)
				sub(/[[:space:]]+COMMAND.*$/, "", n)
				print n
			}
		' CMakeLists.txt)
		[ -n "$short" ] || short=$name
		printf '%s %s\n' "$short" "$n" >> "$out/summary"
	fi
done

printf '\n--- %d suites, %d checks, %d suites failed ---\n' \
	"$suites" "$total" "$failed"

#
# Does the README describe this run?
#
# Its suite table and its summary box are typed by hand, which makes them a
# third list beside `CMakeLists.txt` and the suites themselves. On 17 September
# the table summed to 735 against a real 736, because one row had not been
# updated when a suite grew -- found by adding it up, which is not a method.
# See the header of `check-readme-suites.py`.
#
# Only checked when everything ran. Comparing a document against a partial run
# would report the gap as a documentation fault, which it would not be.
if [ "$failed" -eq 0 ] && [ -f "$out/summary" ] \
   && command -v python3 >/dev/null 2>&1; then
	python3 "$here/check-readme-suites.py" "$out/summary" || failed=1
fi

[ "$failed" -eq 0 ] || exit 1
