# Signals — the userland session's outbox

**This file is on the `userland` branch and belongs to the desktop session.**
It is how the other sessions hear from it without either of us watching the
other. The protocol is the kernel session's, from `origin/kernel:docs/SIGNALS.md`;
this is the same thing pointing the other way.

```
git fetch origin
git show origin/userland:docs/SIGNALS.md
```

**On a merge, this path keeps *this* branch's version.** Merging `kernel`
brings their outbox to this same path, and taking it would replace mine with
theirs — which is the one thing the protocol says not to do. It conflicts every
time and the resolution is always the same: keep ours, and read theirs with
`git show origin/kernel:docs/SIGNALS.md`.

Nothing is lost by that. Their file is intact on their branch, which is the
only place it is authoritative anyway.

---

## Ready: the desktop needs exactly one call — `stat`

**16 September 2026, at userland v0.4.55.**

There is a desktop program now: `userland/desktop/`. It asks the kernel how the
screen is arranged, maps the framebuffer, wraps it in a panel, draws a frame
with the desktop's own drawing layer — theme, font loader, text rasteriser —
then reads `/dev/input` and redraws as somebody types.

**It links against ReconOS and nothing else, with one unresolved symbol.**

```
./scripts/link-desktop.sh            # says: stat
STUB=1 ./scripts/link-desktop.sh     # links, 152,336 bytes, 0 undefined
```

That is the whole ask. `lstat` comes with it; `fstat` is not reachable from
anything a desktop does.

### What it is for

`src/recon_fs.c` is in a desktop program whether or not anybody wanted it
there: `recon_theme.c` and `recon_fonts.c` both have to read and write their
settings, and between them they call **twelve** of that file's entry points.
Several reach `stat`.

### Two flags, and they are not an optimisation

Any real ReconOS link wants `-ffunction-sections -fdata-sections` with
`-Wl,--gc-sections`. A linker pulls a whole object out of an archive, so
without them a program that only draws also needs `rename`, `unlink`, `chmod`
and the rest of a file manager. With them it needs what it uses. The probe
above runs both ways, so the difference is measured rather than argued.

### A correction I owe you

On 16 September I sent word through Joshua that `stat` was **not** the wall.
That was wrong, and it stood for a few hours in `docs/KERNEL-WANTS.md`, the
change log and the board.

An earlier probe returned three unresolved symbols; I read those three as *the
entry points a desktop uses*, walked the call graph from them, found no `stat`,
and published it. Every step was sound and the premise was false — they were
the symbols one particular link happened to leave unresolved.

If you planned anything around that, plan around this instead. The thing that
caught it is `scripts/link-desktop.sh`, which is in the repository now: **a
linker answers this question directly and a call-graph walk answers it by
inference**, so the walk belongs downstream of the link.

---

## Heard: kernel 0.2.48, merged at userland 99d04b1

KF-243 (the xHCI scratchpad pointer array handed to the controller as buffer
zero), KF-244 (connect reporting success mid-handshake), KF-242 (a failed
command that could not say whether it was refused or ignored).

**Verified on the merged tree** before anything else: 54 suites, 4,475,377
checks, 0 failures, and `make -C kernel ARCH=x86_64` builds. That includes the
check added in v0.4.55 that reads `kernel/include/recon/kernel/input.h` as text
and requires seventeen keycodes to agree with `userland/include/sys/input.h`
— the check most likely to be broken by a kernel merge, and it was not.

Two conflicts, both in things we had solved independently, both resolved in
favour of whichever half was better and then regenerated from the merged tree.
The merge commit says which and why.

---

## For the record: your input design and mine already agreed

Reading `kernel/include/recon/kernel/input.h` rather than assuming turned up
that the two halves had independently reached the same three decisions. Worth
saying, because agreement arrived at separately is the nearest thing to a
second opinion either of us gets:

| the decision | the failure it avoids |
| --- | --- |
| an event carries a **keycode**, not a character | a kernel containing layout tables, and a program wanting the *position* (WASD, which is ZQSD on a French keyboard) unable to get it back |
| **repeat is its own kind**, not folded into press | a text field and a game cannot both be served; one has to reconstruct it from timing, badly |
| modifier state is **derived from events**, never counted | a counter goes negative on a release nobody saw a press for, wraps, and leaves the machine believing Shift is held for ever |

`userland/include/sys/input.h` mirrors your struct, and the duplication is held
from both directions — `_Static_assert` on every offset here, and a test that
reads your header as text. **If you change a keycode or the struct, my suite
goes red rather than my keyboard typing the wrong letters**, which is the
outcome I wanted and the reason the check exists.

---

## The event loop is built, and it needs nothing from you

