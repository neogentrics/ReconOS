#!/usr/bin/env python3
"""
Check the README's suite table against what the suites actually did.

--- Why this exists ---

`server/README.md` carries a table of every suite and its check count, plus a
total in the summary box at the top. Both are typed by hand, which makes them a
**third list** beside `CMakeLists.txt` and the suites themselves — and this
project has already learned twice what happens to a list nobody derives.

`http_reason` and its suite each kept a list of statuses and drifted twice.
`scripts/server-tests.sh` exists because the suites were run from memory for
thirteen versions. This is the same fault in the documentation, and it had
already happened: on 17 September the table summed to 735 against a real 736,
because a row said 93 where the suite had grown to 94.

**Found by looking, which is not a method.** The userland session sent word the
same morning, through Joshua, about blunt search-and-replace edits — and the
gap they named was doing the careful thing for code and not for `docs/`, where a
bad edit is permanent and silent. This is that gap closed for the one document
that carries numbers.

--- What it checks ---

Every row of the table against the run it is describing, the total in the
summary box against the sum of the rows, and the suite count against how many
rows there are. A number in that README is now a number somebody derived.

Reads a summary written by `server-tests.sh`: one `name count` per line.
Exit 0 when the README agrees, 1 when it does not.
"""

import os
import re
import sys


def read_summary(path):
    """The run: {suite name: checks}."""
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            bits = line.split()
            if len(bits) == 2:
                out[bits[0]] = int(bits[1])
    return out


def read_table(readme):
    """The README's table: {suite name: checks}, in order."""
    rows = {}
    for m in re.finditer(r"^\|\s*`(server_[a-z_]+)`\s*\|\s*(\d+)\s*\|",
                         readme, re.M):
        rows[m.group(1)] = int(m.group(2))
    return rows


#
# The suite count is written out in words in the summary box, so it has to be
# read back as a number.
#
# **Built rather than listed.** The first version was a literal map that went
# `twelve` to `twenty`, which was every count that had existed when it was
# written; the twenty-first suite made it answer *which this cannot read as a
# number*, so the check that exists to stop the README drifting failed for its
# own vocabulary. A hand-kept list of the numbers that happen to have come up
# is exactly the shape this project has been bitten by three times.
#
UNITS = ("zero", "one", "two", "three", "four", "five", "six", "seven",
         "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
         "fifteen", "sixteen", "seventeen", "eighteen", "nineteen")
TENS = ("", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy",
        "eighty", "ninety")


def number_of(word):
    """The number a written-out count means, or None. 0 to 99."""
    word = word.strip().lower()

    if word in UNITS:
        return UNITS.index(word)
    if word in TENS:
        return TENS.index(word) * 10

    if "-" in word:
        tens, _, unit = word.partition("-")
        if tens in TENS and TENS.index(tens) >= 2 and unit in UNITS[1:10]:
            return TENS.index(tens) * 10 + UNITS.index(unit)
    return None


def main():
    if len(sys.argv) != 2:
        print("usage: check-readme-suites.py <summary-file>", file=sys.stderr)
        return 2

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    readme_path = os.path.join(root, "server", "README.md")

    ran = read_summary(sys.argv[1])
    if not ran:
        # Say which file and what was in it. A checker that reports "nothing
        # to check" without saying where it looked sends the reader to the
        # wrong place, which cost a round of debugging the first time this
        # ran.
        try:
            raw = open(sys.argv[1], encoding="utf-8").read()
        except OSError as e:
            raw = "(could not be read: %s)" % e
        print("suites: nothing to compare the README against -- %s held %d "
              "bytes: %r" % (sys.argv[1], len(raw), raw[:200]),
              file=sys.stderr)
        return 2

    with open(readme_path, encoding="utf-8") as f:
        readme = f.read()

    table = read_table(readme)
    problems = []

    for name, count in sorted(ran.items()):
        if name not in table:
            problems.append("%s ran with %d checks and is not in the table"
                            % (name, count))
        elif table[name] != count:
            problems.append("%s: the table says %d, the run gave %d"
                            % (name, table[name], count))

    for name in sorted(table):
        if name not in ran:
            problems.append("%s is in the table and did not run" % name)

    # The summary box at the top, which is the number anybody actually quotes.
    total = sum(ran.values())
    m = re.search(r"\*\*Checks\*\*\s*\|\s*(\d+) across ([\w-]+) suites", readme)
    if not m:
        problems.append("the summary box's check line could not be read")
    else:
        if int(m.group(1)) != total:
            problems.append("the summary box says %s checks, the suites gave %d"
                            % (m.group(1), total))

        said = number_of(m.group(2))
        if said is None:
            problems.append("the summary box says '%s' suites, which this "
                            "cannot read as a number" % m.group(2))
        elif said != len(ran):
            problems.append("the summary box says %s (%d) suites, %d ran"
                            % (m.group(2), said, len(ran)))

    if problems:
        print()
        print("the README does not match the run:")
        for p in problems:
            print("  " + p)
        return 1

    print("README: %d suites and %d checks, and the table agrees"
          % (len(ran), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
