#!/usr/bin/env python3
"""What this system records and never acts on.

--- Why this exists ---

`required` was read out of every page a browser loaded, stored on the field,
and **nothing anywhere ever looked at it again**. A page said a box had to be
filled in; somebody left it empty; the form went; the server refused it and
answered with a page naming the field. They were told, eventually, by a round
trip, in whatever words the server chose -- and this machine had the answer
before anything was sent.

That was not found by reading. It was found by listing every member of
`struct recon_html_field` and asking which ones nothing outside the parser
mentions. It was the only one with nobody.

**A gap with a note beside it is a gap somebody chose.** This looks for the
other kind: something the system knows, fills in, carries around, and never
acts on. There is no comment saying "not yet", because nobody ever decided.

--- What it can and cannot tell, and how it got there ---

C offers no types here, only text, so `->thing` has to be matched wherever it
appears. The first version of this script did that with two crude filters and
reported four findings; **the first one checked was wrong**. `usage` on a
command registration is read by `src/recon_cmd.c`, which reaches the struct
through `recon_modules.h` rather than by including `recon_module.h` itself.

So includes are followed transitively here. That one fix turned all four of
those findings into nothing, which is the right answer and is why the script is
worth having rather than the list it first printed.

The second filter is about shared names. `->name` proves nothing on its own,
and discarding every shared name threw away most of the question -- 154 of
them. Instead, a file counts as reading a member when it can reach **exactly
one** of the structs that use that name. A file that can reach two is unclear,
and says so.

What it still cannot see is a member read through a function:
`recon_keyring_user()` returns `g_user` and nothing anywhere says `->user`. So
a member with no reader is a **question**, not a verdict -- and the useful half
of the answer is often that the only way to reach it is a call, which is either
an internal detail or a thing nobody outside has ever wanted.

    ./scripts/knows-and-does-not-do.py [--all] [struct-name]
"""
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Bookkeeping most structs have, whose whole job is internal to their owner.
SKIP_MEMBERS = {'used', 'count', 'next', 'prev', 'reserved', 'padding'}

MEMBER = re.compile(
    r'^\s*(?:const\s+|unsigned\s+|signed\s+|struct\s+|enum\s+|static\s+|'
    r'volatile\s+)*'
    r'[A-Za-z_]\w*\s*\**\s*([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;\s*$')


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read()


def all_files():
    out = []
    for d in ('src', 'tests', 'userland', 'include'):
        base = os.path.join(REPO, d)
        if not os.path.isdir(base):
            continue
        for root, _dirs, files in os.walk(base):
            for f in sorted(files):
                if f.endswith(('.c', '.h')):
                    out.append(os.path.relpath(os.path.join(root, f),
                                               REPO).replace('\\', '/'))
    return out


def members_of(body):
    """Member names of a struct body, comments and nesting removed."""
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)

    depth = 0
    out = []
    for line in body.split('\n'):
        if depth == 0:
            hit = MEMBER.match(line)
            if hit:
                name = hit.group(1)
                # A one or two letter name is a loop variable or a fragment of
                # something this regex misread, not a member worth reporting.
                if len(name) > 2 and name not in SKIP_MEMBERS:
                    out.append(name)
        depth += line.count('{') - line.count('}')
        depth = max(depth, 0)
    return out


ASSIGN = re.compile(r'\s*(?:\[[^\]]*\])?\s*(=)(?!=)')


def is_a_write(body, at):
    """Whether the mention ending at `at` is being assigned to.

    --- Why this is the question ---

    The first version asked "does anything read this member", and counted the
    file that *fills it in* as a reader. So `required`, whose only mention
    outside the header was `f->required = has_attribute(...)`, looked read.
    The check could not fail, which is worse than not having it.

    A member every mention of which is an assignment is precisely a thing this
    system records and never acts on. That needs no convention about which
    source owns which header -- it is the question itself.

    `==` and its relatives are reads. `+=` is both, and is counted as a read,
    because something that adds to a value is using it.
    """
    return ASSIGN.match(body, at) is not None


def main():
    show_all = '--all' in sys.argv
    only = [a for a in sys.argv[1:] if not a.startswith('--')]

    text = {p: read(os.path.join(REPO, p)) for p in all_files()}

    # --- what each file can reach, following includes all the way ---------
    #
    # The fix that mattered. A file that includes recon_modules.h reaches
    # recon_module.h through it, and a filter that only looked at direct
    # includes called every member of that struct unread.
    direct = {}
    for path, body in text.items():
        here = set()
        for m in re.finditer(r'#include\s+"([^"]+)"', body):
            leaf = os.path.basename(m.group(1))
            for candidate in text:
                if os.path.basename(candidate) == leaf:
                    here.add(candidate)
        direct[path] = here

    reach = {}
    for path in text:
        seen = set()
        stack = list(direct[path])
        while stack:
            h = stack.pop()
            if h in seen:
                continue
            seen.add(h)
            stack.extend(direct.get(h, ()))
        reach[path] = seen

    # --- every struct, its members, and who else uses each name -----------
    owner = {}
    struct_members = {}
    name_used_by = collections.defaultdict(set)

    for path in sorted(text):
        if not path.startswith('include/'):
            continue
        for m in re.finditer(r'\bstruct\s+(\w+)\s*\{(.*?)\n\};', text[path],
                             re.S):
            struct, body = m.group(1), m.group(2)
            owner[struct] = path
            struct_members[struct] = members_of(body)
            for name in struct_members[struct]:
                name_used_by[name].add(struct)

    silent = []
    unclear_total = 0

    for struct in sorted(struct_members):
        if only and struct not in only:
            continue

        header = owner[struct]
        rows = []

        for name in struct_members[struct]:
            others = [owner[s] for s in name_used_by[name] if s != struct]
            pattern = re.compile(r'(?:->|\.)\s*%s\b' % re.escape(name))

            readers = []
            unclear = False
            for path, body in text.items():
                if path == header:
                    continue
                if header not in reach[path]:
                    continue
                # Reaching another struct that uses this name too means the
                # mention could be either, and guessing is what made the first
                # version of this script print four wrong answers.
                shared = any(o in reach[path] for o in others)

                for m in pattern.finditer(body):
                    if is_a_write(body, m.end()):
                        continue
                    if shared:
                        unclear = True
                    else:
                        readers.append(os.path.basename(path))
                    break

            if not readers and unclear:
                unclear_total += 1
                rows.append((name, None))
            else:
                rows.append((name, readers))

        blind = [n for n, r in rows if r == []]
        if blind:
            silent.append((struct, header, blind))

        if show_all:
            print('--- %s  (%s) ---' % (struct, header))
            for name, readers in rows:
                if readers is None:
                    where = '(only files that also reach another struct '
                    where += 'using this name -- unclear)'
                elif not readers:
                    where = 'ONLY EVER ASSIGNED'
                else:
                    where = ', '.join(sorted(set(readers)))
                print('  %-22s %s' % (name, where))
            print()

    if not show_all:
        print('Members every mention of which is an assignment: written, '
              'carried, and never acted on.')
        print('A member reached through a function is invisible here, so '
              'each of these is a question.')
        print()

    total = 0
    for struct, header, blind in silent:
        print('%s  (%s)' % (struct, header))
        for name in blind:
            print('    %s' % name)
            total += 1
        print()

    print('%d members across %d structs. %d more could not be told apart '
          'from another struct using the same name.'
          % (total, len(silent), unclear_total))


if __name__ == '__main__':
    main()
