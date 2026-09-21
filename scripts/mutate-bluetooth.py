#!/usr/bin/env python3
#
# Break every decision in the Bluetooth layer, one at a time, and report the
# ones nothing noticed.
#
# --- why this exists ----------------------------------------------------------
#
# This branch has found thirteen tests that passed when they should not have.
# Every one of them was found the same way: take something the test claims to
# cover, break it on purpose, and see whether the run goes red. Every one of
# them was chosen by hand, which means the ones found are the ones somebody
# thought to try -- and the thirteenth, an address comparison that could have
# stopped after one byte, had survived three days *after* the gate above it was
# break-tested and reported as covered.
#
# Choosing by hand does not scale and does not say what it missed. So: do it
# mechanically, to every comparison and every boolean return in the layer, and
# print the list of breakages the self-tests sat through.
#
# --- what a survivor is, and what it is not -----------------------------------
#
# A survivor is a mutation that compiled, booted, and produced the same 76
# passes and no failures. That is **not** the same as a bug. Three kinds of
# thing land in the list:
#
#   1. an untested decision -- the interesting kind;
#   2. an equivalent mutation, where the change cannot alter behaviour (a `<`
#      against a bound that nothing reaches, an `||` whose right side is
#      implied by its left);
#   3. a decision only reachable with hardware this cannot boot.
#
# The script does not try to tell them apart, because it cannot. It narrows a
# few thousand lines down to a list short enough to read, and reading it is the
# work. Saying otherwise would be a number that looks like an answer.
#
# --- what it does not cover ---------------------------------------------------
#
# Only comparisons, logical operators and boolean returns, only in the nine
# Bluetooth files, and only outside each file's self-test section -- mutating a
# test to see whether the tests notice answers nothing. Constants, arithmetic,
# assignments and missing statements are untouched. **A clean run of this means
# one class of hole is closed, not that the layer is tested.**
#
#   scripts/mutate-bluetooth.py --list        count the mutants, build nothing
#   scripts/mutate-bluetooth.py               run them all
#   scripts/mutate-bluetooth.py l2cap.c       run one file's
#
import argparse
import fcntl
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORE = os.path.join(ROOT, "kernel", "core")

FILES = [
    "bluetooth.c", "bt_hid.c", "bt_link.c", "bt_mouse.c", "bt_pair.c",
    "bt_stack.c", "hid_report.c", "l2cap.c", "sdp.c",
]

# The banner every one of these files puts above its own tests. Everything from
# here down is the test, and mutating a test tells you nothing about the code.
SELF_TEST_BANNER = re.compile(r"^/\* --- the self-test")

# Relative, and it has to stay relative: every command below runs through a
# shell with `cwd=ROOT`, and this checkout's path has a space in it. Spelled
# absolutely, qemu is handed `-kernel /mnt/e/Github` and boots nothing -- which
# reports as zero passes, which is indistinguishable from a kernel that died.
KERNEL_ELF = "kernel/build/x86_64/reconos-kernel.elf"

# Written while a mutation is applied and removed once every file is verified
# back. See BG-198 -- the desktop's mutation harness died mid-mutation and the
# next run treated what it left as the original source.
SENTINEL = os.path.join(ROOT, ".mutate-in-progress")


def code_mask(text):
    """True for every character that is code: not a comment, string or char
    literal. Written out rather than regexed because these files are more
    comment than code and a regex that gets it slightly wrong produces mutants
    inside prose, which compile fine and mean nothing."""
    mask = [False] * len(text)
    i, n = 0, len(text)
    state = "code"
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "*":
                state = "block"; i += 2; continue
            if c == "/" and nxt == "/":
                state = "line"; i += 1; continue
            if c == '"':
                state = "str"; i += 1; continue
            if c == "'":
                state = "chr"; i += 1; continue
            mask[i] = True
            i += 1
        elif state == "block":
            if c == "*" and nxt == "/":
                state = "code"; i += 2; continue
            i += 1
        elif state == "line":
            if c == "\n":
                state = "code"; mask[i] = True
            i += 1
        elif state in ("str", "chr"):
            if c == "\\":
                i += 2; continue
            if (state == "str" and c == '"') or (state == "chr" and c == "'"):
                state = "code"
            i += 1
    return mask


def line_starts(text):
    starts = [0]
    for i, c in enumerate(text):
        if c == "\n":
            starts.append(i + 1)
    return starts


def line_of(starts, pos):
    lo, hi = 0, len(starts) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if starts[mid] <= pos:
            lo = mid
        else:
            hi = mid - 1
    return lo + 1


def cut_at_self_test(text):
    """Character offset where the self-test section begins, or len(text)."""
    off = 0
    for line in text.splitlines(keepends=True):
        if SELF_TEST_BANNER.match(line):
            return off
        off += len(line)
    return len(text)


# Each entry: (pattern, replacement, short name). Applied to code characters
# only. The two-character operators are matched first so that `<=` is never
# seen as a `<`.
OPERATORS = [
    ("==", "!=", "eq->ne"),
    ("!=", "==", "ne->eq"),
    ("<=", "<",  "le->lt"),
    (">=", ">",  "ge->gt"),
    ("&&", "||", "and->or"),
    ("||", "&&", "or->and"),
]

