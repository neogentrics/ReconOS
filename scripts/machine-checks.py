#!/usr/bin/env python3
r"""
What this server does on the machine, checked rather than looked at.

--- Why this exists, and what it costs to have found out ---

Every suite in `CMakeLists.txt` runs on a host: a real TCP stack, thousands of
descriptors, a `recv` that blocks properly. They are worth having and they are
exhaustive about the things they can reach.

**They were all green on a server that answered twelve requests and then went
silent for the rest of the boot.** VF-034 has the diagnosis; what matters here
is how it was found, which is that a measurement for a completely different
feature happened to need a thirteenth connection. Nothing in this repository
would otherwise have asked for one.

So this is the suite for the properties that only exist on the target. It boots
a real ReconOS machine, talks to it over a real socket through the kernel's own
stack, and asserts. `scripts/machine-tests.sh` is the driver; this is the
checks.

--- What belongs in here, and what does not ---

**In:** anything whose failure mode is invisible on a host. Descriptor and
connection lifetime, the kernel's `recv` answering 0 with nothing buffered,
bodies larger than one read, the volume, the boot token, a hundred connections
in a row.

**Out:** parsing. `test_http.c` hands the parser three hundred hostile strings
in a millisecond and this would take a socket and a second each. A check here
that a host suite could make is a check in the wrong place -- slower, flakier,
and no more true.

--- The shape of a check ---

Each one is named as a sentence about the machine, the same way the C suites
are, and the count is printed the same way. A failure prints what was expected
and what arrived, because the run takes half a minute and nobody wants to do it
twice to find out what went wrong.
"""

import gzip
import socket
import sys
import time

CHECKS = 0
FAILURES = 0
PORT = 8080
TOKEN = ""


def ok(cond, what, detail=""):
    global CHECKS, FAILURES

    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL  %s" % what)
        if detail:
            print("        %s" % detail)


def raw(request, timeout=10, port=None):
    """Send bytes, read everything until the machine closes."""
    s = socket.create_connection(("127.0.0.1", port or PORT), timeout)
    s.settimeout(timeout)
    if isinstance(request, str):
        request = request.encode("latin-1")
    s.sendall(request)
    got = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            got += chunk
    except (socket.timeout, ConnectionResetError, BrokenPipeError):
        # A refusal the machine answers and then closes can reset a connection
        # this is still writing to; the answer has usually arrived already.
        pass
    s.close()
    return got


def parts(answer):
    head, _, body = answer.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    return (lines[0] if lines else ""), lines[1:], body


def header(lines, name):
    name = name.lower() + ":"
    for line in lines:
        if line.lower().startswith(name):
            return line.split(":", 1)[1].strip()
    return None


def get(path, extra="", version="1.1"):
    return raw("GET %s HTTP/%s\r\nHost: m16\r\n%sConnection: close\r\n\r\n"
               % (path, version, extra))


# --- the checks -------------------------------------------------------------


def it_answers():
    status, lines, body = parts(get("/"))
    ok(status == "HTTP/1.1 200 OK", "the machine answers its own console",
       "got %r" % status)
    ok(b"ReconOS" in body, "and the page is this server's")
    ok(header(lines, "Server") is not None, "and it names itself")


def two_hundred_connections():
    """
    The property VF-034 was about, and the reason this file exists.

    A server that stops after twelve is a server nobody would ship, and every
    host suite in this repository passed on one. Two hundred is not a load
    test -- it is one more than any plausible table.
    """
    answered = 0
    stopped_at = None
    for i in range(200):
        try:
            answer = raw("GET /health HTTP/1.1\r\nHost: m16\r\n"
                         "Connection: close\r\n\r\n", timeout=6)
        except OSError as e:
            stopped_at = "%d (%s)" % (i, type(e).__name__)
            break
        if not answer:
            stopped_at = "%d (nothing came back)" % i
            break
        answered += 1

    ok(answered == 200,
       "two hundred connections in a row are all answered",
       "answered %d, stopped at %s" % (answered, stopped_at))


