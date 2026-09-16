"""Which host calls the desktop's three filesystem functions can actually reach.

One level of grep is not the answer: `recon_fs_write` calls `write_file` calls
`fopen`, and a scan that stops at the first level reports that
`recon_fs_write` calls nothing. So this walks the call graph transitively from
the three entry points the linker named and collects every host call on the
way.

It is a text instrument and it knows it, so it errs towards *over*-reporting:
anything that looks like a call is a call, including inside a comment. A
finding of "stat is not reachable" from a tool biased that way is worth
something; the same finding from a tool biased the other way would not be.
"""
import re
import sys

import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'src', 'recon_fs.c')
text = open(SRC, encoding='utf-8').read()

HOST = {
    'fopen', 'fread', 'fwrite', 'fclose', 'fseek', 'ftell', 'fflush',
    'fprintf', 'opendir', 'readdir', 'closedir', 'stat', 'lstat', 'fstat',
    'mkdir', 'rmdir', 'unlink', 'rename', 'chmod', 'realpath', 'access',
    'remove', 'getenv', 'open', 'read', 'write', 'close', 'lseek', 'rewind',
    'truncate', 'ftruncate', 'utime', 'utimes', 'chown', 'symlink', 'link',
    'readlink', 'statvfs', 'fdopen', 'tmpfile', 'popen',
}

# Split the file into function bodies by brace depth, which is exact enough
# for a file written in this style: a definition starts at column 0 and ends
# at a closing brace in column 0.
DEF = re.compile(r'^(?:static\s+)?[A-Za-z_][\w \t*]*?\b(\w+)\s*\([^;]*?\)\s*\{',
                 re.M)

bodies = {}
for m in DEF.finditer(text):
    name = m.group(1)
    end = text.find('\n}\n', m.end())
    bodies[name] = text[m.end():end if end > 0 else len(text)]

print('functions found: %d' % len(bodies))

CALL = re.compile(r'\b(\w+)\s*\(')


def reach(entry):
    seen, host, queue = set(), set(), [entry]
    while queue:
        fn = queue.pop()
        if fn in seen:
            continue
        seen.add(fn)
        for callee in set(CALL.findall(bodies.get(fn, ''))):
            if callee in HOST:
                host.add(callee)
            elif callee in bodies:
                queue.append(callee)
    return host, seen


# The entry points to walk from. Given on the command line, because the
# interesting question changes: "what does a program that writes a settings
# file need" and "what does a program that draws a themed window need" have
# different answers, and hard-coding one of them is how this tool came to give
# a confident wrong answer on 16 September.
ENTRIES = sys.argv[1:] or ['recon_fs_write', 'recon_fs_append',
                           'recon_fs_mkdir']
everything = set()
for e in ENTRIES:
    if e not in bodies:
        print('  !! %s not found -- the parse is wrong, not the answer' % e)
        sys.exit(2)
    host, seen = reach(e)
    everything |= host
    print('\n%s' % e)
    print('  reaches %d functions in this file' % len(seen))
    print('  host calls: %s' % ' '.join(sorted(host)))

print('\n--- all three together ---')
print('  %s' % ' '.join(sorted(everything)))

for call in ('stat', 'lstat', 'fstat'):
    print('  %-6s reachable: %s' % (call, call in everything))

print('\n--- and what the whole file uses, for contrast ---')
whole = set()
for b in bodies.values():
    whole |= {c for c in CALL.findall(b) if c in HOST}
print('  %s' % ' '.join(sorted(whole)))
print('  only reachable from the rest: %s'
      % ' '.join(sorted(whole - everything)))
