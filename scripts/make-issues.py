#!/usr/bin/env python3
"""Turn docs/BUGS.md into GitHub issues.

The register is the source. Every entry becomes one issue with the same title
and the same body, so the two cannot drift apart by being written twice.

Run it from the top of the repository, with `gh` authenticated:

    python scripts/make-issues.py --dry-run    # what it would do
    python scripts/make-issues.py              # do it

Entries that already have an issue are left alone, so it is safe to run again
after adding one. An entry with a **Fixed in** line is created and then closed,
because the record is the point and a closed issue still carries its dates.

Labels come from `AREA` below, assigned by hand. A keyword rule got most of
them wrong in ways that read as careless -- a build-warning bug labelled
"accounts" is worse than no label, because somebody filtering by area then
trusts the filter. Add a line here when you add an entry.
"""
import io
import json
import pathlib
import re
import subprocess
import sys

REPO = 'neogentrics/ReconOS'

# The area for each entry, assigned by hand. A keyword rule got most of these
# wrong in ways that read as careless -- a build-warning bug labelled
# "accounts" is worse than no label, because somebody filtering by area then
# trusts the filter.
AREA = {
    # Keyed by the whole identifier, not by the number.
    #
    # It was keyed by the number while one sequence served both tracks. When
    # they split -- BG- for the desktop, KF- for the kernel, 12 September 2026
    # -- the two tables were merged and 44 numbers turned out to want different
    # areas on either side: BG-114 is a desktop fault about installing programs,
    # KF-114 is a crash harness that never cut the power. Keyed by number, one
    # of every such pair takes the other's label.
    #
    # Add a line here when adding an entry. AREA.get returns None for an
    # identifier with no line, labels_for then says nothing about the area, and
    # filtering by area misses it silently. That is the failure mode a lookup
    # table has, and this table has had it twice.
    'BG-001': 'display', 'BG-002': 'display', 'BG-003': 'display',
    'BG-004': 'display', 'BG-005': 'input', 'BG-006': 'display',
    'BG-007': 'display', 'BG-008': 'input', 'BG-009': 'display',
    'BG-010': 'applications', 'BG-011': 'input', 'BG-012': 'input',
    'BG-013': 'display', 'BG-014': 'applications', 'BG-015': 'display',
    'BG-016': 'display', 'BG-017': 'input', 'BG-018': 'input',
    'BG-019': 'input', 'BG-020': 'input', 'BG-021': 'applications',
    'BG-022': 'display', 'BG-023': 'storage', 'BG-024': 'startup',
    'BG-025': 'skins', 'BG-026': 'accounts', 'BG-027': 'display',
    'BG-028': 'display', 'BG-029': 'accounts', 'BG-030': 'input',
    'BG-031': 'display', 'BG-032': 'network', 'BG-033': 'skins',
    'BG-034': 'display', 'BG-035': 'skins', 'BG-036': 'build',
    'BG-037': 'docs', 'BG-038': 'input', 'BG-039': 'build',
    'BG-040': 'docs', 'BG-041': 'skins', 'BG-042': 'skins',
    'BG-043': 'display', 'BG-044': 'display', 'BG-045': 'input',
    'BG-046': 'help', 'BG-047': 'help', 'BG-048': 'help',
    'BG-049': 'help', 'BG-050': 'programs', 'BG-051': 'programs',
    'BG-052': 'network', 'BG-053': 'network', 'BG-054': 'docs',
    'BG-055': 'programs', 'BG-056': 'help', 'BG-057': 'build',
    'BG-058': 'startup', 'BG-059': 'build', 'BG-060': 'settings',
    'BG-061': 'display', 'BG-062': 'help', 'BG-063': 'display',
    'BG-064': 'input', 'BG-065': 'display', 'BG-066': 'display',
    'BG-067': 'display', 'BG-068': 'input', 'BG-069': 'build',
    'BG-070': 'help', 'BG-071': 'help', 'BG-072': 'settings',
    'BG-073': 'display', 'BG-074': 'storage', 'BG-075': 'input',
    'BG-076': 'firewall', 'BG-077': 'input', 'BG-078': 'display',
    'BG-079': 'skins', 'BG-080': 'display', 'BG-081': 'kernel',
    'BG-082': 'kernel', 'BG-083': 'kernel', 'BG-084': 'kernel',
    'BG-085': 'kernel', 'BG-086': 'kernel', 'BG-087': 'display',
    'BG-088': 'programs', 'BG-089': 'display', 'BG-090': 'build',
    'BG-091': 'display', 'BG-092': 'kernel', 'BG-093': 'kernel',
    'BG-094': 'kernel', 'BG-095': 'kernel', 'BG-096': 'applications',
    'BG-097': 'applications', 'BG-098': 'applications', 'BG-099': 'applications',
    'BG-100': 'display', 'BG-101': 'build', 'BG-102': 'build',
    'BG-103': 'kernel', 'BG-104': 'kernel', 'BG-105': 'kernel',
    'BG-106': 'display', 'BG-107': 'display', 'BG-108': 'skins',
    'BG-109': 'settings', 'BG-110': 'settings', 'BG-111': 'settings',
    'BG-112': 'input', 'BG-113': 'network', 'BG-114': 'programs',
    'BG-115': 'input', 'BG-116': 'applications', 'BG-117': 'input',
    'BG-118': 'input', 'BG-119': 'applications', 'BG-120': 'help',
    'BG-121': 'applications', 'BG-122': 'input', 'BG-123': 'help',
    'BG-124': 'applications', 'BG-125': 'storage', 'BG-126': 'display',
    'BG-127': 'display', 'BG-128': 'startup', 'BG-129': 'applications',
    'BG-130': 'display', 'BG-131': 'firewall', 'BG-132': 'applications',
    'BG-133': 'storage', 'BG-134': 'build', 'BG-135': 'settings',
    'BG-136': 'display', 'BG-137': 'display', 'BG-138': 'input',
    'BG-139': 'display', 'BG-140': 'display', 'BG-141': 'skins',
    'BG-142': 'display', 'BG-143': 'display', 'BG-144': 'skins',
    'BG-145': 'display', 'BG-146': 'display', 'BG-147': 'display',
    'BG-148': 'display', 'BG-149': 'display', 'BG-150': 'display',
    'BG-151': 'display', 'BG-152': 'skins', 'BG-153': 'skins',
    'BG-154': 'skins', 'BG-155': 'skins', 'BG-156': 'display',
    'BG-157': 'startup', 'BG-158': 'display', 'BG-159': 'display',
    'BG-160': 'applications', 'BG-161': 'applications', 'KF-114': 'build',
    'KF-115': 'build', 'KF-116': 'storage', 'KF-117': 'storage',
    'KF-118': 'storage', 'KF-119': 'storage', 'KF-120': 'storage',
    'KF-121': 'storage', 'KF-122': 'storage', 'KF-123': 'kernel',
    'KF-124': 'kernel', 'KF-125': 'kernel', 'KF-126': 'storage',
    'KF-127': 'storage', 'KF-128': 'startup', 'KF-129': 'kernel',
    'KF-130': 'storage', 'KF-131': 'startup', 'KF-132': 'startup',
    'KF-133': 'build', 'KF-134': 'build', 'KF-137': 'startup',
    'KF-138': 'startup', 'KF-139': 'build', 'KF-140': 'storage',
    'KF-141': 'startup', 'KF-142': 'startup', 'KF-143': 'build',
    'KF-144': 'build', 'KF-145': 'kernel', 'KF-146': 'kernel',
    'KF-147': 'kernel', 'KF-148': 'kernel', 'KF-149': 'kernel',
    'KF-150': 'kernel', 'KF-151': 'kernel', 'KF-152': 'kernel',
    'KF-153': 'kernel', 'KF-154': 'kernel', 'KF-155': 'kernel',
    'KF-156': 'kernel', 'KF-157': 'kernel', 'KF-158': 'kernel',
    'KF-159': 'kernel', 'KF-160': 'build', 'KF-161': 'storage',
    'KF-162': 'kernel', 'KF-163': 'storage', 'KF-164': 'kernel',
    'KF-165': 'kernel', 'KF-166': 'build', 'KF-179': 'kernel',
    'KF-180': 'kernel', 'KF-181': 'kernel', 'KF-182': 'kernel',
    'KF-183': 'kernel', 'KF-184': 'storage', 'KF-185': 'startup',
    'KF-186': 'storage', 'KF-187': 'storage', 'KF-188': 'storage',
    'KF-189': 'build', 'KF-190': 'kernel', 'KF-191': 'startup',
    'KF-192': 'storage', 'KF-193': 'kernel', 'KF-194': 'network',
    'KF-195': 'network', 'KF-196': 'kernel', 'KF-197': 'storage',
    'KF-198': 'storage', 'KF-199': 'kernel', 'KF-200': 'build', 'KF-201': 'network', 'KF-202': 'build',
    'KF-203': 'display', 'KF-204': 'kernel',
    'KF-205': 'build',

    # The graphics track, from 15 September 2026. All 'display': they are
    # faults in the display layer and the drivers under it, which is what that
    # label already means on the desktop side.
    'GX-001': 'display', 'GX-002': 'display', 'GX-003': 'display',
    'GX-004': 'display', 'GX-005': 'display', 'GX-006': 'display',
    'GX-007': 'display', 'GX-008': 'display', 'GX-009': 'display', 'GX-010': 'display',
    'KF-206': 'kernel',
    'KF-207': 'build',
    'KF-208': 'build',
    'KF-209': 'kernel',
    'KF-210': 'build',
    'KF-211': 'kernel',
    # Added by the desktop session on 14 September: both were left
    # without a line and make-bug-register.py refuses to run until every
    # entry has one. KF-212 is a stage 2 that no longer fits in the 64KB
    # real mode can address -- a size limit on a build artefact, which is
    # where 'build' has gone before; KF-213 is a test that could not read
    # the loader it was testing.
    'KF-212': 'build', 'KF-213': 'build',
    # Added by the desktop session, 14 September. Third time: the
    # kernel session writes the entry and this table is in a file the
    # desktop session owns, so an entry arrives with no line here and
    # make-bug-register.py refuses to run. Read off the titles.
    'KF-214': 'kernel',
    'KF-215': 'startup',
    'KF-216': 'startup',
    'KF-217': 'kernel',
    'KF-218': 'kernel',
    'KF-219': 'storage',
    'KF-221': 'kernel',
    'KF-222': 'storage',
    # Fourth time, 14 September. Read off the titles: KF-223 is USB root
    # ports asked what was attached before they had power, KF-224 is a
    # memory map read wrongly.
    'KF-223': 'kernel',
    'KF-224': 'kernel',
    # It parses now: its heading put three words between the number
    # and the dash, so make-bug-register.py counted the entry and
    # could not read it. 'display' because what it changes is what
    # reaches the panel.
    'KF-225': 'display',
    # The desktop session's own, same day. KF-226 is 'startup' rather than
    # 'storage' because the fault is in the scheduler and storage is only
    # where it showed: a driver that polls and yields is one caller of it,
    # and labelling it by the symptom would file a scheduling fault where
    # nobody looking for one would find it.
    'KF-226': 'startup',
    'KF-227': 'storage', 'KF-228': 'storage', 'KF-229': 'storage',
    'BG-193': 'storage',
    # The allocator and the two instruments that could not see it.
    # 'build' for the last two, where the toolchain-layer faults go --
    # BG-170 and BG-090 went there for the same reason.
    'BG-194': 'build', 'BG-195': 'build', 'BG-196': 'build',
    'BG-197': 'build',
    'BG-198': 'build', 'BG-199': 'build',
    'BG-200': 'build', 'BG-201': 'build',
    'BG-202': 'build', 'BG-203': 'build',
    'BG-204': 'build', 'BG-205': 'build',
    # And the kernel session's own three, renumbered on the merge.
    #
    # Both sessions reached KF-225 on the same day: this one had 225 as a BIOS
    # handoff fault, 226 as a kprintf width and 227 as a timer. All three are
    # 230, 231 and 232 here. **What is already on the shared branch keeps its
    # number** -- theirs was pushed to main, this was on a branch only one
    # session used, and renumbering the published side would have been the
    # expensive half of the same choice.
    'KF-230': 'startup',
    'KF-231': 'kernel',
    'KF-232': 'kernel',
    'KF-233': 'startup',
    'KF-234': 'kernel',
    'KF-235': 'kernel',
    'KF-236': 'kernel',
    'KF-237': 'storage',
    'KF-238': 'kernel',
    'KF-239': 'kernel',
    'KF-240': 'kernel',
    'KF-241': 'storage',
    'KF-242': 'kernel',
    'KF-243': 'kernel',
    'KF-244': 'network',
    'KF-245': 'startup',
    'KF-246': 'storage',
    'KF-247': 'docs',
    'KF-248': 'kernel',
    'KF-249': 'kernel',
    'KF-250': 'network',
    'KF-251': 'kernel',
    # Read off the entries' own titles when the registers were merged;
    # they had no line at all, which files them with no area label.
    'BG-162': 'applications', 'BG-163': 'applications', 'BG-164': 'applications',
    'BG-165': 'applications', 'BG-166': 'applications', 'BG-167': 'applications',
    'BG-168': 'applications', 'BG-169': 'applications', 'BG-170': 'build',
    'BG-171': 'applications', 'BG-172': 'display', 'BG-173': 'input',
    'BG-174': 'display', 'BG-175': 'help', 'BG-176': 'network',
    'BG-177': 'network', 'BG-178': 'network',
    # The C library. 'build' rather than 'kernel': these are faults in
    # the toolchain layer the desktop is compiled against, which is
    # where BG-090 and BG-170 went for the same reason.
    'BG-179': 'build', 'BG-180': 'build', 'BG-181': 'build',
    'BG-182': 'build', 'BG-183': 'build', 'BG-185': 'build',
    # A public header reaching for a system header it does not use.
    # 'storage' rather than 'build': the header is recon_fs.h and the
    # people who would filter for it are the ones who own that file.
    'BG-184': 'storage',
    # The maths library. 'build' with the rest of userland/.
    'BG-186': 'build', 'BG-187': 'build', 'BG-188': 'build',
    'BG-189': 'build',
    # The first screen. 'display' -- these are about what is drawn,
    # which is where somebody filtering for them would look.
    'BG-190': 'display', 'BG-191': 'display', 'BG-192': 'display',
}

