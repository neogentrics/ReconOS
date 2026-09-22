#!/usr/bin/env python3
r"""
Find C string literals that a patch script broke open.

--- Why this exists ---

Four times in this project a Python patch script has turned a `\n` inside a C
string literal into a real newline, splitting the literal across two lines and
breaking the build. Each time the resolution was "stop using scripts for C
string literals". Each time it happened again within hours.

A resolution that has failed four times is not a rule, it is a hope. This is
the check that makes it a rule: it runs with every suite, takes milliseconds,
and names the file, the line and the cause.

**The fourth time was this file.** The first version of this checker was itself
written by a patch script, and that script turned the `\n` comparisons in its
own scanner into real newlines -- so the tool built to catch the fault was
broken by the fault, before it had ever run clean. It is hard to ask for a
better argument for automating the check instead of remembering the rule, and
that is why the story is in the file rather than in a commit message nobody
will read again.

The compiler catches this too, and catches it perfectly -- but only in the
files some suite happens to build, and only as `missing terminating "
character`, which takes a moment to recognise as a mangled patch rather than a
typo. This walks every C source in the tree and says what the fault is.

--- What it looks for ---

A double-quoted literal that is still open when its line ends, tracking block
comments, line comments, character constants and backslash escapes as it goes.
A correctly written C file never has one: a literal may only continue across a
line with a trailing backslash, which is honoured here.

Exit 0 when clean, 1 when something is broken.
"""

import os
import sys

# The server role's own sources. Deliberately not the whole repository: the
# kernel and the desktop belong to other seats, and a check that fails on
# somebody else's file is a check they never agreed to.
ROOTS = ["server", "scripts"]
SUFFIXES = (".c", ".h")

NEWLINE = chr(10)
BACKSLASH = chr(92)
QUOTE = chr(34)
TICK = chr(39)


def scan(text):
    """Walk a whole file, carrying state across lines.

    Returns a list of (line number, line text) where a literal was left open.

    **State must cross lines or the check is useless.** An earlier version
    walked each line on its own and reported twenty false positives, every one
    a quotation mark inside a multi-line block comment -- which this tree is
    full of, because its comments explain things by quoting them. A checker
    with twenty false positives is a checker somebody switches off, which is
    worse than no checker because it also looks like diligence.
    """
    bad = []
    i = 0
    n = len(text)
    line_no = 1
    line_start = 0
    in_comment = False
    in_str = False
    in_chr = False
    str_line = 0

    while i < n:
        c = text[i]

        if c == NEWLINE:
            if in_str:
                bad.append((str_line, text[line_start:i].strip()[:70]))
                in_str = False
            in_chr = False
            line_no += 1
            line_start = i + 1
            i += 1
            continue

        if in_comment:
            if c == "*" and i + 1 < n and text[i + 1] == "/":
                in_comment = False
                i += 2
                continue
            i += 1
            continue

        if in_str:
            if c == BACKSLASH:
                # An escape. A backslash immediately before a newline
                # continues the literal legally -- rare, and real.
                if i + 1 < n and text[i + 1] == NEWLINE:
                    line_no += 1
                    line_start = i + 2
                    i += 2
                    continue
                i += 2
                continue
            if c == QUOTE:
                in_str = False
            i += 1
            continue

        if in_chr:
            if c == BACKSLASH:
                i += 2
                continue
            if c == TICK:
                in_chr = False
            i += 1
            continue

        if c == "/" and i + 1 < n and text[i + 1] == "*":
            in_comment = True
            i += 2
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != NEWLINE:
                i += 1
            continue
        if c == QUOTE:
            in_str = True
            str_line = line_no
            i += 1
            continue
        if c == TICK:
            in_chr = True
            i += 1
            continue
        i += 1

    if in_str:
        bad.append((str_line, text[line_start:].strip()[:70]))
    return bad


