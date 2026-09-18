#!/usr/bin/env bash
#
# Does closing the Media Player stop the music?
#
# --- Why this is a live run and not a suite ---
#
# What `stop_playing` does is testable in a suite; **whether anything calls it
# when somebody closes the window** is not. That call is made by the window
# seam, from a click on a title bar, through an application's `closed` hook --
# none of which a headless suite links. The same hole the browser's jar was
# sitting in, in a different application.
#
# --- Both halves, and the second is the one worth having ---
#
# Closing must stop it. **Minimising must not.** Those are one callback apart:
# `visibility(false)` fires for both, so a player that reached for it would
# pass the first run here and stop the music every time the window was put
# down -- which is the most ordinary thing to do while listening to something.
#
# So this drives a real window through both, and asks `windows` in between so
# neither run can pass by not having happened.
#
# --- A track long enough to outlive the run ---
#
# The first version of this used a twelve-second tone and the run took
# eighteen, so the track ended on its own and the player showed "Stopped." for
# reasons that had nothing to do with the close. **It proved nothing and
# looked like a pass.** Ten minutes is what makes the question answerable.
#
#   ./scripts/player-close-shot.sh [--out DIR]
#
# Wants ./build/ReconOS and python3. Pictures land in ./shots.

set -u

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

OUT_DIR="./shots"
if [ "${1:-}" = "--out" ]; then
    OUT_DIR="$2"
fi

[ -x ./build/ReconOS ] || { echo "./build/ReconOS is not built"; exit 1; }

LOOK_ROOT="$HOME/.reconos-look"

# --- Why there is a run here that does nothing ---
#
# `look.sh` throws the look root away when its password stamp does not match
# the password it was given, and writes the stamp on the way back. So a file
# placed in that root **before the first run is deleted by that run**.
#
# The first version of this script did exactly that, and it did not fail: it
# reported three pictures written and the pictures said *"Nothing to play. Put
# a .wav, .mp3 or .mp4 in your Music folder."* A harness that reports success
# while photographing an empty player is the same shape as the twelve-second
# tone above -- a run that measured nothing and looked like a pass.
#
# So the root is settled first, with a command that changes nothing, and the
# tone goes in afterwards.
echo "--- settling the look root, so what goes into it survives"
./scripts/look.sh --password reconos --pause 0 --out "$OUT_DIR" \
    "windows" >/dev/null 2>&1

[ -d "$LOOK_ROOT" ] || { echo "look.sh made no root at $LOOK_ROOT"; exit 1; }

TONE="$LOOK_ROOT/.tone.wav"

# Ten minutes of a quiet 220 Hz tone, written from the specification rather
# than shipped as a fixture -- a binary in the tree that nothing can regenerate
# is a fixture nobody can check.
python3 - "$TONE" <<'PY'
import math
import struct
import sys

RATE = 8000
SECONDS = 600

frames = bytearray()
for t in range(RATE * SECONDS):
    frames += struct.pack("<h", int(6000 * math.sin(2 * math.pi * 220 * t / RATE)))

header = (b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVEfmt " +
          struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16) +
          b"data" + struct.pack("<I", len(frames)))

with open(sys.argv[1], "wb") as out:
    out.write(header + bytes(frames))
PY

# Into every account's Music folder, because which one the harness signs into
# is its business rather than this script's.
for user in "$LOOK_ROOT"/Users/*/; do
    [ -d "$user" ] || continue
    mkdir -p "$user/Music"
    cp "$TONE" "$user/Music/tone.wav"
done
rm -f "$TONE"

placed="$(ls "$LOOK_ROOT"/Users/*/Music/tone.wav 2>/dev/null | wc -l)"
if [ "$placed" -eq 0 ]; then
    echo "the tone did not survive into the look root" >&2
    exit 1
fi
echo "    tone.wav in $placed of the accounts"
echo

mkdir -p "$OUT_DIR"

# Controls, read off a running window with `ui hits` rather than guessed:
# play is the second transport button, and the title bar's three are minimize,
# maximize and close from the left.
PLAY="ui click 358 514"
MINIMIZE="ui click 911 101"
CLOSE="ui click 953 101"
TASKBAR="ui click 168 702"

echo "--- closed, then opened again: the music should have stopped"
./scripts/look.sh --password reconos --pause 2 --out "$OUT_DIR" \
    "apps Media Player" \
    "$PLAY" \
    "capture player-playing.png" \
    "$CLOSE" \
    "windows" \
    "apps Media Player" \
    "capture player-after-close.png" 2>&1 | sed -n 's/^/  /p'
echo

echo "--- minimized, then restored: it should still be playing"
./scripts/look.sh --password reconos --pause 2 --out "$OUT_DIR" \
    "apps Media Player" \
    "$PLAY" \
    "$MINIMIZE" \
    "windows" \
    "$TASKBAR" \
    "capture player-after-minimize.png" 2>&1 | sed -n 's/^/  /p'
echo

missing=0
for f in player-playing.png player-after-close.png player-after-minimize.png; do
    if [ -f "$OUT_DIR/$f" ]; then
        echo "wrote $OUT_DIR/$f"
    else
        echo "$f was not captured"
        missing=1
    fi
done
exit "$missing"
