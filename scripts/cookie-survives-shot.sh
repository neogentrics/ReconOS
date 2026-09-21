#!/usr/bin/env bash
#
# Does a cookie survive the browser window closing?
#
# --- Why a live run and not only the suite ---
#
# v0.4.63 seals the cookie jar when a window closes and reads it back when one
# opens, and `tests/test_cookie.c` holds all of that: what is kept, what is
# refused, that the file is not a list of sessions. **None of it goes through
# the browser.** The suite calls `recon_cookie_jar_save` directly; the desktop
# calls it from a window's destructor, after the tabs have gone, with a keyring
# that may or may not be unlocked.
#
# Which is three things a suite cannot see: that the call happens at all, that
# it happens while the jar still has anything in it, and that the keyring is
# open at the moment it runs.
#
# One of those got smaller. `tests/test_appwin.c` (v0.4.76) holds the dispatch
# -- that a close fires `closed` and a minimize does not -- so what is left
# here is the wiring above it and the keyring below it, which is still more
# than enough to justify a live run.
#
# --- The password is not optional here ---
#
# The keyring derives its key from the account password, so an account without
# one has no keyring -- `scripts/look.sh` says so in its own header. With no
# password the jar cannot be sealed, and the browser says nothing, by design.
#
# So this passes `--password`, which is exactly what that flag was added for.
# A run without it would show a cookie not surviving and would be right.
#
#   ./scripts/cookie-survives-shot.sh [--out DIR]
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

PORT=8733
SERVER_PY=/tmp/recon-cookie-server.py

# A server that sets a cookie with an expiry, and another page that says what
# came back. `Max-Age` rather than a session cookie on purpose: a session
# cookie is *defined* as lasting until the browser closes, so one surviving
# this would be a fault rather than a pass. Both are set, so the picture shows
# which kind came back.
cat > "$SERVER_PY" <<'PY'
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        headers = []

        if self.path.startswith("/set"):
            headers.append(("Set-Cookie",
                            "keepme=survived; Path=/; Max-Age=86400"))
            headers.append(("Set-Cookie", "session-only=gone; Path=/"))
            body = ("<!doctype html><title>Signed in</title>"
                    "<h1>Signed in</h1>"
                    "<p>Two cookies set: one with a day to run, one for the "
                    "window only.</p>")
        else:
            got = self.headers.get("Cookie", "")
            body = ("<!doctype html><title>What came back</title>"
                    "<h1>What came back</h1>"
                    "<p>Cookie header: <b>%s</b></p>"
                    % (got if got else "(nothing)"))

        raw = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(raw)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *args):
        pass


HTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
PY

python3 "$SERVER_PY" "$PORT" >/tmp/recon-cookie-server.log 2>&1 &
SERVER=$!
trap 'kill "$SERVER" 2>/dev/null; rm -f "$SERVER_PY"' EXIT

for _ in $(seq 1 40); do
    if python3 - "$PORT" <<'PY' 2>/dev/null
import socket, sys
s = socket.socket()
s.settimeout(0.5)
s.connect(("127.0.0.1", int(sys.argv[1])))
PY
    then
        break
    fi
    sleep 0.25
done

mkdir -p "$OUT_DIR"

# One run: set the cookie, close the window, open a new one, ask what comes
# back. Closing is what writes the jar, so it has to be a real close.
#
# `--fresh` so the jar starts empty and a pass cannot be last run's cookie.
OUT="$(./scripts/look.sh --fresh --password reconos --pause 1 --out "$OUT_DIR" \
    "apps Web" \
    "ui click 640 120" \
    "ui type http://127.0.0.1:$PORT/set" \
    "ui key Return" \
    "capture cookie-set.png" \
    "ui click 1023 57" \
    "apps Web" \
    "ui click 640 120" \
    "ui type http://127.0.0.1:$PORT/show" \
    "ui key Return" \
    "capture cookie-after.png" 2>&1)"

printf '%s\n' "$OUT" | sed -n 's/^/  /p'
echo

# --- And the half the fix could have broken ---
#
# A close ends the session. A **minimize** must not: signing somebody out
# because they put the window down for a second would be a worse bug than the
# one this script found.
#
# The two are one callback apart. `visibility(false)` fires for both, so a
# browser that reached for it would pass the run above and fail this one --
# and nothing else would have said a word, because nothing else in the suite
# drives a window through minimize and back.
#
# So: the same two cookies, minimized instead of closed, restored from the
# taskbar, asked again. Both should come back, the session one included.
OUT2="$(./scripts/look.sh --fresh --password reconos --pause 1 --out "$OUT_DIR" \
    "apps Web" \
    "ui click 640 120" \
    "ui type http://127.0.0.1:$PORT/set" \
    "ui key Return" \
    "ui click 981 57" \
    "windows" \
    "ui click 168 702" \
    "ui click 640 120" \
    "ui type http://127.0.0.1:$PORT/show" \
    "ui key Return" \
    "capture cookie-minimized.png" 2>&1)"

printf '%s\n' "$OUT2" | sed -n 's/^/  /p'
echo

missing=0
for f in cookie-set.png cookie-after.png cookie-minimized.png; do
    if [ -f "$OUT_DIR/$f" ]; then
        echo "wrote $OUT_DIR/$f"
    else
        echo "$f was not captured"
        missing=1
    fi
done
exit "$missing"