def keep_alive():
    """
    Many requests down one socket, which is the shape a browser uses.

    The response is read to its declared length rather than taken from one
    `recv`. The first draft assumed a response per read and reported two of
    forty against a server that was answering all of them -- a client that
    cannot reassemble a response is not measuring the server.
    """
    s = socket.create_connection(("127.0.0.1", PORT), 10)
    s.settimeout(10)
    answered = 0
    buf = b""
    try:
        for _ in range(40):
            s.sendall(b"GET /health HTTP/1.1\r\nHost: m16\r\n\r\n")

            # Head first, then exactly as many body bytes as it promised.
            while b"\r\n\r\n" not in buf:
                chunk = s.recv(65536)
                if not chunk:
                    raise OSError("closed")
                buf += chunk
            head, _, rest = buf.partition(b"\r\n\r\n")
            said = 0
            for line in head.decode("latin-1").split("\r\n"):
                if line.lower().startswith("content-length:"):
                    said = int(line.split(":", 1)[1])
            while len(rest) < said:
                chunk = s.recv(65536)
                if not chunk:
                    raise OSError("closed")
                rest += chunk
            if b"200" not in head:
                break
            buf = rest[said:]
            answered += 1
    except OSError:
        pass
    s.close()
    ok(answered >= 40, "forty requests down one connection",
       "answered %d" % answered)


def host_is_required():
    status, _, _ = parts(raw("GET / HTTP/1.1\r\nConnection: close\r\n\r\n"))
    ok(status.startswith("HTTP/1.1 400"),
       "HTTP/1.1 with no Host is refused", status)

    status, _, _ = parts(raw("GET / HTTP/1.1\r\nHost: m16\r\nHost: m16\r\n"
                             "Connection: close\r\n\r\n"))
    ok(status.startswith("HTTP/1.1 400"),
       "and two Host headers are refused even when they agree", status)

    status, _, _ = parts(raw("GET / HTTP/1.0\r\n\r\n"))
    ok(status.startswith("HTTP/1.1 200"),
       "while HTTP/1.0, which predates the field, is served", status)

    # Written out rather than through `get`, which supplies a Host of its
    # own -- the first draft added a second one and got the 400 the check
    # above exists to produce.
    status, _, _ = parts(raw("GET / HTTP/1.1\r\nHost: somebody.else.example\r\n"
                             "Connection: close\r\n\r\n"))
    ok(status.startswith("HTTP/1.1 200"),
       "and a site with no name answers to every name", status)


def chunked_bodies():
    body = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
            "5\r\nname=\r\n3\r\nM31\r\n0\r\n\r\n" % TOKEN)
    status, _, answer = parts(raw(body))
    ok(status.startswith("HTTP/1.1 200"),
       "a chunked form split mid-value is accepted", status)
    ok(b'"name":"M31"' in answer,
       "and the pieces arrive joined", answer[:60])

    bad = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
           "Authorization: Bearer %s\r\n"
           "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
           "8\nname=M32\r\n0\r\n\r\n" % TOKEN)
    status, _, _ = parts(raw(bad))
    ok(status.startswith("HTTP/1.1 400"),
       "a chunk size ended with a bare LF is refused", status)

    both = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
            "Authorization: Bearer %s\r\n"
            "Transfer-Encoding: chunked\r\nContent-Length: 8\r\n"
            "Connection: close\r\n\r\nname=M33" % TOKEN)
    status, _, _ = parts(raw(both))
    ok(status.startswith("HTTP/1.1 400"),
       "and a request framed two ways is refused", status)


