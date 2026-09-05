#!/usr/bin/env python3
"""Turn docs/BUGS.md into a page somebody can look at.

The register is the file; this is a view of it. Generated rather than written
by hand for the reason every generated thing here is: the hand-written version
went stale at 69 faults while the file said 81, and nobody noticed, because
nothing connected the two.

    python scripts/make-register.py [out.html]

Writes to /tmp/reconos-register.html unless told otherwise. The output is a
self-contained page -- no scripts, no external assets beyond a Google Fonts
stylesheet -- so it can be opened from disk or published as-is.
"""
import collections
import html
import io
import re
import sys

sys.stdout.reconfigure(encoding='utf-8')

OUT = sys.argv[1] if len(sys.argv) > 1 else '/tmp/reconos-register.html'

src = io.open('docs/BUGS.md', encoding='utf-8').read()
areas_src = io.open('scripts/make-issues.py', encoding='utf-8').read()
AREA = {int(n): a for n, a in re.findall(r"(\d+):\s*'([a-z]+)'", areas_src)}


def clean(text, limit=None):
    t = re.sub(r'\s+', ' ', text).strip().rstrip('.')
    t = re.sub(r'\*\*(.+?)\*\*', r'\1', t)
    t = html.escape(t)
    t = re.sub(r'`(.+?)`', r'<code>\1</code>', t)
    if limit and len(t) > limit:
        t = t[:limit].rsplit(' ', 1)[0] + '&hellip;'
    return t


def how(by):
    """How the fault surfaced, from the prose that says so.

    'Measurement' is its own answer and not a kind of investigation: those
    are the ones found by instrumenting something and reading the number,
    which is a different discipline from following a trail from another bug.
    """
    b = by.lower()
    if re.search(r'\bbg-\d+', b) or 'same investigation' in b:
        return 'Following another fault'
    if 'measurement' in b or 'measuring' in b:
        return 'Measuring it'
    if ('joshua' in b or 'using' in b or 'a person' in b or 'somebody' in b
            or 'running it' in b or 'typing' in b or 'counting' in b
            or 'coming out as' in b or 'clicking' in b):
        return 'Somebody using it'
    if 'capture' in b or 'screenshot' in b or 'looking at' in b or 'screen' in b:
        return 'Looking at the screen'
    if 'inject' in b or 'harness' in b or 'driving' in b or 'socket' in b:
        return 'An instrument built for it'
    if 'test' in b or 'self-test' in b:
        return 'A test'
    if 'compil' in b or 'warning' in b or 'build' in b:
        return 'The build'
    if 'read' in b or 'review' in b or 'writing' in b or 'audit' in b:
        return 'Reading the code'
    if 'first time' in b or 'wiring' in b or 'adding' in b:
        return 'The first thing to call it'
    return 'Something else'


entries = []
for block in re.split(r'\n### ', src)[1:]:
    head, _, rest = block.partition('\n')
    m = re.match(r'BG-(\d+)\s+[—-]+\s*(.+)', head.strip())
    if not m:
        continue

    n = int(m.group(1))
    issue = re.search(r'\[#(\d+)\]', rest)
    fi = re.search(r'\*\*Found in\*\*\s*(.+?)\.\s', rest, re.S)
    fb = re.search(r'\*\*Found by\*\*\s*(.+?)\.\s', rest, re.S)
    wa = re.search(r'\*\*Was\*\*\s*(.+?)(?=\n- \*\*|\Z)', rest, re.S)
    fx = re.search(r'\*\*Fixed in\*\*\s*(.+?)(?=\n- \*\*|\Z)', rest, re.S)

    by_raw = fb.group(1) if fb else ''
    entries.append({
        'n': n,
        'title': clean(m.group(2)),
        'issue': issue.group(1) if issue else None,
        'in': clean(fi.group(1)) if fi else '',
        'by': clean(by_raw),
        # BG-040 is a documentation fault and carries no "Was" line; its
        # title is the whole of it, so the summary says so rather than
        # showing an empty cell.
        'was': clean(wa.group(1), 260) if wa else
               '<em>A documentation fault &mdash; the title is the whole of it.</em>',
        'fixed': clean(fx.group(1), 150) if fx else '',
        'open': '**Open.**' in rest,
        'area': AREA.get(n, 'other'),
        'how': how(by_raw),
    })

