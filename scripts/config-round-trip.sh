#!/bin/sh
#
# Configure a machine over HTTP, restart it, and see whether it listened.
#
# --- Why this needs two boots and therefore its own script ---
#
# `scripts/machine-tests.sh` boots once and asks the machine questions. This
# one asks a question a single boot cannot answer: **does a configuration
# written over the network survive a restart and take effect?**
#
# It has to be two boots because that is the design. A generation is written
# and read at the *next* boot, deliberately -- see `config.h`. Applying it live
# would mean the listening port or the site list changing underneath the
# connection that asked for the change, and a mistake would take the console
# with it before anybody could write the correction.
#
# The shape is VF-025's: two boots on one volume, because inside one boot a
# write that reached the volume and one that did not look identical.
#
# --- What it proves, and what it does not ---
#
# It proves the round trip: a candidate is parsed before it is written, a bad
# one is refused with its line and changes nothing, a good one becomes the next
# generation, and the machine comes back running it.
#
# It does not prove recovery from a configuration that parses and is still
# wrong. Nothing on this machine can select an older generation, because
# nothing can read a boot argument -- `docs/KERNEL-WANTS.md` has that entry.
#
set -eu

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
cd "$root"

kernel=kernel/build/x86_64/reconos-kernel.elf
volume=kernel/build/config-round-trip.img
log=kernel/build/config-round-trip.log
port=18470

checks=0
failures=0

ok() {
	checks=$((checks + 1))
	if [ "$1" = "0" ]; then
		failures=$((failures + 1))
		echo "  FAIL  $2"
		[ $# -gt 2 ] && echo "        $3"
	fi
}

boot() {
	rm -f "$log"
	qemu-system-x86_64 -m 1024 -smp 2 -kernel "$kernel" \
		-drive "file=$volume,format=raw,if=none,id=d0" \
		-device virtio-blk-pci,drive=d0 \
		-netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$port-:80" \
		-device virtio-net-pci,netdev=n0 \
		-display none -serial "file:$log" >/dev/null 2>&1 &
	qemu=$!

	waited=0
	while [ "$waited" -lt 90 ]; do
		if [ -f "$log" ] && tr -d '\r' < "$log" | grep -q "listening on"; then
			return 0
		fi
		if ! kill -0 "$qemu" 2>/dev/null; then
			echo "the machine stopped before it listened:" >&2
			tr -d '\r' < "$log" | tail -20 >&2
			exit 1
		fi
		sleep 1
		waited=$((waited + 1))
	done
	echo "the machine never listened" >&2
	kill "$qemu" 2>/dev/null || true
	exit 1
}

stop() {
	kill "$qemu" 2>/dev/null || true
	wait "$qemu" 2>/dev/null || true
	sleep 1
}

token_from_log() {
	tr -d '\r' < "$log" | sed -n 's/.*Bearer \([0-9a-f]*\).*/\1/p' | head -1
}

echo "building the server role"
make -C kernel ARCH=x86_64 ROLE=server >/dev/null

#
# A volume of its own, made fresh.
#
# The machine-test volume accumulates generations from every previous run, and
# a check that asserts "generation 2 is in force" against a disk that already
# has nine is a check that fails for the wrong reason. This one starts empty
# every time, which costs an install and buys a number that means something.
#
echo "making a volume"
rm -f "$volume"
scripts/server-disk.sh "$volume" >/dev/null

# --- the first boot: what a fresh volume says --------------------------------
echo
echo "first boot"
boot
token=$(token_from_log)

first=$(python3 - "$port" "$token" <<'PY'
import socket, sys
port, token = int(sys.argv[1]), sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), 10); s.settimeout(10)
s.sendall(("GET /api/config HTTP/1.1\r\nHost: m16\r\n"
           "Authorization: Bearer %s\r\nConnection: close\r\n\r\n" % token).encode())
got = b""
while True:
    d = s.recv(65536)
    if not d:
        break
    got += d
s.close()
print(got.split(b"\r\n\r\n", 1)[1].decode("latin-1").strip())
PY
)
echo "  $first"
#
# Two true statements, not one: the template is on the volume as generation
# 1, and this boot is running its defaults, because a template configures
# nothing. The first draft asserted only the first and read the second as a
# failure.
#
ok "$(echo "$first" | grep -c '"generation":1')" \
   "a fresh volume has a template on it as generation 1" "$first"
