#!/bin/bash
# Launch ReconOS on real hardware (DRM backend).
#
# Run this from a bare TTY, not from inside an existing desktop session --
# the compositor needs to own the display.
#
# Any WLR_* variables set in the environment are forwarded through sudo, which
# would otherwise strip them. That makes wlroots' debugging knobs usable:
#
#   WLR_SCENE_DEBUG_DAMAGE=rerender ./scripts/run.sh   full redraw every frame
#   WLR_DRM_NO_ATOMIC=1 ./scripts/run.sh               legacy modesetting
#   WLR_NO_HARDWARE_CURSORS=1 ./scripts/run.sh         software cursor
#
# Output is copied to a log file so a session can be examined after the
# compositor has taken over and released the screen.

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/recon_runtime}"
LOG_FILE="${RECONOS_LOG:-$HOME/reconos-last.log}"

mkdir -p "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

if [ ! -x "$REPO_DIR/build/ReconOS" ]; then
    echo "ReconOS is not built. Run scripts/build.sh first." >&2
    exit 1
fi

# Collect WLR_* and RECONOS_* settings to hand to the privileged process.
declare -a PASS_ENV=()
while IFS= read -r assignment; do
    case "$assignment" in
        WLR_*|RECONOS_TERMINAL=*|RECONOS_ASSETS=*) PASS_ENV+=("$assignment") ;;
    esac
done < <(env)

# Software rendering is required on machines with no DRM render node, but let
# an explicit setting win.
if [ -z "${WLR_RENDERER_ALLOW_SOFTWARE:-}" ]; then
    PASS_ENV+=("WLR_RENDERER_ALLOW_SOFTWARE=1")
fi

if [ ${#PASS_ENV[@]} -gt 0 ]; then
    printf 'Passing to compositor: %s\n' "${PASS_ENV[*]}"
fi

cd "$REPO_DIR"

sudo "${PASS_ENV[@]}" XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    ./build/ReconOS "$@" 2>&1 | tee "$LOG_FILE"

sudo chown "$(id -u):$(id -g)" "$LOG_FILE" 2>/dev/null || true

echo
echo "Session log written to: $LOG_FILE"
