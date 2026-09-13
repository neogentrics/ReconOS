#!/usr/bin/env python3
"""Build the bug register page out of docs/BUGS.md.

The page this writes used to be maintained by hand, and by the time anybody
looked it was twenty-nine entries behind the file it claims to summarise. A
summary that drifts from its source is worse than no summary: it is a number
people quote.

So this generates it, and the rule is that **every figure on the page comes out
of docs/BUGS.md**. Nothing is carried over, nothing is remembered between runs.
Run it again and the page is the file.

Two of the groupings are *derived* rather than stated -- how a fault surfaced,
and which part of the system it was in -- because the register does not record
either as a field. They are derived by matching words in the entry, which is a
guess, and a guess that quietly files everything under "something else" looks
exactly like a good classifier. So the page prints how many it could not place,
and this script fails loudly if that fraction goes above a quarter: a classifier
that has stopped working should stop the build rather than draw a tidy chart.

    python3 scripts/make-bug-register.py [--out FILE] [--check]

--check parses and reports without writing, which is what a pre-commit hook
wants.
"""

import argparse
import html
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOURCE = os.path.join(ROOT, "docs", "BUGS.md")

# --- parsing ---------------------------------------------------------------
#
# Two dialects, because the register has been written by two sessions over
# several months and the older entries use a different set of fields. Both are
# accepted rather than one being rewritten: editing ninety entries to please a
# parser is how a record stops matching what was actually written at the time.
#
#   older:  - **Found in** v0.2.17 · by Joshua: "..."
#           - **Was** ...
#           - **Fixed in** v0.2.17, `abc1234`. ...
#
#   newer:  - **Found:** 9 September 2026, by ...
#           - **Cost:** ...
#           - **Was:** ...
#           - **Fixed in** ...

ENTRY_RE = re.compile(r"(?m)^### (BG-\d+)\s*[—-]\s*(.+?)\s*$")
# The colon moves. Ninety-nine entries write `- **Fixed in** ...` and five write
# `- **Fixed in:** ...` with the colon inside the emphasis -- which is invisible
# when reading and fatal to a pattern that assumes it is outside. Those five
# entries were reported as unfixed until this allowed both.
FIELD_RE = re.compile(
    r"(?ms)^- \*\*(?P<name>[A-Za-z][A-Za-z ]*?):?\*\*:?\s*(?P<value>.*?)"
    r"(?=^- \*\*[A-Za-z]|^### |\Z)"
)
ISSUE_RE = re.compile(r"https://github\.com/[^\s)\]]*/issues/(\d+)")


def strip_markdown(text):
    """Enough of it to put a sentence on a web page.

    Deliberately small. A full Markdown renderer here would be a second
    implementation of the help viewer's, and this only ever has to survive
    inline code, bold, italics and links.
    """
    text = re.sub(r"`([^`]+)`", r"<code>\1</code>", html.escape(text))
    text = re.sub(r"&lt;code&gt;", "<code>", text)
    text = re.sub(r"&lt;/code&gt;", "</code>", text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![*\w])\*([^*]+)\*(?!\*)", r"<em>\1</em>", text)
    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r"\1", text)
    return " ".join(text.split())


def clip(text, limit=260):
    """Cut at a word, and say that it was cut."""
    if len(text) <= limit:
        return text
    cut = text[:limit].rsplit(" ", 1)[0]
    return cut + "&hellip;"


