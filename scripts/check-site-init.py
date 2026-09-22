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

**A `struct http_site` on the stack**, with no initializer: an indented
declaration that is not `static`. Those are the ones the language leaves
holding whatever was there before.

Three shapes are deliberately not flagged, because the language already
guarantees them:

  * a declaration at file scope, and a `static` one inside a function --
    zero-initialised, so an unmentioned field is NULL rather than rubbish;
  * a pointer (`struct http_site *s;`), which is not one of these at all;
  * a member inside another struct, initialised by whatever initialises the
    struct that holds it.

`server_init.c` has a file-scope array of them that is filled in by copying the
console site over each element and overriding three fields. That is the good
shape -- a field added next year arrives in every configured site for free --
and an earlier version of this check flagged it, which is how the rule came to
be about **storage duration** rather than about the presence of an `=`.

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
# An *indented* declaration -- so, inside a function -- that is not `static`.
# See the header: at file scope, or `static`, the language zeroes it.
DECL = re.compile(
    r"^[ \t]+(?:const\s+|register\s+)*"
    r"struct\s+http_site\s+(?!\*)([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;"
)

# Inside a struct definition a member is initialised by its container, so
# `struct http_site site;` as a member is not what this looks for. Tracked by
# depth: a member sits inside `struct ... {`, and a declaration does not.
OPENS = re.compile(r"^\s*(?:typedef\s+)?struct\s+\w*\s*\{")


def offenders(path):
    """Return (bad declarations, how many `struct http_site` lines were seen).

    The second number is the point of this function's shape. See `main`.
    """
    found = []
    seen = 0
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
            if "struct http_site" in line:
                seen += 1
            match = DECL.match(line)
            if match:
                found.append((number, match.group(1), line.rstrip()))
    return found, seen


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    files = 0
    sites = 0
    bad = 0

    for root in ROOTS:
        for where, _, names in os.walk(os.path.join(here, root)):
            for name in sorted(names):
                if not name.endswith((".c", ".h")):
                    continue
                path = os.path.join(where, name)
                files += 1
                bad_here, seen_here = offenders(path)
                sites += seen_here
                for number, which, text in bad_here:
                    shown = os.path.relpath(path, here).replace(os.sep, "/")
                    print("%s:%d: `%s` is on the stack with no initializer"
                          % (shown, number, which))
                    print("    %s" % text.strip())
                    print("    every field added to `struct http_site` after "
                          "this line holds whatever the stack did -- see VF-029")
                    bad += 1

    if bad:
        print()
        print("%d site(s) left holding whatever the stack had." % bad)
        return 1

    #
    # --- Saying nothing was the bug ----------------------------------------
    #
    # This printed **nothing at all** on success and returned 0. So a run that
    # examined every file and a run that examined none looked identical, and
    # the second is not hypothetical: point `ROOTS` at a directory that is not
    # there, or move `server/`, and `os.walk` yields nothing, `bad` stays zero,
    # and this reports success having read no code.
    #
    # It is the worst shape of the fault the network session named, because
    # there is not even a sentence to doubt. Theirs at least claimed something.
    #
    # So it says what it read. And the zero case is an **error**, not a quiet
    # pass: this repository certainly contains `struct http_site`, so a run
    # that finds none has failed to read the tree rather than found it clean.
    # That is the independent expectation -- it does not come from the
    # traversal, it comes from knowing what is in the repository.
    #
    if sites == 0:
        print("read %d file(s) and found no `struct http_site` anywhere."
              % files)
        print("This repository has several. The check did not read the tree --")
        print("it has not passed, it has failed to run.")
        return 1

    print("site init: %d site declarations across %d files, all initialised"
          % (sites, files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
