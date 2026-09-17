#!/usr/bin/env bash
#
# Does "Show Desktop" minimize every client window, or only one?
#
# --- Why this is a script and not an argument ---
#
# **BG-210.** `recon_toplevel_minimize` moves a window to the *back* of the
# compositor's list and gives the focus to the next one still visible, and the
# taskbar's "Show Desktop" walked that list with `wl_list_for_each` -- which
# reads each window's successor *after* the body has run. So the walk followed
# what the body had just done to the ordering, read the list head, and stopped.
#
# That is a claim about the semantics of somebody else's linked list, reached
# by reading. This project's rule is that a disagreement is a result, so it is
# settled by starting a desktop, giving it two real client windows, choosing
# the menu entry, and asking what happened.
#
# The built-in windows in the same handler are minimized by a plain indexed
# loop over an array and were never affected, which is the nastiest part of the
# shape: from the outside it looks as though *client windows* are the thing
# that does not minimize.
#
# --- Two clients, not one ---
#
# One cannot show the difference. The fault stops the walk after the first
# window it acts on, so a single client minimizes correctly either way -- which
# is presumably how this survived from whenever the taskbar menu was written.
#
# --- On standing on look.sh ---
#
# The first version of this started ReconOS itself, and got as far as
# rediscovering what `scripts/look.sh` says in its own header: the login screen
# swallows every pointer event, and signing in is not optional even when all
# you want is one menu. look.sh already does that, already keeps a filesystem
# it is allowed to sign in to, and already takes control commands as arguments.
# A second harness would have been a second thing to keep working.
#
#   ./scripts/show-desktop-check.sh
#
# Wants ./build/ReconOS and ./build/decor_client.

set -u

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

for f in ./build/ReconOS ./build/decor_client; do
    [ -x "$f" ] || { echo "$f is not built"; exit 1; }
done

# `spawn` runs a program on the machine underneath, which is not something
# ReconOS does -- it exists for exactly this and refuses unless it is asked.
#
# --fresh because a run that crashed leaves a marker behind, and the next start
# reports an unfinished session rather than signing in. Which is the marker
# doing its job; it just means this cannot reuse the tree.
OUT="$(RECONOS_ALLOW_SPAWN=1 ./scripts/look.sh --fresh \
    "spawn ./build/decor_client --title One" \
    "spawn ./build/decor_client --title Two" \
    "windows" \
    "ui rclick 700 700" \
    "ui menu Show Desktop" \
    "windows" 2>&1)"

printf '%s\n' "$OUT" | sed -n 's/^/  /p'
echo

# A click that missed says so, and that is a different failure from the one
# being looked for. Reported rather than quietly counted as a pass.
case "$OUT" in
    *"No menu entry"*)
        echo "FAIL: the menu entry was not showing, so nothing was tested"
        exit 1
        ;;
esac

if ! printf '%s\n' "$OUT" | grep -q 'One  *running .*client'; then
    echo "FAIL: wanted two client windows running before anything was chosen"
    exit 1
fi

# Everything from the menu being chosen onwards, which is the second `windows`.
AFTER="$(printf '%s\n' "$OUT" | awk '/chose .Show Desktop./{seen=1} seen')"
LEFT="$(printf '%s\n' "$AFTER" | grep -c 'running .*client' || true)"

if [ "$LEFT" -eq 0 ]; then
    echo "every client window minimized"
    exit 0
fi

echo "$LEFT client window(s) still running after Show Desktop -- BG-210"
exit 1