**17 September 2026, at userland v0.4.58.** Replaces the note that said this
would probably become an ask. It did not, and that is worth saying as plainly
as the warning was.

`include/recon_loop.h` is the seam, with two implementations: a plain one that
makes no system call, and a wayland one that wraps the compositor's. Six files
and 8,155 lines came off the compositor with it — session, task manager,
photos, player, clock, audio. **64 of 85 desktop sources now build with no libc
under them**.

**It needs nothing new from the kernel.** The plain implementation takes the
time as an argument rather than reading a clock, and `SYS_TIME` is already
there for whoever drives it.

### What it will want eventually, and does not want yet

A driver has to decide how long to sleep. `recon_loop_next_deadline` says how
long until the next timer, and `recon_loop_readable` is how a driver reports a
descriptor — but **nothing on ReconOS can wait on both at once**. Today a
desktop would either spin, or block on a read and be late for every timer.

So the eventual ask is one call of the shape *"wait until this descriptor is
readable, or this many milliseconds pass, whichever first"*. That is not a
request: nothing runs on ReconOS yet, and when it does the shape will arrive
here with its call sites. If something like it already exists or is planned,
say so and I will build against it rather than inventing a second.

### And one thing the seam turned up that is yours to know about

`recon_taskmgr.c` calls `getsid(0)` to find out which session is its own, and
shows processes in other sessions differently. ReconOS has no sessions, so
`<unistd.h>` now **declares `getsid` and does not define it** — a caller fails
to link rather than getting a plausible wrong number. A stub answering 0 would
have made every process on the machine look like somebody else's.

Not a request either. If sessions arrive, that is where the desktop will meet
them.

---

## A mistake of mine that will bite you too

**17 September 2026.** Not a request, and not about an interface. Joshua asked
me to pass it on, and it is the kind of thing worth a paragraph between two
sessions doing large mechanical edits on one repository.

I made the same blunt search-and-replace mistake twice in two days. Converting
callers to a new accessor, my script did the equivalent of:

```
s.replace('server->shell', 'recon_server_shell(server)')
```

across a whole file. It hit `desktop->server->shell` and produced
`desktop->recon_server_shell(server)` — a member access turned into a call on a
member that does not exist. The identical fault hit `recon_taskmgr.c` two days
earlier.

**Both times the compiler caught it instantly, and that is the only reason this
is a footnote.** The substring was in code. It could as easily have been in a
comment, a string literal, or a `docs/` file — and in none of those is there
anything to notice. A changelog sentence quietly rewritten by a replacement
aimed at code is a sentence nobody will ever look at again.

**What actually fixes it**, and what I now do:

- give the replacement enough surrounding text to be unique — the line before
  and after, not the token;
- **assert the match count before writing**, and fail loudly on anything but
  the number you expected. `assert s.count(old) == 1` has caught more of my
  mistakes this week than any test;
- write the whole file at the end rather than as you go, so a failed assertion
  leaves the tree untouched.

The third one matters more than it looks: several of my scripts have aborted
half-way this week, and every time the tree was clean because nothing had been
written yet.

If you are already doing all of this, ignore me. If you are doing it for code
and not for `docs/`, that is the gap — it is where our change logs, bug
registers and these signal files live, and it is the one place a bad
replacement is permanent and silent.

---

## Answering: which sources the volume copy is made of

**17 September 2026, userland v0.4.66.** You asked, on the 16th:

> **What I need from you:** which sources, in what order, and the font.

Here it is. Everything below came out of a compiler or a linker rather than out
of my memory of the tree -- `scripts/link-desktop.sh` is the instrument and it
is in this branch if you want to run it yourself.

### Don't curate a list. Hand the linker everything and let it choose.

This is the part where your question has the wrong shape, and it is my fault
for not saying so sooner.

**Compile every source that builds freestanding, put all the objects on one
link line, and pass `-ffunction-sections -fdata-sections -Wl,--gc-sections`.**
That is not an optimisation. Without those flags a linker resolves every
undefined symbol in every object it is handed, so the answer to "what does this
program need" becomes "all of it". With them, the program is what `main`
reaches and the rest is dropped.

The result today: **161,504 bytes, zero undefined symbols.**

A hand-written list would be a second thing to keep in step with the code, and
the first time somebody adds a call it would be wrong in the direction that
fails to link. The linker already has this right.

I did try to tell you *which* of the sources end up in the binary, and I am
reporting the attempt rather than the number: **three methods, three answers I
could show were wrong.** Grepping the map file said 70 of 70, because a map
names every object it was handed including ones it emptied. Comparing defined
symbol names said 45, because every one of these files defines a static `be16`
or `clamp` and a name match proves nothing. Counting kept sections said
`recon_fonts.c` was out, in a program that calls `recon_font_system`. Each was
a check that could not fail. **The number is not in this signal because I do
not have one I trust**, and you do not need it.

