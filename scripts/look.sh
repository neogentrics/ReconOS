#!/bin/bash
# Photograph the desktop, headless, from a script.
#
# Every "does it look right?" question in this project has been settled by a
# picture rather than by reasoning about the drawing code, and getting to a
# picture takes four steps that are not obvious. This is those four steps.
#
#   ./scripts/look.sh 'apps Calculator' 'capture calc.png'
#
# Each argument is one control-socket command, run in order with a pause
# between them. Captures land in the harness root and are copied to the
# directory named by --out (./shots by default).
#
# The two things that cost an afternoon to find out:
#
#   * The login screen swallows every pointer event. `apps <name>` will open a
#     window while it is up, and `state` will report that window as focused --
#     but clicking anything in it does nothing at all, because the click never
#     reaches it. Signing in is not optional even when all you want is a
#     picture of one window.
#
#   * `capture` resolves its path inside the ReconOS filesystem, not the host's.
#     `capture /tmp/x.png` reports success and writes to <root>/tmp/x.png, which
#     usually does not exist. Use a plain name.
#
# The harness signs in to a COPY of the filesystem with the account password
# removed, so it never needs one and never touches the real tree.
#
# Some things cannot be photographed that way. The keyring derives its key
# from an account password, so an account without one has no keyring, and
# every screen that offers to keep a secret correctly hides the offer --
# which means the harness could not photograph the offer at all.
# `--password X` sets a known password on every account in the copy instead
# of clearing it, and types it at the login screen. The real tree is still
# never touched: the password being set is the harness's, not anybody's.

set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REAL_ROOT="${RECONOS_ROOT:-$HOME/.reconos}"
LOOK_ROOT="$HOME/.reconos-look"
OUT_DIR="./shots"
PAUSE=0.6
PASSWORD=""

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT_DIR="$2"; shift 2 ;;
        --pause) PAUSE="$2"; shift 2 ;;
        --fresh) rm -rf "$LOOK_ROOT"; shift ;;
        --password) PASSWORD="$2"; shift 2 ;;
        *) break ;;
    esac
done