def parse(md):
    entries = []
    marks = list(ENTRY_RE.finditer(md))

    for i, m in enumerate(marks):
        start = m.end()
        end = marks[i + 1].start() if i + 1 < len(marks) else len(md)
        body = md[start:end]

        fields = {}
        for f in FIELD_RE.finditer(body):
            name = f.group("name").strip().lower()
            # First wins. A field written twice is a mistake in the entry, and
            # taking the first keeps the page matching what a reader sees at
            # the top of it.
            fields.setdefault(name, " ".join(f.group("value").split()))

        issue = None
        hit = ISSUE_RE.search(body)
        if hit:
            issue = hit.group(1)

        found = fields.get("found") or fields.get("found in") or ""
        fixed = fields.get("fixed in") or fields.get("fixed") or ""

        entries.append({
            "id": m.group(1),
            "n": int(m.group(1).split("-")[1]),
            "title": strip_markdown(m.group(2)),
            "found": strip_markdown(found),
            "was": strip_markdown(fields.get("was", "")),
            "cost": strip_markdown(fields.get("cost", "")),
            "fixed": strip_markdown(fixed),
            "note": strip_markdown(fields.get("note", "")),
            "issue": issue,
            "raw": body,
            "plain": plain(m.group(2) + " " + body),
        })

    entries.sort(key=lambda e: e["n"], reverse=True)
    return entries


# --- the two derived groupings ---------------------------------------------
#
# Ordered, and the first match wins, so the more specific phrases come first.
# Each rule is a phrase somebody actually wrote in this register rather than a
# word that might appear -- "by Joshua" is a real convention in the file; "user"
# is not.

SURFACED = [
    ("Somebody using it",       ["by joshua", "by somebody using", "a person",
                                 "on seeing", "somebody clicked", "by the author",
                                 "the author asking"]),
    ("An assertion written for it", ["self-test", "self test", "the rig",
                                 "assertion", "watched to fail",
                                 "negative control", "the matrix"]),
    ("Following another fault", ["bg-0", "bg-1", "while fixing", "testing bg",
                                 "in the same look", "immediately after"]),
    ("Looking at the screen",   ["screen capture", "screenshot", "on screen",
                                 "by looking", "drew", "the screen"]),
    ("An instrument built for it", ["objdump", "disassembl", "dumping",
                                 "instrument", "-d int", "gdb", "trace",
                                 "qemu's own", "measuring", "by measuring"]),
    ("Reading the code",        ["by reading", "reading the", "re-reading",
                                 "reviewing"]),
    ("The build",               ["the compiler", "the build", "by make",
                                 "clean build"]),
    ("The first thing to call it", ["first code", "by wiring", "by adding",
                                 "by calling", "first time", "never been executed",
                                 "never once run"]),
    ("Its own output",          ["own output", "the summary", "printed",
                                 "reported", "came back", "orphaned",
                                 "still running"]),
]

AREA = [
    ("Memory and paging",   ["page table", "paging", "vm_", "tlb", "address space",
                             "bitmap", "allocator", "identity map", "direct map",
                             "cr0.wp", "vector save"]),
    ("Processors",          ["processor", "secondary", "smp", "trampoline",
                             "gicv", "vbar", "apic", "core"]),
    ("Storage",             ["nvme", "ahci", "virtio", "block", "partition",
                             "gpt", "mbr", "disk", "sector", "usb", "xhci",
                             "scsi"]),
    ("Filesystems",         ["reconfs", "fat32", "superblock", "inode",
                             "cluster", "filesystem", "volume"]),
    ("Boot and firmware",   ["bootloader", "reconboot", "uefi", "bios", "ovmf",
                             "esp", "firmware", "stage 1", "stage 2", "signature"]),
    ("Scheduling and time", ["scheduler", "thread", "preempt", "context switch",
                             "timer", "clock", "tick", "spinlock", "process"]),
    ("Drawing and windows", ["window", "draw", "font", "glyph", "title bar",
                             "skin", "wallpaper", "dialog", "compositor",
                             "framebuffer", "console"]),
    ("Input and clicking",  ["click", "keyboard", "mouse", "hit region",
                             "focus", "alt+tab", "scroll", "caret"]),
    ("Help and docs",       ["help", "roadmap", "documented", "documentation",
                             "change log", "readme"]),
    ("Network",             ["network", "socket", "tls", "smtp", "cookie",
                             "firewall", "http"]),
    ("Programs and packages", ["module", "package", "abi", "calculator",
                             "spawn", "install"]),
    ("The build",           ["makefile", "cmake", "prerequisite", "pkill",
                             "encoding", "issue script", "dependency file"]),
    ("Tests and harnesses", ["harness", "the rig", "test suite", "self-test",
                             "fixture", "checker", "verify-kernel", "check.sh",
                             "a test", "tests"]),
    ("Signing and integrity", ["signing key", "signature", "rsa", "ecdsa",
                             "keyring", "secret", "digest"]),
]