def is_fixed(body):
    """
    Does the register say this one is fixed?

    Four phrasings, because the register grew four over two hundred and fifty
    entries, and every narrowing of this function has cost the same thing:
    entries filed as open bugs on a public tracker when the register said they
    were done.

    - `**Fixed in** <version>`, where the version is the point
    - `**Fixed by** <what was done>`, where the change is. Asking for the first
      only filed nine entries as open, including the player's clock and three
      kernel faults.
    - `**Fixed in:**`, the same field with the colon inside the emphasis
    - a `**Status:**` line saying it in prose, which is the later convention

    The Status line wins where there is one: it is the most explicit, and it is
    the only form that can say a thing this register needs to say. *Half fixed*
    reads as open, because it is -- KF-127 says exactly that and means it.
    """
    # The third state, and it is matched as a whole phrase on purpose.
    #
    # Some entries are neither open nor fixed: KF-225 is a question that was
    # asked and answered, recorded so nobody re-derives it, with nothing
    # broken. Before this it read as open for ever -- the checker said so on
    # every run, which is how a check trains people to ignore it.
    #
    # `not a bug` in full, never a prefix. `startswith('not')` would swallow
    # **Status:** not fixed and quietly close the issue for a live fault, which
    # is the one mistake this function must not make.
    if re.search(r'\*\*Status:\*\*\s*\**\s*not a bug\b', body, re.I):
        return True

    said = re.search(r'\*\*Status:\*\*\s*\**\s*([A-Za-z]+)', body)
    if said:
        return said.group(1).lower().startswith('fix')
    return bool(re.search(r'\*\*Fixed (?:in|by):?\*\*', body))


