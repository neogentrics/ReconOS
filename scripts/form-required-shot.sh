#!/usr/bin/env bash
#
# A form that says what it needs before anything is sent.
#
# --- What this is for ---
#
# `required` was read out of every page and **nothing anywhere looked at it
# again**. That was not found by reading: it was found by listing every member
# of `struct recon_html_field` and asking which ones nothing outside the parser
# mentions. It was the only one with nobody.
#
# The check lives in `src/recon_web.c`, which no headless suite links -- it is
# the browser, and it wants a compositor and a window. So the instrument is the
# one this project already uses for that: a real desktop, a real page, and a
# look at it.
#
# Two pictures, because one proves half of it:
#
#   before.png  Send pressed with the required box empty. The form has not
#               gone, the status line names the box, and the caret is in it.
#   after.png   the same form with the box filled in, sent, and the server's
#               answer on screen -- so the refusal is a refusal and not a
#               form that never worked.
#
#   ./scripts/form-required-shot.sh [--out DIR]
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

PORT=8732
DOC=/tmp/recon-required-fixture

rm -rf "$DOC"
mkdir -p "$DOC"

# --- The fixture ---------------------------------------------------------
#
# A required text box, a required checkbox, and a required menu whose first
# option is the usual empty "choose one" -- three kinds that each mean
# something different by "answered", which is the part of this that could be
# quietly wrong.
#
# The optional box is there so the refusal has something to be distinguished
# from: a check that refused every form would pass a picture of one refusal.
cat > "$DOC/index.html" <<'HTML'
<!doctype html>
<title>Sign up</title>
<h1>Sign up</h1>
<form action="/thanks.html" method="get">
<p>Your name <input name="name" placeholder="Your name" required></p>
<p>A note (optional) <input name="note" placeholder="A note"></p>
<p>How did you hear?
<select name="how" required>
<option value="">Choose one</option>
<option value="friend">From a friend</option>
<option value="search">Searching</option>
</select></p>
<p><input type="checkbox" name="terms" required> I agree to the terms</p>
<p><input type="submit" value="Send"></p>
</form>
HTML

cat > "$DOC/thanks.html" <<'HTML'
<!doctype html>
<title>Thank you</title>
<h1>Thank you</h1>
<p>The form was sent.</p>
HTML

python3 -m http.server "$PORT" --directory "$DOC" >/tmp/recon-required-server.log 2>&1 &
SERVER=$!
trap 'kill "$SERVER" 2>/dev/null; rm -rf "$DOC"' EXIT

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

# One run, both pictures. Restarting between them would mean two sign-ins and
# two chances for a coordinate to be wrong; and the second picture is only
# worth anything if it is the same form as the first.
#
# The Send button is reached with Tab and Return rather than a click, because
# a click needs a coordinate and Tab needs only the order the page put things
# in -- which is the thing being photographed anyway.
./scripts/look.sh --out "$OUT_DIR" \
    "apps Web" \
    "ui click 640 120" \
    "ui type http://127.0.0.1:$PORT/" \
    "ui key Return" \
    "ui key Tab" "ui key Tab" "ui key Tab" "ui key Tab" "ui key Tab" \
    "ui key Return" \
    "capture required-before.png" \
    "ui type Joshua" \
    "ui key Tab" "ui key Tab" \
    "ui key Down" \
    "ui key Tab" \
    "ui key space" \
    "ui key Tab" \
    "ui key Return" \
    "capture required-after.png" \
    2>&1 | tail -6

echo
missing=0
for f in required-before.png required-after.png; do
    if [ -f "$OUT_DIR/$f" ]; then
        echo "wrote $OUT_DIR/$f"
    else
        echo "$f was not captured"
        missing=1
    fi
done
exit "$missing"