def plain(text):
    """The words, with no markup at all.

    The classifier reads this rather than the rendered HTML, and that is not a
    detail: rendering turns `**Found by** Joshua` into `by</strong> Joshua`, so
    a rule looking for "by joshua" matched nothing and fifty-four entries fell
    into the unplaced bucket. The chart still drew, tidily, with the wrong
    shape. Classify the source, render the output.
    """
    text = re.sub(r"[`*_\[\]]", " ", text)
    text = re.sub(r"<[^>]+>", " ", text)
    text = re.sub(r"&[a-z]+;", " ", text)
    return " ".join(text.split()).lower()


# --- the one thing here that is not derived ---------------------------------
#
# Which faults are worth reading is a judgement, and no amount of parsing
# produces one. So they are written down, by number, and the text is written
# here rather than pulled from the entry -- the entry explains the fault, and
# this explains why it is worth somebody's time. A number that no longer exists
# in the register is dropped rather than drawn, so deleting an entry cannot
# leave a highlight pointing at nothing.

CURATED = [
    ("BG-017", "Every context menu entry had done nothing, ever",
     "Right-clicking produced a menu and clicking an item in it did nothing -- "
     "every entry, in every menu, for weeks. Nothing could press a button "
     "without a person there to do it, so nothing had. This is why ReconOS can "
     "drive its own input now."),
    ("BG-013", "Four wrong explanations in a row",
     "A flicker that four reasoned theories each failed to explain. It was "
     "settled by measuring rather than by reasoning, which is the lesson: the "
     "fifth theory was not cleverer, it was instrumented."),
    ("BG-050", "The Calculator took the whole system down",
     "A struct grew a field and the ABI number did not. Every application "
     "shares the compositor's address space, so one module compiled against "
     "the old layout ended the desktop. The gate exists; it was not moved when "
     "the thing it gates changed."),
    ("BG-081", "A gigabyte of nothing, counted as used",
     "The kernel's page bitmap indexed from address zero. Invisible on x86_64, "
     "where RAM starts near zero. On aarch64 RAM starts at 1GB, so 262,433 "
     "bits stood for addresses that were never memory. An assumption true on "
     "the machine you develop on, in code written to be portable."),
    ("KF-144", "Every UEFI test booted whatever kernel was in the image first",
     "A fault fixed three times that faulted identically each time. What "
     "settled it was disassembling the reported address and finding it landed "
     "in the middle of an instruction -- so it was not an address in the "
     "binary being looked at. Five boot paths had been reporting green against "
     "a kernel that no longer existed."),
    ("KF-145", "A read-only page the kernel could write through, on some boot paths",
     "CR0.WP was only ever cleared and restored, never set, so whether "
     "read-only meant anything to the kernel depended on which firmware "
     "booted it. Copy-on-write is enforced by mapping a shared page "
     "read-only; on the paths where that bit was clear, the kernel's own "
     "write went into the page every other program was reading."),
    ("KF-146", "Sixteen bytes past the end, into the next field of the same thread",
     "The comment above the vector save area said 512 bytes covered "
     "thirty-two 128-bit registers with their two status words. Thirty-two "
     "sixteen-byte registers is exactly 512. It asserted the arithmetic the "
     "code got wrong, on one architecture only, for three checkpoints."),
]


def classify(entry, table):
    hay = entry["plain"]
    for label, words in table:
        for w in words:
            if w in hay:
                return label
    return None


def tally(entries, table, key):
    counts = {}
    unplaced = 0
    for e in entries:
        label = classify(e, table)
        e[key] = label
        if label is None:
            unplaced += 1
        else:
            counts[label] = counts.get(label, 0) + 1
    rows = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))
    return rows, unplaced


