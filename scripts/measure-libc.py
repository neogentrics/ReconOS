#!/usr/bin/env python3
"""How much of what the desktop needs from a C library does userland/libc have?

    python3 scripts/measure-libc.py [--build build]

--- Why this is a script and not a number somebody wrote down ---

It was a number somebody wrote down, twice, and it was wrong both times.

The first version listed the functions the library was expected to need and
grepped for those. That finds every call of a function on the list and **none
of a function that is not on it**, so the numerator and the denominator were
short by exactly the same calls and the fraction looked right. It missed
`strtok_r`, which is on 44 call sites (BG-182). Corrected by hand, it then
missed `gmtime_r`, `localtime_r` and most of the maths functions.

It could never have found `puts` at all. `src/main.c` calls `printf` with a
string literal containing no conversions, and the compiler rewrites that into
`puts` -- so the desktop needs a function whose name appears nowhere in its
source. No grep can see that and neither can reading the file.

So this asks something that has never heard of any list. Every object file
carries a table of the symbols it needs and a table of the symbols it has; the
difference is the external surface, exactly. `nm` reads both in one command.

--- The two numbers, which answer different questions ---

**Symbols** is how many distinct things must exist for the desktop to link.
That is the completeness measure: 80 missing symbols is 80 functions to write,
however often each is called.

**Call sites** is how much of the desktop's text is library calls. That still
needs a grep -- but the *names* it greps for come from `nm`, which is the half
that was wrong. It is the number that says where the weight is: the allocator
is 5 symbols and 430 call sites.

Requires a completed build, because it reads the object files.
"""
import argparse
import collections
import glob
import io
import io
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Libraries the desktop stands on that are not a C library. By prefix where
# there is one, by name where there is not -- zlib's three have nothing in
# common to match on.
NOT_LIBC_PREFIX = ('wlr_', '_wlr_', 'wl_', 'xkb_', 'pixman_', 'mbedtls_',
                   'psa_', 'snd_', 'av', 'sws_', 'swr_', 'stbi', 'FT_',
                   'hb_', 'recon_', '_ITM_', '__', '_GLOBAL_')
NOT_LIBC_EXACT = {'compress2', 'compressBound', 'crc32', 'uncompress',
                  'inflate', 'deflate'}

# What glibc calls a standard function when it emits a call to it.
#
# These are not compiler internals and the `__` filter above should not eat
# them. `sscanf` really is referenced by seven of the desktop's objects, and
# for as long as this table did not exist the coverage figure neither counted
# it as answered nor reported it as missing -- it simply was not there.
#
# Both directions were wrong. `errno` and `strtoul` *are* answered and were not
# being counted; `sscanf` and `assert` are *not* and were not being reported.
#
# By prefix where glibc uses one -- the `__isoc99_` and `__isoc23_` families
# are versioned spellings of the same functions -- and by name for the four
# that are their own thing. Anything still starting with `__` after this is a
# compiler or runtime symbol, which is what the filter was for.
REDIRECTED_PREFIX = ('__isoc99_', '__isoc23_')

REDIRECTED_EXACT = {
    '__errno_location': 'errno',
    '__ctype_b_loc': 'isalpha',		# the table behind the ctype macros
    '__ctype_tolower_loc': 'tolower',
    '__ctype_toupper_loc': 'toupper',
    '__assert_fail': 'assert',
    '__sysv_signal': 'signal',
}


def as_written(name):
    """The name a program actually wrote, given the one glibc emitted."""
    for prefix in REDIRECTED_PREFIX:
        if name.startswith(prefix):
            return name[len(prefix):]

    return REDIRECTED_EXACT.get(name, name)