if [ $# -eq 0 ]; then
    echo "Usage: $0 [--out DIR] [--pause SECONDS] [--fresh]" \
        "[--password PASS] <command>..." >&2
    echo "Example: $0 'apps Calculator' 'capture calc.png'" >&2
    exit 2
fi

if [ ! -x "$REPO_DIR/build/ReconOS" ]; then
    echo "ReconOS is not built. Run scripts/build.sh first." >&2
    exit 1
fi

command -v socat >/dev/null || { echo "socat is not installed." >&2; exit 1; }

# --- A filesystem the harness may sign in to ---
#
# Copied rather than used in place, and the password cleared in the copy. The
# alternative is a harness that needs somebody's real password typed into it,
# which is a harness nobody runs.
# Rebuilt when the password being asked for is not the one the existing copy
# was built with -- and not otherwise. It used to rebuild on every run with
# --password, which is correct and useless: anything staged in the copy for the
# next run (a file on the desktop to right-click, a setting to photograph) was
# thrown away before it could be looked at.
STAMP="$LOOK_ROOT/.look-password"
if [ -d "$LOOK_ROOT" ]; then
    HAD="$(cat "$STAMP" 2>/dev/null || true)"
    if [ "$HAD" != "$PASSWORD" ]; then
        rm -rf "$LOOK_ROOT"
    fi
fi

if [ ! -d "$LOOK_ROOT" ]; then
    if [ ! -d "$REAL_ROOT" ]; then
        echo "No ReconOS filesystem at $REAL_ROOT to copy." >&2
        echo "Start ReconOS once by hand first." >&2
        exit 1
    fi
    cp -a "$REAL_ROOT" "$LOOK_ROOT"
    if [ -n "$PASSWORD" ]; then
        # Every account gets the same known password. PBKDF2-HMAC-SHA256
        # over a fresh 16-byte salt, 120000 rounds -- the same parameters
        # recon_users uses, restated here because this file is being
        # written from outside it. A mismatch shows up as a harness that
        # cannot sign in, which is loud enough.
        RECONOS_LOOK_PASSWORD="$PASSWORD" python3 - "$LOOK_ROOT/System/Config/users" <<'PYEOF'
import hashlib, os, sys
path = sys.argv[1]
password = os.environ["RECONOS_LOOK_PASSWORD"].encode()
out = []
for line in open(path, encoding="utf-8").read().splitlines():
    if not line or line.startswith("#") or line.count(":") < 4:
        out.append(line)
        continue
    name, role, _i, _s, _h = line.split(":", 4)
    salt = os.urandom(16)
    digest = hashlib.pbkdf2_hmac("sha256", password, salt, 120000, 32)
    out.append("%s:%s:120000:%s:%s" % (name, role, salt.hex(), digest.hex()))
open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
PYEOF
    else
        # name:role:iterations:salt:hash -- 0 iterations means no password.
        sed -i -E 's/^([^#][^:]*):([^:]*):[0-9]+:[^:]*:.*$/\1:\2:0::/' \
            "$LOOK_ROOT/System/Config/users"
    fi
    printf '%s' "$PASSWORD" > "$STAMP"
fi
rm -f "$LOOK_ROOT"/*.png

export RECONOS_ROOT="$LOOK_ROOT"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/recon_look_rt}"
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
export WLR_BACKENDS=headless
export WLR_RENDERER=pixman
export WLR_RENDERER_ALLOW_SOFTWARE=1

SOCKET=/tmp/reconos-look.sock
export RECONOS_CONTROL_SOCKET="$SOCKET"
rm -f "$SOCKET"

cd "$REPO_DIR"
./build/ReconOS >/tmp/reconos-look.log 2>&1 &
PID=$!
# Asked to stop and waited for, rather than killed and abandoned.
#
# It used to kill and exit immediately, which was fine while nothing happened
# at shutdown and stopped being fine the moment something did: ReconOS now
# writes a marker at startup and removes it at a clean stop, so a harness that
# does not wait leaves one behind and the next run reports a crash that was the
# harness. Two seconds is generous for a headless teardown; after that it is
# not stopping and gets killed anyway.
stop_reconos() {
    kill -TERM "$PID" 2>/dev/null || return 0
    for _ in $(seq 1 20); do
        kill -0 "$PID" 2>/dev/null || return 0
        sleep 0.1
    done
    kill -KILL "$PID" 2>/dev/null || true
}
trap stop_reconos EXIT

for _ in $(seq 1 100); do
    [ -S "$SOCKET" ] && break
    sleep 0.2
done
[ -S "$SOCKET" ] || { echo "ReconOS did not come up; see /tmp/reconos-look.log" >&2; exit 1; }

say() {
    printf '%s\n' "$*" | socat -t3 - "UNIX-CONNECT:$SOCKET" 2>/dev/null \
        | tail -n +2
}

# The boot sequence runs for a couple of seconds before the login screen will
# take input at all.
for _ in $(seq 1 40); do
    case "$(say session | head -1)" in *login*) break ;; esac
    sleep 0.25
done

# Pick the account, then Sign In. One click highlights and the second acts,
# system-wide, so the account needs the double click and the button does not.
say "ui dclick 640 353" >/dev/null; sleep 1.0
if [ -n "$PASSWORD" ]; then
    # The caret is already in the password field after choosing the
    # account, so this types into it rather than clicking it first --
    # one fewer coordinate to be wrong about when the screen changes.
    say "ui type $PASSWORD" >/dev/null; sleep 0.4
fi
say "ui click 640 505" >/dev/null; sleep 1.5

STAGE=$(say session | head -1)
case "$STAGE" in
    *done*) ;;
    *) echo "Did not reach the desktop: $STAGE" >&2; exit 1 ;;
esac

for cmd in "$@"; do
    say "$cmd"
    sleep "$PAUSE"
done

# A capture is finished on the next frame, not when the command returns.
sleep 1

mkdir -p "$OUT_DIR"
if compgen -G "$LOOK_ROOT/*.png" >/dev/null; then
    cp "$LOOK_ROOT"/*.png "$OUT_DIR"/
    ls -1 "$OUT_DIR"/*.png
fi