def gh(args):
    """
    Run gh, decoding its output as UTF-8.

    Explicitly, rather than letting `text=True` pick the platform default.
    On Windows that default is the locale encoding, which turned every em
    dash in an issue title into mush -- so no title ever matched the register
    and a second run created a duplicate of all sixty-two. A hundred and
    twenty-six of them, before it was noticed.
    """
    return subprocess.run(['gh'] + args, capture_output=True,
                          encoding='utf-8', errors='replace')


def existing_titles():
    out = gh(['issue', 'list', '--repo', REPO, '--state', 'all',
              '--limit', '500', '--json', 'title,number,state,labels'])
    if out.returncode != 0:
        raise SystemExit(f'gh issue list failed: {out.stderr.strip()}')

    rows = json.loads(out.stdout)
    titles = {row['title']: row for row in rows}

    """
    A sanity check, because the failure this guards against is silent: if the
    listing came back mangled, every title looks new and the run makes a
    second copy of the whole register.
    """
    if rows and not any(t.startswith(('BG-', 'KF-')) for t in titles):
        raise SystemExit(
            'The issue list came back with no BG-/KF- titles in it. Refusing '
            'run: this is what a decoding fault looks like, and continuing '
            'would duplicate every entry.')

    return titles


# An entry heading, and every way one has actually been typed.
#
# `--` was added after fourteen entries written that way were *not seen* --
# not skipped with a warning, not counted as unparsed, not reported at all. A
# bare `-` is added now after twenty-one more went the same way (KF-247). That
# is the same fault twice, so the spellings are written once here and used by
# everything in this file that needs to know what an entry looks like.
#
# The separator is matched *after* the number, never inside it. A pattern
# allowing a bare `-` with optional spaces on either side will happily match
# the hyphen in `KF-246` itself, given the chance -- and the chance is a regex
# that does not say where to start.
# What an entry's identifier looks like -- **two capitals and a number, not a
# list of the tracks that exist today.**
#
# It was a list twice. `GX-` arrived with the graphics branch and had to be
# added to five separate literals spread through this file; collapsing those
# into one constant was right and did not go far enough, because the constant
# was still `(?:BG|KF|GX)` and `NW-` and `BT-` are already written on two other
# branches waiting to merge.
#
# The blind spot that leaves is the interesting part. The check at the bottom
# of `parse` compares what the parser found against a second, looser count --
# and if **both** are built from a list of prefixes, an unknown track is
# invisible to the parser *and* to the thing watching the parser, and the run
# reports a register it cannot see all of. A second opinion drawn from the same
# assumption is not a second opinion.
#
# `check-readme-badges.py` had the same fault with a comment above it claiming
# the opposite -- "every prefix, not a list of the ones that existed when this
# was written", directly above that list. Both are derived now.
ENTRY_ID = r'[A-Z]{2}-\d+'

