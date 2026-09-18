#!/usr/bin/env python3
r"""
Find a `struct http_site` that is declared and then filled in field by field.

--- Why this exists ---

`struct http_site` is configuration, and it grows. It has gained `bytes_sent`,
then `idle`, then `now_ms`, then `unblock`, then `allow` and `allow_ctx`, then
`host` and `next`. Every one of those was a deliberate addition with a comment
explaining it, and every one of them was also a trap for anybody who had
written

    struct http_site site;

    site.routes = ROUTES;
    site.server_name = "ReconOS";
    ...

because that shape sets the fields somebody remembered and leaves the rest
holding whatever the stack happened to contain. It is not a style preference.
The three test files hit it as compiler errors, which is the harmless way; the
init program hit it as **a server dispatching on a host name read out of
uninitialised memory**, which is VF-029, and it answered every request
correctly on the boot it was measured on.

A designated initializer zero-fills everything it does not mention -- by the
language, not by anybody being careful -- and it keeps doing so for every field
added after the line is written.

--- What it looks for ---

A declaration of `struct http_site` with no `=` before its semicolon. A pointer
(`struct http_site *s;`) is not a declaration of one and is left alone, and so
is a member inside another struct, which is initialised by whatever initialises
the struct that holds it.

This is narrow on purpose. It is not a general rule about uninitialised
variables -- the compiler has warnings for that and they do not fire on a
struct that is assigned field by field, which is exactly the case that bit.

Exit 0 when clean, 1 when something is found.
"""

import os
import re
import sys

ROOTS = ("server",)

# `struct http_site name;` -- with any leading storage class and no `=`.
# A `*` before the name makes it a pointer, which is fine.
DECL = re.compile(
    r"^\s*(?:static\s+|const\s+|register\s+)*"
    r"struct\s+http_site\s+(?!\*)([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;"
)

# Inside a struct definition a member is initialised by its container, so
# `struct http_site site;` as a member is not what this looks for. Tracked by
# depth: a member sits inside `struct ... {`, and a declaration does not.
OPENS = re.compile(r"^\s*(?:typedef\s+)?struct\s+\w*\s*\{")


def offenders(path):
    found = []
    inside = 0

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for number, line in enumerate(f, 1):
            if OPENS.match(line):
                inside += 1
                continue
            if inside and line.startswith("}"):
                inside -= 1
                continue
            if inside:
                continue
            match = DECL.match(line)
            if match:
                found.append((number, match.group(1), line.rstrip()))
    return found


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    bad = 0

    for root in ROOTS:
        for where, _, names in os.walk(os.path.join(here, root)):
            for name in sorted(names):
                if not name.endswith((".c", ".h")):
                    continue
                path = os.path.join(where, name)
                for number, which, text in offenders(path):
                    shown = os.path.relpath(path, here).replace(os.sep, "/")
                    print("%s:%d: `%s` is declared and then assigned field by "
                          "field" % (shown, number, which))
                    print("    %s" % text.strip())
                    print("    every field added to `struct http_site` after "
                          "this line is left uninitialised -- see VF-029")
                    bad += 1

    if bad:
        print()
        print("%d site(s) not built with a designated initializer." % bad)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