entries.sort(key=lambda e: -e['n'])
total = len(entries)
open_count = sum(1 for e in entries if e['open'])
hows = collections.Counter(e['how'] for e in entries).most_common()
areas = collections.Counter(e['area'] for e in entries).most_common()

print('entries', total, 'open', open_count)
for k, v in hows:
    print(f'  {v:3} {k}')

# --- The page ---

AREA_LABEL = {
    'display': 'Drawing and windows', 'input': 'Input and clicking',
    'help': 'Help', 'skins': 'Skins', 'build': 'The build',
    'applications': 'Applications', 'network': 'Network', 'docs': 'Documentation',
    'programs': 'Programs', 'storage': 'Storage', 'startup': 'Startup',
    'accounts': 'Accounts', 'settings': 'Settings', 'firewall': 'Firewall',
    'kernel': 'Kernel', 'other': 'Other',
}

def bars(pairs, cls):
    top = max(v for _, v in pairs)
    out = []
    for label, v in pairs:
        pct = round(v * 100 / top)
        out.append(
            f'<div class="bar"><div class="bl">{html.escape(AREA_LABEL.get(label, label))}</div>'
            f'<div class="btrack"><div class="bfill {cls}" style="width:{pct}%"></div></div>'
            f'<div class="bn">{v}</div></div>')
    return '\n'.join(out)


rows = []
for e in entries:
    issue = (f' <a class="iss" href="https://github.com/neogentrics/ReconOS/issues/{e["issue"]}">#{e["issue"]}</a>'
             if e['issue'] else '')
    fixed = f'<div class="fix">Fixed in {e["fixed"]}</div>' if e['fixed'] else ''
    rows.append(f'''<details class="bug">
<summary><span class="bgn">BG-{e['n']:03d}</span><span class="bgt">{e['title']}</span><span class="area">{html.escape(AREA_LABEL.get(e['area'], e['area']))}</span></summary>
<div class="body">
<p class="was">{e['was']}</p>
<p class="meta">Found in {e['in']} &middot; by {e['by']}{issue}</p>
{fixed}
</div>
</details>''')

CASES = [
    ('BG-017', 'Every context menu entry had done nothing, ever',
     'Right-clicking produced a menu and clicking an item in it did nothing '
     '&mdash; every entry, in every menu, for weeks. Nothing could press a '
     'button without a person there to do it, so nothing had. This is why '
     'ReconOS can drive its own input now.'),
    ('BG-013', 'Four wrong explanations in a row',
     'A flicker that four reasoned theories each failed to explain. It was '
     'settled by measuring rather than by reasoning, which is the lesson: '
     'the fifth theory was not cleverer, it was instrumented.'),
    ('BG-050', 'The Calculator took the whole system down',
     'A struct grew a field and the ABI number did not. Every application '
     'shares the compositor&rsquo;s address space, so one module compiled '
     'against the old layout ended the desktop. The gate exists; it was not '
     'moved when the thing it gates changed.'),
    ('BG-080', 'A font could be set, and would never load',
     'The key was written, the page named the font, and every letter on the '
     'screen stayed as it was. The loader opens the file itself and wants '
     'the host&rsquo;s path; a font installed into ReconOS is named by its '
     'place inside ReconOS. Everything above the loader reported success.'),
    ('BG-081', 'A gigabyte of nothing, counted as used',
     'The kernel&rsquo;s page bitmap indexed from address zero. Invisible on '
     'x86_64, where RAM starts near zero. On aarch64 RAM starts at 1GB, so '
     '262,433 bits stood for addresses that were never memory. An assumption '
     'true on the machine you develop on, in code written to be portable.'),
]

cases = '\n'.join(
    f'<article class="case"><h3><span class="bgn">{c[0]}</span> {c[1]}</h3><p>{c[2]}</p></article>'
    for c in CASES)