ENTRY_HEAD = r'### (' + ENTRY_ID + r') *(?:—|–|--|-) *'

# The same heading with no separator required and no anchor, so a caller that
# needs to match mid-string gets it from here rather than slicing a `^` off
# ENTRY_ANY -- a slice that keeps working right up until the pattern's shape
# changes, and then stops silently.

# What an entry heading is when you are only counting them. Deliberately
# looser than ENTRY_HEAD; the gap between the two is what the check below
# measures.
ENTRY_NUM = r'### (' + ENTRY_ID + r')'
ENTRY_ANY = r'^' + ENTRY_NUM


def parse(path):
    text = io.open(path, encoding='utf-8').read()
    # Everything from the first entry on; the preamble is not an entry.
    blocks = re.split('\n' + ENTRY_HEAD, text)
    entries = []
    for i in range(1, len(blocks), 2):
        num = blocks[i]
        rest = blocks[i + 1]
        title_line, _, body = rest.partition('\n')
        # A trailing "---" or a following "## " heading ends the entry.
        body = re.split(r'\n---\n|\n## ', body)[0].strip()
        entries.append({
            'id': num,
            # However the separator was typed, the title carries an em dash:
            # every issue title on GitHub has one, so a heading written four
            # different ways still finds its own issue instead of filing a
            # second one beside it.
            'title': '%s — %s' % (num, title_line.strip()),
            'body': body,
        })

    # --- the check this file was missing twice ---------------------------
    #
    # Both faults above were invisible for one reason: **the filer counted the
    # entries its own parser had found**, so the count could never disagree
    # with the parser. "302 entries" was true of the parser and false of the
    # file, and no green run could have said so.
    #
    # Counting a second way is the fix, and it is not the same as a stricter
    # parser -- a stricter parser has the identical blind spot pointed
    # somewhere else. It refuses rather than warns, because a warning inside a
    # run that prints three hundred lines is a warning nobody reads.
    #
    # `check_open` below has always split on ENTRY_ANY. So the two halves of
    # this one file have disagreed about what an entry *is* for as long as
    # both have existed, and neither could see the other.
    headings = re.findall(ENTRY_ANY, text, flags=re.M)
    if len(headings) != len(entries):
        found = set(e['id'] for e in entries)
        lost = [h for h in headings if h not in found]
        raise SystemExit(
            '%s has %d entry headings and %d of them parse.\n'
            'Unparsed, and so invisible to every issue this script files:\n'
            '  %s\n'
            'The heading separator must be an em dash, an en dash, `--` or '
            '`-`.\n'
            'Refused rather than warned about: a register nobody can see all '
            'of\nis not a smaller register, it is a wrong one.'
            % (path, len(headings), len(entries), '\n  '.join(lost)))

    return entries


