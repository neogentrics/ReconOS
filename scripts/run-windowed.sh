#!/bin/bash
# Run ReconOS in a window, inside an existing desktop.
#
# scripts/run.sh takes over a whole screen, which means a bare TTY or a virtual
# machine, and a rebuild-and-look cycle that costs a reboot. This runs the same
# binary as a nested compositor: wlroots opens a window on whatever desktop is
# already there and puts ReconOS inside it. Everything works -- the taskbar,
# the menus, dragging windows -- it is simply in a box.
#
# Two hosts this is known to work on:
#
#   A Linux desktop running Wayland or X11. Nothing to set up.
#
#   Windows, through WSL. WSLg is a Wayland compositor, so a Linux program that
#   asks for a window gets one on the Windows desktop, in the taskbar, next to
#   everything else. No virtual machine, no reboot, and the build is already
#   here.
#
# Software rendering is forced on, because neither host is guaranteed to hand a
# GPU render node through, and falling back is better than refusing to start.

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_FILE="${RECONOS_LOG:-$HOME/reconos-last.log}"

if [ ! -x "$REPO_DIR/build/ReconOS" ]; then
    echo "ReconOS is not built. Run scripts/build.sh first." >&2
    exit 1
fi

if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
    echo "No desktop to open a window on: neither WAYLAND_DISPLAY nor DISPLAY" >&2
    echo "is set. Use scripts/run.sh from a TTY instead." >&2
    exit 1
fi

# The host's runtime directory, deliberately.
#
# A directory of our own looked tidier and did not work: the nested backend
# connects to the host compositor through the socket in XDG_RUNTIME_DIR, so
# pointing it somewhere else means there is no display to open a window on.
# Nothing collides -- ReconOS takes the next free wayland-N in there, and its
# control socket is an absolute path.
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/recon-windowed}"
mkdir -p "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR" 2>/dev/null || true

export WLR_RENDERER_ALLOW_SOFTWARE=1

# How big the window opens. wlroots reads this for its nested backends.
export WLR_WL_OUTPUTS="${WLR_WL_OUTPUTS:-1}"
export WLR_X11_OUTPUTS="${WLR_X11_OUTPUTS:-1}"

cd "$REPO_DIR"

RECON_EXIT_RESTART=42

while true; do
    XDG_RUNTIME_DIR="$RUNTIME_DIR" ./build/ReconOS "$@" 2>&1 | tee "$LOG_FILE"
    STATUS=${PIPESTATUS[0]}

    [ "$STATUS" = "$RECON_EXIT_RESTART" ] || break
    echo
    echo "Restarting ReconOS..."
    sleep 1
done

echo
echo "Session log written to: $LOG_FILE"