# --- the page --------------------------------------------------------------

STYLE = """
  :root {
    --ground:#F2F2F4; --panel:#FFFFFF; --sunk:#E8E8EC; --edge:#D2D2D9; --edge-firm:#B4B4BE;
    --ink:#15171C; --ink-soft:#4B505C; --ink-faint:#7C818E;
    --navy:#1F2A44; --oxblood:#8E1F1F;
    --done:#1B6136; --done-bg:#E4F0E8;
    --rule:1px solid var(--edge);
    --lift:0 1px 1px rgba(21,23,28,.04),0 10px 28px -20px rgba(21,23,28,.32);
  }
  @media (prefers-color-scheme: dark) {
    :root:not([data-theme="light"]) {
      --ground:#16181C; --panel:#1F2229; --sunk:#191C21; --edge:#2E323B; --edge-firm:#434955;
      --ink:#E8EAEE; --ink-soft:#A8AEBC; --ink-faint:#767D8C;
      --navy:#9DB4E8; --oxblood:#E0817F;
      --done:#77CE9B; --done-bg:#16281D;
      --lift:0 1px 1px rgba(0,0,0,.3),0 10px 28px -20px rgba(0,0,0,.8);
    }
  }
  :root[data-theme="dark"] {
    --ground:#16181C; --panel:#1F2229; --sunk:#191C21; --edge:#2E323B; --edge-firm:#434955;
    --ink:#E8EAEE; --ink-soft:#A8AEBC; --ink-faint:#767D8C;
    --navy:#9DB4E8; --oxblood:#E0817F;
    --done:#77CE9B; --done-bg:#16281D;
    --lift:0 1px 1px rgba(0,0,0,.3),0 10px 28px -20px rgba(0,0,0,.8);
  }

  * { box-sizing:border-box; }
  body { margin:0; background:var(--ground); color:var(--ink);
    font:400 15px/1.55 "IBM Plex Sans",ui-sans-serif,system-ui,sans-serif;
    -webkit-font-smoothing:antialiased; }
  .page { max-width:900px; margin:0 auto; padding:40px 22px 72px;
    display:flex; flex-direction:column; gap:34px; }
  h1,h2,h3 { font-family:Archivo,ui-sans-serif,system-ui,sans-serif; margin:0; text-wrap:balance; }
  .eyebrow { font:500 11px/1 "IBM Plex Mono",ui-monospace,monospace; letter-spacing:.16em;
    text-transform:uppercase; color:var(--ink-faint); }
  h1 { font-size:clamp(28px,5vw,40px); font-weight:700; letter-spacing:-.02em; line-height:1.08; }
  .standfirst { color:var(--ink-soft); max-width:62ch; margin:0; }
  h2 { font-size:13px; font-weight:700; letter-spacing:.13em; text-transform:uppercase;
    color:var(--ink-faint); padding-bottom:8px; border-bottom:1px solid var(--edge-firm); }
  section { display:flex; flex-direction:column; gap:14px; }
  .lede { margin:-4px 0 0; color:var(--ink-soft); font-size:14px; max-width:66ch; }
  code { font-family:"IBM Plex Mono",ui-monospace,monospace; font-size:.9em;
    background:var(--sunk); padding:1px 5px; border-radius:2px; }
  a { color:var(--navy); }

  .figures { display:grid; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); gap:14px; }
  .fig { background:var(--panel); border:var(--rule); border-top:3px solid var(--oxblood);
    border-radius:3px; padding:14px 16px; box-shadow:var(--lift); }
  .fig.ok { border-top-color:var(--done); }
  .fig .n { font-family:Archivo,sans-serif; font-weight:700; font-size:30px;
    letter-spacing:-.02em; line-height:1; font-variant-numeric:tabular-nums; }
  .fig .l { color:var(--ink-soft); font-size:13px; margin-top:4px; }

  .bar { display:grid; grid-template-columns:1fr 3fr auto; gap:12px; align-items:center;
    padding:6px 0; }
  .bl { font-size:13.5px; color:var(--ink-soft); }
  .btrack { background:var(--sunk); border:1px solid var(--edge); border-radius:2px; height:16px; }
  .bfill { height:100%; background:var(--navy); }
  .bfill.b2 { background:var(--oxblood); }
  .bn { font:500 13px "IBM Plex Mono",ui-monospace,monospace; color:var(--ink-soft);
    font-variant-numeric:tabular-nums; min-width:2.5ch; text-align:right; }

  .cases { display:grid; grid-template-columns:repeat(auto-fit,minmax(280px,1fr)); gap:14px; }
  .case { background:var(--panel); border:var(--rule); border-left:3px solid var(--oxblood);
    border-radius:3px; padding:14px 16px; }
  .case h3 { font-size:15px; font-weight:600; margin-bottom:6px; line-height:1.3; }
  .case p { margin:0; color:var(--ink-soft); font-size:13.5px; }

  .register { display:flex; flex-direction:column; gap:1px; background:var(--edge);
    border:var(--rule); border-radius:3px; overflow:hidden; }
  .bug { background:var(--panel); }
  .bug summary { padding:10px 14px; cursor:pointer; display:grid;
    grid-template-columns:auto 1fr auto; gap:10px; align-items:baseline; list-style:none; }
  .bug summary::-webkit-details-marker { display:none; }
  .bug summary:hover { background:var(--sunk); }
  .bug:focus-within summary { outline:2px solid var(--navy); outline-offset:-2px; }
  .bgt { font-size:14px; font-weight:500; }
  .bgn { font:500 12px "IBM Plex Mono",ui-monospace,monospace; color:var(--oxblood);
    margin-right:8px; }
  .area { font:500 10.5px/1 "IBM Plex Mono",ui-monospace,monospace; letter-spacing:.05em;
    text-transform:uppercase; color:var(--ink-faint); white-space:nowrap; }
  .body { padding:2px 14px 14px 14px; border-top:1px dashed var(--edge); margin-top:2px; }
  .was { margin:10px 0 8px; color:var(--ink-soft); font-size:13.5px; }
  .meta, .fix { margin:0; font:400 12.5px "IBM Plex Mono",ui-monospace,monospace;
    color:var(--ink-faint); }
  .fix { color:var(--done); margin-top:4px; }
  .iss { margin-left:6px; }

  .derived { border:var(--rule); border-left:3px solid var(--navy); background:var(--panel);
    padding:12px 15px; font-size:13.5px; color:var(--ink-soft); }
  .derived strong { color:var(--ink); }

  footer { color:var(--ink-faint); font-size:12.5px; border-top:var(--rule); padding-top:16px; }
  @media (max-width:560px) {
    .bar { grid-template-columns:1fr auto; }
    .btrack { display:none; }
    .bug summary { grid-template-columns:auto 1fr; }
    .area { display:none; }
  }
"""