def labels_for(entry):
    out = ['bug']
    body = entry['body']
    if '**Security.**' in body:
        out.append('security')
    if re.search(r'regression', body, re.I):
        out.append('regression')

    area = AREA.get(entry['id'])
    if '**Documentation.**' in body:
        area = 'docs'
    if area is not None and area not in out:
        out.append(area)
    return out


def link_into_register(bg, number):
    """
    Put the issue link under its heading in docs/BUGS.md.

    Written by the script rather than by hand, because by hand it was written
    *wrong*: two entries cited numbers guessed at ahead of the run instead of
    read back from it, and `gh issue view` on both replied "Could not resolve".
    A register whose links do not resolve is worse than one with no links,
    because a number reads as evidence that somebody checked.
    """
    path = pathlib.Path('docs/BUGS.md')
    text = path.read_text(encoding='utf-8')
    i = text.find('### ' + bg + ' ')
    if i < 0:
        return False

    j = text.index('\n', i) + 1
    if text[j:j + 120].lstrip('\n').startswith('[#'):
        return False

    link = '\n' + '[#%s](https://github.com/%s/issues/%s)%s' % (
        number, REPO, number, '\n')
    text = text[:j] + link + text[j:]
    path.write_bytes(text.encode('utf-8').replace(b'\r\n', b'\n'))
    return True


