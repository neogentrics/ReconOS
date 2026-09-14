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


ASM = [
    os.path.join(ROOT, 'kernel', 'arch', 'x86_64', 'user_entry.S'),
    os.path.join(ROOT, 'kernel', 'arch', 'aarch64', 'user_test.S'),
]


# Only a move into the register the system call number is read from, and only
# with the name in the comment beside it. The first version of this pattern was
# looser -- any immediate followed by a SYS_ name in a comment -- and it
# immediately reported
#
#   cmn x0, #2      /* SYS_EFAULT, and nothing else will do */
#
# as a call number that was wrong. That line compares a *return value* against
# SYS_EFAULT, which is -2, and has nothing to do with call numbers. A checker
# whose first output is a false positive is one nobody reads the second time.
SETS_CALL_NUMBER = re.compile(
    r'\s*(?:'
    r'movq?\s+\$(?P<x86>\d+)\s*,\s*%rax'          # AT&T: source, then dest
    r'|mov\s+x8\s*,\s*#(?P<arm>\d+)'              # ARM: dest, then source
    r')\s*/\*\s*(?P<name>SYS_[A-Z0-9_]+)')


def check_assembly(order):
    """The third copy, which is written as bare numbers.

    The embedded ring-3 programs load the call number as an immediate and name
    it in the comment beside it:

        movq    $1, %rax                /* SYS_WRITE */
        mov     x8, #6                  /* SYS_MACHINE */

    The comment is the only thing asserting that 1 is SYS_WRITE, and a comment
    asserts nothing. These programs are the kernel's own proof that user mode
    works -- so a shifted number here does not break a build, it makes the
    proof test a different call and still pass.
    """
    index = {name: i for i, name in enumerate(order)}
    wrong = []
    seen = 0

    for path in ASM:
        if not os.path.exists(path):
            continue
        for n, line in enumerate(io.open(path, encoding='utf-8'), 1):
            m = re.match(SETS_CALL_NUMBER, line)
            if not m:
                continue
            seen += 1
            value = int(m.group('x86') or m.group('arm'))
            name = m.group('name')
            if name not in index:
                wrong.append((path, n, name, value, 'no such call'))
            elif index[name] != value:
                wrong.append((path, n, name, value, index[name]))

    return seen, wrong


kernel = read_enum(KERNEL, 'SYS_EXIT', 'SYS_MAX')
user = read_enum(USER, 'SYS_EXIT', None)

asm_seen, asm_wrong = check_assembly(kernel)

if kernel == user and not asm_wrong:
    print('  %d system calls; both headers and %d hand-written numbers agree'
          % (len(kernel), asm_seen))
    sys.exit(0)

if asm_wrong:
    print('a hand-written call number does not match the enumeration')
    print()
    for path, n, name, value, want in asm_wrong:
        print('  %s:%d  loads %d and calls it %s, which is %s'
              % (os.path.relpath(path, ROOT), n, value, name, want))
    print()
    print('These programs are how the kernel proves user mode works. A number')
    print('that is wrong here does not fail to build -- it tests a different')
    print('call and passes.')
    if kernel == user:
        sys.exit(1)
    print()

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
