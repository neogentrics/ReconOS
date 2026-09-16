#!/usr/bin/env python3
"""Recovery is offered on every boot, including the boots that go wrong.

Checked here rather than by booting something, because the condition that
removes it cannot be produced under QEMU. KF-233 was three `return 0` paths
sitting above `add_recovery()` in a function whose caller shows no menu at all
when it returns zero -- so firmware whose handle enumeration surprised the scan
got no recovery entry, and recovery is the entry you want exactly when a machine
is surprising. Every one of those paths needs a firmware that does not behave,
and every firmware the matrix can reach behaves.

`boot-menu-test.sh` asserts recovery is listed, and passes in every matrix run,
including the ones where this bug was present. It was checking the path where
the scan succeeds -- which is the path that was never broken.

So the check is on the shape of the code: **menu_discover leaves by exactly one
door, and add_recovery is before it.** Not a proxy for the property. It is the
property, stated the only way a machine can be asked about it here.
"""
import re
import sys
import pathlib

SRC = pathlib.Path(__file__).resolve().parent.parent / 'boot' / 'src' / 'menu.c'


def body_of(text, signature):
    """The text between the function's opening brace and its match."""
    at = text.find(signature)
    if at < 0:
        return None

    start = text.find('{', at)
    depth = 0

    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[start:i]

    return None


def main():
    text = SRC.read_text(encoding='utf-8')
    body = body_of(text, 'unsigned menu_discover(')

    if body is None:
        print('menu_discover is not where this check expects it: %s' % SRC)
        return 1

    # Comments hold the words "return" and "add_recovery" on purpose -- the
    # entry above says why the shape is what it is. Stripping them first is
    # the difference between reading the code and reading the essay about it.
    code = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    code = re.sub(r'//[^\n]*', '', code)

    returns = [m.start() for m in re.finditer(r'\breturn\b', code)]
    added = code.find('add_recovery()')

    if added < 0:
        print('menu_discover no longer offers recovery at all')
        return 1

    if len(returns) != 1:
        print('menu_discover leaves by %d doors; recovery is added at one of '
              'them, so the others are boots with no recovery entry'
              % len(returns))
        return 1

    if returns[0] < added:
        print('menu_discover can return before add_recovery(), which is KF-233 '
              'again')
        return 1

    print('  recovery is offered on every path out of menu_discover')
    return 0


if __name__ == '__main__':
    sys.exit(main())