ok "$(echo "$first" | grep -c '"from":"defaults"')" \
   "and is running its defaults, because a template configures nothing" \
   "$first"

# --- a candidate that does not parse ----------------------------------------
bad=$(python3 - "$port" "$token" <<'PY'
import socket, sys
port, token = int(sys.argv[1]), sys.argv[2]
body = "name m16\nprot 80\n"
req = ("POST /api/config HTTP/1.1\r\nHost: m16\r\n"
       "Authorization: Bearer %s\r\nContent-Type: text/plain\r\n"
       "Content-Length: %d\r\nConnection: close\r\n\r\n%s" % (token, len(body), body))
s = socket.create_connection(("127.0.0.1", port), 10); s.settimeout(10)
s.sendall(req.encode())
got = b""
while True:
    d = s.recv(65536)
    if not d:
        break
    got += d
s.close()
head, _, rest = got.partition(b"\r\n\r\n")
print(head.split(b"\r\n")[0].decode("latin-1"), "|", rest.decode("latin-1").replace("\n", " ").strip())
PY
)
echo "  $bad"
ok "$(echo "$bad" | grep -c '400')" \
   "a candidate that does not parse is refused" "$bad"
ok "$(echo "$bad" | grep -c 'line 2')" \
   "and named the line, the same way the boot path would" "$bad"
ok "$(echo "$bad" | grep -c 'Nothing was written')" \
   "and said that nothing was written" "$bad"

# --- a candidate that does ---------------------------------------------------
good=$(python3 - "$port" "$token" <<'PY'
import socket, sys
port, token = int(sys.argv[1]), sys.argv[2]
body = ("name m17\nclock time.example\n"
        "[site shop.example]\nroot /System/Web\n"
        "[site gone.example]\nroot /System/NotThere\n")
req = ("POST /api/config HTTP/1.1\r\nHost: m16\r\n"
       "Authorization: Bearer %s\r\nContent-Type: text/plain\r\n"
       "Content-Length: %d\r\nConnection: close\r\n\r\n%s" % (token, len(body), body))
s = socket.create_connection(("127.0.0.1", port), 10); s.settimeout(10)
s.sendall(req.encode())
got = b""
while True:
    d = s.recv(65536)
    if not d:
        break
    got += d
s.close()
head, _, rest = got.partition(b"\r\n\r\n")
print(head.split(b"\r\n")[0].decode("latin-1"), "|", rest.decode("latin-1").strip())
PY
)
echo "  $good"
ok "$(echo "$good" | grep -c '201')" \
   "a candidate that parses is written" "$good"
ok "$(echo "$good" | grep -c '"written":2')" \
   "as the next generation" "$good"
ok "$(echo "$good" | grep -c 'next boot')" \
   "and says when it takes effect" "$good"

# It has not taken effect yet, which is the whole point.
still=$(python3 - "$port" "$token" <<'PY'
import socket, sys
port, token = int(sys.argv[1]), sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), 10); s.settimeout(10)
s.sendall(("GET /api/status HTTP/1.1\r\nHost: m16\r\n"
           "Authorization: Bearer %s\r\nConnection: close\r\n\r\n" % token).encode())
got = b""
while True:
    d = s.recv(65536)
    if not d:
        break
    got += d
s.close()
print(got.split(b"\r\n\r\n", 1)[1].decode("latin-1").strip())
PY
)
ok "$(echo "$still" | grep -vc '"name":"m17"')" \
   "and has not taken effect while this boot is still running" "$still"

stop

# --- the second boot ---------------------------------------------------------
echo
echo "second boot, same volume"
boot
token=$(token_from_log)

after=$(python3 - "$port" "$token" <<'PY'
import socket, sys
port, token = int(sys.argv[1]), sys.argv[2]
out = []
for path in ("/api/config", "/api/status"):
    s = socket.create_connection(("127.0.0.1", port), 10); s.settimeout(10)
    s.sendall(("GET %s HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\nConnection: close\r\n\r\n"
               % (path, token)).encode())
    got = b""
    while True:
        d = s.recv(65536)
        if not d:
            break
        got += d
    s.close()
    out.append(got.split(b"\r\n\r\n", 1)[1].decode("latin-1").strip())
print(" | ".join(out))
PY
)
echo "  $after"
ok "$(echo "$after" | grep -c '"generation":2')" \
   "the machine came back running the generation that was written" "$after"