SINGLES = [
    ("<", "<=", "lt->le"),
    (">", ">=", "gt->ge"),
]


def find_mutants(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()

    limit = cut_at_self_test(text)
    mask = code_mask(text)
    starts = line_starts(text)
    out = []

    # `#include <recon/kernel/bt_hid.h>` has a `<` and a `>` in it, and each
    # one is a mutant the compiler rejects instantly. They are caught, so they
    # are not wrong -- they are just a third of the run spent proving that a
    # broken include is a broken include.
    preproc = set()
    for k, line in enumerate(text.splitlines(), 1):
        if line.lstrip().startswith("#"):
            preproc.add(k)

    i = 0
    while i < limit:
        if not mask[i] or line_of(starts, i) in preproc:
            i += 1
            continue
        two = text[i:i + 2]
        hit = None
        for pat, rep, name in OPERATORS:
            if two == pat and mask[i + 1]:
                hit = (pat, rep, name)
                break
        if hit:
            out.append((i, len(hit[0]), hit[1], hit[2], line_of(starts, i)))
            i += 2
            continue

        c = text[i]
        if c in "<>":
            prev = text[i - 1] if i else ""
            nxt = text[i + 1] if i + 1 < len(text) else ""
            # not <<, >>, <=, >=, ->, and not the tail of one of those
            if nxt not in "<>=" and prev not in "<>-=":
                for pat, rep, name in SINGLES:
                    if c == pat:
                        out.append((i, 1, rep, name, line_of(starts, i)))
                        break
        i += 1

    # Boolean returns, which are the other half of a decision.
    for m in re.finditer(r"\breturn (true|false);", text[:limit]):
        if not mask[m.start()]:
            continue
        was = m.group(1)
        now = "false" if was == "true" else "true"
        out.append((m.start(), len(m.group(0)), "return %s;" % now,
                    "ret %s->%s" % (was, now), line_of(starts, m.start())))

    out.sort()
    return text, out


def source_line(text, lineno):
    return text.splitlines()[lineno - 1].strip()


def run(cmd, timeout=120):
    try:
        p = subprocess.run(cmd, shell=True, cwd=ROOT, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return 124, ""


# --- and the lock, which this did not take for its first run ------------------
#
# `verify-kernel.sh` and `quick-check.sh` both take `/tmp/reconos-kernel-tree.lock`
# before building. This did not, and its first full run -- 333 builds and 333
# QEMU boots over 37 minutes -- overlapped most of a `verify-kernel.sh` run in
# another worktree.
#
# The tree that lock is named for is per-worktree, so no binary was mixed. What
# is shared is the machine, and **BT-014's first instance was exactly that**: a
# boot cut short by a 25-second timeout because two builds were running, which
# reported `FAIL: 0` and `pass: 4`. A rig with timeouts in it produces wrong
# answers under load, and the wrong answer here would have landed in somebody
# else's matrix rather than in this run.
#
# Taken per mutant rather than once for the run: a 37-minute hold would block
# every other session, and seven seconds at a time interleaves with them
# instead. It waits rather than failing -- a mutation run that stops because
# somebody started a verify is a mutation run nobody will use.
LOCKFILE = os.environ.get("RECON_TREE_LOCK", "/tmp/reconos-kernel-tree.lock")
_lockfd = None
_waited = False


def tree_lock():
    global _lockfd, _waited

    if _lockfd is None:
        _lockfd = open(LOCKFILE, "w")

    try:
        fcntl.flock(_lockfd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return
    except OSError:
        pass

    if not _waited:
        print("\n  (waiting for the tree lock -- something else is building "
              "or booting)", flush=True)
        _waited = True

    fcntl.flock(_lockfd, fcntl.LOCK_EX)


def tree_unlock():
    if _lockfd is not None:
        fcntl.flock(_lockfd, fcntl.LOCK_UN)


def build_and_boot():
    """Returns (verdict, detail). Verdict is one of:
       'compile'  -- the compiler refused it, which counts as caught
       'fail'     -- it booted and something reported FAIL
       'panic'    -- it crashed or hung
       'short'    -- fewer passes than the baseline, so a test stopped early
       'survived' -- the baseline's passes, no failures, nothing noticed"""
    tree_lock()
    try:
        rc, out = run("make -C kernel ARCH=x86_64", timeout=300)
        if rc != 0:
            return ("compile",
                    out.strip().splitlines()[-1] if out.strip() else "")

        rc, out = run("timeout 60 qemu-system-x86_64 -m 512M -nographic "
                      "-no-reboot -kernel %s -append poweroff" % KERNEL_ELF,
                      timeout=90)
    finally:
        tree_unlock()

    out = out.replace("\r", "")
    npass = len(re.findall(r": pass", out))
    nfail = len(re.findall(r": FAIL", out))

    if nfail:
        names = re.findall(r"^\s*(.*?): FAIL", out, re.M)
        return "fail", ", ".join(names[:3])
    if re.search(r"kernel fault|PANIC|panic", out) or rc == 124:
        return "panic", "hung" if rc == 124 else "panicked"
    if BASELINE is None:
        return "survived", npass
    if npass != BASELINE:
        return "short", "%d passes, expected %d" % (npass, BASELINE)
    return "survived", ""


# Measured, not written down. A count in the source is a count that goes stale
# the next time a test is added, and a stale one turns every mutant into a
# "short" -- a run where everything looks caught and nothing was.
BASELINE = None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*", help="which of the nine (default: all)")
    ap.add_argument("--list", action="store_true", help="count, build nothing")
    args = ap.parse_args()

    targets = args.files or FILES
    for t in targets:
        if t not in FILES:
            print("not one of the Bluetooth files: %s" % t)
            return 2

    # The fault BG-198 records, and the reason it cost twenty minutes: a run
    # that dies while a mutation is applied leaves it in the tree, and the
    # *next* run reads the mutated file as the original. A mutation that
    # survives is exactly the one that passes the baseline check below, so the
    # baseline cannot catch this. This can.
    if os.path.exists(SENTINEL):
        with open(SENTINEL) as f:
            left = f.read().strip()
        print("A previous run did not finish restoring. These files may hold")
        print("a mutation, and this run would take it for the original:")
        print(left)
        print("\nPut them back (git checkout -- <paths>) and delete %s"
              % SENTINEL)
        return 2

    plan = []
    for name in targets:
        path = os.path.join(CORE, name)
        text, muts = find_mutants(path)
        for m in muts:
            plan.append((name, path, text, m))
        print("%-14s %4d mutants" % (name, len(muts)))
    print("%-14s %4d total" % ("", len(plan)))

    if args.list:
        return 0

    # The baseline first, because a run that starts red reports every mutant as
    # caught and looks like a perfect score.
    global BASELINE
    print("\nbaseline: ", end="", flush=True)
    verdict, detail = build_and_boot()
    if verdict != "survived":
        print("the tree is not green before starting -- %s %s" % (verdict, detail))
        return 1
    BASELINE = detail
    if not BASELINE:
        print("the kernel booted and ran no tests at all")
        return 1
    print("%d passes, no failures\n" % BASELINE)

    backups = {}
    for name in targets:
        path = os.path.join(CORE, name)
        fd, tmp = tempfile.mkstemp(prefix="mutate-", suffix="-" + name)
        os.close(fd)
        shutil.copyfile(path, tmp)
        backups[path] = tmp
    with open(SENTINEL, "w") as f:
        f.write("\n".join(sorted(backups)) + "\n")

    survivors = []
    counts = {"compile": 0, "fail": 0, "panic": 0, "short": 0, "survived": 0}
    started = time.time()

    try:
        for n, (name, path, text, (pos, length, rep, kind, lineno)) in enumerate(plan, 1):
            mutated = text[:pos] + rep + text[pos + length:]
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(mutated)

            verdict, detail = build_and_boot()
            counts[verdict] += 1

            # Put it back before anything else can read it.
            shutil.copyfile(backups[path], path)

            mark = "SURVIVED" if verdict == "survived" else verdict
            line = source_line(text, lineno)
            print("%4d/%d %-9s %s:%d %-14s %s"
                  % (n, len(plan), mark, name, lineno, kind, line[:70]),
                  flush=True)
            if verdict == "survived":
                survivors.append((name, lineno, kind, line))
    finally:
        # BG-198 was a harness that died between mutating and restoring, and
        # the mutation it left read as a fault in correct code for twenty
        # minutes. So: restore, then *check* the restore, then say so.
        restored = True
        for path, tmp in backups.items():
            shutil.copyfile(tmp, path)
            # A copy carries a fresh timestamp, but saying so costs nothing
            # and BG-198's second half was make declining to rebuild.
            os.utime(path, None)
            with open(tmp, "rb") as a, open(path, "rb") as b:
                if a.read() != b.read():
                    print("THE TREE IS NOT AS IT WAS: %s did not restore, and"
                          " the copy is at %s" % (path, tmp))
                    restored = False
            if restored:
                os.unlink(tmp)
        if restored:
            try:
                os.unlink(SENTINEL)
            except OSError:
                pass
        # And rebuild, so the tree is not left holding a mutant's binary.
        tree_lock()
        try:
            run("make -C kernel ARCH=x86_64", timeout=300)
        finally:
            tree_unlock()

    mins = (time.time() - started) / 60
    print("\n%d mutants in %.0f minutes" % (len(plan), mins))
    for k in ("compile", "fail", "panic", "short", "survived"):
        print("  %-9s %4d" % (k, counts[k]))

    if not survivors:
        print("\nNothing survived. One class of hole is closed; see the note at")
        print("the top of this file for the ones it does not look for.")
        return 0

    print("\n--- %d survived, read them ------------------------------" % len(survivors))
    for name, lineno, kind, line in survivors:
        print("%s:%d  %s" % (name, lineno, kind))
        print("    %s" % line)
    return 1


if __name__ == "__main__":
    sys.exit(main())