# What each symbol the library does not have would take. Assigned by hand,
# which is safe in a way the old list was not: these names came *out* of the
# linker rather than going into it, so the table describes what was found
# rather than deciding what to look for. An unassigned name stops the run.
NEEDS = {
    'an allocator': 'malloc calloc realloc free strdup strndup',
    'floating-point maths': ('sqrt pow floor ceil round fabs sin cos tan '
                             'asin acos atan atan2 log log10 exp fmod cbrt '
                             'ldexp lrintf'),
    'files and directories': ('open close read write lseek stat lstat fstat '
                              'opendir readdir closedir mkdir rmdir unlink '
                              'rename access chmod umask realpath mmap '
                              'munmap sysconf'),
    'sockets': ('socket connect bind listen accept send recv sendto '
                'recvfrom setsockopt getsockopt shutdown getaddrinfo '
                'freeaddrinfo gai_strerror getnameinfo getifaddrs '
                'freeifaddrs inet_pton inet_ntoa htons htonl'),
    'processes and signals': 'fork execvp kill raise _exit getsid signal',
    'loading a module at run time': 'dlopen dlsym dlclose dlerror',
    'an errno': 'strerror',
    # sscanf is emitted by glibc as `__isoc99_sscanf`, so it sat behind the
    # `__` filter and was in neither total until 14 September 2026.
    'the rest of stdio': 'feof ferror ungetc sscanf',
    'an assertion': 'assert',
    'the rest of string': 'memmem strcasestr',
    'an environment': 'setenv',
}


def owners():
    out = {}
    for what, names in NEEDS.items():
        for n in names.split():
            out[n] = what
    return out


def nm(kind, *directories):
    objects = []
    for directory in directories:
        objects += [os.path.join(directory, n)
                    for n in sorted(os.listdir(directory))
                    if n.endswith('.o')]
    if not objects:
        return collections.Counter()
    text = subprocess.run(['nm', kind] + objects,
                          capture_output=True, text=True).stdout
    found = collections.Counter()
    for line in text.splitlines():
        parts = line.split()
        if parts and re.match(r'^[A-Za-z_][A-Za-z0-9_.]*$', parts[-1]):
            found[parts[-1]] += 1
    return found