ok "$(echo "$after" | grep -c 'time.example')" \
   "with the time server it names" "$after"
ok "$(echo "$after" | grep -c '"name":"m17"')" \
   "and the name it names" "$after"
ok "$(tr -d '\r' < "$log" | grep -c 'the configuration: name m17')" \
   "and said so on its console as it started" \
   "$(tr -d '\r' < "$log" | grep 'configuration' | head -3)"

#
# --- and the thing VF-028 said was not proved on the machine ----------------
#
# 0.24.0 built name-based virtual hosts and said plainly what its own
# verification did not cover: *a second named site. `server_init.c` configures
# one site with no name, so what a boot shows is that adding the dispatch
# changed nothing for a server that has one site.*
#
# Everything needed to close that arrived later and separately: a configuration
# file (0.25.0), and a way to write one over the network (0.33.0). The site
# above is configured, not compiled, and this is a machine that was told about
# it by HTTP and then restarted.
#
# The two sites are told apart by what they serve rather than by a header. The
# console answers `/` from a handler that builds the dashboard; the named site
# answers it from `/System/Web/index.html` on the volume. Same path, same
# machine, two documents.
#
# `/System/Uploads` would have been the tidier root to demonstrate with and is
# deliberately not used: `server_init.c` says nothing serves that directory,
# and a test that pointed a site at it would be quietly removing the property
# it was written to keep.
#
hosts=$(python3 - "$port" <<'PY'
import socket, sys
port = int(sys.argv[1])

def ask(host):
    s = socket.create_connection(("127.0.0.1", port), 15); s.settimeout(15)
    s.sendall(("GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
               % host).encode())
    got = b""
    while True:
        d = s.recv(65536)
        if not d:
            break
        got += d
    s.close()
    head, _, body = got.partition(b"\r\n\r\n")
    which = ("volume" if b"It works." in body
             else "console" if b"ReconOS Server" in body
             else "other")
    return "%s=%s/%d" % (host, which, len(body))

print(" ".join(ask(h) for h in ("shop.example", "not-a-site.example",
                                "10.0.2.15")))
PY
)
echo "  $hosts"
ok "$(echo "$hosts" | grep -c 'shop.example=volume')" \
   "a configured site serves the volume's own file" "$hosts"
ok "$(echo "$hosts" | grep -c 'not-a-site.example=console')" \
   "a name no site claims still reaches the console, which has no name" \
   "$hosts"
ok "$(echo "$hosts" | grep -c '10.0.2.15=console')" \
   "and so does this machine's own address, which is how it is reached" \
   "$hosts"

#
# --- a site that parses and is still wrong ----------------------------------
#
# `/System/NotThere` is a perfectly good path and is not on this volume.
# `config.c` cannot catch that -- it is pure, and the volume is not its
# business -- so the configuration is accepted and the site answers 404 to
# everything.
#
# The machine cannot refuse it either: the directory might be populated later,
# and refusing to boot over a directory that does not exist yet would be worse
# than serving nothing from it. What it can do is **say so**, once, where
# somebody will see it. This is the *parses and is still wrong* case, which is
# the one thing left in the configuration ask to the kernel session.
#
ok "$(tr -d '\r' < "$log" | grep -c 'WHICH HAS NO READABLE INDEX')" \
   "a site whose root is not there is named on the console at boot" \
   "$(tr -d '\r' < "$log" | grep 'the configuration: site' | head -3)"
ok "$(tr -d '\r' < "$log" | grep -c 'site shop.example from /System/Web$')" \
   "and the one that is there is not" \
   "$(tr -d '\r' < "$log" | grep 'the configuration: site' | head -3)"

readable=$(python3 - "$port" "$token" <<'PY'
import socket, sys
port, token = int(sys.argv[1]), sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), 15); s.settimeout(15)
s.sendall(("GET /api/config HTTP/1.1\r\nHost: m16\r\n"
           "Authorization: Bearer %s\r\nConnection: close\r\n\r\n"
           % token).encode())
got = b""
while True:
    d = s.recv(65536)
    if not d:
        break
    got += d
s.close()
print(got.split(b"\r\n\r\n", 1)[1].decode("latin-1").strip())
PY
)
ok "$(echo "$readable" | grep -c '"index_readable":false')" \
   "and a program can ask, rather than having to read a console it cannot see" \
   "$readable"

stop

echo
echo "  $checks checks, $failures failed"
[ "$failures" -eq 0 ] || exit 1
