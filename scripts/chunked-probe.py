#!/usr/bin/env python3
#
# What a running ReconOS server does with a chunked request body.
#
# --- Why this exists beside the suites ---
#
# `server/tests/test_chunked.c` proves the decoder against strings and
# `test_http_serve.c` proves it over a host socket. Neither can show the thing
# this shows: the same bytes going through the kernel's own sockets, this
# machine's C library, and the connection loop in the init program -- where
# `recv` answers 0 with nothing buffered and a body arrives in as many pieces
# as the kernel chooses. VF-013 is this project's entry about how differently
# that behaves from a host.
#
# It is deliberately not curl. curl sends a chunked body correctly, which is
# half the cases here; the other half are bodies that must be refused, and a
# client that cannot send a malformed request cannot test a refusal.
#
# Usage, with the machine booted and :80 forwarded -- the token is printed on
# the serial console at boot, on the line starting `the guard:`
#
#     python3 scripts/chunked-probe.py <port> <token>
#
# It prints one line per case. What the answers should be is in VF-031 in
# `docs/VERIFICATION.md`, beside the run they came from; a script that judged
# its own output would need the expectations written down twice.
#
import socket
import sys


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
    except (socket.timeout, BrokenPipeError, ConnectionResetError):
        # Not a failure. A refusal this server answers and then closes can
        # reset a connection the client is still writing to, and the answer
        # has usually arrived already.
        pass
    s.close()
    return got


def head(got):
    return got.split(b"\r\n", 1)[0].decode("latin-1") if got else "(nothing)"


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    token = sys.argv[2] if len(sys.argv) > 2 else ""

    guard = "Authorization: Bearer %s\r\n" % token

    cases = [
        # The ordinary case: a form sent chunked, in two chunks split in the
        # middle of the field value, so the handler only sees the right thing
        # if the pieces were joined.
        ("a form sent chunked, split mid-value",
         "POST /api/name HTTP/1.1\r\nHost: m16\r\n" + guard +
         "Content-Type: application/x-www-form-urlencoded\r\n"
         "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
         "5\r\nname=\r\n3\r\nM17\r\n0\r\n\r\n"),

        ("the same, with a trailer after it",
         "POST /api/name HTTP/1.1\r\nHost: m16\r\n" + guard +
         "Content-Type: application/x-www-form-urlencoded\r\n"
         "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
         "8\r\nname=M18\r\n0\r\nX-Checksum: 42\r\n\r\n"),

        ("a chunk size ended with a bare LF",
         "POST /api/name HTTP/1.1\r\nHost: m16\r\n" + guard +
         "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
         "8\nname=M19\r\n0\r\n\r\n"),

        ("a chunk extension",
         "POST /api/name HTTP/1.1\r\nHost: m16\r\n" + guard +
         "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
         "8;a=b\r\nname=M19\r\n0\r\n\r\n"),

        ("framed twice: chunked and a length",
         "POST /api/name HTTP/1.1\r\nHost: m16\r\n" + guard +
         "Transfer-Encoding: chunked\r\nContent-Length: 8\r\n"
         "Connection: close\r\n\r\nname=M19"),

        ("chunked on HTTP/1.0, which predates it",
         "POST /api/name HTTP/1.0\r\n" + guard +
         "Transfer-Encoding: chunked\r\n\r\n8\r\nname=M19\r\n0\r\n\r\n"),

        ("and what the machine is called now",
         "GET /api/status HTTP/1.1\r\nHost: m16\r\nConnection: close\r\n\r\n"),
    ]

    for what, request in cases:
        got = ask(port, request)
        line = head(got)
        extra = ""
        body = got.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in got else b""
        if b'"name"' in body:
            at = body.index(b'"name"')
            extra = "  " + body[at:at + 24].decode("latin-1", "replace")
        print("%-40s %s%s" % (what, line, extra))
    return 0


if __name__ == "__main__":
    sys.exit(main())
