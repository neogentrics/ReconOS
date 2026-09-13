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
    'KF-198': 'storage', 'KF-199': 'kernel', 'KF-200': 'build',
    # Read off the entries' own titles when the registers were merged;
    # they had no line at all, which files them with no area label.
    'BG-162': 'applications', 'BG-163': 'applications', 'BG-164': 'applications',
    'BG-165': 'applications', 'BG-166': 'applications', 'BG-167': 'applications',
    'BG-168': 'applications', 'BG-169': 'applications', 'BG-170': 'build',
    'BG-171': 'applications', 'BG-172': 'display', 'BG-173': 'input',
    'BG-174': 'display', 'BG-175': 'help', 'BG-176': 'network',
    'BG-177': 'network', 'BG-178': 'network',
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


def parse(path):
    text = io.open(path, encoding='utf-8').read()
    # Everything from the first entry on; the preamble is not an entry.
    blocks = re.split(r'\n### ((?:BG|KF)-\d+ *(?:—|–|--) )', text)
    entries = []
    for i in range(1, len(blocks), 2):
        head = blocks[i]
        rest = blocks[i + 1]
        title_line, _, body = rest.partition('\n')
        # A trailing "---" or a following "## " heading ends the entry.
        body = re.split(r'\n---\n|\n## ', body)[0].strip()
        entries.append({
            'id': head[:6],
            # The heading may have been typed with `--`. Every issue
            # title on GitHub uses an em dash, so one form reaches the
            # filer however the entry was written.
            'title': re.sub(r' *(?:—|–|--) *',
                            ' — ', (head + title_line).strip(),
                            count=1),
            'body': body,
        })
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

    for m in re.finditer(r'^### ((?:BG|KF)-\d+) .*$', text, re.M):
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
    for block in re.split(r'(?=^### (?:BG|KF)-)', text, flags=re.M):
        m = re.match(r'### ((?:BG|KF)-\d+)', block)
        if m and not is_fixed(block.split('\n## ')[0]):
            open_now.append(m.group(1))

    named = set(re.findall(r'(?:BG|KF)-\d+', head.group(1)))
    missing = [b for b in open_now if b not in named]
    extra = [b for b in named if b not in open_now]

    for b in missing:
        print('  %s is open and the Open section does not say so' % b)
    for b in extra:
        print('  %s is named as open and its own entry says otherwise' % b)
    if not missing and not extra:
        print('  Open names all %d open entries and no others' % len(open_now))
    return 1 if (missing or extra) else 0


def main():
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