#
# A control byte outside a string or character literal.
#
# **Narrowed twice, and both narrowings were earned.** The first version flagged
# every byte outside printable ASCII, and the very first run produced three
# findings that were all correct code:
#
#   * a section sign in a comment in `server/auth.h`, which is prose;
#   * `\xc3\xa9` in `test_http_json.c`, which is a test **feeding the escaper a
#     non-ASCII byte** -- a suite about refusing hostile bytes has to contain
#     hostile bytes;
#   * `\x01` in the same file, for the same reason.
#
# So bytes above ASCII are left alone entirely -- they may be prose or they may
# be test data, and this cannot tell -- and control bytes are flagged only where
# they cannot be data: outside a literal. That is where the fault this exists
# for landed. A comment in `server/http/jsonread.c` said `\u0000` is valid JSON,
# and the tool that wrote the file read the escape and put a real NUL there. It
# compiled, because a comment is not parsed, and it sat in the source as a
# landmine for whatever read it next.
#
# The same shape as the `\n`-in-a-literal fault this file was built for,
# arriving through a different door -- and this project's memory of it is older
# still: a heredoc once turned `\b` in a regular expression into a raw 0x08 and
# silently removed every word boundary from it.
#
def stray_bytes(raw):
    """(offset, byte) for every control byte outside a literal."""
    out = []
    i = 0
    n = len(raw)
    state = "code"      # code, line, block, string, char

    while i < n:
        b = raw[i]

        if state == "code":
            if b == 0x2F and i + 1 < n and raw[i + 1] == 0x2F:
                state = "line"
                i += 2
                continue
            if b == 0x2F and i + 1 < n and raw[i + 1] == 0x2A:
                state = "block"
                i += 2
                continue
            if b == 0x22:
                state = "string"
                i += 1
                continue
            if b == 0x27:
                state = "char"
                i += 1
                continue
        elif state == "line":
            if b == 0x0A:
                state = "code"
        elif state == "block":
            if b == 0x2A and i + 1 < n and raw[i + 1] == 0x2F:
                state = "code"
                i += 2
                continue
        else:
            # Inside a literal: a backslash hides the next byte, and the
            # closing quote ends it. Control bytes here may be deliberate.
            if b == 0x5C:
                i += 2
                continue
            if (state == "string" and b == 0x22) \
                    or (state == "char" and b == 0x27):
                state = "code"
            i += 1
            continue

        if b in (9, 10):
            i += 1
            continue
        if b == 13 and i + 1 < n and raw[i + 1] == 10:
            i += 1
            continue
        if b < 32 or b == 0x7F:
            out.append((i, b))
        i += 1

    return out


def line_of(raw, offset):
    return raw.count(b"\n", 0, offset) + 1


def check(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return scan(f.read())
    except OSError as e:
        print("could not read %s: %s" % (path, e), file=sys.stderr)
        return [(0, "unreadable")]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    found = 0
    looked = 0

    for top in ROOTS:
        for dirpath, _dirs, files in os.walk(os.path.join(root, top)):
            for name in sorted(files):
                if not name.endswith(SUFFIXES):
                    continue
                path = os.path.join(dirpath, name)
                looked += 1

                #
                # Bytes that are not text, before the literal scan.
                #
                # **Found by this happening.** A comment in
                # `server/http/jsonread.c` said that `\u0000` is valid JSON,
                # and the tool that wrote the file read that escape and put a
                # real NUL byte in the source. It compiled, because the byte
                # was inside a comment, and it sat there as a landmine for
                # whatever read the file next -- which is the same shape as the
                # `\n` fault this checker was built for, arriving through a
                # different door.
                #
                # This project's memory of it is older than that: a heredoc
                # once turned `\b` in a regular expression into a raw 0x08 and
                # silently removed every word boundary from it.
                #
                try:
                    with open(path, "rb") as f:
                        raw = f.read()
                except OSError:
                    raw = b""
                for offset, byte in stray_bytes(raw):
                    rel = os.path.relpath(path, root).replace(BACKSLASH, "/")
                    print("%s:%d: a byte that is not text -- 0x%02x at offset"
                          " %d" % (rel, line_of(raw, offset), byte, offset))
                    print("    an edit probably turned an escape into the byte"
                          " it names; see the header")
                    found += 1

                for number, text in check(path):
                    rel = os.path.relpath(path, root).replace(BACKSLASH, "/")
                    print("%s:%d: string literal left open -- a patch script"
                          " probably turned an escaped newline into a real one"
                          % (rel, number))
                    print("    %s" % text)
                    found += 1

    if found:
        print()
        print("%d fault(s) across %d files." % (found, looked))
        return 1

    #
    # Zero is an unread tree, not a clean one.
    #
    # The denominator beside this number says how much was looked at, and a
    # denominator only helps a reader who brings an expectation. This needs
    # nobody: it encodes a fact about the repository that cannot quietly stop
    # being true. See `check-site-init.py` for where this started, and the
    # network session's check_citations for the case that makes it urgent --
    # the environment that produces zero there is the one the matrix runs in,
    # so the check most likely to meet the conditions that break it was the one
    # that passed under them.
    #
    if not looked:
        print("read no C files at all.")
        print("This repository is written in C. The check did not read the")
        print("tree -- it has not passed, it has failed to run.")
        return 1


    #
    # Zero is an unread tree, not a clean one.
    #
    # The denominator beside this number says how much was looked at, and a
    # denominator only helps a reader who brings an expectation. This needs
    # nobody: it encodes a fact about the repository that cannot quietly stop
    # being true. See `check-site-init.py` for where this started, and the
    # network session's check_citations for the case that makes it urgent --
    # the environment that produces zero there is the one the matrix runs in,
    # so the check most likely to meet the conditions that break it was the one
    # that passed under them.
    #
    if not looked:
        print("read no C files at all.")
        print("This repository is written in C. The check did not read the")
        print("tree -- it has not passed, it has failed to run.")
        return 1

    print("literals: %d files, none left open and no stray bytes" % looked)
    return 0


if __name__ == "__main__":
    sys.exit(main())
