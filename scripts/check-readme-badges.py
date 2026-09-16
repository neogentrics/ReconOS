#!/usr/bin/env python3
"""The README's badges, checked against the tree that produced them.

--- why this exists ---------------------------------------------------------

The badges said **version 0.4.0** and **135 bugs recorded** on 15 September
2026, when the desktop was at 0.4.37 and the register held 311 entries. They
are the first thing anybody sees, and they had been wrong for weeks.

Nobody had been careless. **Nothing was counting.** Each number was measured
once by hand and typed in, which makes it a sentence rather than a
measurement -- and a sentence cannot notice that it has stopped being true.
The desktop session found the identical fault on its checkpoint board the same
morning: *a figure on a board is a claim, and a claim needs an instrument.*

So this is the instrument. It reads the numbers out of the tree and compares
them against what the README asserts, and `--fix` writes them back.

--- one number it will not invent ---------------------------------------------

`1971 checks` cannot be derived from the repository -- it is a figure from a
run, and a run is a thing that happened rather than a thing a file contains.
Reading it from whatever log was lying around would be worse than leaving it:
the log might be from any commit, and a badge sourced that way lies quietly
instead of loudly. It is printed as unchecked on every run, so it is visibly
uncheckable rather than silently green.

**The suite count beside it is a different matter, and this file said
otherwise for a day.** It claimed both halves were underivable and checked
neither -- while `add_test(` in CMakeLists.txt was sitting there countable, and
the badge drifted 34 to 45 behind a note that excused looking. The desktop
session split the two and was right to.

*A wrong claim of impossibility is worse than no claim*, because it stops the
next person looking. Kept as a comment rather than quietly corrected, since
this file exists to argue that unchecked numbers rot.
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Every prefix the register uses. Kept in the same shape as the list in
# `scripts/make-issues.py` and for the same reason: a prefix spelled into a
# regex is a prefix that every new session edits in place, and two sessions
# claiming one at the same time then conflict on the line rather than on the
# list. Add a prefix here.
#
# Counting by prefix rather than summing two named ones also means a track
# whose prefix nobody added is *missing from the total*, which the badge then
# reports as a mismatch. That is the loud failure. Summing `BG` and `KF` by
# name was the quiet one: `GX-` entries existed on `graphics` and this file
# counted none of them while reporting itself green.
PREFIXES = ("BG", "KF", "GX", "BT")


def read(path):
    return io.open(os.path.join(ROOT, path), encoding="utf-8",
                   newline="").read()


def bug_counts():
    """How many entries the register actually holds, by prefix.

    Every prefix the register carries, not a named two -- and any heading that
    looks like an entry but carries a prefix nobody registered is returned
    under its own key too, so that it is visible rather than uncounted.
    """
    heads = re.findall(r"^### ([A-Z]{2})-\d+", read("docs/BUGS.md"), re.M)
    counts = dict((p, 0) for p in PREFIXES)

    for h in heads:
        counts[h] = counts.get(h, 0) + 1

    return counts


def suite_count():
    """How many test suites CMake registers.

    `add_test(`, counted. This was reported as not derivable along with the
    check count beside it, and only one of the two actually is -- the desktop
    session caught that, having watched the number drift from 34 to 45 behind
    a note that excused looking at it.

    Returns None where there is no CMakeLists to read, rather than zero: a
    tree without one has no answer, which is a different fact from a tree with
    no tests.
    """
    try:
        return read("CMakeLists.txt").count("add_test(")
    except IOError:
        return None


def desktop_version():
    """The desktop's version, which is what the top badges are about.

    From `docs/CHANGELOG.md`'s newest heading rather than from a build file,
    because the desktop is a CMake project whose version lives in several
    places and the change log is the one a person reads.
    """
    m = re.search(r"^##\s*(?:v)?(\d+\.\d+\.\d+)", read("docs/CHANGELOG.md"), re.M)
    return m.group(1) if m else None


def kernel_version():
    m = re.search(r"^VERSION := (\S+)", read("kernel/Makefile"), re.M)
    return m.group(1) if m else None


def main():
    fix = "--fix" in sys.argv
    readme = read("README.md")
    problems = []
    fixed = readme

    bugs = bug_counts()
    total = sum(bugs.values())
    dv = desktop_version()

    # --- the bug count -------------------------------------------------
    m = re.search(r"bugs_recorded-(\d+)-", readme)
    if not m:
        problems.append("no bug-count badge found; has the README changed shape?")
    elif int(m.group(1)) != total:
        breakdown = ", ".join("%d %s" % (n, p)
                              for p, n in sorted(bugs.items()) if n)
        problems.append("bugs badge says %s, the register holds %d (%s)"
                        % (m.group(1), total, breakdown))
        fixed = re.sub(r"(bugs_recorded-)\d+(-)", r"\g<1>%d\g<2>" % total, fixed)

    # --- the version, in both badges -----------------------------------
    if dv:
        for pat, what in ((r"badge/version-([\d.]+)-", "version"),
                          (r"latest_release-v([\d.]+)-", "release")):
            m = re.search(pat, readme)
            if m and m.group(1) != dv:
                problems.append("%s badge says %s, the change log's newest is %s"
                                % (what, m.group(1), dv))
        fixed = re.sub(r"(badge/version-)[\d.]+(-)", r"\g<1>%s\g<2>" % dv, fixed)
        fixed = re.sub(r"(latest_release-v)[\d.]+(-)", r"\g<1>%s\g<2>" % dv, fixed)
        fixed = re.sub(r"(releases/tag/v)[\d.]+(\))", r"\g<1>%s\g<2>" % dv, fixed)

    # --- the tests badge, which is two numbers and only one of them
    # --- can be checked here ------------------------------------------
    m = re.search(r"tests-(\d+)_suites,_(\d+)_checks", readme)
    if m:
        suites = suite_count()

        if suites is not None and int(m.group(1)) != suites:
            problems.append("tests badge says %s suites, CMakeLists registers "
                            "%d" % (m.group(1), suites))
        if suites is not None:
            fixed = re.sub(r"(tests-)\d+(_suites)",
                           r"\g<1>%d\g<2>" % suites, fixed)

        print("  note: the badge's %s checks is a figure from a run, not from "
              "the tree, so it stays unchecked -- the suite count beside it "
              "is not" % m.group(2))

    if fix and fixed != readme:
        io.open(os.path.join(ROOT, "README.md"), "w", encoding="utf-8",
                newline="").write(fixed)
        print("  rewrote the badges from the tree")
        return 0

    if problems:
        for p in problems:
            print("  README badge: " + p)
        print("  run scripts/check-readme-badges.py --fix")
        return 1

    print("  the README's badges match the tree (%d bugs, version %s, "
          "kernel %s)" % (total, dv, kernel_version()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