def check_links():
    """
    Every link in the register must resolve to an issue whose title is that
    entry's title.

    Nothing else here can catch a link that is simply wrong: creation skips an
    entry whose title it already sees on GitHub, so a bad number is never looked
    at again. Run after any hand-editing of the register.
    """
    text = pathlib.Path('docs/BUGS.md').read_text(encoding='utf-8')
    out = gh(['issue', 'list', '--repo', REPO, '--state', 'all',
              '--limit', '500', '--json', 'number,title'])
    if out.returncode != 0:
        print('could not list issues: ' + out.stderr.strip()[:160])
        return 2

    titles = {i['number']: i['title'] for i in json.loads(out.stdout)}
    linked = wrong = missing = 0

    for m in re.finditer(ENTRY_ANY + r' .*$', text, re.M):
        bg = m.group(1)
        after = text[m.end():m.end() + 200].lstrip('\n')
        cite = re.match(r'\[#(\d+)\]', after)

        if not cite:
            print('  %s  no issue link' % bg)
            missing += 1
            continue

        linked += 1
        n = int(cite.group(1))
        have = titles.get(n)
        if have is None:
            print('  %s  cites #%d, which does not exist' % (bg, n))
            wrong += 1
        elif not have.startswith(bg):
            print('  %s  cites #%d, which is %r' % (bg, n, have[:50]))
            wrong += 1

    print('%d links checked, %d wrong, %d entries unlinked'
          % (linked, wrong, missing))
    return 1 if wrong else 0


def check_open(text):
    """
    The `## Open` section is a sentence kept beside the data that would
    contradict it, and it had drifted: it said "None. Every bug below was found
    and closed" while five entries said they were open. Checked here rather
    than trusted, because every count in this project that was remembered
    instead of derived has drifted the same way.
    """
    head = re.search(r'^## Open$(.*?)^---$', text, re.M | re.S)
    if not head:
        print('  no ## Open section to check')
        return 0

    open_now = []
    for block in re.split(r'(?=' + ENTRY_ANY + r')', text, flags=re.M):
        m = re.match(ENTRY_NUM, block)
        if m and not is_fixed(block.split('\n## ')[0]):
            open_now.append(m.group(1))

    named = set(re.findall(ENTRY_ID, head.group(1)))
    missing = [b for b in open_now if b not in named]
    extra = [b for b in named if b not in open_now]

    for b in missing:
        print('  %s is open and the Open section does not say so' % b)
    for b in extra:
        print('  %s is named as open and its own entry says otherwise' % b)
    if not missing and not extra:
        print('  Open names all %d open entries and no others' % len(open_now))
    return 1 if (missing or extra) else 0


