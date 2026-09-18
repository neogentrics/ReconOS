#!/usr/bin/env python3
#
# What a running ReconOS server does with the `Host` header.
#
# --- Why this is a script and not a suite ---
#
# `server/tests/test_http.c` and `test_http_serve.c` prove this against the
# parser and over a host socket, exhaustively, and they run in milliseconds.
# What they cannot show is the same rule running **on the machine** -- past the
# kernel's sockets, the C library, and the init program that configures the
# site. VF-023 is about a version of this role that built, installed and booted
# perfectly while serving nothing, so a property that has only ever been
# checked on a host is a property with a gap under it.
#
# This is deliberately a handful of raw requests rather than curl: curl always
# sends `Host`, and two of the six cases are about what happens when it is
# missing or doubled. A client that cannot send a malformed request cannot test
# a server's refusal of one.
#
# Usage, with the machine booted and :80 forwarded to 127.0.0.1:8080 --
# `scripts/server-disk.sh` prints the qemu line that does it:
#
#     python3 scripts/host-probe.py [port]
#
# It prints one line per case and says nothing about pass or fail: the expected
# answers are in VF-028 in `docs/VERIFICATION.md`, beside the run they came
# from. A script that judged its own output would need the expectations written
# twice.
#
import socket
import sys

CASES = [
    ("ordinary request, Host present",
     "GET / HTTP/1.1\r\nHost: m16\r\nConnection: close\r\n\r\n"),
    ("HTTP/1.1 with no Host at all",
     "GET / HTTP/1.1\r\nConnection: close\r\n\r\n"),
    ("two different Host headers",
     "GET / HTTP/1.1\r\nHost: m16\r\nHost: elsewhere\r\nConnection: close\r\n\r\n"),
    # Refused as well, and not an oversight: a duplicate that agrees is still a
    # message with two answers to one question.
    ("two identical Host headers",
     "GET / HTTP/1.1\r\nHost: m16\r\nHost: m16\r\nConnection: close\r\n\r\n"),
    ("HTTP/1.0 with no Host (predates the field)",
     "GET / HTTP/1.0\r\n\r\n"),
    # The console's site has no name, so it claims every name. This is the
    # check that the dispatch changed nothing for a server with one site.
    ("a name this machine was never given",
     "GET / HTTP/1.1\r\nHost: someone.else.example\r\nConnection: close\r\n\r\n"),
]


def ask(port, request):
    s = socket.create_connection(("127.0.0.1", port), 10)
    s.settimeout(10)
    s.sendall(request.encode())
    got = b""
    try:
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            got += chunk
    except socket.timeout:
        # Not a failure here. A refusal this server answers and then closes
        # arrives whole; anything that does not close is reported as whatever
        # did arrive, which is more useful than an exception.
        pass
    s.close()
    return got


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

    for what, request in CASES:
        got = ask(port, request)
        line = got.split(b"\r\n", 1)[0].decode("latin-1") if got else "(nothing)"
        print("%-45s %s" % (what, line))
    return 0


if __name__ == "__main__":
    sys.exit(main())
