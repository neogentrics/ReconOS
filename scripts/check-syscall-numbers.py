#!/usr/bin/env python3
"""Do the kernel's system call numbers and user mode's agree?

`userland/include/recon.h` carries its own copy of the enumeration, and says
why in its own words:

    Copied rather than included, because `recon/kernel/user.h` is a kernel
    header: it pulls in kernel types and declares kernel functions, and a user
    program that included it would compile against half a kernel. **A number
    here that disagrees with the kernel's is a program that calls the wrong
    thing and is told it succeeded**, so the enumeration is written in the same
    order with the same names.

That reasoning is right and the duplication is the correct answer. What was
missing is anything that checks it. The hazard is named in a comment, and
nothing acts on a comment -- which is the sentence this project wrote about
KF-212, where a comment had predicted the exact failure for weeks.

**The failure mode is what makes this worth a script rather than care.** Insert
a call anywhere but the end of the kernel's list and every number after it
shifts. A program compiled against the old header then calls the next one
along: `recon_close(fd)` becomes `SYS_READ`, and it is *told it succeeded*.
Nothing faults, nothing logs, and the first symptom is data.

So the two lists are read out of the two headers and compared name by name and
position by position. Both are plain C enumerations that start at zero and
number consecutively, which is the only assumption here and is checked: an
explicit `= n` on anything but the first entry stops the run rather than being
interpreted.

    python3 scripts/check-syscall-numbers.py

Exits non-zero and says what differs.
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

KERNEL = os.path.join(ROOT, 'kernel', 'include', 'recon', 'kernel', 'user.h')
USER = os.path.join(ROOT, 'userland', 'include', 'recon.h')


def read_enum(path, first, last):
    """The SYS_* names, in order, from the enumeration containing `first`.

    Comments are stripped before anything is matched. A name inside a comment
    -- and both of these headers discuss their own calls at length -- would
    otherwise be read as an entry, which is a check that fails on prose.
    """
    s = io.open(path, encoding='utf-8').read()
    s = re.sub(r'(?s)/\*.*?\*/', '', s)
    s = re.sub(r'//[^\n]*', '', s)

    start = s.find(first)
    if start < 0:
        raise SystemExit('%s: no %s' % (path, first))
    start = s.rfind('enum', 0, start)
    end = s.find('};', start)
    if start < 0 or end < 0:
        raise SystemExit('%s: could not find the enumeration' % path)

    body = s[start:end]
    names = []
    for m in re.finditer(r'\b(SYS_[A-Z0-9_]+)\s*(=\s*([^,\s]+))?', body):
        name, _, value = m.groups()
        if name == last:
            break
        if value is not None:
            # `SYS_EXIT = 0` is fine and is how both start. Anything else
            # assigned explicitly means the positions are not the order, and
            # this script would be comparing two things it does not understand.
            if not (not names and value.strip() == '0'):
                raise SystemExit(
                    '%s: %s is given an explicit value (%s).\n'
                    'This check assumes both lists number consecutively from '
                    'zero. Teach it the new rule or take the value off.'
                    % (path, name, value.strip()))
        names.append(name)
    return names


kernel = read_enum(KERNEL, 'SYS_EXIT', 'SYS_MAX')
user = read_enum(USER, 'SYS_EXIT', None)

if kernel == user:
    print('  %d system calls, and both headers number them the same'
          % len(kernel))
    sys.exit(0)

print('the two system call lists disagree')
print()
print('  %-4s %-22s %-22s' % ('n', 'kernel/...user.h', 'userland/recon.h'))
for i in range(max(len(kernel), len(user))):
    k = kernel[i] if i < len(kernel) else '-'
    u = user[i] if i < len(user) else '-'
    mark = '' if k == u else '   <-- differs'
    print('  %-4d %-22s %-22s%s' % (i, k, u, mark))

print()
print('A program built against the second calls the kernel by the first.')
print('Where they differ, it reaches a different call and is told it worked.')
sys.exit(1)