def bars(rows, unplaced, css_class=""):
    out = []
    top = max([n for _, n in rows] + [1])
    for label, n in rows:
        pct = max(4, int(round(100.0 * n / top)))
        out.append(
            '<div class="bar"><div class="bl">%s</div>'
            '<div class="btrack"><div class="bfill %s" style="width:%d%%"></div></div>'
            '<div class="bn">%d</div></div>' % (html.escape(label), css_class, pct, n))
    if unplaced:
        out.append(
            '<div class="bar"><div class="bl">Not placed by the rules below</div>'
            '<div class="btrack"><div class="bfill %s" style="width:%d%%"></div></div>'
            '<div class="bn">%d</div></div>'
            % (css_class, max(4, int(round(100.0 * unplaced / top))), unplaced))
    return "\n".join(out)


def build(entries, source_mtime):
    fixed = [e for e in entries if e["fixed"]]
    open_ = [e for e in entries if not e["fixed"]]
    highest = max(e["n"] for e in entries)

    how, how_unplaced = tally(entries, SURFACED, "surfaced")
    where, where_unplaced = tally(entries, AREA, "area")

    parts = []
    parts.append('<!doctype html><html lang="en"><head><meta charset="utf-8">')
    parts.append('<meta name="viewport" content="width=device-width,initial-scale=1">')
    parts.append("<title>ReconOS Bug Register</title>")
    parts.append('<link rel="preconnect" href="https://fonts.googleapis.com">')
    parts.append('<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>')
    parts.append('<link rel="stylesheet" href="https://fonts.googleapis.com/css2?'
                 'family=Archivo:wght@600;700&family=IBM+Plex+Sans:ital,wght@0,400;0,500;0,600;1,400'
                 '&family=IBM+Plex+Mono:wght@400;500&display=swap">')
    parts.append("<style>%s</style></head><body>" % STYLE)

    parts.append('<div class="page">')
    parts.append('<header>')
    parts.append('<div class="eyebrow">ReconOS &middot; the register</div>')
    parts.append('<h1>%d faults, and what each one really was</h1>' % len(entries))
    parts.append('<p class="standfirst">Every fault ever found in ReconOS has a number, '
                 'assigned in the order it was found and never reused. A commit says what '
                 'changed; it does not say something was broken, that somebody hit it, or '
                 'that it is fixed now. This does &mdash; and this page is generated from '
                 '<code>docs/BUGS.md</code>, so it cannot drift from it.</p>')
    parts.append('</header>')

    parts.append('<div class="figures">')
    parts.append('<div class="fig"><div class="n">%d</div><div class="l">recorded, since the first</div></div>' % len(entries))
    # "with a fix recorded", not "fixed". Nine entries state no fix, and some of
    # those were certainly fixed -- the entry simply never said so. Calling them
    # open would put a claim on this page that the register does not make, and
    # calling them fixed would put one there that nobody checked.
    parts.append('<div class="fig ok"><div class="n">%d</div><div class="l">with a fix recorded</div></div>' % len(fixed))
    parts.append('<div class="fig"><div class="n">%d</div><div class="l">no fix recorded</div></div>'
                 % len(open_))
    parts.append('<div class="fig"><div class="n">%d</div><div class="l">highest number issued</div></div>' % highest)
    parts.append('</div>')

    if open_:
        parts.append('<div class="derived"><strong>%d entries record no fix.</strong> '
                     'That is what the register says, and it is not the same as saying '
                     'they are open &mdash; several were certainly fixed and the entry '
                     'never said so. They are: %s. Adding the missing line to each is a '
                     'job for whoever knows which commit closed it; guessing here would '
                     'put a claim on this page that nobody checked.</div>'
                     % (len(open_), ", ".join(e["id"] for e in open_)))

    parts.append('<div class="derived">The two charts below are <strong>derived, not '
                 'recorded</strong>. The register has no field for how a fault surfaced or '
                 'which part of the system it was in, so both are worked out by matching '
                 'phrases in the entry &mdash; which is a guess. The guesses that failed are '
                 'counted in each chart rather than being folded into a catch-all, because a '
                 'classifier that files everything under &ldquo;something else&rdquo; draws '
                 'exactly the same tidy picture as one that works.</div>')

    parts.append('<section><h2>How they surfaced</h2>')
    parts.append('<p class="lede">Ordered by how many. Read this as a rough shape rather '
                 'than a measurement.</p>')
    parts.append(bars(how, how_unplaced))
    parts.append('</section>')

    parts.append('<section><h2>Where they were</h2>')
    parts.append(bars(where, where_unplaced, "b2"))
    parts.append('</section>')

    known = {e["id"]: e for e in entries}
    picked = [(n, t, w) for (n, t, w) in CURATED if n in known]
    if picked:
        parts.append('<section><h2>Worth reading twice</h2>')
        parts.append('<p class="lede">Chosen rather than counted &mdash; which is why '
                     'these are the only words on this page not taken out of the '
                     'register.</p>')
        parts.append('<div class="cases">')
        for n, title, why in picked:
            parts.append('<article class="case"><h3><span class="bgn">%s</span> %s</h3>'
                         '<p>%s</p></article>'
                         % (n, html.escape(title), html.escape(why)))
        parts.append('</div></section>')

    parts.append('<section><h2>The register</h2>')
    parts.append('<p class="lede">Newest first. Each says what was actually wrong, rather '
                 'than what it looked like. Every one is also a '
                 '<a href="https://github.com/neogentrics/ReconOS/issues">GitHub issue</a>.</p>')
    parts.append('<div class="register">')

    for e in entries:
        parts.append('<details class="bug">')
        parts.append('<summary><span class="bgn">%s</span><span class="bgt">%s</span>'
                     '<span class="area">%s</span></summary>'
                     % (e["id"], e["title"], html.escape(e["area"] or "unplaced")))
        parts.append('<div class="body">')
        if e["was"]:
            parts.append('<p class="was">%s</p>' % clip(e["was"], 320))
        issue = ('<a class="iss" href="https://github.com/neogentrics/ReconOS/issues/%s">#%s</a>'
                 % (e["issue"], e["issue"])) if e["issue"] else ""
        if e["found"]:
            parts.append('<p class="meta">Found %s %s</p>' % (clip(e["found"], 240), issue))
        if e["cost"]:
            parts.append('<p class="meta">Cost: %s</p>' % clip(e["cost"], 200))
        if e["fixed"]:
            parts.append('<div class="fix">Fixed in %s</div>' % clip(e["fixed"], 260))
        else:
            parts.append('<div class="fix" style="color:var(--oxblood)">'
                         'No fix recorded in the register</div>')
        if e["note"]:
            parts.append('<p class="meta">Note: %s</p>' % clip(e["note"], 220))
        parts.append('</div></details>')

    parts.append('</div></section>')

    parts.append('<footer>Generated from <code>docs/BUGS.md</code> by '
                 '<code>scripts/make-bug-register.py</code>. '
                 'Every figure on this page comes out of that file; nothing is carried '
                 'between runs. Source last changed %s.</footer>' % html.escape(source_mtime))
    parts.append('</div></body></html>')
    return "\n".join(parts), how_unplaced, where_unplaced


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "bug-register.html"))
    ap.add_argument("--check", action="store_true",
                    help="parse and report without writing")
    args = ap.parse_args()

    md = io.open(SOURCE, encoding="utf-8").read()
    entries = parse(md)

    if not entries:
        sys.stderr.write("no entries found in %s -- has the heading format changed?\n"
                         % SOURCE)
        return 1

    import datetime
    stamp = datetime.datetime.fromtimestamp(
        os.path.getmtime(SOURCE)).strftime("%d %B %Y")

    page, how_unplaced, where_unplaced = build(entries, stamp)

    # The guard the header talks about. A quarter is not a principled number;
    # it is far enough above today's figure to leave room and far below the
    # point where the chart would be fiction.
    worst = max(how_unplaced, where_unplaced)
    if worst * 4 > len(entries):
        sys.stderr.write(
            "%d of %d entries could not be classified -- the phrase rules have "
            "stopped matching how entries are written. Fix the rules rather than "
            "the threshold.\n" % (worst, len(entries)))
        return 1

    print("%d entries, %d fixed, %d open, highest %d"
          % (len(entries),
             len([e for e in entries if e["fixed"]]),
             len([e for e in entries if not e["fixed"]]),
             max(e["n"] for e in entries)))
    print("unplaced: %d by how it surfaced, %d by area" % (how_unplaced, where_unplaced))

    if args.check:
        return 0

    outdir = os.path.dirname(args.out)
    if outdir and not os.path.isdir(outdir):
        os.makedirs(outdir)
    io.open(args.out, "w", encoding="utf-8", newline="\n").write(page)
    print("wrote %s (%d bytes)" % (args.out, len(page)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
