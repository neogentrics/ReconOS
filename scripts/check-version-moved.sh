#!/usr/bin/env bash
#
# Does this tree's version still name this tree?
#
# **The fault:** a branch takes work -- usually by merge -- and leaves `VERSION`
# where it was. The number then names a tree that no longer exists.
#
# **The first version of this file claimed it would have caught KF-259 and
# KF-260. It would not have**, and the claim was checked before it shipped
# rather than after. Those two touched `scripts/` only, and `kernel/` is
# byte-identical across all four commits from the 0.5.0 merge to 3ff2d99 --
# subtree 64af7e42 throughout. A check comparing `kernel/` alone would have said
# the version still named the tree, correctly, and missed both.
#
# So the compared set is `kernel/`, `boot/` and `scripts/`, which is this
# project's own ruling about what earns a patch rather than a guess:
#
#   kernel/, boot/   the binary. Obvious.
#   scripts/         KF-200 settled this and cost 0.2.2 for a scripts/ fix --
#                    "the change does not feel large enough is the reasoning
#                    the rule exists to rule out". A script is a thing that
#                    runs and can be wrong.
#   docs/, README    NOT included. NW-015 declined a bump for a document
#                    correction on the grounds that the binary was identical
#                    and a number would imply a difference that is not there.
#                    Including them would make this cry wolf on every typo.
#
# Measured on 20 September 2026 across every branch head: five version numbers
# named more than one kernel, on four branches. Nobody did anything wrong --
# every session numbers independently and merges later, which is the right
# arrangement and exactly what makes this reachable, because nothing inside one
# branch can see another's number.
#
# **Why a check and not a comment.** `kernel/Makefile` already carries the rule
# beside the line, at length, and it works -- the network session bumped twice
# and declined once tonight on the strength of it. It works because editing
# VERSION is a thing somebody does deliberately. This fault arrives in a merge
# resolution, where nobody stops to decide a version; they take what resolved.
# A comment prevents where the reader must stop and choose. This is not that.
#
# --- what it compares, and why not history -------------------------------
#
# The newest commit that CHANGED the version line, against HEAD. If `kernel/`
# differs between them, the line has not moved since the tree did.
#
# **`--diff-merges=first-parent` is load-bearing.** `git log -p` shows no diff
# for a merge commit unless asked, so a version set inside a merge is invisible
# to the obvious form of this question -- which is NW-017, found by the network
# session when their own checker reported a released version as never set.
#
# Committed state only. A working tree mid-change legitimately has `kernel/`
# edits before the bump, and failing on that would make this cry wolf on every
# session that builds before it numbers.
#
# Exits 2 rather than 0 when it cannot answer. A shallow clone has no history
# to walk, and "the version names this tree" is the one answer it must not give
# then -- the same reason the network session's uniqueness check exits 2 with
# no refs fetched, and the same reason verify-kernel.sh refuses rather than
# deleting a stray signing key.
set -u

cd "$(dirname "$0")/.."

MAKEFILE=kernel/Makefile

git rev-parse --git-dir >/dev/null 2>&1 || {
	echo "  not a git repository; cannot say whether the version moved"
	exit 2
}

[ -f "$MAKEFILE" ] || { echo "  no $MAKEFILE"; exit 2; }

now=$(git show "HEAD:$MAKEFILE" 2>/dev/null | grep -m1 '^VERSION' | awk '{print $3}')
[ -n "$now" ] || { echo "  no VERSION line at HEAD; cannot answer"; exit 2; }

# The newest commit whose VERSION differs from its first parent's. Walked
# rather than asked for with -S, because -S counts occurrences of a string and
# a version that changes and changes back would read as never having moved.
setter=
for c in $(git rev-list --first-parent HEAD 2>/dev/null | head -400); do
	v=$(git show "$c:$MAKEFILE" 2>/dev/null | grep -m1 '^VERSION' | awk '{print $3}')
	p=$(git show "$c^:$MAKEFILE" 2>/dev/null | grep -m1 '^VERSION' | awk '{print $3}')

	# No parent: the first commit in a shallow clone, or the root. Either
	# way there is nothing to compare against and guessing is the failure
	# this whole file is about.
	[ -n "$p" ] || break

	if [ "$v" != "$p" ]; then
		setter=$c
		break
	fi
done

if [ -z "$setter" ]; then
	echo "  could not find the commit that set $now within 400 commits;"
	echo "  a shallow clone cannot answer this. Not treating that as a pass."
	exit 2
fi

WATCHED="kernel/ boot/ scripts/"

# shellcheck disable=SC2086
if git diff --quiet "$setter" HEAD -- $WATCHED 2>/dev/null; then
	echo "  kernel $now names this tree ($(git rev-parse --short "$setter"))"
	exit 0
fi

echo "  The tree has changed since $now was set, and the line has not moved."
echo
echo "    $now set at $(git rev-parse --short "$setter")"
echo "    HEAD          $(git rev-parse --short HEAD)"
echo
echo "  So $now names a tree this one is not, and any entry saying"
echo "  \"fixed in kernel $now\" is about a binary that may not contain it."
echo "  This is KF-259 and KF-260's shape. Move the line, or say here why not."
echo
echo "  What changed under $WATCHED since then:"
# shellcheck disable=SC2086
git diff --stat "$setter" HEAD -- $WATCHED | tail -12 | sed 's/^/    /'
exit 1