def a_body_larger_than_one_read():
    """
    VF-013's territory: `recv` on this kernel answers 0 with nothing buffered,
    and a body that does not arrive in one read is the case that broke every
    request over four kilobytes for three versions.

    Sent as an upload rather than as a form. The first draft posted a six
    kilobyte *form field* and got 431, which is correct -- `HTTP_FORM_VALUE_MAX`
    is 512, and a field past it is refused. A check has to pick an endpoint
    that wants what it is sending, or it measures the bound instead of the
    property.
    """
    mark = "----------------machinecheck"
    content = "y" * 6000
    payload = ("--%s\r\n"
               "Content-Disposition: form-data; name=\"file\";"
               " filename=\"large.txt\"\r\n"
               "Content-Type: text/plain\r\n\r\n%s\r\n--%s--\r\n"
               % (mark, content, mark))
    request = ("POST /api/upload HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: multipart/form-data; boundary=%s\r\n"
               "Content-Length: %d\r\nConnection: close\r\n\r\n%s"
               % (TOKEN, mark, len(payload), payload))
    status, _, answer = parts(raw(request, timeout=60))

    #
    # Three answers mean the body arrived, and the check is about the body.
    #
    #   201/200  it was stored
    #   409      a file of that name is already there from an earlier run --
    #            the upload endpoint refuses to overwrite
    #   503      there is no volume, which is a diskless boot
    #
    # A 500 is not on the list on purpose: it used to be what a diskless boot
    # answered, and it says *the server's own answer failed* when the truth is
    # that there is nowhere to put the file.
    #
    ok(status.startswith("HTTP/1.1 201")
       or status.startswith("HTTP/1.1 200")
       or status.startswith("HTTP/1.1 409")
       or status.startswith("HTTP/1.1 503"),
       "a body of six kilobytes arrives whole, across many reads",
       "%s %r" % (status, answer[:80]))


def a_form_field_past_its_bound():
    """
    The status a body-side bound is answered with.

    `HTTP_FORM_VALUE_MAX` is 512 and a field past it is refused, which is
    right. It used to be refused with **431, which names headers** -- so a
    client with a long text field was told to look at the one part of its
    request that was fine. The verdict codes are shared between the header
    parser and the form parser; only the caller knows which end they came
    from, so the caller is where the translation belongs.
    """
    payload = "name=M40&filler=" + ("x" * 600)
    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: application/x-www-form-urlencoded\r\n"
               "Content-Length: %d\r\nConnection: close\r\n\r\n%s"
               % (TOKEN, len(payload), payload))
    status, _, _ = parts(raw(request, timeout=30))
    ok(status.startswith("HTTP/1.1 413"),
       "a form field past its bound is 413, which names the body", status)


def the_configuration():
    """What the machine is running as, which nothing could ask before 0.32.0."""
    status, _, _ = parts(get("/api/config"))
    ok(status.startswith("HTTP/1.1 401"),
       "the configuration is guarded, because it names paths on the volume",
       status)

    status, lines, body = parts(get("/api/config",
                                    "Authorization: Bearer %s\r\n" % TOKEN))
    ok(status.startswith("HTTP/1.1 200"),
       "and readable with the token", status)
    ok(header(lines, "Content-Type") == "application/json",
       "as JSON", header(lines, "Content-Type"))
    ok(b'"port"' in body and b'"resolver"' in body and b'"clock"' in body,
       "naming the port, the resolver and the clock", body[:80])
    ok(b'"from"' in body,
       "and where it came from, so defaults and a refused file are not the "
       "same answer", body[:80])


def the_guard():
    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Content-Type: application/x-www-form-urlencoded\r\n"
               "Content-Length: 8\r\nConnection: close\r\n\r\nname=M34")
    status, lines, _ = parts(raw(request))
    ok(status.startswith("HTTP/1.1 401"),
       "a write with no token is refused", status)
    ok(header(lines, "WWW-Authenticate") is not None,
       "and the refusal says what would be accepted")

    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: application/x-www-form-urlencoded\r\n"
               "Content-Length: 8\r\nConnection: close\r\n\r\nname=M34"
               % TOKEN)
    status, _, _ = parts(raw(request))
    ok(status.startswith("HTTP/1.1 200"),
       "and one with it is not", status)

    status, _, _ = parts(get("/api/log/segments"))
    ok(status.startswith("HTTP/1.1 401"),
       "the durable log is guarded while the live ring is not", status)


