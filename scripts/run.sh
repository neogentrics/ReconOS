#!/bin/bash
# Launch ReconOS on real hardware (DRM backend).
#
# Run this from a bare TTY, not from inside an existing desktop session --
# the compositor needs to own the display.
#
# WLR_RENDERER_ALLOW_SOFTWARE is set because machines without a DRM render
# node (virtual machines especially) have no GPU acceleration available.

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/recon_runtime}"

mkdir -p "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

if [ ! -x "$REPO_DIR/build/ReconOS" ]; then
    echo "ReconOS is not built. Run scripts/build.sh first." >&2
    exit 1
fi

cd "$REPO_DIR"
exec sudo WLR_RENDERER_ALLOW_SOFTWARE=1 XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    ./build/ReconOS "$@"
