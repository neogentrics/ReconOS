#!/bin/bash
# Launch ReconOS on real hardware (DRM backend).
#
# Run this from a bare TTY, not from inside an existing desktop session --
# the compositor needs to own the display.
#
# WLR_RENDERER_ALLOW_SOFTWARE is set because machines without a DRM render
# node (virtual machines especially) have no GPU acceleration available.
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

cd "$REPO_DIR"

sudo WLR_RENDERER_ALLOW_SOFTWARE=1 XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    ./build/ReconOS "$@" 2>&1 | tee "$LOG_FILE"

# tee runs as the invoking user, but the log may end up root-owned on some
# setups; make sure it stays readable.
sudo chown "$(id -u):$(id -g)" "$LOG_FILE" 2>/dev/null || true

echo
echo "Session log written to: $LOG_FILE"
