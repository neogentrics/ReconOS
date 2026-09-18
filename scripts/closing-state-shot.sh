#!/usr/bin/env bash
#
# What the task manager says between Close being pressed and an application
# going.
#
# --- The five seconds nothing was said in ---
#
# An application is only called *not responding* once it has been asked to
# close and has ignored the request for `RECON_APP_UNRESPONSIVE_MS` -- five
# seconds, long enough that a program saving a file is not accused of hanging.
# That number is right.
#
# What was wrong is what the list said in the meantime. Close was pressed, the
# request went, and the State column carried on saying **Running** -- identical
# to a moment before, and to every other application on the list. So the five
# seconds read as a button that had not worked, and the obvious thing to do
# about that is press it again.
#
# `recon_app_info` has carried `close_requested` all along, written by
# `recon_apps.c` and read by nothing. Found by
# `scripts/knows-and-does-not-do.py`, which asks which struct members are only
# ever assigned.
#
# Two pictures, in one run, because the interesting thing is the *change*:
#
#   closing.png         a second after End Task -- "Closing"
#   not-responding.png  seven seconds after -- "Not responding", and now the
#                       offer to force is available
#
#   ./scripts/closing-state-shot.sh [--out DIR]
#
# Wants ./build/ReconOS and ./build/decor_client. Pictures land in ./shots.

set -u

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

OUT_DIR="./shots"
if [ "${1:-}" = "--out" ]; then
    OUT_DIR="$2"
fi

for f in ./build/ReconOS ./build/decor_client; do
    [ -x "$f" ] || { echo "$f is not built"; exit 1; }
done

mkdir -p "$OUT_DIR"

# `--ignore-close` is what makes any of this visible: without a client that
# declines to go, every close is instant and there is no in-between to
# photograph.
#
# The coordinates were read off the running program rather than guessed:
# `ui hits` lists every clickable region of the focused window, and a probe run
# printed the two rows and the two buttons with their centres.
#
# `--pause 1` so the two captures are a known distance apart, and the seven
# `state` commands between them are how the five seconds are spent -- each
# costs a pause and says what the shell has open, which is harmless.
RECONOS_ALLOW_SPAWN=1 ./scripts/look.sh --fresh --pause 1 --out "$OUT_DIR" \
    "spawn ./build/decor_client --ignore-close --title Stubborn" \
    "apps Watchtower" \
    "ui click 500 245" \
    "ui click 723 545" \
    "capture closing.png" \
    "state" "state" "state" "state" "state" "state" "state" \
    "capture not-responding.png" 2>&1 | tail -8

echo
missing=0
for f in closing.png not-responding.png; do
    if [ -f "$OUT_DIR/$f" ]; then
        echo "wrote $OUT_DIR/$f"
    else
        echo "$f was not captured"
        missing=1
    fi
done
exit "$missing"
