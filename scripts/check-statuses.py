#!/usr/bin/env python3
r"""
Every status this server sends must have a reason phrase.

--- Why this exists ---

`http.h` carries `HTTP_STATUSES`, an X-macro list of every status with its
phrase, and its own comment says why: `http_reason` and the suite used to
enumerate them separately and drifted twice -- 304 went out as `304 Unknown`
the first time a conditional request was answered, and 206 and 416 did the same
a day later, *after* a case had been added to the suite for every status then
known.

The list fixed the drift between the function and the suite. **It did not fix
the other half**, which is a status used in a handler and never added to the
list at all. That happened again in 0.28.0: content negotiation answered

    HTTP/1.1 406 Unknown

on the machine, because 406 was the first status this server had ever needed
and nobody had put it in the table. The suite could not catch it -- it walks
the same table the function is built from, so a missing entry is invisible from
both sides. Only the wire showed it.

So this reads the other direction: every status **literal** in the server's
source, checked against the list. Enumerating by hand in two places is not a
thing that can be done carefully enough; this is the third reader.

--- What it looks for ---

An integer in the status position of the calls that send one:

    http_response_simple(out, 406, ...)
    send_status(fd, 406, ...)
    http_stream_begin(&sink, 206, ...)

Three-digit numbers between 100 and 599, taken only from those call sites, so
an unrelated `406` in arithmetic is not a status and is not read as one.

Exit 0 when clean, 1 when a status has no phrase.
"""

import os
import re
import sys

ROOTS = ("server",)
SUFFIXES = (".c", ".h")

# The calls whose second argument is a status.
CALLS = (
    "http_response_simple",
    "send_status",
    "http_stream_begin",
)

CALL_RE = re.compile(
    r"\b(?:%s)\s*\(\s*[^,()]+,\s*(\d{3})\b" % "|".join(CALLS))

TABLE_RE = re.compile(r"^\s*X\((\d{3}),")


def statuses_in_table(path):
    known = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = TABLE_RE.match(line)
            if m:
                known.add(int(m.group(1)))
    return known


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)

    table = os.path.join(root, "server", "http", "http.h")
    known = statuses_in_table(table)
    if not known:
        print("could not read HTTP_STATUSES out of server/http/http.h")
        return 1

    used = {}
    for top in ROOTS:
        for dirpath, _dirs, files in os.walk(os.path.join(root, top)):
            for name in sorted(files):
                if not name.endswith(SUFFIXES):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                with open(path, encoding="utf-8", errors="replace") as f:
                    for number, line in enumerate(f, 1):
                        for m in CALL_RE.finditer(line):
                            status = int(m.group(1))
                            if status < 100 or status > 599:
                                continue
                            used.setdefault(status, (rel, number))

    missing = sorted(s for s in used if s not in known)
    for status in missing:
        where, number = used[status]
        print("%s:%d: %d is sent and has no reason phrase" % (where, number,
                                                             status))
        print("    add it to HTTP_STATUSES in server/http/http.h, or it goes "
              "out as \"%d Unknown\"" % status)

    if missing:
        print()
        print("%d status(es) sent without a phrase." % len(missing))
        return 1


    #
    # Zero is an unread tree, not a clean one.
    #
    # The denominator beside this number says how much was looked at, and a
    # denominator only helps a reader who brings an expectation. This needs
    # nobody: it encodes a fact about the repository that cannot quietly stop
    # being true. See `check-site-init.py` for where this started, and the
    # network session's check_citations for the case that makes it urgent --
    # the environment that produces zero there is the one the matrix runs in,
    # so the check most likely to meet the conditions that break it was the one
    # that passed under them.
    #
    if not used:
        print("found no status literals anywhere in the source.")
        print("This server sends statuses on every request. The check did not")
        print("read the tree -- it has not passed, it has failed to run.")
        return 1


    #
    # Zero is an unread tree, not a clean one.
    #
    # The denominator beside this number says how much was looked at, and a
    # denominator only helps a reader who brings an expectation. This needs
    # nobody: it encodes a fact about the repository that cannot quietly stop
    # being true. See `check-site-init.py` for where this started, and the
    # network session's check_citations for the case that makes it urgent --
    # the environment that produces zero there is the one the matrix runs in,
    # so the check most likely to meet the conditions that break it was the one
    # that passed under them.
    #
    if not used:
        print("found no status literals anywhere in the source.")
        print("This server sends statuses on every request. The check did not")
        print("read the tree -- it has not passed, it has failed to run.")
        return 1

    print("statuses: %d sent, all of them in the table" % len(used))
    return 0


if __name__ == "__main__":
    sys.exit(main())
