#!/bin/bash
# Install ReconOS onto this machine.
#
#   sudo ./install.sh              install, do not change what boots
#   sudo ./install.sh --boot-into  also start ReconOS at boot
#   sudo ./install.sh --uninstall  remove it again
#
# ReconOS is installed under /opt/reconos, its filesystem lives at /recon, and
# a launcher is put on the path as `reconos`. Nothing outside those is touched
# unless --boot-into is given, and that is reversible.

set -e

PREFIX="/opt/reconos"
FS_ROOT="/recon"
LAUNCHER="/usr/local/bin/reconos"
SERVICE="/etc/systemd/system/reconos.service"

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$(id -u)" != "0" ]; then
    echo "This needs to run as root: sudo ./install.sh" >&2
    exit 1
fi

# --- Uninstall ---

if [ "$1" = "--uninstall" ]; then
    echo "Removing ReconOS."

    if [ -f "$SERVICE" ]; then
        systemctl disable --now reconos.service 2>/dev/null || true
        rm -f "$SERVICE"
        systemctl daemon-reload
    fi

    rm -f "$LAUNCHER"
    rm -rf "$PREFIX"

    # The filesystem is left alone deliberately: it holds the user's files, and
    # uninstalling a program is not a request to delete their documents.
    if [ -d "$FS_ROOT" ]; then
        echo
        echo "Left $FS_ROOT in place; it holds your files."
        echo "Remove it yourself if you want it gone: sudo rm -rf $FS_ROOT"
    fi

    echo "Done."
    exit 0
fi

# --- Check what is needed before changing anything ---

MISSING=""
for library in libwlroots wayland-server xkbcommon pixman-1; do
    if ! pkg-config --exists "$library" 2>/dev/null && \
       ! ldconfig -p 2>/dev/null | grep -q "${library%%-*}"; then
        MISSING="$MISSING $library"
    fi
done

if [ -n "$MISSING" ]; then
    echo "Warning: these may be missing:$MISSING"
    echo "On Debian or Ubuntu:"
    echo "  sudo apt install libwlroots-dev libwayland-server0 libxkbcommon0 libpixman-1-0"
    echo
fi

if [ ! -x "$SOURCE_DIR/bin/ReconOS" ]; then
    echo "bin/ReconOS is missing from this package." >&2
    exit 1
fi

# --- Install ---

echo "Installing to $PREFIX"

mkdir -p "$PREFIX/bin" "$PREFIX/share"
install -m 755 "$SOURCE_DIR/bin/ReconOS" "$PREFIX/bin/ReconOS"

if [ -d "$SOURCE_DIR/share/assets" ]; then
    rm -rf "$PREFIX/share/assets"
    cp -r "$SOURCE_DIR/share/assets" "$PREFIX/share/"
fi

for doc in README.md LICENSE.txt THIRD_PARTY.md BUILD-INFO.txt; do
    [ -f "$SOURCE_DIR/$doc" ] && install -m 644 "$SOURCE_DIR/$doc" "$PREFIX/"
done

# The ReconOS filesystem. Group-writable so the account that runs the desktop
# can use it without being root.
mkdir -p "$FS_ROOT"
chmod 775 "$FS_ROOT"

# Whoever invoked sudo is the person who will be using this.
REAL_USER="${SUDO_USER:-root}"
if [ "$REAL_USER" != "root" ]; then
    chown "$REAL_USER" "$FS_ROOT"
fi

cat > "$LAUNCHER" <<'LAUNCHER_EOF'
#!/bin/bash
# Start ReconOS. Run this from a bare TTY, not from inside another desktop --
# the compositor needs to own the display.

PREFIX="/opt/reconos"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/reconos-runtime-$(id -u)}"

mkdir -p "$RUNTIME_DIR"
chmod 700 "$RUNTIME_DIR"

export XDG_RUNTIME_DIR="$RUNTIME_DIR"
export RECONOS_ASSETS="${RECONOS_ASSETS:-$PREFIX/share/assets}"

# Machines without a GPU render node -- most virtual ones -- need this, and it
# costs nothing where there is one.
export WLR_RENDERER_ALLOW_SOFTWARE="${WLR_RENDERER_ALLOW_SOFTWARE:-1}"

exec "$PREFIX/bin/ReconOS" "$@"
LAUNCHER_EOF

chmod 755 "$LAUNCHER"

echo "Installed."
echo

# --- Optionally start at boot ---

if [ "$1" = "--boot-into" ]; then
    if [ "$REAL_USER" = "root" ]; then
        echo "Refusing to boot into ReconOS as root." >&2
        echo "Run this with sudo from the account that should own the desktop." >&2
        exit 1
    fi

    echo "Setting ReconOS to start at boot as $REAL_USER."

    cat > "$SERVICE" <<SERVICE_EOF
[Unit]
Description=ReconOS desktop
# Start once the machine is otherwise up, so the graphics driver and seat are
# ready before the compositor tries to take the display.
After=systemd-user-sessions.service plymouth-quit-wait.service
Conflicts=getty@tty1.service
After=getty@tty1.service

[Service]
Type=simple
User=$REAL_USER
PAMName=login
TTYPath=/dev/tty1
TTYReset=yes
TTYVHangup=yes
StandardInput=tty
StandardOutput=journal
StandardError=journal
Environment=XDG_RUNTIME_DIR=/run/user/$(id -u "$REAL_USER")
ExecStart=$LAUNCHER
# A compositor that has crashed leaves a machine with no way in. Coming back
# gives the user a desktop again rather than a black screen.
#
# 42 is what ReconOS exits with when somebody chose Restart, so that counts as
# a reason to start again rather than as a failure to report.
Restart=on-failure
RestartForceExitStatus=42
SuccessExitStatus=42
RestartSec=2

[Install]
WantedBy=multi-user.target
SERVICE_EOF

    systemctl daemon-reload
    systemctl enable reconos.service

    echo
    echo "ReconOS will start on tty1 at the next boot."
    echo "To stop that:  sudo systemctl disable reconos.service"
    echo "If it will not start, switch to another console with Ctrl+Alt+F2"
    echo "and read the log with: journalctl -u reconos"
else
    echo "Start it from a bare TTY with:  reconos"
    echo "To start it at boot instead:    sudo ./install.sh --boot-into"
fi
