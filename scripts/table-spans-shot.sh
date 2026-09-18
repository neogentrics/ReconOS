#!/usr/bin/env bash
#
# Photograph a table whose cells cover rows and columns.
#
# --- Why a photograph and not only the suite ---
#
# `tests/test_html.c` holds the *parser* to its answer: which column each cell
# is in, with 237 checks and eight mutations that all bite. That is where being
# wrong is quiet, and it is most of the value.
#
# What it cannot see is the drawing. Two passes in `src/recon_web.c` walk a
# table -- one to measure the columns, one to draw -- and both used to work out
# which column they were in by counting cells. Both now read the column the
# parser resolved, and **a suite over the parser would stay green if either of
# them had been left counting.** So: a real compositor, a real page, and a look
# at it.
#
# The fixture is served over HTTP because that is the only way this browser
# reaches a page -- `src/recon_url.c` knows http and https and nothing else,
# deliberately, and a `file:` scheme is not a thing this viewer has.
#
#   ./scripts/table-spans-shot.sh [--out DIR]
#
# Wants ./build/ReconOS and python3. The picture lands in ./shots.

set -u

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_DIR"

OUT_DIR="./shots"
if [ "${1:-}" = "--out" ]; then
    OUT_DIR="$2"
fi

[ -x ./build/ReconOS ] || { echo "./build/ReconOS is not built"; exit 1; }

PORT=8731
DOC=/tmp/recon-span-fixture

rm -rf "$DOC"
mkdir -p "$DOC"

# --- The fixture ---------------------------------------------------------
#
# Three tables, each a shape that is wrong in a *visible* way when spans are
# mishandled, rather than three variations on one shape.
#
#   1. rowspan alone. The second row has two cells where the others have
#      three, and they must sit under the second and third headings. A viewer
#      that counts cells puts them under the first and second.
#
#   2. rowspan and colspan on the same cell. The row beneath has to step over
#      *both* of its columns.
#
#   3. a span that runs over a column something is already in -- the case the
#      mutation pass found unverified.
cat > "$DOC/index.html" <<'HTML'
<!doctype html>
<title>Spans</title>
<h1>A cell that covers rows</h1>
<table>
<tr><th>Month</th><th>Region</th><th>Sales</th></tr>
<tr><td rowspan="2">January</td><td>North</td><td>1,040</td></tr>
<tr><td>South</td><td>2,310</td></tr>
<tr><td rowspan="2">February</td><td>North</td><td>1,180</td></tr>
<tr><td>South</td><td>2,004</td></tr>
</table>

<h1>Rows and columns at once</h1>
<table>
<tr><th colspan="2" rowspan="2">Quarter and year</th><th>Target</th></tr>
<tr><th>Actual</th></tr>
<tr><td>Q1</td><td>2026</td><td>12,000</td></tr>
<tr><td>Q2</td><td>2026</td><td>13,500</td></tr>
</table>

<h1>A span crossing a column that is taken</h1>
<table>
<tr><td>A</td><td rowspan="3">B, three rows deep</td><td>C</td></tr>
<tr><td colspan="3">D, three columns wide</td></tr>
<tr><td>E</td><td>F</td></tr>
</table>
HTML

python3 -m http.server "$PORT" --directory "$DOC" >/tmp/recon-span-server.log 2>&1 &
SERVER=$!
trap 'kill "$SERVER" 2>/dev/null; rm -rf "$DOC"' EXIT

# The server needs a moment, and a browser that reaches a refused connection
# reports that instead of the page -- which is a different picture from the one
# being asked for.
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

# No `firewall` command in front of this, and that is measured rather than
# assumed: the first version of this script ran one, the control socket
# answered "there is no rule 127.0.0.1" -- the command takes a direction and a
# port, not an address -- and the page loaded regardless. The outgoing default
# allows this, so the line was a misleading error printed in front of a fetch
# that was always going to work.
./scripts/look.sh --out "$OUT_DIR" \
    "apps Web" \
    "ui click 640 120" \
    "ui type http://127.0.0.1:$PORT/" \
    "ui key Return" \
    "capture table-spans.png" 2>&1 | tail -12

echo
if [ -f "$OUT_DIR/table-spans.png" ]; then
    echo "wrote $OUT_DIR/table-spans.png"
else
    echo "no picture was captured"
    exit 1
fi
