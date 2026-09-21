#!/bin/bash
#
# One number, one kernel. Checked across every branch, not just this one.
#
# --- Why ---------------------------------------------------------------------
#
# The kernel session put the reason better than the register had: a version can
# be disambiguated in an entry and cannot be disambiguated on a machine. The
# binary prints
#
#     ReconOS kernel 0.5.0
#
# on its first line, and two people holding two machines that print the same
# string should not be holding different kernels. A branch qualifier is
# something a reader of `docs/BUGS.md` gets and a person in front of a laptop
# does not.
#
# Every session numbers independently and merges later, which is the right
# arrangement and is exactly what makes this reachable: two branches can pick
# the same next number without either being able to see the other.
#
# --- What it does and does not check -----------------------------------------
#
# It compares **branch heads as they stand now**, which is the question somebody
# holding a build can act on.
#
# It deliberately does *not* flag two commits in history that once set the same
# number on branches that later merged. Those were siblings at the time and the
# merge settled them; treating history as live produces noise, and a check that
# is mostly noise is a check people learn to skip. An earlier draft of this did
# exactly that and reported six collisions, of which the honest answer was that
# the question had been asked wrong.
#
# It compares the **`kernel/` subtree**, not the whole repository. Two branches
# holding the same kernel and different documents are not shipping two kernels,
# and saying they are would make the check cry wolf on every branch that edits a
# README.
#
#   scripts/check-version-unique.sh              the remote branches
#   scripts/check-version-unique.sh --local      local ones too
#
# Exits 0 when every version names one kernel, 1 when one names more than one,
# and 2 when it cannot tell -- no refs, no git, a fetch that never happened.

set -u
cd "$(dirname "$0")/.."

command -v git >/dev/null || { echo "no git"; exit 2; }

scope=refs/remotes/origin
[ "${1:-}" = "--local" ] && scope="refs/remotes/origin refs/heads"

# shellcheck disable=SC2086
refs=$(git for-each-ref --format='%(refname:short)' $scope 2>/dev/null | grep -v 'HEAD$')

if [ -z "$refs" ]; then
	echo "  no branches to compare -- has this clone ever fetched?"
	echo "  saying nothing rather than saying every version is unique."
	exit 2
fi

rows=""
for b in $refs; do
	v=$(git show "$b:kernel/Makefile" 2>/dev/null |
	    sed -n 's/^VERSION := \([0-9.]*\).*/\1/p' | head -1)
	k=$(git rev-parse "$b:kernel" 2>/dev/null)
	[ -n "$v" ] && [ -n "$k" ] && rows="$rows$v $k $b"$'\n'
done

[ -n "$rows" ] || { echo "  no branch carried a readable VERSION"; exit 2; }

bad=0
for v in $(printf '%s' "$rows" | awk '{print $1}' | sort -u); do
	kernels=$(printf '%s' "$rows" | awk -v v="$v" '$1==v {print $2}' | sort -u | wc -l)
	if [ "$kernels" -gt 1 ]; then
		bad=$((bad + 1))
		echo "  $v names $kernels different kernels:"
		printf '%s' "$rows" | awk -v v="$v" '$1==v {printf "      %-22s kernel/ %s\n", $3, substr($2,1,8)}'
	fi
done

total=$(printf '%s' "$rows" | awk '{print $1}' | sort -u | wc -l)

if [ "$bad" -eq 0 ]; then
	echo "  $total version(s) across $(printf '%s' "$rows" | wc -l) branches, each naming one kernel"
	exit 0
fi

echo "  $bad of $total version(s) name more than one kernel."
echo "  Two branches picked the same next number without being able to see"
echo "  each other. Whoever merges owns the renumber; the precedent is to lay"
echo "  the sequences end to end rather than interleave them."
exit 1
