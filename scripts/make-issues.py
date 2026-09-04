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
import re
import subprocess
import sys

REPO = 'neogentrics/ReconOS'

# The area for each entry, assigned by hand. A keyword rule got most of these
# wrong in ways that read as careless -- a build-warning bug labelled
# "accounts" is worse than no label, because somebody filtering by area then
# trusts the filter.
AREA = {
    1: 'display',  2: 'display',  3: 'display',  4: 'display',
    5: 'input',    6: 'display',  7: 'display',  8: 'input',
    9: 'display', 10: 'applications', 11: 'input', 12: 'input',
    13: 'display', 14: 'applications', 15: 'display', 16: 'display',
    17: 'input',  18: 'input',   19: 'input',   20: 'input',
    21: 'applications', 22: 'display', 23: 'storage', 24: 'startup',
    25: 'skins',  26: 'accounts', 27: 'display', 28: 'display',
    29: 'accounts', 30: 'input', 31: 'display', 32: 'network',
    33: 'skins',  34: 'display', 35: 'skins',   36: 'build',
    37: 'docs',   38: 'input',   39: 'build',   40: 'docs',
    41: 'skins',  42: 'skins',   43: 'display', 44: 'display',
    45: 'input',  46: 'help',    47: 'help',    48: 'help',
    49: 'help',   50: 'programs', 51: 'programs', 52: 'network',
    53: 'network', 54: 'docs',   55: 'programs', 56: 'help',
    57: 'build',  58: 'startup', 59: 'build',   60: 'settings',
    61: 'display', 62: 'help',   63: 'display', 64: 'input',
}


def existing_titles():
    out = subprocess.run(
        ['gh', 'issue', 'list', '--repo', REPO, '--state', 'all',
         '--limit', '500', '--json', 'title,number,state'],
        capture_output=True, text=True, check=True).stdout
    return {row['title']: row for row in json.loads(out)}


def parse(path):
    text = io.open(path, encoding='utf-8').read()
    # Everything from the first entry on; the preamble is not an entry.
    blocks = re.split(r'\n### (BG-\d+ — )', text)
    entries = []
    for i in range(1, len(blocks), 2):
        head = blocks[i]
        rest = blocks[i + 1]
        title_line, _, body = rest.partition('\n')
        # A trailing "---" or a following "## " heading ends the entry.
        body = re.split(r'\n---\n|\n## ', body)[0].strip()
        entries.append({
            'id': head[:6],
            'title': (head + title_line).strip(),
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

    area = AREA.get(int(entry['id'][3:]))
    if '**Documentation.**' in body:
        area = 'docs'
    if area is not None and area not in out:
        out.append(area)
    return out


def main():
    dry = '--dry-run' in sys.argv
    entries = parse('docs/BUGS.md')
    have = existing_titles()
    print(f'{len(entries)} entries, {len(have)} issues already there')

    for e in entries:
        if e['title'] in have:
            print(f"  {e['id']}  exists as #{have[e['title']]['number']}")
            continue

        closed = '**Fixed in**' in e['body']
        labels = labels_for(e)
        body = e['body'] + (
            '\n\n---\n\nFrom the register in '
            '[`docs/BUGS.md`](https://github.com/neogentrics/ReconOS/blob/main/docs/BUGS.md).'
        )

        if dry:
            print(f"  {e['id']}  {'CLOSE' if closed else 'open '}  "
                  f"{','.join(labels)}  {e['title'][:60]}")
            continue

        made = subprocess.run(
            ['gh', 'issue', 'create', '--repo', REPO,
             '--title', e['title'], '--body', body,
             '--label', ','.join(labels)],
            capture_output=True, text=True)
        if made.returncode != 0:
            print(f"  {e['id']}  FAILED: {made.stderr.strip()[:120]}")
            continue

        url = made.stdout.strip().splitlines()[-1]
        if closed:
            subprocess.run(
                ['gh', 'issue', 'close', url, '--repo', REPO,
                 '--reason', 'completed'],
                capture_output=True, text=True)
            print(f"  {e['id']}  closed   {url}")
        else:
            print(f"  {e['id']}  open     {url}")


main()