def build_type(build):
    """What CMake was told to build, straight out of its own cache.

    Read rather than assumed, because assuming it is the fault this answers:
    the script had no opinion about the optimisation level and the answer
    depends on it entirely.
    """
    cache = os.path.join(build, 'CMakeCache.txt')

    if not os.path.isfile(cache):
        return None

    with io.open(cache, encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith('CMAKE_BUILD_TYPE:'):
                return line.split('=', 1)[1].strip()

    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', default='build')
    ap.add_argument('--any-build-type', action='store_true',
                    help='report a number from a build that is not a release '
                         '-- which is a different number, see BG-203')
    args = ap.parse_args()

    build = os.path.join(HERE, args.build)
    desktop = os.path.join(build, 'CMakeFiles/ReconOS.dir/src')

    # Every object compiled from userland/libc/, wherever in the build tree,
    # rather than the one test target this script used to name.
    #
    # It named `recon_libc_file_tests`; when the allocator arrived in a suite
    # of its own, the script went on reporting its 430 call sites as
    # unanswered. A measurement that was wrong and said nothing, which is what
    # BG-182 and BG-185 were both about.
    libc_dirs = sorted(glob.glob(
        os.path.join(build, 'CMakeFiles', '*', 'userland', 'libc')))

    if not os.path.isdir(desktop):
        sys.stderr.write('no object files for the desktop at %s -- build '
                         'first\n' % desktop)
        return 1

    if not libc_dirs:
        sys.stderr.write('no object files for the C library under %s -- '
                         'build first\n' % build)
        return 1

    # --- and it has to be a release ---------------------------------------
    #
    # **The compiler writes calls the source does not contain**, and it starts
    # doing it at -O2. `sin(x)` and `cos(x)` of the same argument become one
    # `sincos`; `atoll(s)` becomes `strtoll(s, 0, 10)` in the caller, reaching
    # past whatever `atoll` this library defines; a `sqrt` whose argument and
    # result are both floats becomes `sqrtf`.
    #
    # So the set of symbols the desktop needs is not a property of the desktop's
    # source. It is a property of the desktop's source *and the flags it was
    # built with*, and a measurement that does not say which flags is not a
    # measurement. All three of those were reported as answered for as long as
    # this script read whatever build happened to be lying around.
    kind = build_type(build)

    if not args.any_build_type and (kind or '').lower() != 'release':
        sys.stderr.write(
            'the build at %s is %s, and this number is only true of a '
            'release.\n'
            'At -O2 and above the compiler emits calls the source does not '
            'contain -- sincos, strtoll, sqrtf -- and a library measured '
            'against a debug build links where a release of the same source '
            'does not.\n'
            'Configure with -DCMAKE_BUILD_TYPE=Release, or pass '
            '--any-build-type to see the other number on purpose.\n'
            % (build, kind if kind else 'of no stated type'))
        return 1

    # A source with no object anywhere is the condition that made this wrong
    # before, so it is named rather than absorbed.
    #
    # Two deliberate exceptions, and they are the same kind of file: the ones
    # that make system calls, which cannot be compiled for the host at all.
    # `syscalls.c` is the syscall boundary itself; `mem_recon.c` is where the
    # allocator gets its memory. Their absence is a fact about the library
    # rather than a gap in the measurement -- and listing them here is what
    # makes any *other* absence a refusal to report a number.
    not_on_the_host = {'syscalls.c', 'mem_recon.c'}

    sources = {os.path.basename(p) for p in
               glob.glob(os.path.join(HERE, 'userland', 'libc', '*.c'))}

    objects = set()
    for d in libc_dirs:
        for o in glob.glob(os.path.join(d, '*.o')):
            objects.add(os.path.basename(o)[:-len('.o')])

    missing = sorted(c for c in sources
                     if c not in not_on_the_host and c not in objects)

    if missing:
        sys.stderr.write(
            'these library sources have no object in the build, so anything '
            'they define would be counted as missing: %s\n'
            % ', '.join(missing))
        return 1

    print('read from the %s build at %s'
          % ((kind or 'untyped').lower(), os.path.relpath(build, HERE)))
    print()

    needed = collections.Counter()
    for name, count in nm('--undefined-only', desktop).items():
        needed[as_written(name)] += count

    provided = set(nm('--defined-only', desktop))

    external = sorted(n for n in needed
                      if n not in provided
                      and not n.startswith(NOT_LIBC_PREFIX)
                      and n not in NOT_LIBC_EXACT)

    # prefix.h renamed everything in the library; recover the plain spelling.
    have = set()
    for name in nm('--defined-only', *libc_dirs):
        if name.startswith('recon_'):
            plain = name[len('recon_'):]
            if plain.startswith('libc_'):
                plain = plain[len('libc_'):]
            have.add(plain)

    answered = [n for n in external if n in have]
    missing = [n for n in external if n not in have]

    sites = collections.Counter()
    for here, _dirs, files in os.walk(os.path.join(HERE, 'src')):
        for name in files:
            if not (name.endswith('.c') or name.endswith('.h')):
                continue
            text = io.open(os.path.join(here, name), encoding='utf-8',
                           errors='replace').read()
            code = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
            code = re.sub(r'//[^\n]*', ' ', code)
            for f in re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(', code):
                if f in have or f in missing:
                    sites[f] += 1

    answered_sites = sum(sites[n] for n in answered)
    missing_sites = sum(sites[n] for n in missing)

    print('Symbols -- what must exist for the desktop to link')
    print('  %3d C library symbols referenced' % len(external))
    print('  %3d answered by userland/libc' % len(answered))
    print('  %3d not answered' % len(missing))
    print()
    print('Call sites -- how much of the desktop is library calls')
    print('  %5d over those same names' % (answered_sites + missing_sites))
    print('  %5d answered' % answered_sites)
    print('  %5d not' % missing_sites)
    print()

    owner = owners()
    by_need = collections.Counter()
    symbols_by_need = collections.Counter()
    unplaced = []
    for n in missing:
        what = owner.get(n)
        if what is None:
            unplaced.append(n)
        else:
            by_need[what] += sites[n]
            symbols_by_need[what] += 1

    print('What the %d unanswered symbols need:' % len(missing))
    for what, n in by_need.most_common():
        print('  %-30s %3d symbols, %4d call sites'
              % (what, symbols_by_need[what], n))

    if unplaced:
        print()
        print('These have no line in NEEDS, so they are in no total above:')
        print('  ' + ' '.join(unplaced))
        print('Add each to the table at the top of this script. A symbol')
        print('quietly missing from the arithmetic is the fault this whole')
        print('script exists to stop happening a third time.')
        return 1

    return 0


if __name__ == '__main__':
    sys.exit(main())