page = f'''<title>ReconOS Bug Register</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Archivo:wght@600;700&family=IBM+Plex+Sans:ital,wght@0,400;0,500;0,600;1,400&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
  :root {{
    --ground:#F2F2F4; --panel:#FFFFFF; --sunk:#E8E8EC; --edge:#D2D2D9; --edge-firm:#B4B4BE;
    --ink:#15171C; --ink-soft:#4B505C; --ink-faint:#7C818E;
    --navy:#1F2A44; --oxblood:#8E1F1F;
    --done:#1B6136; --done-bg:#E4F0E8;
    --rule:1px solid var(--edge);
    --lift:0 1px 1px rgba(21,23,28,.04),0 10px 28px -20px rgba(21,23,28,.32);
  }}
  @media (prefers-color-scheme: dark) {{
    :root:not([data-theme="light"]) {{
      --ground:#16181C; --panel:#1F2229; --sunk:#191C21; --edge:#2E323B; --edge-firm:#434955;
      --ink:#E8EAEE; --ink-soft:#A8AEBC; --ink-faint:#767D8C;
      --navy:#9DB4E8; --oxblood:#E0817F;
      --done:#77CE9B; --done-bg:#16281D;
      --lift:0 1px 1px rgba(0,0,0,.3),0 10px 28px -20px rgba(0,0,0,.8);
    }}
  }}
  :root[data-theme="dark"] {{
    --ground:#16181C; --panel:#1F2229; --sunk:#191C21; --edge:#2E323B; --edge-firm:#434955;
    --ink:#E8EAEE; --ink-soft:#A8AEBC; --ink-faint:#767D8C;
    --navy:#9DB4E8; --oxblood:#E0817F;
    --done:#77CE9B; --done-bg:#16281D;
    --lift:0 1px 1px rgba(0,0,0,.3),0 10px 28px -20px rgba(0,0,0,.8);
  }}

  * {{ box-sizing:border-box; }}
  body {{ margin:0; background:var(--ground); color:var(--ink);
    font:400 15px/1.55 "IBM Plex Sans",ui-sans-serif,system-ui,sans-serif;
    -webkit-font-smoothing:antialiased; }}
  .page {{ max-width:900px; margin:0 auto; padding:40px 22px 72px;
    display:flex; flex-direction:column; gap:34px; }}
  h1,h2,h3 {{ font-family:Archivo,ui-sans-serif,system-ui,sans-serif; margin:0; text-wrap:balance; }}
  .eyebrow {{ font:500 11px/1 "IBM Plex Mono",ui-monospace,monospace; letter-spacing:.16em;
    text-transform:uppercase; color:var(--ink-faint); }}
  h1 {{ font-size:clamp(28px,5vw,40px); font-weight:700; letter-spacing:-.02em; line-height:1.08; }}
  .standfirst {{ color:var(--ink-soft); max-width:62ch; margin:0; }}
  h2 {{ font-size:13px; font-weight:700; letter-spacing:.13em; text-transform:uppercase;
    color:var(--ink-faint); padding-bottom:8px; border-bottom:1px solid var(--edge-firm); }}
  section {{ display:flex; flex-direction:column; gap:14px; }}
  .lede {{ margin:-4px 0 0; color:var(--ink-soft); font-size:14px; max-width:66ch; }}
  code {{ font-family:"IBM Plex Mono",ui-monospace,monospace; font-size:.9em;
    background:var(--sunk); padding:1px 5px; border-radius:2px; }}
  a {{ color:var(--navy); }}

  .figures {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); gap:14px; }}
  .fig {{ background:var(--panel); border:var(--rule); border-top:3px solid var(--oxblood);
    border-radius:3px; padding:14px 16px; box-shadow:var(--lift); }}
  .fig.ok {{ border-top-color:var(--done); }}
  .fig .n {{ font-family:Archivo,sans-serif; font-weight:700; font-size:30px;
    letter-spacing:-.02em; line-height:1; font-variant-numeric:tabular-nums; }}
  .fig .l {{ color:var(--ink-soft); font-size:13px; margin-top:4px; }}

  .bar {{ display:grid; grid-template-columns:1fr 3fr auto; gap:12px; align-items:center;
    padding:6px 0; }}
  .bl {{ font-size:13.5px; color:var(--ink-soft); }}
  .btrack {{ background:var(--sunk); border:1px solid var(--edge); border-radius:2px; height:16px; }}
  .bfill {{ height:100%; background:var(--navy); }}
  .bfill.b2 {{ background:var(--oxblood); }}
  .bn {{ font:500 13px "IBM Plex Mono",ui-monospace,monospace; color:var(--ink-soft);
    font-variant-numeric:tabular-nums; min-width:2.5ch; text-align:right; }}

  .cases {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(280px,1fr)); gap:14px; }}
  .case {{ background:var(--panel); border:var(--rule); border-left:3px solid var(--oxblood);
    border-radius:3px; padding:14px 16px; }}
  .case h3 {{ font-size:15px; font-weight:600; margin-bottom:6px; line-height:1.3; }}
  .case p {{ margin:0; color:var(--ink-soft); font-size:13.5px; }}
  .bgn {{ font:500 12px "IBM Plex Mono",ui-monospace,monospace; color:var(--oxblood);
    margin-right:8px; }}

  .register {{ display:flex; flex-direction:column; gap:1px; background:var(--edge);
    border:var(--rule); border-radius:3px; overflow:hidden; }}
  .bug {{ background:var(--panel); }}
  .bug summary {{ padding:10px 14px; cursor:pointer; display:grid;
    grid-template-columns:auto 1fr auto; gap:10px; align-items:baseline; list-style:none; }}
  .bug summary::-webkit-details-marker {{ display:none; }}
  .bug summary:hover {{ background:var(--sunk); }}
  .bug:focus-within summary {{ outline:2px solid var(--navy); outline-offset:-2px; }}
  .bgt {{ font-size:14px; font-weight:500; }}
  .area {{ font:500 10.5px/1 "IBM Plex Mono",ui-monospace,monospace; letter-spacing:.05em;
    text-transform:uppercase; color:var(--ink-faint); white-space:nowrap; }}
  .body {{ padding:2px 14px 14px 14px; border-top:1px dashed var(--edge); margin-top:2px; }}
  .was {{ margin:10px 0 8px; color:var(--ink-soft); font-size:13.5px; }}
  .meta, .fix {{ margin:0; font:400 12.5px "IBM Plex Mono",ui-monospace,monospace;
    color:var(--ink-faint); }}
  .fix {{ color:var(--done); margin-top:4px; }}
  .iss {{ margin-left:6px; }}

  footer {{ color:var(--ink-faint); font-size:12.5px; border-top:var(--rule); padding-top:16px; }}
  @media (max-width:560px) {{
    .bar {{ grid-template-columns:1fr auto; }}
    .btrack {{ display:none; }}
    .bug summary {{ grid-template-columns:auto 1fr; }}
    .area {{ display:none; }}
  }}
</style>

<div class="page">
  <header>
    <div class="eyebrow">ReconOS &middot; the register</div>
    <h1>{total} faults, and what each one really was</h1>
    <p class="standfirst">Every fault ever found in ReconOS has a number, assigned in the
      order it was found and never reused. A commit says what changed; it does not say
      something was broken, that somebody hit it, or that it is fixed now. This does.</p>
  </header>

  <div class="figures">
    <div class="fig"><div class="n">{total}</div><div class="l">recorded, since the first</div></div>
    <div class="fig ok"><div class="n">{total - open_count}</div><div class="l">fixed</div></div>
    <div class="fig ok"><div class="n">{open_count}</div><div class="l">still open</div></div>
    <div class="fig"><div class="n">{dict(hows).get('Somebody using it', 0)}</div><div class="l">found by somebody using it</div></div>
  </div>

  <section>
    <h2>How they surfaced</h2>
    <p class="lede">The largest single answer, by a distance, is somebody clicking
      something. Most of the rest were found by looking at what the system actually
      drew, or by following the trail out of another fault &mdash; and a handful by
      being the first code ever to call a path that had been sitting there working
      in theory.</p>
    {bars(hows, '')}
  </section>

  <section>
    <h2>Where they were</h2>
    {bars(areas, 'b2')}
  </section>

  <section>
    <h2>Worth reading twice</h2>
    <div class="cases">{cases}</div>
  </section>

  <section>
    <h2>The register</h2>
    <p class="lede">Newest first. Each says what was actually wrong, rather than what it
      looked like. Every one is also a
      <a href="https://github.com/neogentrics/ReconOS/issues">GitHub issue</a>.</p>
    <div class="register">
{chr(10).join(rows)}
    </div>
  </section>

  <footer>
    Generated from <code>docs/BUGS.md</code>, which is the register.
    <code>python scripts/make-issues.py</code> keeps GitHub in step with it.
  </footer>
</div>
'''

io.open(OUT, 'w', encoding='utf-8', newline='').write(page)
print('wrote', OUT, len(page), 'bytes')