def json_in():
    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: 15\r\nConnection: close\r\n\r\n"
               '{"name":"M35"}\n' % TOKEN)
    status, _, answer = parts(raw(request))
    ok(status.startswith("HTTP/1.1 200"),
       "the API takes a JSON document", status)
    ok(b'"name":"M35"' in answer, "and acts on it", answer[:60])

    doc = '{"name":"M36","name":"M37"}'
    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: %d\r\nConnection: close\r\n\r\n%s"
               % (TOKEN, len(doc), doc))
    status, _, answer = parts(raw(request))
    ok(status.startswith("HTTP/1.1 400"),
       "a document with the same key twice is refused", status)

    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: text/xml\r\n"
               "Content-Length: 4\r\nConnection: close\r\n\r\nnope" % TOKEN)
    status, _, _ = parts(raw(request))
    ok(status.startswith("HTTP/1.1 415"),
       "and a media type this cannot read is 415, not 400", status)


def negotiation():
    status, lines, text = parts(get("/api/log", "Accept: text/plain\r\n"))
    ok(status.startswith("HTTP/1.1 200"), "the log answers as text", status)
    ok((header(lines, "Content-Type") or "").startswith("text/plain"),
       "with a text content type", header(lines, "Content-Type"))

    status, lines, js = parts(get("/api/log", "Accept: application/json\r\n"))
    ok(status.startswith("HTTP/1.1 200"), "and as JSON", status)
    ok(header(lines, "Content-Type") == "application/json",
       "with a JSON content type", header(lines, "Content-Type"))
    ok(js.lstrip().startswith(b"{"), "and a document", js[:40])
    ok("accept" in (header(lines, "Vary") or "").lower(),
       "and says its body depends on Accept",
       header(lines, "Vary"))

    status, _, _ = parts(get("/api/log", "Accept: image/png\r\n"))
    ok(status == "HTTP/1.1 406 Not Acceptable",
       "a type this cannot send is 406, with its phrase", status)

    status, _, _ = parts(get("/api/log", "Accept: text/\r\n"))
    ok(status.startswith("HTTP/1.1 400"),
       "and an Accept header that does not parse is 400", status)


def compression():
    status, lines, packed = parts(get("/", "Accept-Encoding: gzip\r\n"))
    ok(status.startswith("HTTP/1.1 200"), "a page asked for compressed", status)
    ok(header(lines, "Content-Encoding") == "gzip",
       "comes back compressed", header(lines, "Content-Encoding"))

    plain = b""
    try:
        plain = gzip.decompress(packed)
        ok(True, "and decompresses, in a library this project did not write")
    except Exception as e:
        ok(False, "and decompresses, in a library this project did not write",
           str(e))

    status, lines, whole = parts(get("/"))
    ok(header(lines, "Content-Encoding") is None,
       "a client that did not ask gets no encoding")
    #
    # Not compared byte for byte, and the reason is a property of the page
    # rather than a weakening of the check: the console reports how many
    # requests it has served, so two fetches of it differ by however many
    # arrived in between. The first draft asserted equality and failed at
    # 1545 against 1546, which is one digit.
    #
    ok(plain and b"ReconOS" in plain and abs(len(plain) - len(whole)) <= 16,
       "and the two forms are the same page, give or take its own counter",
       "%d compressed-then-expanded, %d plain" % (len(plain), len(whole)))
    ok(len(packed) < len(whole),
       "and the compressed form is smaller",
       "%d against %d" % (len(packed), len(whole)))

    said = header(lines, "Content-Length")
    ok(said is not None and int(said) == len(whole),
       "the length a response declares is the number of bytes it sends",
       "%s declared, %d arrived" % (said, len(whole)))