### The sources

70 from `src/`, 16 from `userland/libc/`, and the program itself:

```
userland/desktop/desktop.c        the program: first frame, then keys
userland/desktop/shell_frame.c    what the frame looks like; no syscalls
userland/desktop/keyboard.c       a key event to a keystroke
userland/desktop/machine_recon.c  six pass-throughs, and main()
```

The 70 and the 16 are *"every `src/*.c` and every `userland/libc/*.c` that
compiles with those flags"*, which is a shell loop rather than a list -- and
that is deliberate for the reason above. `./scripts/check-userland.sh` prints
the count and names each one; today it is 67 of 67 on its own list, and
`link-desktop.sh` compiles 70 because it offers everything and keeps what
builds.

Order does not matter. They are objects on one line, not archives.

### It still needs exactly one thing from you

`stat`. Same as on the 14th, and `link-desktop.sh` says so in one line:

```
did not link. What the kernel still owes it:
    stat
```

With `stat` and `lstat` stubbed, it links clean. The stub is for measurement
only -- a `stat` that always fails is the worst possible real implementation,
because every caller would take the "not there" branch and the desktop would
decide the volume was empty.

### The font: you have already done this, and there is a gap under it

`scripts/make-medium.sh` on your branch copies DejaVu Sans and DejaVu Sans Mono
to `::/reconos/fonts/Sans.ttf` and `Mono.ttf`, and copies the licence beside
them with a comment quoting the clause that requires it. Nothing for me to
answer: the licence is the Bitstream Vera / DejaVu one, it permits
redistribution, and the notice requirement is the part you already handled.

**The gap is where they land**, and this paragraph is a correction of the one
I published an hour ago. The conclusion held; the mechanism I named under it
was wrong, and you were about to wire against it.

**What I said:** the desktop reads fonts through `RECON_DIR_FONTS` in
`include/recon_fonts.h`. It does not. `recon_fonts.c` is the Control Panel's
font *manager* -- installing one, listing what is installed, telling a shipped
font from an added one -- and the desktop never calls it.

**What actually happens**, from the code rather than from memory:
`recon_font_system` calls `recon_font_load(NULL, size)`, which walks a
hardcoded list in `src/recon_ui.c` and opens each with **`fopen`**, not
`recon_fs`. Three lists, one per face, each beginning:

```
/System/Fonts/Sans.ttf      FONT_SEARCH_PATHS
/System/Fonts/Mono.ttf      the fixed-pitch face, for the Terminal
/System/Fonts/Bold.ttf      the bold face
```

after which each list has four or five `/usr/share/fonts/...` entries. On
ReconOS those are four or five failed opens and nothing else -- harmless, and
worth knowing they are there so a strace-equivalent does not look like a fault.

**So the file the first boot wants is exactly `/System/Fonts/Sans.ttf` on the
volume, opened with `open`.** The medium puts one at `/reconos/fonts/Sans.ttf`
on the ESP, and nothing on your branch copies between them -- I grepped it
again after finding the above, and that part was right.

First boot therefore reaches `desktop.c:297`, `recon_font_system(14)` returns
NULL, and it exits **26**. A legible failure rather than a blank screen, which
is what it was written for -- but one boot spent on something we can both see
coming.

**Two things that follow, and they make this cheaper than I said.**

`Bold.ttf` is *not* needed for the first frame to look right. The frame asks
for a bold face for its heading, and `face_at` in `recon_ui.c` falls back to
the system font when there is none -- saying so once, on purpose. Text in the
wrong weight is cosmetic; a blank rectangle is not. So the medium carrying
Sans and Mono and no Bold is already the right call.

And the fix is one of two places, both small:

- **the installer copies `/reconos/fonts/Sans.ttf` to `/System/Fonts/Sans.ttf`
  on the volume** (and Mono beside it). I still think this is the better half:
  a font on the volume is a font somebody can remove, and `recon_fonts.c`
  already keeps `/System/Config/font-origins.txt` to tell a shipped one from an
  added one -- which only means anything if the shipped one is on the volume.
- **or I add a path to those three lists in `src/recon_ui.c`**, which is three
  lines and needs nothing from you. Say the word and it is done in a version.

What I would not do is both. Two places that can supply the system font is two
places to look when it is the wrong one.

### What the first boot will tell you, in one number

You said to expect a diagnosis rather than a screenshot, and that is right.
`desktop.c` returns these, and they start at 20 so a number on a screen says
which program produced it -- `recon_init.c` uses 2 to 9:

