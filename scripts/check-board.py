#!/usr/bin/env python3
#
# A third reader for the board.
#
# --- Why this exists ---
#
# VF-033 settled the shape of this problem for HTTP statuses: two readers of one
# table agree with each other and are both wrong about an entry nobody wrote, so
# the fix is a *third* reader that goes the other way. `check-statuses.py` reads
# every status literal in the source and checks it against the table; it found
# a 502 nobody had ever seen, one minute after being written.
#
# `docs/SERVER.md` and `docs/WEB.md` are the same shape and had no third reader
# at all. They are tables of claims -- **blocked**, **built**, **unblocked** --
# about work that is gated on the kernel session's tree, and the kernel session
# does not read them. So a row can say a thing is unblocked for as long as
# nobody happens to re-read it beside `docs/KERNEL-WANTS.md`.
#
# It had. On 21 September the reverse-proxy row in `SERVER.md` said
# **unblocked, not built** -- *`connect` is fixed (KF-244)* -- while the row for
# the same subsystem in `WEB.md` said **blocked**, and `KERNEL-WANTS.md` carried
# an open entry saying `connect` never reports a completed handshake. Two of
# this project's own documents contradicting each other about whether a day of
# work was possible. Nothing could have noticed.
#
# --- What it checks ---
#
# A board row may cite the kernel entries it waits on:
#
#     | Reverse proxy | server | **blocked** | ... [needs: kw-connect-handshake] |
#
# The ids are the `{#kw-...}` tags on `## ` headings in `KERNEL-WANTS.md`. An
# entry whose heading is struck through (`~~like this~~`) is **answered**.
#
#   1. Every id is unique.
#   2. Every citation names an id that exists.
#   3. A row that cites anything and is marked **blocked** must cite at least
#      one id that is still open -- otherwise the thing it waits on has landed
#      and the row is stale.
#   4. A row marked **built** or **unblocked** must cite no open id -- which is
#      the reverse-proxy fault above, and the one that costs somebody a day.
#
# **What it deliberately does not check:** that a blocked row cites anything at
# all. Plenty are blocked on this role's own unbuilt work -- there is no kernel
# entry for "no TLS" -- and a checker that demanded a citation would be answered
# with a fake one. It reports how many rows carry no citation so the number is
# visible rather than enforced.
#
# Usage, from the repository root:
#
#     python3 scripts/check-board.py
#
import os
import re
import sys

WANTS = "docs/KERNEL-WANTS.md"
BOARDS = ("docs/SERVER.md", "docs/WEB.md")

# A status cell, with or without the bold markers the boards use.
BUILT = ("built", "unblocked")
BLOCKED = ("blocked",)


def read(root, name):
    with open(os.path.join(root, name), encoding="utf-8") as f:
        return f.read()


def entries(text):
    """id -> (answered, heading), from the `## ` headings of KERNEL-WANTS."""
    found = {}
    duplicates = []

    for line in text.split("\n"):
        if not line.startswith("## "):
            continue
        m = re.search(r"\{#([a-z0-9-]+)\}\s*$", line)
        if not m:
            continue
        ident = m.group(1)
        heading = line[3:m.start()].strip()
        if ident in found:
            duplicates.append(ident)
        # Struck through means the kernel session answered it.
        found[ident] = ("~~" in heading, heading)

    return found, duplicates


def rows(text):
    """(line number, status, [cited ids]) for every table row with a status."""
    out = []

    for n, line in enumerate(text.split("\n"), 1):
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 3:
            continue

        # The boards have two shapes: `| subsystem | owner | status | note |`
        # and `| console | status | note |`. Rather than guess which, take the
        # status as whichever cell holds one of the words -- a note mentioning
        # "blocked" in prose is not a status cell because it is not the whole
        # cell.
        status = None
        for c in cells[1:]:
            bare = c.replace("*", "").strip().lower()
            # A status cell is a word, sometimes with a qualification after it:
            # "blocked on one thing now", "unblocked, not built",
            # "blocked -- nothing can start a program". Take the first word and
            # require the cell to *start* with it, so that a note whose prose
            # happens to contain "blocked" is not read as a status.
            head = re.split(r"[ ,\u2014-]", bare, 1)[0].strip()
            if head in BUILT + BLOCKED:
                status = head
                break

        cited = []
        for m in re.finditer(r"\[needs:\s*([^\]]+)\]", line):
            cited += [x.strip() for x in m.group(1).split(",") if x.strip()]
        # A row this could not read a status from still gets reported when
        # it carries a citation, because a citation nobody reads is worse than
        # no citation at all: it looks like the row is being checked.
        out.append((n, status, cited, cells[0]))

    return out


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    problems = []

    known, duplicates = entries(read(root, WANTS))
    for ident in duplicates:
        problems.append("%s: two entries carry the id {#%s}" % (WANTS, ident))

    if not known:
        print("no {#kw-...} ids in %s -- nothing to check against" % WANTS)
        return 2

    uncited = 0
    checked = 0

    for board in BOARDS:
        for n, status, cited, subject in rows(read(root, board)):
            where = "%s:%d  %s" % (board, n, subject[:40])

            if status is None:
                if cited:
                    problems.append(
                        "%s\n    carries a citation and no status this can "
                        "read, so nothing checks it\n    -- which looks exactly "
                        "like a row that is being checked" % where)
                continue

            if not cited:
                if status in BLOCKED:
                    uncited += 1
                continue
            checked += 1
            missing = [c for c in cited if c not in known]
            if missing:
                problems.append("%s\n    cites an id that does not exist: %s"
                                % (where, ", ".join(missing)))
                continue

            open_ones = [c for c in cited if not known[c][0]]
            answered = [c for c in cited if known[c][0]]

            if status in BLOCKED and not open_ones:
                problems.append(
                    "%s\n    is marked blocked and everything it waits on has "
                    "landed: %s\n    the row is stale -- the work is possible "
                    "now" % (where, ", ".join(answered)))
            if status in BUILT and open_ones:
                problems.append(
                    "%s\n    is marked %s while what it waits on is still "
                    "open: %s\n    somebody reading this would start work that "
                    "cannot be finished" % (where, status,
                                            ", ".join(open_ones)))

    if problems:
        print("the board does not match what the kernel session has answered:")
        for p in problems:
            print("  " + p)
        return 1

    answered = sum(1 for v in known.values() if v[0])
    print("board: %d citations across %d kernel entries (%d answered, %d "
          "open), and every row agrees with them"
          % (checked, len(known), answered, len(known) - answered))
    if uncited:
        print("  note  %d blocked rows cite no kernel entry -- blocked on this "
              "role's own work, which this cannot check" % uncited)
    return 0


if __name__ == "__main__":
    sys.exit(main())
