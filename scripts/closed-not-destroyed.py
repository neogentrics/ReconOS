#!/usr/bin/env python3
"""What an application does at shutdown that it should do when it is closed.

--- Why this exists ---

Closing a built-in application does not destroy its window. `recon_apps.c`
calls `recon_appwin_hide`, the window keeps everything it had, and opening the
application again shows the same one back. That is deliberate and worth having.

What it means is that **a destructor runs at shutdown and never when somebody
actually closed the thing** -- and four separate faults came out of that in one
week:

    v0.4.74  the browser kept a session cookie past the browser closing
    v0.4.75  the Media Player kept playing, with no window and no taskbar entry
    v0.4.75  Mail kept its connection and the password in memory
    v0.4.80  the registry editor stayed unlocked, having said it would not

Each was found by hand, and the fourth only because somebody went looking. So
this is the question asked of every application at once.

--- What it looks for ---

Two hooks answer two different questions, and either can be the right one:

  `visibility(user, bool)` -- *should I still be doing work?* Fires on show,
  hide, minimize and restore, because a minimized window has nobody reading it
  either. The Task Manager uses this, correctly: it stops sampling.

  `closed(user)` -- *is this over?* Fires from `recon_appwin_hide` alone. The
  browser, the player, Mail and the Control Panel use this, because a minimize
  is not the end of a session.

So the question this asks is **which of the two an application needs, and
whether it has that one**:

  **A destructor that does more than free**, in an application with *neither*
  hook. `stop_playing`, `disconnect`, a session torn down, a dialog cancelled
  -- work that means "this is over", in a function that only runs when the
  machine is going off.

  **A window that holds a secret or a lock**, in an application with no
  `closed`. This one is not satisfied by `visibility`, and that is the whole
  point: `visibility` cannot tell a close from a minimize, and re-locking on a
  minimize would take the key from somebody who put the window down. A masked
  `recon_edit` is a password box; a flag named for being unlocked or signed in
  is a gate. Either outliving a close is the v0.4.80 shape.

--- It was wrong on its first run ---

It flagged the Task Manager for sampling forever after a close, because it
asked only whether `closed` was implemented. The Task Manager has a
`visibility` hook that disarms the timer and takes two samples on the way back,
CPU being a rate. **Correct code, and this was one build away from rewriting
it worse** -- which is the same fault `knows-and-does-not-do.py` had on its
first run and the same fault the theme coverage measurement had in v0.4.71: an
instrument answering confidently about something it was not pointed at.

--- What it cannot do ---

Decide. Every finding here is a question for somebody who knows what the
application is for: a Terminal that keeps its shell session across a close may
be exactly right, and a Notepad that keeps your document certainly is. The
report says what was found and why it looked like something; the triage is in
`docs/CLOSED-AUDIT.md`, so that an answered question stays answered instead of
being re-asked every run.

Exit status is 1 only when something is found that the audit file does not
already account for.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'src')
AUDIT = os.path.join(ROOT, 'docs', 'CLOSED-AUDIT.md')

# A destructor doing only these is doing nothing that means "this is over".
# `free` and friends release memory; ending an edit and clearing a flag are
# tidying. Anything else is work with a consequence outside the struct.
HARMLESS = re.compile(
    r'^\s*(\}|\{|/\*|\*|//|return\b|$)'
    r'|free\s*\('
    r'|recon_edit_end\s*\('
    r'|recon_secure_erase\s*\('
    r'|^\s*\(void\)'
    r'|^\s*struct\s+\w+\s*\*\w+\s*=\s*(user|data);'
)


def read(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        return f.read()


def impl_tables(text):
    """Every `struct recon_appwin_impl` table, as (name, body)."""
    out = []
    for m in re.finditer(
            r'static const struct recon_appwin_impl\s+(\w+)\s*=\s*\{(.*?)\n\};',
            text, re.S):
        out.append((m.group(1), m.group(2)))
    return out


def function_body(text, name):
    """The body of `static ... name(...)`, or None."""
    m = re.search(r'^static\s+\w[\w \t*]*\b' + re.escape(name) +
                  r'\s*\([^)]*\)\s*\{', text, re.M)
    if m is None:
        return None
    start = m.end()
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
        i += 1
    return text[start:i - 1]


def real_work(body):
    """The lines of a destructor that are not tidying."""
    lines = []
    for line in body.splitlines():
        if HARMLESS.search(line):
            continue
        if line.strip() == '':
            continue
        lines.append(line.strip())
    return lines


def secrets(text):
    """Names that look like a password box or a gate."""
    found = []

    # A masked edit is a password box, whatever the member is called.
    for m in re.finditer(r'(\w+)\.masked\s*=\s*true', text):
        found.append(('a masked field', m.group(1)))

    # A flag named for a gate being open.
    for m in re.finditer(
            r'\bbool\s+(\w*(?:unlocked|authenticated|signed_in|elevated)\w*)\s*;',
            text):
        found.append(('a gate', m.group(1)))

    seen = set()
    unique = []
    for kind, name in found:
        if name in seen:
            continue
        seen.add(name)
        unique.append((kind, name))
    return unique


def accounted_for(audit, app):
    """Whether the audit file already rules on this application."""
    return re.search(r'^\|\s*`?' + re.escape(app) + r'`?\s*\|', audit, re.M) \
        is not None


def main():
    audit = read(AUDIT) if os.path.exists(AUDIT) else ''
    findings = []
    checked = 0

    for name in sorted(os.listdir(SRC)):
        if not name.endswith('.c'):
            continue
        path = os.path.join(SRC, name)
        text = read(path)

        for impl_name, body in impl_tables(text):
            checked += 1
            has_closed = re.search(r'\.closed\s*=', body) is not None
            has_visibility = re.search(r'\.visibility\s*=', body) is not None

            destroy = re.search(r'\.destroy\s*=\s*(\w+)', body)
            work = []
            if destroy is not None:
                d = function_body(text, destroy.group(1))
                if d is not None:
                    work = real_work(d)

            held = secrets(text)

            # Work that means "this is over" is answered by either hook: an
            # application that stops on a minimize has stopped on a close too.
            if has_closed or has_visibility:
                work = []

            # A secret is answered by `closed` alone. `visibility` fires on a
            # minimize, and forgetting a password because somebody put the
            # window down is its own fault.
            if has_closed:
                held = []

            if not work and not held:
                continue

            findings.append({
                'file': name,
                'impl': impl_name,
                'work': work,
                'held': held,
                'known': accounted_for(audit, name),
            })

    print('Applications whose window can be closed without anything happening.')
    print('%d window implementations read.\n' % checked)

    unknown = 0
    for f in findings:
        mark = '' if f['known'] else '  <-- not in docs/CLOSED-AUDIT.md'
        if not f['known']:
            unknown += 1
        print('%s  (%s)%s' % (f['file'], f['impl'], mark))
        for line in f['work'][:4]:
            print('    does at shutdown:  %s' % line[:68])
        for kind, held in f['held'][:4]:
            print('    holds:             %s, `%s`' % (kind, held))
        print('')

    print('%d found, %d of them not yet ruled on.' % (len(findings), unknown))

    if unknown > 0:
        print('\nEach is a question rather than a fault. Rule on it in')
        print('docs/CLOSED-AUDIT.md -- an answer written down is an answer')
        print('that stays answered.')
    return 1 if unknown > 0 else 0


if __name__ == '__main__':
    sys.exit(main())