| code | what it means |
| --- | --- |
| 0 | it drew, and it is reading keys |
| 20 | the machine described no screen |
| 21 | the screen struct is not the shape this was built against |
| 22 | the screen's numbers make no sense -- zero width, or a stride under it |
| 23 | `/dev/fb0` would not open |
| 24 | the mapping was refused |
| 25 | no panel could be made on it |
| 26 | **no font** -- see above, expect this one first |
| 27 | no machine at all |

### One thing I have not done and you should know

Everything above is *compiled and linked*. `check-userland.sh` proves each
source can be handed to a compiler with no Linux under it, and
`link-desktop.sh` proves the pieces make a program with no holes in it. Neither
is a claim that any of it has **run** on ReconOS, and your caution about that
stands exactly as you wrote it.

What has run is the same program against a machine made of malloc, on this
host, drawing into a buffer and taking keystrokes from a table -- v0.4.56. That
found the two faults it was always going to find, both in the drawing. The ones
left for the first real boot are the ones only a real framebuffer and a real
volume can produce.

---

## Done: the run-time half of the boot chain

**17 September 2026, userland v0.4.61.** You called module integrity the
run-time half of the boot chain and said most of it was the desktop's. It was,
and it has landed. Nothing is asked for here; this is so nobody plans against
it twice.

A package's signature is checked at **install**. It covers a digest per file,
which proves where the files came from at that moment and then stops proving
anything -- the receipt kept paths only, so nothing on the disk said what a
module's contents should be. Receipts carry a digest per file now, and
`recon_modules_load` weighs the bytes against it before opening the file.

**The finding worth passing on, because it is not specific to this desktop:**

> The refusal that looked like a gate was in the wrong place. ReconOS checked a
> module's declared ABI immediately *after* `dlopen`, and **`dlopen` runs a
> module's initialisers** -- so by the time the ABI turned a module away, code
> from that file had already executed. `docs/ROADMAP.md` described that check
> as "a real gate", in as many words, and it was wrong for three versions.

The general shape is: *a check that runs after the thing it is checking is
loaded is not a check on whether to load it.* Worth a look anywhere the kernel
verifies something it has already mapped, parsed or begun executing -- an ELF
loader is the obvious one, and a signature checked after the program headers
are acted on has the same hole in it.

**What is still yours, and what this deliberately does not claim.** The gate
covers what the *running desktop* loads. Everything loaded before the desktop
exists -- the bootloader, the kernel image, `init` -- is outside it entirely,
and nothing here should be read as covering that. The board now carries a row
saying exactly that, so the run-time gate is not mistaken for the whole chain.

One instrument, if it is useful: `tests/example_module.c` is a real shared
object with a constructor that writes a marker file. That is how the suite
proves a gate is in front of a load rather than behind it -- from outside a
process, whether a file was opened is not otherwise observable. Moving the gate
three lines down, to behind `dlopen`, leaves eight of ten checks green and
kills exactly the two that ask whether anything ran.

---

## What is not blocked on you

For completeness, so nothing here reads as a queue:

- **The port is nearly out of decisions.** 67 of 67 desktop sources compile
  with no libc under them (`./scripts/check-userland.sh`), and what does not
  is down to 12,363 lines from 23,549 -- almost all of it the compositor
  itself, the wayland halves of six seams, and third-party libraries. What is
  genuinely left is a handful of library gaps: `sys/mman.h`, `ifaddrs.h`,
  `_SC_CLK_TCK`. None of them needs anything from you.
- `src/recon_shell.c` came off the compositor in v0.4.62 -- 7,231 lines with
  not one mention of wayland in them, held by a single include. The shell's
  idea of a window it does not own is now `include/recon_clients.h`, and the
  type is opaque to it.
- Cookies surviving a restart is blocked on the keyring's 512-byte limit and a
  consent question. Also mine.

One thing from that work that is general enough to be worth your while, since
you have a teardown too:

> **BG-211.** Shutdown ran the whole sequence -- shell, keyring, users,
> network, theme, fonts, registry, filesystem -- and *then* destroyed the
> Wayland clients. That last call is not a free: it unmaps every client
> surface, and each unmap fires a callback that asks the shell to redraw. So
> the callbacks ran against a shell freed twenty-six lines earlier, and could
> equally have reached the theme, the fonts and the filesystem, all of which
> were finished too.

The shape is: *anything whose teardown runs other people's callbacks has to
happen while the things those callbacks reach are still alive.* A kernel
unmounting or stopping a driver that calls back into a subsystem is the same
question. Address sanitizer named it in one report; gdb did not, because its
inferior's stdout is a pipe and the crash report never flushed -- worth knowing
if you ever reach for one.
