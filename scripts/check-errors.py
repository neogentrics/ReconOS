#!/usr/bin/env python3
"""Every error code either has a site that raises it, or a reason it does not.

A code with no caller is a code nobody will ever see. It is documented, it is
in `errors`, somebody can look it up -- and nothing in the system can produce
it, so the entry describes a fault that cannot be reported.

**This exists because the number went stale rather than wrong.** Seven codes
were wired on 12 September in `2b45b53`, and the board went on saying they were
work for three days afterwards, because the figure beside them was counted once
by hand and nothing ever counted it again. The fault was not in the wiring; it
was that "34 of 43 reachable" was a sentence rather than a measurement.

--- It is checked in both directions, and that is the point ---

Refusing a code that has no site is the obvious half. The other half is
refusing an *excused* code that has since gained one: an exception list nobody
re-checks is exactly how a number goes stale, and this file would otherwise be
a second place for that to happen. So an excused code that is now raised is an
error here, and the fix is to delete its line.

--- What counts as a site ---

A `recon_error_raise` or `recon_error_raisef` call naming the code, in `src/`.
Not a mention in a comment, not a table, not a test -- a test can raise any
code it likes and proves nothing about whether the system ever does.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEF = os.path.join(HERE, 'include', 'recon_errors.def')
SRC = os.path.join(HERE, 'src')

# Codes with no site, and why. **A reason, not a name** -- a bare list is a
# list somebody adds to rather than argues with.
#
# Each of these is a decision about the system, and if the decision changes the
# line goes rather than the code being quietly wired.
EXCUSED = {
    'A006': 'the startup checks it reports on do not exist yet; there is no '
            'framework for a check that runs before the session and can fail',
    'E005': 'uninstalling is written not to fail -- it removes what is there '
            'and reports what was already gone, so there is no path that '
            'could raise this',
}


def defined():
    """Every code in the one file that defines them."""
    with open(DEF, encoding='utf-8') as f:
        text = f.read()

    # RECON_ERROR(B, 001, FAULT, "...", "...")
    found = re.findall(r'^\s*RECON_ERROR\(\s*([A-Z])\s*,\s*(\d{3})\s*,',
                       text, re.MULTILINE)
    return ['%s%s' % (a, n) for a, n in found]


def raised():
    """Every code something in src/ actually reports."""
    seen = set()
    want = re.compile(
        r'recon_error_raisef?\s*\([^;]*?\bRECON_ERR_([A-Z]\d{3})\b', re.S)

    for root, _dirs, files in os.walk(SRC):
        for name in files:
            if not name.endswith(('.c', '.h')):
                continue
            with open(os.path.join(root, name), encoding='utf-8',
                      errors='replace') as f:
                for code in want.findall(f.read()):
                    seen.add(code)

    return seen


def main():
    all_codes = defined()

    if not all_codes:
        sys.stderr.write('no codes found in %s -- the pattern this script '
                         'reads them with no longer matches the file\n' % DEF)
        return 1

    # A code defined twice is a code two faults answer to.
    twice = sorted({c for c in all_codes if all_codes.count(c) > 1})
    if twice:
        sys.stderr.write('these codes are defined more than once: %s\n'
                         % ', '.join(twice))
        return 1

    codes = sorted(set(all_codes))
    live = raised()

    unknown = sorted(live - set(codes))
    missing = sorted(c for c in codes if c not in live and c not in EXCUSED)
    stale = sorted(c for c in EXCUSED if c in live)
    gone = sorted(c for c in EXCUSED if c not in codes)

    bad = False

    if unknown:
        sys.stderr.write(
            'these codes are raised and are not defined in '
            'include/recon_errors.def: %s\n' % ', '.join(unknown))
        bad = True

    if missing:
        sys.stderr.write(
            'these codes have no site that raises them, and no reason '
            'recorded for why not: %s\n'
            'Either give each one a caller, or add it to EXCUSED in '
            'scripts/check-errors.py with the reason. A code nothing can '
            'produce is an entry describing a fault that cannot be '
            'reported.\n' % ', '.join(missing))
        bad = True

    if stale:
        sys.stderr.write(
            'these codes are excused from having a site and now have one: '
            '%s\n'
            'Delete their lines from EXCUSED. An exception nobody re-checks '
            'is how the figure this script replaced went stale.\n'
            % ', '.join(stale))
        bad = True

    if gone:
        sys.stderr.write(
            'EXCUSED names codes that no longer exist: %s\n' % ', '.join(gone))
        bad = True

    if bad:
        return 1

    print('%d of %d error codes have a site that raises them'
          % (len(live), len(codes)))

    for code in sorted(EXCUSED):
        print('  %s has none, on purpose -- %s' % (code, EXCUSED[code]))

    return 0


if __name__ == '__main__':
    sys.exit(main())