USAGE = """usage: make-issues.py [--dry-run | --check]

  --dry-run   say what would be created, closed and relabelled
  --check     verify the register against itself and against the issues
  (none)      create, close and relabel issues on %s

Run from the top of the repository, with `gh` authenticated.
""" % REPO


def main():
    """
    An argument this script does not recognise is a **refusal**, not a run.

    The default mode writes to a public issue tracker -- it creates issues,
    closes them and edits their labels -- and every other mode is read-only.
    So "no recognised flag" and "no flag at all" used to mean the same thing,
    and `--help`, which this file did not implement, fell through to the one
    branch that mutates something outside the repository.

    That is NW-009, and it happened: eight issues were created by a command
    typed to ask what the options were. The result was correct, because the
    register was correct -- which is exactly what makes it worth a guard. A
    mistake that produces the right answer teaches nothing and repeats.

    The rule this settles: **a tool whose default is the side-effecting mode
    must treat an unknown argument as a question, not as consent.**
    """
    known = ('--dry-run', '--check', '--help', '-h')
    unknown = [a for a in sys.argv[1:] if a not in known]

    if unknown:
        sys.stderr.write('unrecognised: %s\n\n' % ' '.join(unknown))
        sys.stderr.write(USAGE)
        sys.exit(2)

    if '--help' in sys.argv or '-h' in sys.argv:
        sys.stdout.write(USAGE)
        sys.exit(0)

    dry = '--dry-run' in sys.argv
    if '--check' in sys.argv:
        text = pathlib.Path('docs/BUGS.md').read_text(encoding='utf-8')
        sys.exit(check_links() | check_open(text))

    entries = parse('docs/BUGS.md')
    have = existing_titles()
    print(f'{len(entries)} entries, {len(have)} issues already there')

    for e in entries:
        if e['title'] in have:
            row = have[e['title']]
            '''
            An entry that already has an issue is left alone -- except when
            the register says it is fixed and the issue is open, which is the
            two drifting apart, and this file exists to stop that. Closing to
            match is the contract: the register is the source.
            '''
            note = []
            if is_fixed(e['body']) and row['state'] == 'OPEN':
                note.append('close')
                if not dry:
                    gh(['issue', 'close', str(row['number']), '--repo', REPO,
                        '--reason', 'completed'])

            '''
            Labels too, for the same reason and one of its own: an area is
            assigned by hand in AREA below, so an entry filed before its line
            was added carries only "bug" forever. Filtering by area then
            quietly misses it, which is worse than the label being absent --
            the filter looks like it worked.
            '''
            want = set(labels_for(e))
            has = {label['name'] for label in row.get('labels', [])}
            if want - has:
                note.append('label ' + ','.join(sorted(want - has)))
                if not dry:
                    gh(['issue', 'edit', str(row['number']), '--repo', REPO,
                        '--add-label', ','.join(sorted(want - has))])

            if note:
                print(f"  {e['id']}  #{row['number']}: "
                      f"{'would ' if dry else ''}{'; '.join(note)}")
            else:
                print(f"  {e['id']}  exists as #{row['number']}")
            continue

        closed = is_fixed(e['body'])
        labels = labels_for(e)
        body = e['body'] + (
            '\n\n---\n\nFrom the register in '
            '[`docs/BUGS.md`](https://github.com/neogentrics/ReconOS/blob/main/docs/BUGS.md).'
        )

        if dry:
            print(f"  {e['id']}  {'CLOSE' if closed else 'open '}  "
                  f"{','.join(labels)}  {e['title'][:60]}")
            continue

        made = gh(['issue', 'create', '--repo', REPO,
                   '--title', e['title'], '--body', body,
                   '--label', ','.join(labels)])
        if made.returncode != 0:
            print(f"  {e['id']}  FAILED: {made.stderr.strip()[:120]}")
            continue

        url = made.stdout.strip().splitlines()[-1]
        link_into_register(e['id'], url.rsplit('/', 1)[-1])
        if closed:
            gh(['issue', 'close', url, '--repo', REPO,
                '--reason', 'completed'])
            print(f"  {e['id']}  closed   {url}")
        else:
            print(f"  {e['id']}  open     {url}")


main()