def files_off_the_volume():
    status, lines, body = parts(get("/console.css"))
    if status.startswith("HTTP/1.1 404"):
        print("  note  no volume attached; the file checks are skipped")
        return
    ok(status.startswith("HTTP/1.1 200"),
       "a file is served off the volume", status)
    ok((header(lines, "Content-Type") or "").startswith("text/css"),
       "with a type from its name", header(lines, "Content-Type"))
    ok(len(body) > 0, "and something in it")


def the_status_line():
    """
    Every status this server sends has a reason phrase.

    `scripts/check-statuses.py` proves that from the source. This proves it
    from the wire, which is where `406 Unknown` was actually seen.
    """
    status, _, _ = parts(get("/nothing-is-here"))
    ok(status == "HTTP/1.1 404 Not Found", "404 carries its phrase", status)

    status, _, _ = parts(raw("POST /health HTTP/1.1\r\nHost: m16\r\n"
                             "Content-Length: 0\r\nConnection: close\r\n\r\n"))
    ok(status == "HTTP/1.1 405 Method Not Allowed",
       "405 carries its phrase", status)

    status, _, _ = parts(raw("BREW / HTTP/1.1\r\nHost: m16\r\n"
                             "Connection: close\r\n\r\n"))
    ok(status == "HTTP/1.1 501 Not Implemented",
       "501 carries its phrase", status)


def conditional_requests():
    """
    Read a thing, get a validator, and use it both ways.

    This is the property that lets two programs share a machine without one
    of them losing a change it never knew it made. It is checked here rather
    than only in a host suite because the tag is computed from state the
    machine holds, and the whole point is that it changes when the machine
    does.
    """
    # Put the name somewhere known first, so the tag below is this check's.
    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: 15\r\nConnection: close\r\n\r\n"
               '{"name":"M50"}\n' % TOKEN)
    raw(request)

    status, lines, body = parts(get("/api/name"))
    ok(status.startswith("HTTP/1.1 200"), "the name can be read", status)
    ok(b'"M50"' in body, "and is what was set", body[:40])

    tag = header(lines, "ETag")
    ok(tag is not None and tag.startswith('"'),
       "and comes with a validator, in quotes", "%r" % tag)
    if not tag:
        return

    # The same tag back: nothing to send.
    status, lines2, body2 = parts(get("/api/name", 'If-None-Match: %s\r\n' % tag))
    ok(status == "HTTP/1.1 304 Not Modified",
       "asking again with that validator is 304, with its phrase", status)
    ok(body2 == b"", "and carries no body", "%d bytes" % len(body2))
    ok(header(lines2, "ETag") == tag,
       "and the validator again, so the next request can be conditional too",
       header(lines2, "ETag"))

    # A tag from somebody else's read: the body comes.
    status, _, body3 = parts(get("/api/name", 'If-None-Match: "not-the-one"\r\n'))
    ok(status.startswith("HTTP/1.1 200"),
       "a validator that does not match gets the body", status)
    ok(b'"M50"' in body3, "and it is the current one")

    # A write against the view just read: allowed.
    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\nIf-Match: %s\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: 15\r\nConnection: close\r\n\r\n"
               '{"name":"M51"}\n' % (TOKEN, tag))
    status, _, _ = parts(raw(request))
    ok(status.startswith("HTTP/1.1 200"),
       "a write against the view that was read is allowed", status)

    # The same write again, with the tag that is now stale.
    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\nIf-Match: %s\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: 15\r\nConnection: close\r\n\r\n"
               '{"name":"M52"}\n' % (TOKEN, tag))
    status, _, _ = parts(raw(request))
    ok(status == "HTTP/1.1 412 Precondition Failed",
       "and the same write again is 412, because the name has moved", status)

    status, _, body4 = parts(get("/api/name"))
    ok(b'"M51"' in body4,
       "so the refused write changed nothing", body4[:40])

    # A weak tag never guards a write, whatever it says.
    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\nIf-Match: W/%s\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: 15\r\nConnection: close\r\n\r\n"
               '{"name":"M53"}\n' % (TOKEN, header(parts(get("/api/name"))[1],
                                                   "ETag")))
    status, _, _ = parts(raw(request))
    ok(status == "HTTP/1.1 412 Precondition Failed",
       "a weak validator does not guard a write, even when it matches",
       status)

    # And no header at all is still an unconditional write, which is what the
    # console's own form sends.
    request = ("POST /api/name HTTP/1.1\r\nHost: m16\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: application/x-www-form-urlencoded\r\n"
               "Content-Length: 8\r\nConnection: close\r\n\r\nname=M54"
               % TOKEN)
    status, _, _ = parts(raw(request))
    ok(status.startswith("HTTP/1.1 200"),
       "and a write with no precondition still works, as the console's "
       "form sends none", status)


