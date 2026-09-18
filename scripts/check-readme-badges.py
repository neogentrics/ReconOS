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

--- the check count, and the third option ------------------------------------

This script used to leave the check count alone, for a reason that was right as
far as it went: it is a figure from a run, not a thing a file contains, and
reading it from *a log lying around* would be worse than leaving it, because
the log might be from any commit.

**The option it did not take was doing the run.** A figure unavailable to
something that only reads files is perfectly available to something willing to
execute the suites and read what they say. `--run` does that, and writes the
number it just watched happen -- no log, no cache, no trusting a build
directory to be from this commit rather than checking.

What it cost to leave alone: on 15 September 2026 the badge said **1,971
checks** and a run said **4,464,141**. It had drifted by three orders of
magnitude while sitting beside a note explaining why nobody was checking it --
the same fault this script exists to fix, one field to the right. *A count
nobody can check should be visibly uncheckable*; a count somebody could have
checked by running it is just unchecked.

--- what it still does not do -----------------------------------------------

Without `--run` the check count is left exactly as before and reported as a
figure from a run, because guessing is still worse than admitting. `--run`
needs a built tree; where there is not one, it says so and changes nothing.
"""
import subprocess
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read(path):
    return io.open(os.path.join(ROOT, path), encoding="utf-8",
                   newline="").read()


def bug_counts():
    """How many entries the register actually holds, by prefix."""
    heads = re.findall(r"^### (BG|KF)-\d+", read("docs/BUGS.md"), re.M)
    return {"BG": heads.count("BG"), "KF": heads.count("KF")}


def suite_count():
    """How many suites there are, which the tree does know.

    `add_test(` in CMakeLists.txt, counted. The check count beside it in the
    same badge genuinely cannot be derived -- it is a figure from a run -- but
    this one is a fact the repository holds, and it had drifted from 34 to 45
    while sitting next to a number that excused it.
    """
    try:
        text = read("CMakeLists.txt")
    except IOError:
        # None rather than zero, which is the kernel session's point and a
        # good one: a tree with no CMakeLists has no answer, and that is a
        # different fact from a tree with no tests. Zero would have been
        # written into the badge as though it were measured.
        return None

    # Anchored at the start of a line. `add_test(` counted anywhere would also
    # count the one in this comment, and every mention in a commented-out
    # block.
    return len(re.findall(r"^add_test\(", text, re.M))


def checks_from_a_run():
    """Run every suite and total what they report. None if it cannot.

    Each suite prints its own `N checks, M failures` line and this adds them
    up, rather than counting `check(` in the sources -- because a check inside
    a branch that is never taken is a line in a file, not a check that ran, and
    the whole point of this figure is that it is a thing that happened.

    **"cases" counts as well as "checks".** The malformed-input suite ends
    `10788 cases, 0 failures` because what it runs are inputs rather than
    assertions, and matching only the one noun left the total short by an
    entire suite without saying so.

    Which is why the number of suites that reported comes back too: the caller
    holds it against the number that exist, so a suite that stops reporting
    makes the figure complain rather than quietly shrink.
    """
    build = os.path.join(ROOT, "build")
    if not os.path.isdir(build):
        return None

    try:
        out = subprocess.run(
            ["ctest", "--test-dir", build, "-j8", "-V"],
            capture_output=True, text=True, timeout=1800).stdout
    except (OSError, subprocess.SubprocessError):
        return None

    lines = re.findall(r"(\d+) (?:checks|cases), (\d+) failures", out)
    if not lines:
        return None

    checks = sum(int(c) for c, _ in lines)
    failures = sum(int(f) for _, f in lines)
    return checks, failures, len(lines)


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
    run = "--run" in sys.argv
    readme = read("README.md")
    problems = []
    fixed = readme

    bugs = bug_counts()
    total = bugs["BG"] + bugs["KF"]
    dv = desktop_version()

    # --- the bug count -------------------------------------------------
    m = re.search(r"bugs_recorded-(\d+)-", readme)
    if not m:
        problems.append("no bug-count badge found; has the README changed shape?")
    elif int(m.group(1)) != total:
        problems.append("bugs badge says %s, the register holds %d (%d BG + %d KF)"
                        % (m.group(1), total, bugs["BG"], bugs["KF"]))
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

        # --- and the one written in words ------------------------------
        #
        # "Where it is at, and why" opens by saying the version out loud,
        # because a badge answers *which* and a sentence answers *why it is
        # that one*. A number in prose drifts the first time somebody ships
        # without rereading the paragraph -- which is every time -- so it is
        # checked here rather than left to be noticed.
        m = re.search(r"^\*\*v([\d.]+)\.\*\*", readme, re.M)
        if not m:
            problems.append("no version sentence found; has 'Where it is at' "
                            "changed shape?")
        elif m.group(1) != dv:
            problems.append("the version sentence says v%s, the change log's "
                            "newest is %s" % (m.group(1), dv))
        fixed = re.sub(r"^(\*\*v)[\d.]+(\.\*\*)", r"\g<1>%s\g<2>" % dv,
                       fixed, count=1, flags=re.M)

    # --- the suite count from the tree, the check count from a run -----
    #
    # The suite count is `add_test(` counted, which the repository knows. The
    # check count is a figure from a run -- so with `--run` this does the run
    # rather than guessing, and without it says plainly that it did not.
    #
    # Plain digits, no separators: the first version wrote a URL-encoded comma
    # so the badge would render `4,464,141`, and this pattern then could not
    # read it back -- the field went unchecked while the script reported
    # everything as matching. A number its own checker cannot parse is a number
    # nobody is checking, which is the fault this file was written against.
    m = re.search(r"tests-(\d+)_suites,_(\d+)_checks", readme)
    if m:
        suites = suite_count()

        # A tree with no CMakeLists answers None rather than zero, and neither
        # comparing against it nor writing it into the badge would mean
        # anything. Left alone instead, which is what "no answer" should do.
        if suites is None:
            problems.append("there is no CMakeLists.txt to count suites in")
            suites = int(m.group(1))

        if int(m.group(1)) != suites:
            problems.append("tests badge says %s suites, CMakeLists.txt "
                            "registers %d" % (m.group(1), suites))
        fixed = re.sub(r"(tests-)\d+(_suites,)", r"\g<1>%d\g<2>" % suites,
                       fixed)

        was = int(m.group(2).replace(",", ""))
        ran = checks_from_a_run() if run else None

        if ran is None:
            if run:
                print("  the run could not happen -- no build directory, or "
                      "no suite reported a count; the check count is left "
                      "alone")
            else:
                print("  note: the tests badge's %s checks is a figure from a "
                      "run; pass --run to make the run and write what it says"
                      % m.group(2))
        else:
            checks, failures, reporting = ran
            if reporting != suites:
                problems.append("%d of %d suites reported a count; a total "
                                "missing a suite is not a total"
                                % (reporting, suites))
            if failures:
                problems.append("the run had %d failures, so its count is not "
                                "a figure to put on a badge" % failures)
            else:
                if checks != was:
                    problems.append("tests badge says %s checks, a run of %d "
                                    "suites reports %d"
                                    % (m.group(2), reporting, checks))
                fixed = re.sub(r"(_suites,_)\d+(_checks)",
                               r"\g<1>%d\g<2>" % checks, fixed)
                print("  the check count is from a run: %s checks across %d "
                      "suites, 0 failures" % ("{:,}".format(checks),
                                              reporting))

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