def the_console_itself():
    """
    What a person sees, rather than what a program is told.

    VF-022 is this project's entry about an API change that broke the
    console's own form for three versions while every `curl` test passed:
    *testing the API is not testing the console*. So these read the page.
    """
    status, lines, page = parts(get("/"))
    ok(status.startswith("HTTP/1.1 200"), "the console is served", status)
    ok((header(lines, "Content-Type") or "").startswith("text/html"),
       "as HTML", header(lines, "Content-Type"))

    # The form a person actually uses, with both fields the guard needs.
    ok(b'method="post"' in page and b'action="/api/name"' in page,
       "and carries the form that renames the machine")
    ok(b'name="name"' in page and b'name="token"' in page,
       "with a field for the name and one for the token, because a browser "
       "form cannot send a header")

    # The sections, each with a real value rather than a heading over nothing.
    ok(b"<h2>Configuration</h2>" in page,
       "the page says what the machine is configured as")
    ok(b"server.conf" in page,
       "and where that came from")
    ok(b"time server" in page and b"resolver" in page,
       "naming the clock and the resolver it is using")

    ok(b"<h2>Recently asked for</h2>" in page,
       "and what it has recently been asked for")
    ok(b"/api/name" in page or b"/health" in page,
       "with real entries in it, not an empty table")


def the_console_escapes_what_it_shows():
    """
    A target a stranger chose, shown to somebody else's browser.

    The access log holds the request target, and the console now prints it.
    `request.c` admits every printable byte in a target, including `<`, so
    this is the one place on this machine where text from a stranger reaches
    a document another person parses.

    VF-010 is the entry about a value that reached a JSON document unescaped
    and was safe only because a validator happened to forbid a quote. There is
    no such coincidence here, so this asks for a path that would close a tag
    and then reads the page back.
    """
    raw("GET /%3Cscript%3Ealert(1)%3C/script%3E HTTP/1.1\r\nHost: m16\r\n"
        "Connection: close\r\n\r\n")

    status, _, page = parts(get("/"))
    ok(status.startswith("HTTP/1.1 200"), "the console still renders", status)

    ok(b"<script>alert(1)" not in page,
       "a target that would close a tag does not reach the page as markup")
    ok(b"&lt;script&gt;" in page,
       "it arrives escaped, which is what makes it safe to show at all",
       "the page does not contain the escaped form either, so the check may "
       "be looking at the wrong thing")


def main():
    global PORT, TOKEN

    PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    TOKEN = sys.argv[2] if len(sys.argv) > 2 else ""

    print("what this server does on the machine")

    for check in (it_answers, host_is_required, the_guard,
                  the_configuration, json_in,
                  chunked_bodies, a_body_larger_than_one_read,
                  a_form_field_past_its_bound, conditional_requests,
                  the_console_itself, the_console_escapes_what_it_shows,
                  negotiation,
                  compression, files_off_the_volume, the_status_line,
                  keep_alive, two_hundred_connections):
        try:
            check()
        except Exception as e:                  # noqa: BLE001
            # A check that throws is a failed check, not a failed run: the
            # rest still have something to say, and a harness that stops at
            # the first surprise reports one fault where there are five.
            ok(False, "%s did not finish" % check.__name__,
               "%s: %s" % (type(e).__name__, e))

    print("  %d checks, %d failed" % (CHECKS, FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
