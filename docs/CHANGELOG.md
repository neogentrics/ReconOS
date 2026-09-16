# ReconOS change log

What changed in each version, newest first. The version number tracks what
works, not what is planned.

This file is the source. `scripts/make-help.sh` turns it into the pages the
Help application shows, so there is one place to write a change down and no
way for the two to disagree.

---

## v0.4.58 — waiting, as its own thing

`scripts/port-blockers.sh` said it in one line last version: six files, 8,155
lines, and between them they used **exactly one thing** from the compositor —
the event loop. `recon_session.c` is three thousand lines about signing in and
shutting down and mentioned wayland twice. `recon_player.c` is twelve hundred
lines about playing a film and mentioned it not at all; it only held a
`wl_event_source` so a frame could arrive on time.

`include/recon_loop.h` is the seam, and it is the fifth this project has drawn
— after the allocator, the screen, the panel and the kernel.

**62 of 85 sources now build with no libc under them**, up from 58. Session,
task manager, photos and player are among them.

### The interface was written to what the callers already did

A timer is **one-shot** and re-armed by its own callback, and **zero disarms**.
That is not a simplification of wayland's — it *is* wayland's, and more to the
point it is what all six files already assumed. `on_tick` in `recon_player.c`
ends by re-arming itself, and so do the audio top-up, the boot progress and the
task manager's refresh.

Making it repeating instead would have been the quiet kind of wrong: every
caller re-arms, so **every rate in the desktop would have doubled** — which
reads as a machine that feels busy rather than one that is broken.

### Two things it deliberately does not do

**It does not read a clock.** `recon_loop_tick` takes the time. That is the
same choice `screen.c` and `shell_frame.c` make, and it is what lets every
interesting question be asked from a test rather than by watching a machine and
counting: does it fire late, does it fire twice, does a callback that re-arms
itself run away.

**It does not poll.** There is no `poll` in the interface, because ReconOS has
none — putting one in the shape of the header would have been describing a
mechanism that does not exist, and the first implementation would have had to
invent it. Whoever owns the loop knows how it finds out and says so with
`recon_loop_readable`.

### Two implementations, which is the point

`src/recon_loop.c` is a fixed table and no system call. `src/recon_loop_wl.c`
wraps the compositor's, and its mapping is close **because wayland's shape was
the right one to copy** — a seam that had to translate between two different
ideas of a timer would be a seam with a bug in it.

The wayland half refuses the driving calls rather than omitting them, so a
caller that wandered onto the wrong half gets a null pointer rather than a link
error somebody silences by adding a file. And `recon_timer_is_armed` answers
**-1** there, because wayland does not say and a guess would be worse than an
admission.

### 57 checks, 14 of 14 mutations caught — and one of them deleted a guard

The first pass caught 12. One miss was a bad anchor. The other was the
interesting one.

**The clock-went-backwards guard was guarding against nothing.** Its comment
said unsigned arithmetic would wrap and every timer would come due in half a
billion years. Deleting it changed no output — so I looked at why, instead of
writing a test that would have made it appear load-bearing. **Every use of the
time is an addition or a comparison**, and the one subtraction is already
guarded by the check above it. The failure it described cannot happen here.

And it cost something real: with the clamp, a timer armed *after* a backward
jump got a deadline relative to the old time and waited for the clock to catch
up — fifty seconds late for a fifty-second jump. Gone, and the check that
replaces it holds the behaviour that matters: **a timer is armed relative to
the clock its caller is reading.**

That is the third comment this session to claim a guard and have none. The rule
holds, and so does the way of finding out: delete the line and see whether
anything notices.

### Four things the port turned up on the way

- **`recon_clock.c` reads before it judges now.** Its handler took a mask and
  decided *"the time server did not answer"* before reading anything. Reading
  first tells apart the two cases that matter — nothing came back, and
  something came back that is not SNTP — so the seam having no mask made the
  diagnosis **sharper**, not poorer.
- **`return 0;` in callbacks that now return nothing**, in five files. The
  ordinary build reports that as a warning and lets it through; the
  freestanding check refuses it. Stricter, and right to be.
- **`recon_taskmgr.c` used `typeof`**, which is a GNU extension and not C11.
  `__typeof__` is the spelling that is legal in strict mode. Pre-existing, and
  invisible until something compiled that file strictly.
- **`getsid` is declared and not defined** in `<unistd.h>` now. ReconOS has no
  sessions, and a stub answering 0 would make the task manager show every
  process on the machine as somebody else's.

---

## v0.4.57 — what is left of the port, measured

`scripts/check-userland.sh` says how many sources build with no Linux under
them. It says nothing about the rest, and **the rest is where the decisions
are.**

`scripts/port-blockers.sh` answers the other question: for every source that
does not build, what stops it and **how many lines it is holding**. Three
columns, and the shape to look for is a small marker count beside a large line
count — a file that is about something else, held by an accident.

It was written because that shape keeps turning up and nobody keeps predicting
it:

| | what was holding what |
|---|---|
| v0.4.42 | `recon_ui.h` pulled xkbcommon for **one typedef**. Twenty-four files. |
| v0.4.44 | `recon_ui.c` was 3,100 lines of drawing, **28** of which said wlroots. |
| v0.4.46 | `recon_server.h` — a whole compositor — included by five applications for one field, and by **two of them for nothing at all**. |
| v0.4.57 | `recon_appwin.c` was 1,629 lines held by **one forwarding function**. |

Each time the file's own header had a theory about why it was stuck, and each
time the theory was wrong.

### `recon_appwin.c` is off wlroots, and it was one line

Sixteen hundred lines about what a window *is* — its frame, its title, its
buttons, where its edges are, what a drag on one of them means — and not one of
them is about wayland. What held it was `recon_appwin_node()`, which forwards
to `recon_panel_node(win->panel)`, and `recon_panel_node` already lived in
`recon_ui_wlr.c`: the file v0.4.44 split off for exactly this.

So `src/recon_appwin_wlr.c` is the same split one layer up, and it is
deliberately twenty-five lines. **If it grew, the seam would be in the wrong
place.** What replaced the function here is `recon_appwin_panel()` — an
accessor, not an internal header, because nothing needed to reach *inside* a
window the way the presentation reaches inside a panel.

The other include, `<wlr/util/log.h>`, was used by nothing.

Two more went onto `recon_server_facts.h`, which is the file that exists to be
*"the one place allowed to include recon_server.h on their behalf"*:
`recon_server_window_panel()` (a window asking for something to draw on was
reaching through `server->layer_windows`, which is a compositor idea a window
has no other reason to know) and a declaration of `recon_damage_all` — one
function, one definition, and what changes is which header a caller needs.

**58 of 85 sources now build with no libc under them.**

### And the report named the next piece in one line

Twenty-seven sources still do not build, holding **32,593 lines**. The report
makes it obvious that they are not twenty-seven problems:

| source | lines | mentions a compositor |
|---|---|---|
| `recon_shell.c` | 7,219 | 21 |
| `recon_cmd.c` | 3,626 | 3 |
| `recon_session.c` | 3,078 | **2** |
| `recon_taskmgr.c` | 1,616 | 2 |
| `recon_player.c` | 1,254 | **0** |
| `recon_photos.c` | 1,190 | **0** |
| `recon_clock.c` | 616 | 1 |
| `recon_audio.c` | 401 | 1 |

Asking what those actually *use* gives one answer: **the event loop.**
`wl_event_loop_add_timer`, `wl_event_loop_add_fd`, `wl_event_source_remove`,
`wl_event_source_timer_update`. Nothing else.

Six files — **8,155 lines** — are behind one seam that does not exist yet, and
it is a seam for two things a system needs anyway: *call me back later*, and
*tell me when this is ready to read*. That is a decision before it is code, so
it is written down here rather than started at five in the morning.

### One duplicate include

`recon_photos.c` included `recon_server.h` twice, three lines apart, with
another include between them — which is how it hid: the list reads correctly if
you stop at the first one. Harmless to the compiler, and exactly the sort of
thing a report that reads every file turns up on the way past.

---

## v0.4.56 — the desktop program runs

Not on hardware — `stat` is still the one thing missing for that. It runs
**here**, against a machine made of `malloc` and a table of keystrokes, and the
suite watches what it does.

### The file that mattered most was the only one nothing could execute

Everything else in `userland/desktop/` was already testable on the host:
`shell_frame.c` takes a panel and draws, `keyboard.c` takes events and gives
back keystrokes. Both were written that way on purpose.

`main.c` was the exception — and it is **the worst file to have as the
exception**, because it is the one that decides whether a machine shows a
desktop or a black screen. Eight ways to fail, each with its own exit code, and
not one of them had ever been executed anywhere.

It could not be shimmed the way the C library is. `libc/` reaches the kernel
through `recon_sys_open` and friends, so `userland/tests/hostsys.c` replaces
that layer and the whole library runs on Linux. But `recon_screen` and
`recon_map` are **`static inline` in `<recon.h>` and expand to a raw `syscall`
instruction** — there is no function to replace, and running one on Linux would
issue a Linux system call with a ReconOS number.

### So the calls went behind the seam, for the fourth time

`userland/desktop/machine.h`: six function pointers — screen, open, map, read,
facts, and *carry on*. `machine_recon.c` is the ReconOS implementation and is
**six pass-throughs and an entry point**, which is the measure of whether a
seam is in the right place: everything with a decision in it ended up on the
other side.

The same argument as `recon_memory_source` for the allocator and
`recon_panel_present` for the screen, and it has held each time.

`carry_on` is worth a line. The real machine always says yes and the loop never
ends, which is right — a desktop that returned would leave whatever the kernel
draws next on the screen, and a frame that drew correctly would look like one
that crashed. A test says no, eventually. **It is an honest hook rather than a
test-only one**: "should this keep running" is a real question that a real
system happens to always answer the same way.

`main.c` is `desktop.c` now, since `main()` moved to the file that chooses a
machine. A file called `main.c` with no `main` in it is worse than a rename.

### What the 22 checks hold

All of it about what happens when something is wrong, because that is all this
file decides:

- **Eight ways to fail, each saying which.** An exit code is the only thing a
  program that cannot draw can still say, and a wrong one sends somebody
  looking at the wrong half of the machine.
- **The frame is drawn before the keyboard is opened.** Reversing it fails
  three checks, and the extra two are the point: with the order swapped, a
  machine with nothing plugged in **never draws at all**. That is the black
  screen the order exists to avoid.
- **A short read is dropped rather than parsed.** Half an event read as a whole
  one is a keypress that never happened.
- **A machine that cannot describe itself still gets a desktop**, and so does
  one with no way to ask at all.

### And the order check was wrong first

It proxied *"had it drawn?"* through a flag set in `carry_on` — which is not
called until the loop starts, so it read false however early the drawing
happened, and failed a program doing the right thing. It looks at the
framebuffer now. **A proxy for the thing is not the thing.**

### Two includes corrected on the way

`userland/include/recon_machine.h` included `<recon.h>` with angle brackets,
which works for a program built with `-I userland/include` and fails for
anything reaching it by relative path. Quoted now, like `sys/input.h` and
`libc/posix.c` — *"so it finds the one beside it"*.

That is not decoration: a suite **cannot** put `userland/include` on its path,
because ReconOS's `<stdio.h>` would then answer instead of the host's, and it
deliberately has no `printf`. Its own header argues there is no `printf` to a
stream this library cannot usefully flush, which is a decision rather than a
gap — so the include path is what has to bend, and it did.

---

## v0.4.55 — the desktop can be typed at

The desktop program reads `/dev/input` and redraws. A key goes down, the kernel
turns a scancode into a **position**, `recon_key.c` turns the position into a
**meaning**, and the frame shows the letter — which is the whole path, and the
first time any of it has had a caller.

`recon_key_from_hid` was written and mutation-tested in v0.4.51 **with nothing
calling it at all.** This is the caller.

### The kernel's design and this one already agreed

Reading `kernel/include/recon/kernel/input.h` rather than assuming turned up
that the two halves had independently reached the same three decisions, which
is worth more than the code:

| the decision | why both sides made it |
|---|---|
| An event carries a **keycode**, not a character | a kernel that hands up characters has to contain a layout table, and a program wanting the *position* (WASD, which is ZQSD on a French keyboard) can never get it back |
| **Repeat is its own kind**, not folded into press | a text field wants repeats and a game does not; collapsing them makes the first impossible to tell from the second |
| Modifier state is **derived from events**, never counted | a counter goes negative on a release nobody saw a press for, wraps, and leaves the machine believing Shift is held for ever |

### The hazard is a struct written down twice

`userland/include/sys/input.h` mirrors a kernel struct, and the two headers
cannot be one file — the kernel's pulls in kernel types and its VFS. **A struct
read with the wrong layout does not fail; it returns numbers.** A `code` read
at the wrong offset is a different key, and a `kind` read at the wrong offset
is a press that arrives as a release.

So it is held from both directions: `_Static_assert` on every offset and the
total size makes a disagreement a **compile error**, and the suite **reads the
kernel's header as text** and requires seventeen constants to match — which is
what catches the kernel changing a number underneath us. The same arrangement
`layout.c` and `recon_fs.h` already use for the directory list.

And at run time, a read whose length is not a whole number of events is
**dropped rather than parsed**: half an event read as a whole one is a keypress
that never happened.

### 74 checks, 11 of 12 mutations caught, and two findings in the misses

Every mutation produces a keyboard that *works* — letters appear, keys respond
— and is wrong in a way somebody would blame on themselves. Two survived, and
they were different in kind:

- **A Caps Lock branch was dead, and its comment was wrong.** It claimed to
  catch a case *"the comparison above cannot see"* — pressing Caps Lock when it
  is already on. But the modifiers differ from before in *both* directions, so
  the general rule catches it either way. Deleted. The same fault as the
  v0.4.45 comment calling an early return a guard when the formula was exact at
  both ends: **a comment claiming a guard has to be a guard.**
- **The other cannot be caught, provably.** Swapping `keyboard->modifiers` for
  `before` changes nothing, because the line above returns unless the two are
  equal. It is recorded as an equivalent mutant rather than chased — writing a
  check that appeared to catch it would be writing a check that cannot fail,
  and three of those were written yesterday already.

### Two small gaps closed on the way

`recon.h` had `u8`, `u32`, `u64` and `i64` and **not `u16` or `i32`** — so a
program could not spell a sixteen-bit field at all, and anything describing one
had to reach for `unsigned short` and hope.

And `sys/input.h` deliberately does **not** include `sys/types.h`, the sibling
a file in that directory would reach for by habit: it defines `off_t`, which
collides with the host's the moment anything includes both — and a suite that
drives this struct is exactly such a thing.

---

## v0.4.54 — there is a desktop program

`userland/desktop/` is the program that runs from the volume. It asks the
kernel how the screen is arranged, maps the framebuffer, wraps it in a panel,
and draws a frame with **the desktop's own drawing layer** — `recon_theme`,
`recon_ui`, the font loader, the text rasteriser. Then it stays up.

It is the other half of the split agreed with the kernel session: **the
built-in stays `screen.c`, the copy on the volume becomes the desktop.** The
obvious alternative is wrong in a way that is easy to miss — growing the
built-in program into the desktop would put forty-odd sources inside the
`.incbin`'d fallback, making the program that exists for *"the volume is
unreadable"* the one with the most code in it.

**It links.** `scripts/link-desktop.sh` builds it against every desktop source
that compiles freestanding, the ReconOS C library and nothing else: with `stat`
supplied it is **152,336 bytes with zero undefined symbols**, and without it,
`stat` is the one thing missing. The probe measures **the real program** now
rather than a stand-in — which it earned in its first run, by refusing to
compile `main.c` over two field names that had been written from memory.

It is written before it can run, deliberately. `docs/KERNEL-WANTS.md` records
what happened the last time: *"Nothing in userland/ was rebuilt for it —
mem_recon.c had been sending that exact call since the allocator was written,
so the day the kernel accepted it, malloc worked."*

### The drawing is a separate file, and that is the point

`shell_frame.c` takes a panel and some facts and makes no system call, so the
whole picture renders on the host in a millisecond. 46 checks hold what a
suite can hold and a person cannot see — that **every** pixel is written (a
band left undrawn is the boot loader's text showing through), that the padding
at the end of every row is untouched at five different pitches, that the window
stops above the task bar, and that it draws at 640x480 through 3840x2160.

### And then it was looked at, which found two things the 46 could not

**Every line of text was one ascent too high.** `recon_ui.h` says plainly that
`recon_draw_text` takes *"the baseline at y"*, and this was handing it a top
edge — so the window's title floated in the wallpaper **above its own title
bar**, and "Start" sat above the task bar. Forty checks had just passed,
because not one of them asked where a glyph was. The first rendering showed it
in a second. BG-174's lesson in a new place.

**And the Start label was centred on the bar rather than on its button**, which
is inset — so it sat in the button's top half. Close enough to look
deliberate, wrong enough to look unfinished.

### Three attempts at the check that would have caught them

Worth writing down, because two of the three were checks that **could not
fail**:

1. Compare the task bar's pixels against the row above. In the default theme
   `RECON_THEME_BAR` and `RECON_THEME_WINDOW_FRAME` are **the same colour** —
   `ffc2bfc8` both — so two assertions passed whatever the code did and the
   third failed for the same reason.
2. Count pixels exactly equal to `TITLE_TEXT` outside the title bar. Found
   them, in the body text: glyphs are anti-aliased, so dark text on white
   produces every grey between, and that ink is a pale grey. **The colours are
   distinct; the blends are not.**
3. Ask the opposite question. **The strip above the window is nothing but
   wallpaper** — anything there escaped, whatever colour it ended up. No blend
   can confuse it, and putting the baseline fault back fails it immediately,
   which is how we know it holds.

---

## v0.4.53 — the desktop links, and one symbol is missing

A program that reads its theme off the disk, fills a screen, frames a window,
writes text in it and makes a directory now **links against ReconOS and nothing
else** — 146,768 bytes, and the only symbol it cannot resolve is **`stat`**.
With `stat` stubbed it links with zero undefined symbols, which is how we know
`stat` is the last thing missing rather than the first.

`src/recon_fs.c` is the 57th source to compile with no Linux under it, and it
is the one the whole port was waiting on.

### What was actually in the way, which was not what the file said

`docs/KERNEL-WANTS.md` described this as eleven symbols over thirty-one call
sites. Handing the file to the freestanding compiler rather than reading about
it turned up **four things**, and **two of them were already written**:

| gap | what was really wrong |
|---|---|
| `strerror` | implemented in `libc/errno.c`, declared in no header — all 43 desktop call sites were reaching it by implicit declaration |
| `realpath` | implemented in `libc/posix.c`, declared in no header |
| `rename` | no system call; now declared-and-not-defined, the treatment `sys/stat.h` already gives `stat` |
| `O_NOFOLLOW` | not defined |

`O_NOFOLLOW` is defined as **POSIX's real bit and not as 0**, which compiles
identically today and is the whole point: ReconOS has no symbolic links, so 0
would work — and would silently disarm every caller asking for that protection
on the day links arrive, with nothing in any source changed to notice.

### And one fault underneath, of the quietest kind

`realpath(path, NULL)` returned **EFAULT**. It compiles, it links, it has no
undefined behaviour, and the function returns cleanly — it just refuses.

`recon_fs.c`'s `stays_inside` passes NULL, reads NULL as *"not there yet"*,
steps up to the parent and asks again, and ends at the root returning **false**
— which means *"this path is outside the filesystem"*. So on ReconOS **every
write, every mkdir and every open in the desktop would have been refused**,
each with a sensible message, and nothing anywhere pointing at this function.

The caller was right: NULL is POSIX, and the comment beside that call already
explains why it passes NULL rather than a buffer — the host's version demands
PATH_MAX, a smaller buffer is undefined rather than truncated, and an optimised
glibc **aborts the process** over it, which it once did.

---

### The correction: `stat` is the wall after all

**v0.4.52 said it was not, and that was wrong.** How it went wrong is the part
worth keeping.

An earlier linker probe came back with three unresolved symbols —
`recon_fs_write`, `recon_fs_append`, `recon_fs_mkdir`. Those three were read as
*what a desktop needs from `recon_fs.c`*, a call graph was walked from them, no
`stat` was found, and the conclusion went into the change log, the board, the
kernel session's inbox and this file.

**Every step of that was sound and the premise was not.** Those were the
symbols one particular link happened to leave unresolved, not the entry points
a desktop uses. `recon_theme.c` and `recon_fonts.c` between them call
**twelve** — `read`, `write`, `append`, `list`, `exists`, `remove`, `copy`,
`mkdir`, `join`, `resolve`, `unique_name`, `last_error` — and several reach
`stat`. A correct walk from a false premise is a confident wrong answer.

The fix is not "check harder". **A linker answers this question directly and a
call-graph walk answers it by inference**, so the walk belongs downstream,
explaining an answer rather than producing one. `scripts/link-desktop.sh` is
that linker, and it said `stat` in one line.

`scripts/fs-reachable.py` survives as the explanation of *which* paths reach
`stat`, and it takes its entry points on the command line now, because
hard-coding three of them is how it gave the wrong answer.

**Two flags matter, to this and to any real ReconOS link.**
`-ffunction-sections` with `--gc-sections`: a linker pulls a whole object out
of an archive, so without them a program that draws would also need `rename`,
`unlink`, `chmod` and the rest of a file manager. With them it needs what it
uses — and the difference is measured rather than assumed, because the probe
runs both ways.

---

## v0.4.52 — a form can carry a file

The file picker, which was the rest of `form-gaps`. A page that asked for a
photograph used to get a control drawn dead and a refusal at Send; it now gets
`multipart/form-data`, and the request was read back off the wire by a parser
that is not ours.

### The policy, which was a decision before it was code

A viewer that can open a file chooser is a viewer that can be asked to read
anything on the disk. Four rules, and each one closes something:

| the rule | what it stops |
|---|---|
| **A page never names a file.** It says there is a file field; a click opens the chooser | a page that opens a chooser at a path of its choosing |
| **The page learns the name and the bytes, never the path** | a page reading the account name off `/Users/...` |
| **The choice does not outlive the submission** — Reset clears it, and so does leaving the page | a file still quietly attached, sent by accident |
| **Where the chooser may go is the chooser's business** — it browses `recon_fs`, the same confined view every program gets | a viewer with a wider reach than the file manager |

The path is not merely unused: `recon_form_body_multipart` has **no way to send
one**, so a later change to the viewer cannot leak a path by accident.

### The boundary is derived, not drawn

A multipart body is a small protocol with a delimiter in it, and **a wrong one
parses** — the server reads it, accepts it, and stores something other than what
was sent. The dangerous part is the boundary: one that also occurs inside an
upload splits the request there instead, and somebody who can choose the bytes
of a file can end the body early and append **fields the person filling in the
form never saw**.

The usual answer is a long random string and the argument that a collision is
unlikely. That argument is fine against accident and worthless against somebody
who has read the file. So the boundary is **checked**: built, looked for in
every part it will separate, and rebuilt with a different number if it is found.
Certain rather than probable, for one pass over the content.

`recon_smtp_message.c` reached the same conclusion for a letter, independently,
some versions ago. Finding that was worth more than the code: two arguments
arriving at the same place is the nearest thing to a second opinion this
project gets.

### What it took to believe it

**102 checks and 12 of 12 mutations caught** — but the first pass caught only
nine, and the three misses were the interesting part:

- **The filename was never searched for the boundary.** A real hole. The test
  was at fault: it put the boundary in a file's *contents*, which takes a file,
  and never in its *name*, which takes a suggestion.
- **The buffer-size check was not load-bearing** — `snprintf` already caught the
  case the test used. It is not dead, it holds the *contract*; the test now
  uses a buffer too small to be right and large enough to work by luck, which
  is the only size that can tell the two apart.
- **One byte of slack in the fit check was invisible**, and it wrote the
  terminator one past the end of the allocation. Every other check in the suite
  has slack in the buffer, and an off-by-one fit test is correct everywhere
  there is slack. Caught now by asking for the exact length back and demanding
  that a buffer of exactly that size is refused.

And then the whole path was **run** rather than reasoned about: a server on
localhost, the desktop driven headless through the chooser, and the request it
sent handed to Python's `email` parser. Three parts, 338 bytes, the file's zero
byte intact in the middle of it, the filename with no path in it, and the
declared length agreeing with the body. A wrong body that passes the encoder's
own tests is an agreed misunderstanding of the format; a foreign parser is the
only thing that can catch one.

### And the Recovery page stopped saying something untrue

Control Panel's Recovery page lists five things it cannot offer, with the
reason for each. One reason had gone stale: *"Needs a boot path of our own.
ReconOS is started by whatever is underneath it."* ReconOS is started by its own
loader now, and that loader has a recovery entry.

**The replacement is not "coming soon".** The loader's own comment settles it:
*"recovery is a decision made in front of the machine"* — the menu choice
overrides the file on the EFI partition rather than merging with it. So the
desktop is not going to get a button that restarts into recovery, and that is
an answer rather than a gap: **a repair mode software can start is one a broken
program can start**, and the whole point of it is that it is where you go when
the software is broken.

What the page can do is say it exists and how to reach it, which is what
somebody standing in front of a machine actually needs. The other four reasons
were checked as well — this is the second list this week found to be describing
finished work — and all four are still true.

### Two things that were found on the way

**One media-type table, not two.** `recon_smtp_message.c` had a private one.
A `.jpeg` that is `image/jpeg` when mailed and `application/octet-stream` when
uploaded is one fact with two answers, and the second copy is the one nobody
updates. It moved to `src/recon_media.c` — **a file of its own**, after a first
attempt put it in `recon_http.c` and the form test showed what that meant:
anything wanting a media type would link an HTTP client to get one. The same
trap the port kept hitting, caught this time by a build rather than by a
reading.

**And `stat` is not the wall it was called.** The linker probe in v0.4.51 named
`recon_fs_write`, `recon_fs_append` and `recon_fs_mkdir` as all a desktop
program needs from `recon_fs.c`. Walking the call graph transitively from those
three — not one level of grep, which reports that `recon_fs_write` calls nothing
— they reach **`fopen`, `fwrite`, `fclose`, `mkdir` and `realpath`. No `stat`,
no `lstat`.** Those two are reachable only from the rest of the file.

That corrects what this session told both Joshua and the kernel session.
`docs/KERNEL-WANTS.md` now says so.

---

## v0.4.51 — a keyboard and something to read with

Joshua asked how long until the desktop runs on the kernel. Checking rather
than guessing turned up that **`/dev/input` already exists and a program can
open it** — so the machine can already deliver keys — and that what stood in the
way was a keymap and a font. Both are here.

### The keymap: positions to meanings

The kernel delivers **USB HID usage codes**, and they name *positions*. `KEY_A`
is 4, and it is 4 whether the key under that finger produces an `a`, a `q` or
an `ä`. Turning a position into a meaning needs a layout, which on Linux is
what xkbcommon does — **and there is no xkbcommon on a ReconOS machine.**

`recon_key_from_hid(keycode, modifiers)` is that, for the `us` layout, in the
file that already owns what a key is and already builds with no Linux under it.
The modifier bits moved there too, from `recon_appwin.h` where they were named
*"as the compositor reports them"*: they are not the compositor's any more.

**Four rules that every keyboard has and almost nobody writes down:**

| the rule | what breaks without it |
|---|---|
| Shift and Caps Lock combine by **exclusive or** | Caps Lock becomes impossible to type around, and reads as a stuck Shift |
| Caps Lock does not touch the digits | somebody's password types `@` when they meant `2` |
| Shift-Tab is `ISO_Left_Tab`, a *different key* | Shift-Tab walks forwards, and it looks like a focus bug |
| Ctrl-C is the letter C with Ctrl held | a text box gets a `0x03` in it |

And one about the machine rather than the layout: **a release clears a modifier
whether or not a press was seen.** The kernel's own queue makes the same choice
for a release nobody saw, and the reason is the same — a Shift stuck on because
one event was lost is a keyboard that types in capitals until it is restarted.

**43 checks, and five mutations all caught.** Every one of those mutations
produces a keyboard that *works* — letters appear, keys respond — and is wrong
in a way somebody would blame on themselves before they blamed the machine.

There is no reference to sweep against: xkbcommon's `us` keymap is indexed by
*evdev* codes and the kernel sends *HID usage* codes, and the table between
those two is the thing that would have to be written to do the comparison. So
the checks hold properties — and one chains to something already verified: a
symbol this produces goes into `recon_key_to_char`, which **is** swept against
xkbcommon, so a plausible-looking wrong symbol still shows up as the wrong
character. Which is what would actually be typed.

### The font: borrowed once at the medium, owned afterwards

The desktop read the host's fonts — `/usr/share/fonts/...` — which is another
borrowed thing nobody had written down. A machine running its own kernel has no
`/usr/share`, and **a desktop with no font draws nothing anybody can read**: not
a degraded screen, a blank one.

- `/System/Fonts` is the **eleventh** directory of the volume layout, laid down
  every boot like the other ten. The suite holds
  `include/recon_fs.h` and `userland/init/layout.c` to each other in both
  directions, so adding to one alone fails — which it did, first try.
- `scripts/make-medium.sh` carries **DejaVu Sans** and **Sans Mono** on the
  medium, verified on a built image at 759,720 and 343,140 bytes.
- `src/recon_ui.c` looks in `/System/Fonts` **before** the host's paths. Linux
  falls through exactly as before, and the order says the right thing either
  way: a machine given fonts of its own meant those fonts.

**And the licence ships with them.** The Bitstream Vera terms say *"the above
copyright and trademark notices and this permission notice shall be included in
all copies"* — a stick with the font on it is a copy, and `THIRD_PARTY.md`
travels with the repository rather than with the stick. So the notice goes on
the medium beside the fonts. The file is renamed and the font is **not
modified**: the clause about renaming applies to modified fonts, and this is
byte for byte the file the host had, still identifying itself as DejaVu Sans.

### And the linker was asked what a desktop program needs

The kernel session asked for the source list. Rather than write one from
memory, a small program that draws a wallpaper, a window frame and some text
was linked against an archive of every source that builds freestanding.

**It resolved everything except three symbols** — `recon_fs_write`,
`recon_fs_append` and `recon_fs_mkdir`, from `recon_theme.c` and
`recon_fonts.c`, both core drawing and both needing to read and write their
settings.

So the desktop is gated on `src/recon_fs.c`, which is gated on `stat` — which
is already the largest open entry in `docs/KERNEL-WANTS.md`, and that entry
already names `recon_fs.c` as what it blocks. **Two instruments from opposite
directions naming the same call**: the entry was written by counting call sites
in the library, and the probe knew nothing about that.

---

## v0.4.50 — the host must not show through

`coverage.sh --zero` named five functions of `recon_fs.c` that no suite ran,
and the board's row for it said what was left was *"the trash, copying and
listing"*. **The trash landed in v0.4.39 and the other two were covered
already** — the row had been describing finished work. What was actually left
is the volume layer, and the one function underneath every error message.

`recon_fs.c` at **48.94% to 58.25%**, and nothing in it at zero.

### `guest_path`, and a test that proved nothing

The rule this file opens with:

> *"ReconOS is the only thing on screen and Linux is underneath it."*

`guest_path` strips the host root off a path before it reaches a sentence
somebody reads, and its comment records the day that broke: deleting something
that was not there reported `cannot read
'/tmp/lookroot/Users/Joshua/Documents/x'`. **That path exists on no ReconOS
machine**, and it is invisible to every check that looks at a return value —
the call failed exactly as it should have. Only the sentence was wrong.

**My first test for it proved nothing.** It asked `recon_fs_remove` about a
file that was not there — which reports with the *ReconOS* path it was handed
and never reaches `guest_path` at all. The check passed, and a mutation making
`guest_path` return the host path unchanged did not move it.

That is BG-162's lesson in a new place: *a test written against a path you
assumed passes and proves nothing.* The sites that do reach it handle a host
path directly — the recursive delete and the file copy — so the check copies a
file it has made unreadable, and reads the sentence. Mutated, it now fails.

### And the mutation harness could not tell a crash from a pass

A third mutation removed the range guard from `recon_volume_usage`, so an
out-of-range volume indexes past the table. The suite **crashed**, produced no
result line, and the harness reported it as *MISSED* — because it looks for a
failure count and found none.

That is the same family as a mutation that silently matches nothing: **a
harness that cannot tell a crash from a pass reports the sharpest result it
will ever get as the weakest.** A suite that did not reach its own last line
is a suite that found something.

### And it found a real hole while doing it

The out-of-range checks asked `recon_volume_name`, `_root` and `_detail` and
**not `recon_volume_usage`** — which is the one where it matters most, because
the other three return a fixed string for a volume that does not exist and this
one indexes a table with it. Added, and the mutation now crashes rather than
passing.

### The rest of what moved

Three volumes — System, Programs, User — with their names, roots and the line
each shows on the Storage page, held against the header rather than against
what the code happens to return. And usage **walked rather than cached**, which
the header promises: write a second file, and both the count and the byte total
move by exactly one file and exactly ten bytes.

**34 checks**, 109 to 143.

---

## v0.4.49 — a file field was a text box

`<input type="file">` fell through the input dispatch to the default, so the
viewer drew it **as a text box**. A photograph of a page with two file fields
on it shows them indistinguishable from the name field beside them.

That is not merely unhelpful, it is an invitation: somebody types a filename
into it, presses Send, and the server receives a word where it expected a
document. The same failure as a table whose headings sit over the wrong
columns — **legible, and wrong**.

And it went further than the drawing. The confirmation before sending counted
those fields as answers — *"Send 3 answers to localhost?"* for a form with one
real answer and two files it could not attach — and the body would have gone
url-encoded to a server that had asked for `multipart/form-data`.

### Drawn as what it is, which the tree already had a shape for

`RECON_HTML_FIELD_FILE`, drawn like `RECON_HTML_FIELD_BUTTON`: visible,
**disabled**, with a tip saying *"ReconOS's viewer cannot attach a file yet"*.
The argument was already written next to that kind, for script-only buttons:

> *"A page whose 'Show more' is simply missing looks like a page this failed to
> read; one whose button is there and visibly does nothing says the true
> thing."*

Turned up a notch here, because a dead button cannot be typed into and a text
box can. Tab skips it, and its words are the viewer's — a file input carries no
`value` a page may set, because a page that could set one could read a path off
somebody's disk.

### A form that asks for multipart is refused, with a reason

**Url-encoded is not a degraded multipart body, it is an unintelligible one.**
The server looks for a boundary that is not there and finds nothing, and the
failure it reports is its own — so somebody watching this viewer would see a
page saying something went wrong, with no way to learn that what went wrong was
the shape of the request.

Refused before a body is built, and said where the reason is known: *"This
form attaches a file, and ReconOS's viewer cannot attach one yet. Nothing was
sent."*

Only `multipart/form-data` counts. `enctype` also takes `text/plain`, and
anything unreadable is the page being wrong — reading the mark generously would
refuse forms that work today, and refusing a form is a real cost.

### And a file field in an ordinary form still sends, which is the precise part

A form that does **not** ask for multipart is not refused. The standard has
such a form send a file field's *name* rather than its content, and a browser
with no file chosen sends nothing after the equals sign.

This viewer has no file chosen either, so it sends the same thing. Proven
against an echo server rather than argued: `stray=&notes=ordinary`.

### 14 checks, three mutations, all caught

| the mutation | what the suite says |
|---|---|
| `type=file` falls through to a text box | 2 failures |
| a multipart form is not marked | 1 failure |
| **any** enctype counts as multipart | 2 failures |

The third is the over-generous reading the comment warns about, and it is worth
having a check for: it would not break anything visibly, it would just quietly
stop ordinary forms working.

### What is still missing

A file picker, and a multipart encoder. `recon_filedlg` exists and is not
reachable from `recon_web.c` at all, which is the next question rather than an
oversight — a viewer that can open a file chooser is a viewer that can be asked
to read anything on the disk, and what it may offer is a decision before it is
code.

---

## v0.4.48 — a `#` that ended the command

**The freestanding check had stopped checking, and I broke it.**

`scripts/check-userland.sh` builds its compile line across ten continued lines,
and two versions ago I put explanatory comments *inside* that continuation. A
`#` after a line continuation ends the command: the shell joins the lines,
tokenises, and everything from the comment onwards runs as commands of its own.

    ./scripts/check-userland.sh: line 260: -isystem: command not found

So for three versions this check compiled **without `-Werror`, without
`-DRECONOS_VERSION`, and without `third_party` on the path**, while its output
said it had used all three. Every flag after the first comment was silently
gone.

### The answer it gave was right, and that is luck rather than a defence

The file list is hand-maintained and built from `try_thirdparty.sh`, a sweep
that spells its flags on one line — so the files it added were measured
properly even while the check was not measuring anything much. Re-running with
the continuation fixed: **55 of 55**, unchanged.

**Two instruments, and only the second one was intact.** That is the whole
reason this was findable, and it is not an argument for having one.

The comments are above the command now, with the trap written where the next
person will be adding a flag.

### And two real gaps found on the way there

**The library had no `<limits.h>`.** Two vendored headers include it, the
compiler's own chains to `syslimits.h`, and with `-nostdinc` there is nothing
to chain to — *"no include path in which to search for limits.h"*. That is not
the vendored code being awkward: **a C library provides `<limits.h>`** and this
one did not.

Every value in it is one of the compiler's own predefined macros rather than a
number typed in, so the header cannot disagree with the compiler it is compiled
by — which is the failure mode a hand-written one has, and it is silent. The
minimums are `(-MAX - 1)` rather than literals, because **`-2147483648` is not
an `int`**: it is `2147483648` negated, which does not fit, so it promotes and
changes the type of every comparison it takes part in. The same trap the tree
already has written down about `(int)~0U >> 1`.

`PATH_MAX` is 1024, this system's, deliberately not Linux's 4096 — a difference
that has bitten this project before, in `recon_fs.c`'s note about an optimised
glibc aborting the process over a short `realpath` buffer. **29 checks against
the host's**, because a header of nothing but macros looks like the one file
that cannot be wrong and is the opposite: a wrong limit is silent.

**And `third_party` was being treated as ours.** With `-I` the compiler holds
vendored headers to the project's warnings, and `-Werror` then refused
`src/recon_ocr_match.c` because `stb_truetype.h` defines two functions that
file does not call. `-isystem` is the mechanism that exists for this and is
what `THIRD_PARTY.md` already says those files are.

### 53 of 80 became 55

`recon_ocr_match.c` and `recon_stb.c`, both of which had been failing for
reasons that were not about ReconOS at all.

`recon_codec.c` is the one that did not make it: with `<limits.h>` it got
further and now wants `<sys/mman.h>`, because `minimp3_ex.h` maps files into
memory to read them. That is a real dependency and a different question — this
kernel has `SYS_MAP`, but what that vendored header wants it for is file
loading, which ReconOS does another way.

---

## v0.4.47 — signals, and two files left off the list on purpose

The kernel has had `SYS_KILL`, `SYS_SIGACTION`, `SYS_SIGMASK` and
`SYS_SIGRETURN` since before the desktop could compile for it at all. Nothing
in `userland/` could reach them.

`userland/include/signal.h` and `userland/libc/signal.c` are the two halves the
desktop asks for: **sending one** — End Task is `kill(pid, SIGTERM)` and the
second press is `SIGKILL` — and **saying what to do about one**, which is
`recon_error.c` installing a handler for the four faults that mean the system
stopped, writing a code somebody can read, and letting the default handler
finish the job.

### The restorer is the whole of the difficulty

`SYS_SIGACTION` refuses a handler without one, and the kernel's comment says
why: *"a handler with nowhere to return to runs once and then executes
whatever follows it in memory."*

So the library has to build one, and it has to be machine code — **there is no
C for "return through a system call"**. Two instructions per architecture, in
a top-level `__asm__` block rather than a `naked` function, because `naked` is
not available everywhere and a function that merely *looks* empty still gets a
prologue that would run first.

**It is not in `signal.c`, and the layering is what said so.** `internal.h`
deliberately does not pull in `recon.h` — the library talks to `recon_sys_*`,
not to the raw calls — so `signal.c` could not see `SYS_SIGRETURN` and the
build stopped. The restorer belongs with the system calls: its entire content
*is* a system call, `libc/syscalls.c` is the file that has them, and it is the
file `hostsys.c` replaces wholesale on a host. Which also answers what the
host does: it provides one that aborts, because reaching it would mean a
handler returned through a frame `SYS_SIGRETURN` did not write.

**The number in that assembly is checked by the compiler.** A literal in an
`__asm__` block is exactly the kind of hand-written syscall number
`scripts/check-syscall-numbers.py` exists to police and cannot see, so a
`_Static_assert` holds it against the enum. If the numbers shift, the file
stops building rather than returning into the wrong call.

### 30 checks, against the host's

`hostsys.c` answers the three primitives with POSIX `kill` and `sigaction`, so
a handler installed through ReconOS's `signal()` is installed with the real
delivery machinery underneath: raising the signal has to actually run the
handler, in a real process, at a real moment. Twice, because a handler that
runs once and then does not is what a missing re-arm looks like.

And the differences are held to on purpose:

- **SIGKILL is refused here rather than passed down.** The kernel refuses it
  too — that is the point of one signal that cannot be caught — and refusing at
  this end means the same `errno` whichever side says no.
- **Signal zero is refused**, not used as POSIX's "does this process exist"
  probe. The kernel takes a signal to deliver and this is not one.
- **The previous handler is what this library last installed**, because
  `SYS_SIGACTION` answers whether it accepted a change and not what was there
  before. Stated in the header, and held by four checks.

**What the suite cannot reach is the restorer** — glibc has its own and
`hostsys.c` drops ours, so every check passes whether that assembly is right or
wrong. Written into both files rather than left to be assumed: a green suite
over signals reads like coverage of signals, and this one covers everything
except the part that is hardest to get right. What settles that is a program
in ring 3, the same way the socket claim was settled.

### And `_exit`, which is the one a handler may call

`exit` here runs no atexit handlers and flushes no streams, so today the two do
the same thing. Written as its own function anyway, so that when there *is*
something for `exit` to run, adding it does not silently add it to the one
signal handlers are allowed to use.

### Two files left off the list on purpose

**52 of 80 became 53**, not 55, and the two that did not go on are the point.

Both would compile if handed a header. **Compiling is not what the check
asks.** It asks whether a file could *run* on ReconOS, and a file that builds
because it was given a declaration for something this system answers with a
refusal has a worse answer than one that does not build — it looks ready.

| file | what it wants | why giving it that would be wrong |
|---|---|---|
| `recon_control.c` | `<sys/time.h>` for `struct timeval` | it uses it with `setsockopt(SO_RCVTIMEO)`, and this library refuses every `setsockopt` option. The timeout exists to stop a peer that connects and says nothing from freezing the thread drawing the desktop. Compile it and the freeze comes back with nothing saying so. |
| `recon_procinfo.c` | `sysconf(_SC_CLK_TCK)` | to divide numbers it read out of `/proc`. There is no `/proc` here; the file is a Linux scraper top to bottom, and it is on the KERNEL-WANTS list as *Processes*, not on this one. |

---

## v0.4.46 — the same shape a third time, and once in the instrument

**46 of 80 sources built with no Linux underneath. 52 do now**, and not one of
the six was a library gap.

`include/recon_server.h` is the compositor: a wlroots scene, a seat, an output
layout, a cursor manager, an xdg shell and a dozen `wl_listener`s. **Five of
the desktop's applications were including all of it for one field.**

| what it wanted | files |
|---|---|
| `server->shell` | File Explorer, Help, Control Panel |
| and the screen size | Control Panel |
| **nothing at all** | Calendar, Mail |

The last row is the flattest version of it: `recon_calendar.c` and
`recon_mailwin.c` include `recon_server.h`, write `struct recon_server *` in
one prototype each, and pass it along — and `recon_appwin.h` has forward-
declared that name all along. **The include was doing nothing.** Deleting it
builds both files.

`include/recon_server_facts.h` is the seam for the other three: they ask for
the shell and the screen size rather than reaching in, and the implementation
is the one file that includes both headers — which is where the wayland stops.
`recon_background_reload` moved there too, because setting the wallpaper is
something an application asks for rather than something the compositor keeps
to itself.

### And the same shape a third time in two days

- **v0.4.42** — `recon_ui.h` included xkbcommon for one typedef, and held
  twenty-four files.
- **v0.4.44** — `recon_ui.c` had twenty-eight lines of wlroots holding three
  thousand lines of drawing.
- **This one** — a header describing a compositor, included by files that
  wanted a pointer.

None was a design problem and none was a missing library. Each was **an
include**, and each was found the same way: by offering files to a compiler
instead of reading them.

### The check was stricter than the build, which is the same fault reversed

`src/recon_explorer.c` came free of `recon_server.h` and still failed — on two
**unused callback parameters**. `CMakeLists.txt` sets `-Wno-unused-parameter`
deliberately; `check-userland.sh` did not.

That check's own header carries the rule in the other direction: *"a predictor
with looser rules than the thing it predicts is worth less than no
predictor"*, written after a probe put a file on a list the check then refused.
**The converse costs the same way round: a check stricter than the build
rejects files that would build**, and answers a question nobody asked. It uses
the build's flags now.

### And `pid_t`, which had been measured and left out

It was tried on 15 September and left out on the grounds that every file
wanting it was behind `wayland-server-core.h` anyway — measured, correct, and
it stopped being true the same afternoon, because two of those files were
reaching wayland only through an include they did not use. **A typedef nobody
can use does not belong in a header; one two files need does.**

### What is left

Four files still reach `recon_server.h`, and three of them want something that
really is wayland's:

| file | what it wants | why it is different |
|---|---|---|
| `recon_cmd.c` | `toplevels` | a `wl_list` of windows — needs a way to walk the open windows that is not a wayland list |
| `recon_photos.c`, `recon_player.c` | `wl_display` | a timer — *"call me back in N milliseconds"*, which is a seam worth having and is not this one |
| `recon_server_facts.c` | all of it | by design; it is the file where the wayland stops |

Behind those: fifteen files that are the compositor itself, eight wanting one
missing thing each, and one wanting a hosted `<math.h>`.

---

## v0.4.45 — the other side of the seam

v0.4.44 put a table of function pointers between the desktop's drawing and the
thing that shows it. **This is the second implementation, and it is the one
ReconOS uses**: a panel drawn straight onto a screen, with no compositor
underneath.

    open("/dev/fb0") -> SYS_SCREEN (shape) -> SYS_MAP (address) -> stores

After the map the kernel is not in the path. Drawing is stores to memory, and
so is this.

**It takes no system calls.** The caller passes the address and the shape it
already asked the kernel for, so `src/recon_ui_fb.c` does not know what a file
descriptor is and runs exactly the same against a plain buffer — which is what
its suite drives it with. The half that can be wrong is arithmetic on pixels,
and arithmetic on pixels does not need a machine.

A second implementation is also the argument that the seam is in the right
place: **if it had needed the drawing half to change, the seam had been drawn
around one implementation.** It needed nothing.

### What it deliberately is not

**There is no z-order, and there cannot be.** A scene graph knows what is in
front of what and repaints what becomes visible; a direct store knows only
where the pixels went. `raise_to_top` does nothing and says so — a function
that quietly fails to raise a window is worse than one that was never offered,
because the caller stops looking for the reason. Hiding is the same: what was
underneath does not come back, because nothing here remembers it.

That is the difference between a compositor and a program with a screen, and
which of those the shell becomes is the board's `addr-space` row, not this
file's business.

### The pitch, which is the reason SYS_SCREEN exists

Bytes per row is **not** `width * 4` on real hardware — adapters pad rows — and
it is the one fact about a display a program cannot recover by looking at the
pixels. So every canvas in the suite is wider in bytes than it is in pixels,
with the padding filled with a value nothing may write.

Where the two are equal, and they are on most emulators, a program that
confuses them **draws a perfect picture and shears on the first real laptop**.
Mutating the row address to `width * 4` fails nine checks here.

A pitch too small to hold a row is refused rather than clamped: every row would
be written partly over the last one, which reads as a drawing fault rather than
as a caller who passed a width where bytes were wanted.

### 32 checks, five mutations, all caught — after two that were not

| the mutation | what the suite says |
|---|---|
| a row addressed as `width * 4` | 9 failures |
| source and destination weights swapped | 1 failure |
| the rounding dropped | 1 failure |
| no left/right clipping | 1 failure |
| a pitch too small accepted | 1 failure |

**The blend mutations missed on the first run, and the fault was the test's.**
It blended half-white over black and accepted anything from 126 to 130 — a
tolerance I chose, not one the code has. Swapping the weights gives 127 there
and dropping the rounding gives 128, so both sat inside it. A symmetric input
over a black background has no asymmetry to lose; it cannot tell those apart.
It is one exact value now, on a source that is not grey over a ground that is
not black at an alpha that divides unevenly.

**And a third mutation missed because there was nothing to miss.** Deleting the
`alpha == 0` early return changed no output at all — worked out afterwards,
the general formula is exact at *both* ends of the range, for all 256 values,
which the `+127` rounding is what makes true. Both early returns are shortcuts
for the common case, not correctness guards, and the comment claiming
otherwise had been carried over from the wlroots side, where that fault is real
because that path premultiplies. **The comment was wrong and the mutation is
what said so.**

### And the sanitizer found a leak the suite could not

6,808 bytes in three allocations: the last check in `test_what_it_refuses`
makes a panel that succeeds and never let go of it. No visible symptom, no
failing check — `check.sh` builds every suite twice and that is the run that
said so.

---

## v0.4.44 — the last inch

`src/recon_ui.c` was three thousand one hundred lines, and **twenty-eight of
them mentioned wlroots.** Those twenty-eight were enough to keep the other
three thousand off a compiler with no Linux underneath — and those three
thousand are the desktop's drawing: every widget, every button, every themed
fill, every rounded corner, all the text.

**A panel is a pixel buffer.** `uint32_t *pixels`, ARGB8888, width by height.
Everything above writes into that array and nothing else. What wlroots was
doing was the *last inch*: handing the array to a scene graph, and moving,
raising and hiding the node that holds it.

On ReconOS that inch is a write to the framebuffer device. So it is behind a
table of function pointers now — `struct recon_panel_present` — and the split
is:

| file | what it is | leaves Linux |
|---|---|---|
| `src/recon_ui.c` | 2,947 lines of drawing | **yes** |
| `src/recon_ui_wlr.c` | the last inch, moved unchanged | no |
| `src/recon_ui_internal.h` | what a panel is, shared by the two | — |

The same shape as `struct recon_memory_source` in the allocator and the
make-a-line function `recon_wrap` takes: **put the part that is tied to the
host behind a seam, and the part that can be wrong on the other side of it.**

### The header needed no change at all

`include/recon_ui.h` only ever *forward-declared* `struct wlr_scene_tree` and
`struct wlr_scene_buffer`. An incomplete type in a prototype compiles
anywhere, so the public interface was already portable and nobody had noticed.
The whole split is inside one `.c` file plus a new one.

### The premultiply went with wlroots, and that is the point

The conversion from straight to premultiplied alpha carried a comment arguing
it belongs *at the boundary*: *"one model inside the program and one at the
boundary, which is the only arrangement where 'what alpha means' has a single
answer in each place."*

That sentence turned out to be the argument for which side of the seam it goes
on. **Premultiplied is what wlroots wants, not what a screen wants** — a
framebuffer has nothing to blend against. So it moved, and a future
presentation writes whatever its screen takes.

### And the font code stopped having an opinion about stderr

Five `wlr_log` calls sat inside the *font* code — the only reason the drawing
half needed a wayland header besides the presentation. They are
`recon_ui_say` now, which formats into a buffer and hands it to a hook.

**The hook does nothing when nobody sets one**, which is right for a drawing
library and wrong for a compositor that has a log right there — so `main.c`
calls `recon_ui_log_to_wlroots()` immediately after the log exists and before
anything draws. Without that the messages would not be misdirected, they would
be *gone*, which is the quiet way a refactor loses something.

### 45 of 45 became 46 of 46

`recon_ui.c` is the largest file to cross, and the one that mattered most: it
is what every window in the system draws through.

### What proves it did not change the picture

**49 suites, and a photograph.** The suites cover the drawing directly, but the
thing a refactor of this shape breaks is compositing — and the comment on the
premultiply says exactly what that looks like when it is wrong: *"every window
looked square with a notch cut in it."*

So the check is a running desktop: wallpaper, taskbar, two windows, title bars
with their buttons, rounded corners, the Control Panel's icon grid. Identical.
A test could have passed with the alpha model on the wrong side of the seam;
the photograph is what says the corners are still round.

---

## v0.4.43 — it is the workstation, and its text stays in the box

### The machine says which of the five it is

The kernel session raised this while there was almost nothing built on the
assumption, which is the cheap moment: `docs/ROLES.md` has **five roles chosen
at first boot** — server, firewall, workstation, thin client, NAS — out of one
install, one kernel and one image. `recon_init.c` was written as *the* first
program ReconOS runs. It is not. It is the workstation's, and it is the only
one of the five that exists.

`userland/init/role.h` says so, and the screen a person reads now says
**ReconOS workstation** rather than ReconOS.

**What is deliberately not renamed: the installed path.** `/System/init.elf` is
a contract between two halves and only one of them is here — `make-medium.sh`
and `install-then-boot-test.sh` write it, the kernel's loader reads it and falls
back to its built-in copy. Renaming the writing side alone produces an installed
disk that boots the copy inside the kernel while reporting that it did, which is
not a failure anybody would see and is the worst kind. `kernel/Makefile` names
the sources under `../userland/init/` directly, so the directory is theirs to
move too. Their lead, my same-hour follow.

### BG-207 — the text left the panel and ran off the screen

Photographing the boot to check the role appeared found something else in the
same frame: **the heap line leaving the panel, crossing the gap and running off
the right-hand edge of a 2560-wide screen.**

`put_pixel` clips to the *canvas*, so it was never wrong enough to fault. It was
simply text outside its box — and **306,797 existing checks could not see it,
because every one of them looks at a canvas and the text never left the
canvas.** It left the panel, which nothing was measuring.

A status panel whose text escapes its box is worse than one that cuts it: the
reader cannot tell whether what they can see is all of it. So `recon_screen_text`
takes a right-hand bound now, and a line that does not fit ends with a `>` —
not an ellipsis, because this font is ASCII and an ellipsis would draw as a
space, which is the silent version of the same fault.

Zero means the whole canvas, which is what every caller meant before there was a
bound at all.

**And the first fix drew the marker over the last character.** It put the `>` at
`cursor - step`, where a character had already been drawn, and nothing here
clears a background — so the line ended in a smudge that read as a corrupt
glyph. The marker gets a cell of its own now: when this is the last cell that
fits and there is more text after it, the cell is spent on the marker instead of
on a character that would have been the last one anyway.

Found by photograph, both times. The second one is the reason to look again
after a fix rather than at the test result.

**Four checks**, and they bite: with the bound removed, *nothing is drawn at or
past the bound* and *the last cell inside it is marked* both fail. The suite is
306,801 now.

### Three more files, and a flag that was measuring the wrong thing

`scripts/check-userland.sh` did not have `third_party/` on its include path, so
eight files failed on `stb_image.h` and `stb_truetype.h`. **Those headers ship in
this repository and travel to ReconOS with everything else, exactly like the
icons** — leaving them off was asking "does this build without the code we
vendored", which is not the question. The question is whether it builds without
glibc.

With the path: `recon_ico.c`, `recon_icons.c` and `recon_web.c`. **45 of 45.**

`recon_stb.c` still does not — it wants a hosted `<math.h>` as well.

### And `pid_t`, measured and left out

`include/recon_procinfo.h` names `pid_t` and `userland/include/sys/types.h`
deliberately has none. Adding it was tried and **gains nothing today**: every
file that includes that header is behind `wayland-server-core.h` anyway. It
waits with them rather than going in as a typedef nothing can use.

### The socket syscall numbers, checked rather than taken

The kernel session sent a correction — that the numbers they gave earlier were
off by one, and are 26 to 30 rather than 27 to 31 — with the note that they had
*"counted by eye instead of asking the file"*.

**Asked the file. The original numbers were right.** The compiler says
`SYS_SOCKET` is 27, `SYS_BIND` 28, `SYS_LISTEN` 29, `SYS_ACCEPT` 30,
`SYS_CONNECT` 31, on this tree and on `origin/kernel`, whose copy of
`user.h` is identical to this one. `scripts/check-syscall-numbers.py` agrees:
*32 system calls; both headers and 42 hand-written numbers agree.*

So nothing written down here needed changing — and **the correction was the
recount, not the original**, which is the same fault one layer further out.

---

## v0.4.42 — one include held twenty-four files

**20 of the desktop's sources compiled with no libc under them. 42 do now.**

`scripts/check-userland.sh` takes glibc away and asks a compiler to build the
desktop anyway, and it is the honest measure of how close this half is to
running on its own kernel. Its list is hand-maintained on purpose — *"a script
that decides its own input can always pass, by shrinking it"* — and it was
twenty files, with a header explaining that the blocker was the allocator.

**That explanation had stopped being true two days earlier.** The allocator
landed in v0.4.28 and the kernel answered `SYS_MAP` in v0.4.33. So the first
thing this landing did was offer every file in `src/` to the compiler again,
with exactly the flags the check uses, and read what came back.

### The answer was one header

Twenty-four of the fifty-three that failed stopped at the same line:
`xkbcommon/xkbcommon.h`, reached through `include/recon_ui.h`.

And `recon_ui.h` wanted **one thing** from it: the typedef `xkb_keysym_t`,
which is a `uint32_t`. Not a function, not a keymap — a name for a number.

That one include held the file manager, the Terminal, Notepad, Help, the
Control Panel, the theme engine, the title bar, the wallpaper, the icon
generator, the avatar, the file dialog and the whole widget layer off a
compiler with no Linux underneath it. **Not because any of them talk to a
keyboard library. Because they name a key.**

The script's own header had a theory about why those files were stuck — *"these
files need their drawing half separated from their compositor half before they
can move"* — and it was wrong. They needed to be able to say "Escape".

### include/recon_key.h

176 constants, `recon_keysym`, and the two questions anything asks about a key:
what character it stands for, and which key has this name.

The numbers are X11 keysym values, taken out of xkbcommon **once, by asking
it** rather than typed in from a table — the same shape as the icons and the
trusted roots: borrowed at the boundary, owned afterwards. Keeping the
numbering is deliberate: the compositor half still runs on wlroots and hands
these values straight up, so a second numbering would mean a translation layer
in the one place a mistake is invisible. **A key that does the wrong thing looks
like a key that does nothing.**

`src/main.c` still links xkbcommon, because turning a keycode into a keysym
through a keymap is genuinely that library's job on Linux. It is the only file
that does.

### What holds the borrow

`tests/test_key.c`, which is now the one place on the desktop side xkbcommon
appears — the file whose job is to disagree with it.

| what it sweeps | result |
|---|---|
| all 176 names, against `xkb_keysym_from_name` | 0 disagreements |
| every keysym 0x0000–0xffff, against `xkb_keysym_to_utf32` | 64,773 compared, 0 disagreements |
| the Unicode range, 0x01000000 upward | 159,744 compared, 0 disagreements |

**763 keysyms xkbcommon maps and this does not** — Greek, Cyrillic, Hebrew,
Arabic, Thai. Counted rather than hidden, because nothing in ReconOS can
produce one: there is a single keymap and it is `us`. The rule that makes the
gap safe is tested too — it never contains a key this system claims to know.

### The sweep found two faults, both in the new code

**`key a` at the Terminal injected a capital A.** The table is in value order,
so `A` at 0x41 is listed before `a` at 0x61, and a case-insensitive scan takes
the first match. All twenty-six letters were wrong the same way. An exact name
wins now, and case-insensitivity stays as what it was for — so somebody does
not have to know X11 spelled it `Page_Up`.

I had written the opposite into the test as a comment explaining why it was
fine, beside a check that passed because it asserted the theory. **It was a
theory about code I had not read back.**

**A surrogate came back as a character.** 0xd800–0xdfff are the halves of a
UTF-16 pair and are not characters on their own; xkbcommon returns zero for the
whole block and this returned the number. 292 disagreements in the sweep, every
one of them that block — a document would have got an unpaired surrogate in it.

And a third, in the test rather than the code: the check establishing that
there *is* a gap asked whether xkbcommon knows `Hebrew_aleph`. **It does not**,
in this build — so a check meant to prove a difference was really confirming
that two libraries were both ignorant, and passed for the wrong reason.
`Greek_alpha` it knows.

### One behaviour change, in the desktop's favour

The control socket's `key <name>` went through
`xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE)`, and measuring that
call turned up something worth knowing: **it returns the lower-case keysym for
both `a` and `A`.** `key A` could not send a capital letter. It can now. No
script in the tree sends an upper-case key name, so nothing depended on the old
answer.

### And the include guard was a key

`RECON_KEY_H` is the H key. An include guard by that name is a macro that
expands inside the enum it is guarding, and the compiler says
`expected identifier before '='` on the line for H and nothing at all about the
guard. It is `RECON_KEY_H_INCLUDED`, with the reason written above it.

### What is left, and it is three groups

| what is missing | files |
|---|---|
| **the compositor** — `wayland-server-core.h`, `wlr/` | 15 |
| vendored `stb_image.h` / `stb_truetype.h` | 8 |
| one thing each — `realpath`, `pid_t`, `signal.h`, `sys/time.h`, `dlfcn.h`, `ifaddrs.h`, `zlib.h`, mbedtls, libdrm | 14 |

Only the first is structural, and it is the board's `display-boundary` row
rather than a library gap: a program on the ReconOS kernel draws on the
framebuffer device and does not speak Wayland to itself.

---

## v0.4.41 — four sentences nobody was checking

`recon_tls.c` had **fifteen functions no suite ran**, and they were all of one
half of the file: the half that decides whether to believe somebody else.
`test_tls.c` covers the identity this machine *presents* — made once, survives
a restart, key unreadable by anyone else. Nothing covered the roots it checks
other people against, or what it says when a check fails.

Verification itself is mbedTLS's, which is right; writing certificate path
validation again is how a project acquires a vulnerability it cannot find. What
is ours is the two things either side of it, and both are the kind of claim
that is wrong quietly.

### The messages are the feature

The header's argument for them:

> *"'Certificate error' leaves somebody with three very different possibilities
> and no way to tell them apart: their clock is wrong, their bundle is short a
> root, or somebody is sitting in the middle of the connection. The last of
> those is the reason this code exists and it deserves its own sentence."*

Four sentences, each chosen from a flag mbedTLS sets — and **which flag it sets
for a given kind of bad certificate is a fact about mbedTLS rather than
something to reason out.** So the suite mints a certificate authority and five
certificates with a different fault built into each, stands up a server to
present them, and runs a real handshake per scenario over a socket pair:

| what the server presents | what somebody reads |
|---|---|
| a valid certificate for **somebody else's name** | *for a different name — either the wrong address, or something is answering in its place* |
| one that **ran out** | *expired, or this machine's clock is wrong* |
| one that has **not started** | *not valid yet, which usually means this machine's clock is behind* |
| one **nothing vouches for** | *nothing this machine trusts has vouched for that certificate* |

The first is the sharpest: that certificate is real, in date, and signed by a
root this machine trusts. Everything about it is right except *whose* it is.

Elliptic-curve keys rather than RSA purely so it runs in a moment — six keys at
2048 bits is several seconds of nothing.

### A bundle with a bad entry in it loads anyway

The other half is root loading, and the rule worth holding is the one that
looks wrong: **a bundle containing entries the library cannot parse is loaded
without them rather than refused.** Refusing it fails *closed*, which reads as
the safe choice and is not — a machine that trusts nothing cannot fetch mail,
so somebody goes looking for the switch that makes it work. Real bundles off
real machines routinely carry one or two roots mbedTLS will not take.

So the fixture bundle is two real roots and one block that is shaped like a
certificate and is not: **two loaded, one counted as rejected**, and the count
is readable through `recon_tls_roots` because this file has no logging in it
and should gain none.

### There is no way to turn verification off, and that is testable

The header says so and the code says `MBEDTLS_SSL_VERIFY_REQUIRED` rather than
`OPTIONAL`. The property is not "the flag is set" — with OPTIONAL the handshake
*finishes* and leaves the result in a flag for somebody to remember to check.
So what the test holds is that **a connection to an untrusted server does not
exist at all**: no object, nothing to read or write through, nothing for a
forgetful caller to use.

### And the bytes, which nothing had ever sent

`recon_tls_read`, `recon_tls_write` and `recon_tls_fd` were at zero — every byte
the browser and the mail client send. One test opens a connection and uses it,
and holds the two answers the read path exists to give: **`RECON_TLS_AGAIN`
when nothing has arrived**, distinct from a fault, because on a non-blocking
socket that is the ordinary case and its own comment says this used to be a
loop with the desktop inside it; and **zero for a peer that said goodbye**, so
"they hung up" is distinguishable from "something broke".

### What moved, and what did not

**33 checks**, and five mutations, all caught:

| the mutation | what the suite says |
|---|---|
| the wrong-name sentence removed | 3 failures |
| expired and not-yet-valid swapped | 4 failures |
| verification made `OPTIONAL` | 16 failures |
| rejected roots not counted | 1 failure |
| one bad root refuses the whole bundle | 15 failures |

**Fifteen functions at zero, now three** — `fail`, and `recon_tls_accept` and
`recon_tls_connect`, which both run the handshake to completion before
returning. That is correct for the blocking sockets they are for, and it is
also why neither can be driven from the same loop as its peer: one of the two
has to be somewhere else while the other blocks. Covering them means a thread
or a second process, and everything they do besides the loop is the
begin-and-step path, which is tested. Written down in the suite rather than
left to be noticed.

**The percentage did not move at all: 29.27%, before and after.** That is not a
disappointment, it is the measurement's documented shape — `coverage.sh`
reports *the most any single suite runs of that file*, because gcov will not
merge counters. The two suites cover different halves and both land on 29.27%
of 369 lines, so the maximum is unchanged while twelve functions came off zero.
**The number that answers the question moved; the number on the row did not**,
which is worth knowing about that row before reading it again.

### And the mutation harness reported a fault against correct code

Worth recording because it is the third time this shape has appeared. The
harness restored the source, **verified it byte for byte**, and then ran a
binary built from the mutation — the restored file arrives from the Windows side
with an mtime `make` sometimes reads as not newer, and says so: *"Clock skew
detected. Your build may be incomplete."* It printed one failure against code
that was right.

BG-198's lesson was *restore in a `finally`, force the rebuild, and check that
the restore restored*. **Checking the source restored is not checking the
rebuild happened.** The harness now deletes the object and the executable and
refuses to report at all unless the binary is newer than the source — and with
that, one of the five rows changed from 1 failure to 15, so the earlier number
had been wrong too.

---

## v0.4.40 — a cell can cover several columns

`colspan` is the part of table layout real pages use constantly and this viewer
did not read at all. A header spanning two columns was drawn as one cell in the
first of them, and **every row under it lined up against the wrong heading** —
which is worse than not drawing the table, because it is legible and wrong.

The span rides on the run that starts the cell, beside `starts_cell`, because
that is already where a cell boundary lives — *"a cell is not a paragraph: it
is a piece of a line"*. One `short` field, and a table with more than
thirty-two thousand columns is a document doing something other than being a
table.

Two places had to learn it, and the second is the one that goes wrong quietly:

**Measuring.** A cell covering three columns must not make the first of them
three columns wide. Its width is spread across the columns it covers, and only
where they are narrower than their share — so a spanning header cannot shrink a
column some ordinary cell elsewhere already needs wider. Getting that backwards
shows up as one enormous column and three thin ones, which is what a viewer
that ignores the attribute does by accident.

**Drawing.** The pen starts in the first column of the span and its right edge
is the end of the last, so the text runs across the span instead of wrapping
inside one column's width. The column index advances *by the span*, or every
cell after a spanning one is off by however many columns it covered.

The widest-word floor is the exception: it goes to the first column only.
Spreading an unbreakable word across a span would let every column it covers
stay too narrow for it, which is the floor failing to be a floor.

### Anything that is not a number above zero is one

`colspan="0"` means *"to the end of the column group"* in the standard. There
are no column groups here, so it is read as one rather than guessed at — and
`colspan="banana"` is a page being wrong, not a cell covering nothing. **Zero
is the dangerous answer**: a cell that takes no column shifts the whole rest of
the row left, which is the same fault as the one below.

### BG-206 — an empty cell took no column

Finding this is what the work was actually worth.

A cell boundary is carried by a run, and **a cell with nothing in it produces no
run to carry it**. The boundary stayed pending and the *next* cell's text
consumed it, so one column simply vanished and everything after it on that row
shifted left.

The most ordinary shape that hits is the blank top-left corner of a table whose
header has two rows — `<th></th><th>area</th>` put "area" in the first column.
Every heading under the wrong one, on a table shape that appears on a great many
real pages.

It is not colspan's doing and it was not new. It was invisible for as long as
the whole row was merely off by one and nothing sat above it to disagree with;
the spanning-header fixture made it obvious in a single photograph.

The fix is the shape the file already uses for a control — *"its run is empty
and sits at the end of the text"* — so an empty run is a supported thing here
rather than a new idea. **Written directly rather than through `emit`**, which
merges a run into the one before it when they look alike, and an empty run looks
like everything. The first attempt used `emit` and the run disappeared into its
neighbour, which is the same bug wearing a different hat.

### What proves it

**13 checks** in `test_html.c`, taking the suite from 183 to 196, and both
mutations bite:

| the code as it was an hour ago | what the suite says |
|---|---|
| the parser ignores `colspan` | 2 failures |
| an empty cell produces no run | 2 failures |

They check the *parser's* answer rather than the drawing, because that is where
being wrong is quiet: the layout can only be as right as the number it is
handed. Seven cells in two rows cover the ordinary case and all four refusals —
`0`, a word, a negative, and an ordinary cell after them.

And three photographs through `scripts/look.sh`, against a `/span` fixture, on
the real compositor. A photograph proves this build on this fixture on the day
somebody looked, which is why it is not the only instrument here — but it is
the one that found BG-206, and no unit test was ever going to.

### The badge said 1,971 checks. A run says 4,474,929.

Adding thirteen checks meant touching the figure on the README badge, and the
figure turned out to be off by three orders of magnitude.

`check-readme-badges.py` was written on 15 September for exactly this — *"a
figure on a board is a claim, and a claim needs an instrument"* — and it
deliberately left this one field alone, for a reason that was right as far as
it went:

> *"`1971 checks` cannot be derived from the repository — it is a figure from a
> run, and a run is a thing that happened rather than a thing a file contains.
> Reading it from a log lying around would be worse than leaving it."*

True, and it skips the third option: **do the run.** A figure unavailable to
something that only reads files is perfectly available to something willing to
execute the suites and read what they say. `--run` does that and writes the
number it just watched happen — no log, no cache, no trusting a build directory
to be from this commit rather than checking.

The cost of leaving it was not hypothetical. The field had drifted from 1,971
to 4,474,929 while sitting beside a note explaining why nobody was checking it:
**the same fault the script exists to fix, one field to the right.** A count
nobody can check should be visibly uncheckable; a count somebody could have
checked by running it was just unchecked.

### Two ways it was still wrong, both found by running it

**It was short by a whole suite.** 46 of 47 suites end `N checks, M failures`;
`malformed-input` ends `10788 cases, 0 failures`, because what it runs are
inputs rather than assertions. Matching one noun dropped an entire suite from
the total without a word. Both nouns count now, and the number of suites that
reported is held against the number that exist — so a suite that stops
reporting makes the figure complain instead of quietly shrinking.

**And the first fix broke the thing it fixed.** It wrote `4%2C464%2C141`, a
URL-encoded comma, so the badge would render `4,464,141` — and its own pattern
then could not read it back. The next run reported every badge as matching the
tree without ever having looked at that one. **A number its own checker cannot
parse is a number nobody is checking**, which is precisely the fault the script
was written against, reintroduced by the fix for it. Plain digits.

`scripts/check.sh` passes `--run`, because by the time it reaches that pass the
suites are already built — it is the one place that can measure the figure
rather than note that nobody has, and the run costs about eight seconds.

The README carried the figure twice, too — the badge and a row in the table of
what the system is. Two copies of one measurement is how the disagreement
started, so the row says *"47 suites, counted by running them"* and the number
lives in one place.

### Still missing from tables

`rowspan`, nested tables, and borders. The checkpoint row says so rather than
being ticked, because the row is a claim.

---

## v0.4.39 — the Recycle Bin, which nothing had ever run

`scripts/coverage.sh --zero` lists the functions no suite reaches. **Fourteen
of them were the bin** — `recon_fs_trash`, `_restore`, `_purge`, `_empty`,
`_origin`, `_count`, `_is_trash`, `_usage` and their per-volume versions. The
whole of it, untouched.

It is worth its own file rather than more of `test_fs.c` because what it is for
is different. Every other part of `recon_fs.c` either works or reports that it
did not; **the bin's entire job is that deleting is recoverable**, and the way
that breaks is silent. A restore that puts a file somewhere else, a second file
with the same name landing on the first, a purge that misses — none of those
announce themselves. Somebody finds out when they go looking for something that
is not there.

**56 checks**, held to the sentences in `include/recon_fs.h`, which is unusually
specific and every line of which is a claim:

| the claim | what it costs when it breaks |
|---|---|
| *"Refuses /System, and refuses the bin itself"* | the system, or a bin inside a bin |
| *"Fails if something is there now"* | a restore that silently replaces the file somebody made since |
| *"restored or purged, not deleted again"* | something in the bin going into the bin |
| *"the name in the bin is made unique"* — from the source | two files called the same thing, and the count says one |

That last one is the classic way a bin loses a file, and it has a test of its
own: two `report.txt` from different folders, and both sets of contents have to
still be in there afterwards.

A folder goes in whole and comes back whole, too — with a file two levels down,
because that is where a bin that moves entries one at a time drops the ones it
did not reach.

### The percentage barely moved, and that is the measurement's shape

`recon_fs.c` went from **46.82% to 48.94%** of 848 lines, which looks like
almost nothing for 56 checks. It is not: `coverage.sh` reports *the most any
single suite runs of that file*, deliberately, because gcov will not merge
counters — and this is a separate suite, so the figure is the maximum of two
suites that cover different halves.

**The number it exists to answer moved properly: fourteen functions at zero,
now none.** Its own header says so — *"0% means nothing runs it"* — and that is
the question, not the percentage.

The last one to fall was not the bin at all. `recon_fs_read_head` reads the
first bytes of a file without reading the file, and its own header warns that
treating them as a string *"is how a NUL in the middle of a PNG turns into a
truncated read nobody notices"* — so its test writes a header with a NUL in the
middle and requires it back whole. It went into `test_fs.c`, where it belongs.

### Two guards on /System, and the suite can only see one

Cutting `path_is_protected` out of `recon_fs_trash` leaves the suite passing,
because the `recon_fs_rename` it finishes with refuses a structural path anyway.
Defence in depth working — and written beside the test so a green run is not
read as proof of the bin's own guard. The guard below it has one guard and does
bite: cut it out and *"cannot be deleted again"* fails.

**And the first two mutations that reported a gap were both wrong.**
`path_is_protected` is guarded in two places in the file and the harness took
the first, in a different function entirely; a second mutation was typed from
memory and silently matched nothing. A mutation that does not apply reports as
a hole in the tests, which is the most misleading thing a harness can do — so
the third version cuts the text out *between two anchors read from the file*
rather than writing it.

---

## v0.4.38 — sockets, and the claim nobody could test

The kernel took five socket numbers on 15 September and they had **no caller
at all**. This is the caller, and the reason it is worth its own entry is what
came with them:

> *"The claim that a socket fd works with read/write is argued, not measured.
> The kernel-side self-test can't take a descriptor — a kernel thread has no
> process, so `fd_install` fails there for sockets exactly as it does for
> pipes. Proving it needs a ring-3 program."*

`userland/init/recon_init.c` is a ring-3 program. On an installed disk, booted
by itself:

```
  a socket: a descriptor, closed once, refused twice;
            listening, and accept says EAGAIN
```

**Closing twice is the sharp half.** A socket that was never really installed
in the descriptor table closes as many times as it likes; one that is a file
refuses the second time, because the first took it out. That, and two sockets
getting different numbers, is what settles "a socket is a file" — and it is the
tenth check on `install-then-boot-test.sh` now, so it cannot quietly stop being
true.

`accept` answering `EAGAIN` with nobody waiting is checked in the same breath,
because it is documented behaviour that reads exactly like a failure: a
listener has no wait queue yet, so a server polls, and a program that treats
every -1 as an error closes a listener that is working perfectly.

### What the library adds is almost nothing, and that is the design

A socket is a `struct file`, so `read`, `write` and `close` already serve a
connection — `posix.c` took **no changes at all**. `send` and `recv` are one
line each on top of them. The rest of `libc/socket.c` is unpacking a
`sockaddr_in` into the two numbers the kernel wants and turning a status into an
`errno`.

Three places it differs from the system it replaces, each written into the
header because each is a loop somebody would otherwise write wrong:

| | |
|---|---|
| `accept` does not block | answers `EAGAIN`; on Linux the same code blocks |
| an address is two numbers | not a `sockaddr` — so anything that is not `AF_INET` is **refused**, not reinterpreted |
| `setsockopt` refuses everything | there is no timeout and no `SO_REUSEADDR` underneath |

That last one is the uncomfortable choice and it is the right one.
`recon_control.c` asks for a receive timeout at five call sites and ignores the
answer at all five, so refusing costs those callers nothing — while reporting
success would mean a program that set a timeout waiting for ever on a read that
was supposed to give up, with nothing anywhere saying why.

`shutdown`, `sendto` and `recvfrom` stay **declared and not defined**: a caller
fails to link, naming the symbol, at the moment somebody can still decide what
to do about it.

### Four faults, all of them mine, all in the arrangement that was already there

The suite is differential — `hostsys.c` answers the five primitives with real
POSIX sockets, so ReconOS's `connect` and the host's talk to the same listener.
Getting there took four corrections, and every one was a rule the tree already
stated:

- **`sys/socket.h` redefined `struct in_addr` and `struct sockaddr_in`**, which
  `netinet/in.h` has owned since v0.4.32. Two of our own headers disagreeing
  about one struct is the fault `time.c`'s exception exists to prevent.
- **the target put `-I userland/include` on itself**, which `CMakeLists.txt`
  says not to do in so many words: *"putting them on the include path here
  would hand the same header to the test file, and the comparison would become
  the implementation against itself."*
- **it put `-include prefix.h` on the test file too**, which renames the
  *host's* `bind` — so the differential suite would have compared ReconOS
  against ReconOS.
- **the test read the host's `errno`** and saw 9 and 11 left over from calls
  made on the way. Both look enough like answers to be believed. It reads
  `recon_errno` now, which is what the library sets.

And a fifth in `hostsys.c`, which is the older lesson: its stand-in for
`listen` makes the socket **non-blocking**, because the kernel's `accept` does
not block and *"a stand-in with a looser contract than the real call is worse
than none"* — a test written against a blocking accept would pass on the host
and hang on the machine.

### Where it leaves things

| | before | now |
|---|---|---|
| call sites answered | 2,985 of 3,076 | **3,006 of 3,076** |
| sockets outstanding | 14 symbols, 35 call sites | **6 symbols, 14 call sites** |
| symbols outstanding | 37 | **29** |

`stat` and its relatives are now the largest group left at 31 call sites.

---

## v0.4.37 — a cell wraps inside its column

The board said *"a long cell sets a wide column and the row runs off the
side."* **Half of that had already been fixed** — columns are scaled down
together to fit the window, and have been for a while. What was still true was
worse and less visible: a cell whose text overran its column pushed the pen
along, so **every column after it on that row stopped lining up**. The comment
beside it called that readable, and it is; it is also the table quietly
stopping being a table half way across.

Three things change, and a table is a table all the way across now:

- a cell's lines break at the end of **its column** rather than at the edge of
  the window
- a line that breaks inside a cell starts again at that cell's left edge
- every cell starts at the top of its row, and the row ends at the bottom of
  whichever cell reached furthest down

### The wrapper that was written and thrown away

The first attempt was a clean little module — `recon_wrap`, a measuring
function rather than a font so the host could measure in characters, 836 checks
against it, and it found two real faults in itself on the first run. One of them
would have made every row measure as **zero** lines high: `(int)~0U >> 1` is not
the largest `int`, it is −1.

It was also the wrong shape, and it went in the bin. **`recon_web.c` already
wraps text** — that is what the word loop does — and the paragraph at the top of
`flow` says exactly why a second copy is the thing to avoid: *"two copies of a
word-wrap loop drift, and the drift shows up as a scrollbar that does not reach
the bottom of the page."* What a cell was missing was never a wrapper. It was a
right-hand edge.

Recorded because the tests passing is not what makes a piece of code right, and
836 of them is not an argument for keeping it.

### A column has two widths, and only one was measured

Making cells snap back to their column turned the old misalignment into
something worse — cells drawn **on top of each other**, which the first
photograph showed immediately. Columns were being scaled down proportionally,
which pays no attention to whether the result still holds a word, and a column
narrower than a word in it now overlaps its neighbour instead of merely running
past it.

So a column is measured twice:

| | |
|---|---|
| the most it wants | its widest whole cell |
| the least it can be | its widest single **word**, because there is nowhere to break inside one |

Scaling moves each column from the first towards the second in proportion to the
room it has *spare*, and stops at the floor. A column already at its minimum
gives up nothing. A table whose minimums do not fit gets its minimums and runs
over the edge — at that point there is nothing left to give, and squeezing
further would put the overlap back rather than remove it.

### How it was checked

By photograph, through `scripts/look.sh`, against a page served with one column
far too narrow for its contents — and then against a table that already fits, to
show the common case takes the path it always did. The first photograph is what
found the overlap; no test in the tree would have.

**Still not done**, and the row stays on the board saying so: no colspan, no
rowspan, no nested tables, no borders.

---

## v0.4.36 — what installing a package actually does

`recon_package` had **21 checks on reading a manifest and none on doing
anything with it**, which the signing suite's own opening paragraph had already
called out: *"the install path -- the one that takes somebody else's shared
object and loads it into this process -- was the least tested thing in the
system."* Half of that was answered by the signature tests. This is the other
half: **73 checks** on installing, upgrading, verifying and removing.

### Why it can run without a display or a kernel

`recon_package_install` copies files, places them, writes settings and writes a
receipt. The only step needing anything else is `recon_modules_load` at the very
end, reached only by a package that declares a module — and a package of
*content*, a wallpaper pack or a set of skins, is a real thing the manifest
supports that exercises everything up to that line.

The module path is tested from the other side, which is the more interesting
one: **a package whose code will not load must leave nothing behind.** The
install has already copied the module, the icon and the wallpaper and written a
receipt by the time it finds out.

Three symbols of the compositor's are stood in for at link time, and what that
costs is written in the file rather than left to be discovered: the loader is
not under test here; what is under test is what install does when a loader
refuses.

### The claims, and that they bite

Every behaviour `include/recon_package.h` promises fails **silently** when it
breaks. Nothing crashes if an install quietly overwrites a wallpaper somebody
chose, or if a failed upgrade takes a working program away — somebody's machine
is just wrong afterwards. So each claim was broken on purpose to see whether the
check written for it noticed:

| broken on purpose | caught by |
|---|---|
| install records a file it did not place | *removing the package leaves their file behind* |
| install answers a setting somebody had answered | *the setting they had chosen is still theirs* |
| a failed module load leaves its files | *the module it copied is gone* |
| the allow-list lets anything through | *a package that wants /System/Config is refused* |
| upgrade accepts the same version | *the same version is refused* |
| installing twice is allowed | *the second is refused* |
| a failed upgrade does not restore the files | *its own code, not the one that would not load* |
| a failed upgrade does not restore the receipt | *the program that worked is still installed* |

**The one that could not be broken is worth more than the seven that could.**
Deleting install's *"leave a file that is already there alone"* check changes
nothing, because `recon_fs_copy` refuses an existing destination on its own.
That property has two independent guards, so no mutation of one can show which
held — which is defence in depth working, and is now written beside the test so
a green run is not read as proof of install's own check.

### What the suite learned about the code

Two of its first failures were the test being wrong, and both are recorded in it
rather than quietly fixed:

- **A setting is `key value`, not `key = value`.** `split_two` splits on the
  first space, the same as `place = file directory` — so `setting = notes.width
  = 80` set the key to the string `"= 80"`. Caught because the check compares
  the value rather than asking whether something was written.
- **Signing refuses a package that brings nothing**, not just installing. The
  refusal lives in reading the manifest and both paths read it, so a package
  with an icon and some settings and no files is turned down a step earlier than
  expected. That is the better place for it, and it has a check of its own now.

---

## v0.4.35 — five of them were not text

The board's next row: *"Fourteen places where an optimised build says a path
may be cut ... Every one builds a string to display rather than to open, which
is why they were left rather than fixed in the same sweep."*

**There were seventeen, and five of them were not text.** They build a name
something is then looked up by, and `include/recon_fs.h` has carried the
sentence about that for months, above `recon_fs_join`: *"A truncated path is
not a shortened name for the same file, it is the name of a different one."*

| | what a cut one does |
|---|---|
| a keyring entry name | two mail accounts collide and one silently overwrites the other's password |
| where `move` and `copy` put things | the file lands somewhere else and the command reports success |
| an icon's stamp key | the cache vouches for a different file |
| a redirect's `Location` | a different URL is fetched |
| a pinned menu entry | pins something that can never be found again |

### The keyring one

`secret_name` builds `mail/user@host` because — its own comment — *"a single
`mail/password` would have them overwriting each other with no sign that
anything had happened."* `user` holds 128 bytes and `host` 192; a keyring name
holds 128. So an address over about 122 characters is cut, and two accounts on
one host with a long shared prefix produce **the same name** — which is exactly
the collision the function exists to prevent, reintroduced by the one line that
builds it.

All five refuse now. Four refuse invisibly and correctly; the fifth changes what
a window offers, so it says why — **VT-J003**, *a password could not be
remembered*.

### And the other twelve say they are text

`include/ReconOS.h` wrote `recon_text_copy` and `recon_text_printf` for exactly
this, and said why: *"a build with fourteen warnings in it is a build where the
fifteenth is invisible. This says the same thing in a way that names the intent,
so what is left on the list is what nobody meant."*

So the desktop is at **zero**, and `scripts/check-truncation.sh` holds it there
as the seventh pass of `check.sh`. It compiles rather than greps, because the
warning is the optimiser's and no pattern over the source can tell an
`snprintf` into a buffer that provably fits from one that does not. And it
touches `src/` first, because an incremental build compiles nothing and reports
nothing — a check reading one would pass on any tree whose build directory was
warm, which is every tree after the first run.

**It says what it does not cover, too.** `-Wall` gives level 1: the compiler
warns where it can *prove* a cut is reachable. Level 2 assumes any argument can
be arbitrarily long, and the desktop has **147 sites** at that level — measured,
not estimated — so it is not a bar this tree holds today. A check whose limits
are unstated gets read as covering everything.

Both mutations bite: a named cut turned back into a bare `snprintf`, and a new
one appearing in a file that never had one. The first version of that second
mutation copied from an unbounded `const char *` and was **not** caught — which
was the check being honest about level 1, and is why its scope is now written
down beside it.

---

## v0.4.34 — every error code has something that can raise it

The checkpoint board asked for *"the seven error codes that have sites"* to be
wired: `B-001`, `B-002`, `B-005`, `D-002`, `F-001`, `J-002`, `L-003`.

**They were wired already.** `git log -S` puts every one of them in `2b45b53`,
on 12 September, under the commit message *"Wire the seven error codes that had
sites and no callers"* — and the board went on calling them work for three days
afterwards, because the figure beside them, *34 of 43 reachable*, was counted
once by hand and never counted again.

So the work was not the wiring. **The work was the thing that would have said
so.**

### What the measurement found on its first run

`scripts/check-errors.py` reads every code out of `include/recon_errors.def`,
finds every `recon_error_raise` in `src/`, and refuses when the two disagree.
The true figure is **41 of 43**, and the two without a site are exactly the two
that were always meant to be:

| | |
|---|---|
| `A-006` | the startup checks it reports on do not exist yet |
| `E-005` | uninstalling is written not to fail, so no path could raise it |

And it found three more (**BG-204**). The start sequence runs eight checks; two
of them raise their fault *and* show it on the splash, and three show it only.
`VT-L001`, `VT-L002` and `VT-E001` reached a person's eyes and never reached
`/System/Logs` — so somebody who watched a start, saw a code and then typed
`errors log` would not find it. All three are FAULTs, which the table defines as
*reported where it happened*.

**A code that is shown looks wired from every angle except the log.** It is in
the enumeration, it is in `docs/ERRORS.md`, `errors VT-L001` describes it, and a
person really does see it. The one question that separates the two cases is
*can this be found again tomorrow*, and nothing was asking it.

### Checked in both directions, because that is how the last one went stale

An exception list nobody re-reads is the same fault in a new file. So the
script refuses four more things besides a code with no site: an **excused** code
that has since gained one, an excused code that no longer exists, a code raised
that nothing defines, and a number defined twice. Five mutations, five distinct
messages, each caught.

The sixth pass of `scripts/check.sh`.

---

## v0.4.33 — memory a program can ask for

**The first entry in `docs/KERNEL-WANTS.md` is answered.** `SYS_MAP` with an fd
of -1 returns a demand-paged anonymous range, and `userland/libc/mem_recon.c`
has been sending exactly that call since the allocator was written. Nothing in
`userland/` was rebuilt or relinked for it: the program on the disk already
linked the allocator, and the call it had always made started working.

An installed disk, booted by itself, running a program loaded off its own
volume, now says:

```
  the heap: 64 blocks and 512 KiB written, read back and freed;
            1536 KiB from the kernel
```

`recon_init` allocates sixty-four blocks, frees every other one and takes them
again -- so the free lists and the coalescing do work rather than a bump upwards
through fresh memory -- takes one block large enough to need a second range from
the kernel, reads every one of them back, and audits the heap. The kernel half
is in `docs/KERNEL-CHANGELOG.md` under 0.2.39.

**The first ReconOS program that allocates.** What its header used to say is
worth keeping: *"It allocates nothing. There is no allocator on this kernel and
that is the first entry in `docs/KERNEL-WANTS.md`."*

### Three functions nothing in ReconOS calls

`sincos`, `sqrtf` and `strtoll` appear in no line of this source. **The compiler
writes them.** At -O2 and above GCC fuses a `sin(x)` and a `cos(x)` of one
argument into a single `sincos`, rewrites `atoll(s)` as `strtoll(s, 0, 10)` *in
the caller* — reaching past whatever `atoll` the library defines — and narrows a
`sqrt` whose argument and result are both floats into `sqrtf`.

So a release of the desktop needed three symbols that a debug build did not, and
the coverage measurement had been reading whichever build happened to be in
`build/`. That is BG-203, and it is the third time this library has been
measured by an instrument sharing a premise with it.

**The number it changes.** v0.4.32 published 3,043 of 3,134 call sites. The
release build — the one a machine would actually run — says **2,998 of 3,089**.
The measurement now reads the build type out of `CMakeCache.txt`, prints it with
the result, and refuses a build that is not a release.

### Two warnings are errors now, and only two

`recon_libc_math_tests` reported `sqrtf(0)` as 1. The square root was right; the
suite had no prototype for it, so C assumed it returned `int` and read a
floating-point register as an integer.

The compiler had said so, twice, in a build with no `-Werror` — two lines among
a few thousand. `-Werror=implicit-function-declaration` and
`-Werror=implicit-int` are errors now. Not `-Werror` in general, which is a
policy change across a tree two sessions are working in; these two can only ever
mean a wrong answer, and the whole tree compiles with no instance of either.

### Where it leaves things

| | before | now |
|---|---|---|
| call sites answered | 3,043 of 3,134 *(debug build)* | **2,998 of 3,089** *(release)* |
| symbols outstanding | 40 | **37** |
| the installed-disk boot test | 8 of 8 | **9 of 9** |

The two call-site figures are not a regression and not comparable: the left one
counts a build nobody ships. The maths suite runs 3,614,012 checks and the file
suite 438,496, both clean.

The 91 call sites still missing all need the kernel: 35 for sockets that open
something, 31 for `stat` and its relatives, 12 for `dlopen`, 11 for processes and
signals, 2 for an environment.

---

## v0.4.32 — the nine that need nothing from the kernel

Sockets are the largest group left, and most of it needs a kernel. **Nine
symbols do not**, and they were worth taking now rather than waiting: a program
that parses an address does not need a network, and `src/recon_net.c` reads
`/proc/net/route` and turns the result into dotted quads long before it opens
anything.

`htons`, `htonl`, `ntohs`, `ntohl`, `inet_pton`, `inet_ntoa`, `gai_strerror`,
and the three stdio readers `feof`, `ferror` and `ungetc` — plus `clearerr` and
`assert`, which came with them.

### Byte order is not a byte swap

`htons` is written as arithmetic on the **value**, not as a reordering of its
storage. The two are the same thing only on a little-endian machine — and both
architectures ReconOS runs on are little-endian, so a byte-swap version would
agree with glibc on every machine in the rig and be wrong on the first one that
is not, with nothing able to say so.

Which means **comparing against the host is not enough here**, and the suite
says so: alongside the comparison, the bytes of the result are required to be
the value's own bytes, most significant first. That assertion fails on a
byte-swap implementation even on this machine.

### `inet_pton` is mostly a refusal

Its value is what it turns down. `inet_aton` reads `010.0.0.1` as octal,
`1.2.3` as a three-part address and a bare `16777217` as a number — and those
readings are how a check that two strings name the same host is defeated.

So most of the corpus is text that must **not** parse: leading zeroes,
hexadecimal, four parts and a trailing dot, a space at either end, a negative
octet. Every one is put to the host as well, because *both refuse* is a
stronger statement than *we refuse*.

### `feof` is the one people use wrongly

It is **false** after the last byte has been read and **true** only after a
further read found nothing. A stream that sets it one read early makes every
`while (!feof(f))` loop drop the last line; one that never sets it makes the
same loop never end.

That is a question about *timing*, not about a value, so the suite asks the
flag after every single step on both libraries over one file — the only way
the timing can be compared rather than asserted from memory.

`ungetc` takes one character, which is all the standard promises. More would
mean deciding what happens when a caller pushes back characters it never read,
and what `ftell` should then say about a position that is partly invented.

### Three faults, and the first is an old friend

**BG-200.** `struct recon_stream` gained a `pushed_back` field and
`take_stream` — which resets every field of a slot before handing it out — was
not told. Static storage makes an unreset field **0**, and 0 is a perfectly
good character, so every `fopen` produced a stream that would hand back a NUL
before the first byte of the file.

That is exactly the fault the kernel's `sched_init` has a paragraph about: *a
structure cleared wholesale, and one field whose zero is a meaningful and wrong
value.* Caught on the first run by the file suite, which compares contents
rather than return values.

**BG-201.** `assert` was put in `stdlib.c` because that is where `exit` lives.
It prints, so it needs `stdio.c`'s streams — and four suites stopped linking. A
function placed by what it is *about* rather than by what it *needs*.

**And one in a test**, which is worth as much: the pushback case used the
shared fixture, which an earlier test deletes when it finishes. It reported
*the fixture would not open* as though the library had failed. A test that
depends on the order it runs in will eventually fail for a reason that has
nothing to do with what it checks; it brings its own file now.

### Where it leaves things

| | before | now |
|---|---|---|
| call sites answered | 3,034 of 3,134 | **3,043 of 3,134** |
| socket symbols outstanding | 19 | **14** |

91 call sites left, and every one of them now needs the kernel: 35 for the
sockets that open something, 31 for `stat` and its relatives, 12 for `dlopen`,
11 for processes and signals, 2 for an environment.

**That is the whole of the C library that can be written without the kernel.**

---

## v0.4.31 — sscanf

Fourteen call sites, and until v0.4.30 it was in neither half of the coverage
figure: glibc emits it as `__isoc99_sscanf`, which sat behind a filter meant
for compiler internals (BG-197).

**What the desktop asks for was measured before any of it was written**, by
reading all fourteen calls: `%d` with widths, `%u` `%lu` `%llu`, `%lx`, `%63s`,
`%n`, and literal text around them — `MemTotal: %lu kB`, `* %d EXISTS`,
`%4d-%2d-%2d %2d:%2d %n`. Those fourteen formats open the suite, verbatim, on
realistic input.

Implemented is that set plus the rest of the integer and floating conversions,
`*` suppression, and the `hh h l ll z j` modifiers. **Scansets and `%p` are
not**, and a format holding one **stops the scan** rather than skipping it — so
a caller gets a short count instead of a field in the wrong variable.

### The failure that matters is not the count

It is assigning the right value to the **wrong variable**. So every case
declares a block of storage, fills it with a sentinel, runs both libraries on
identical copies, and compares the whole block — a variable that should not
have been touched is checked to be untouched.

### Two faults in the tools, and neither in the library

**BG-198.** The mutation harness crashed between mutating and restoring — a
mutated build printed the sentinel bytes, which are not UTF-8 — and left the
mutation in the tree. The next run took that as its original and restored it.
Then `rsync --checksum` brought the correct source back with an older
modification time than the object, so `make` did nothing and the next run
tested the mutation again.

The result was a function that had passed cleanly an hour earlier failing the
one case its own comment says must work, with `diff` reporting the source
identical to the kept copy — true, and useless, because the kept copy was the
mutated one. **A harness that can leave the tree in a state it invented is
worse than no harness.** The restore is now in a `finally`, forces a rebuild,
and is verified.

**BG-199.** Once the harness worked it caught four of five mutations and missed
one: an unimplemented conversion being *skipped* rather than stopping. The
suite tested that with `%[a-z]`, and a version that skipped it would then match
the leftover `a-z]` as literals and fail anyway — returning the same 0. The
test was asserting a number that agrees either way, which is the exact failure
its own header exists to catch. Fixed with an unknown conversion **between**
two numbers, where stopping and skipping write to different variables.

### And one fault in the library, caught by building it for the machine

`posix.c` called `recon_strlen`. On the host that **compiles and works** —
`prefix.h` renames `strlen` to precisely that — so every suite passed. It only
fails where there is no renaming, which is the freestanding build.

`scanf.c` and `posix.c` were added to the program on the disk for exactly this:
it is the only place the library is compiled with no glibc, the kernel's flags
and `-Werror`. A library file built only for Linux is a library file that has
quietly stopped being portable.

### Where it leaves things

| | before | now |
|---|---|---|
| call sites answered | 3,020 of 3,134 | **3,034 of 3,134** |
| desktop sources with no libc | 19 of 79 | **20 of 79** |

`src/recon_cookie.c` is the twentieth — it came off the list an hour earlier
for wanting `sscanf`, and has one now.

Sockets is the largest group left at 44 call sites, then the ten remaining file
calls at 31.

---

## v0.4.30 — files by name, by number, and by directory

Eleven of the twenty-one symbols in the largest group left, and they are the
eleven that have a system call underneath them already: `open`, `close`,
`read`, `write`, `lseek`, `mkdir`, `opendir`, `readdir`, `closedir`,
`realpath` and `sysconf`.

The other ten -- `stat` and its two relatives, `unlink`, `rmdir`, `access`,
`chmod`, `umask`, `mmap`, `munmap` -- are **declared in the headers and
deliberately not defined**, so a caller fails to link naming the symbol. Each
header says what its absentees are waiting for. That is the stance
`<stdlib.h>` took about `malloc` for as long as there was nothing to build an
allocator on, and the reason is the same: a stub that answers -1 with a
plausible errno would have a program ask whether a file exists, be told no,
create it, and do that every time.

### Everything here is a translation, and the translation is the whole risk

`RECON_O_READ` is 1 and `O_RDONLY` is 0. A wrapper that passed the flags
through unchanged would open every read-only file for writing and every
write-only one for reading -- it builds, it passes the first test, and it
destroys a file on the fourth.

So the suite makes every call twice, once through ReconOS's wrappers and once
through the host's, on the same files, and requires the same answers. Plus
three things a comparison cannot see:

- **That the flags are really translated**, by writing through a read-only
  descriptor and requiring it to fail. Two libraries that got this wrong the
  same way would agree with each other.
- **That `realpath` will not climb out of the root.** `/System/../../Users` is
  `/Users`, not something above the root. That is ReconOS's containment rule,
  not POSIX's, so there is nothing to compare it against.
- **That the descriptor limit matches the kernel's**, read out of
  `kernel/include/recon/kernel/process.h` while the suite runs.

### `readdir` over a call that answers the whole directory at once

`SYS_LIST` returns every name in one call and refuses rather than truncating.
That is the opposite shape from `readdir`, and it is the better one: a
directory read in pieces has no guarantee about what a caller sees when it
changes between two of them, and nothing in the interface can say so.

So `opendir` reads the lot and `readdir` walks it. `d_type` is always
`DT_UNKNOWN`, which POSIX allows — and which was checked before being relied
on: **the desktop reads `d_name` at every one of its sites and `d_type` at
none of them.**

### The stand-in was looser than the thing it stands in for

`userland/tests/hostsys.c` answers the primitives with POSIX so the library can
run on Linux. It existed for the `FILE` layer, which only ever asked whether a
result was negative -- so returning POSIX's `-1` was good enough and nobody
noticed it was not the kernel's contract.

The descriptor layer asks more: it turns that answer into `errno`. Against the
old stand-in, a file that was not there reported `ENOSYS` instead of `ENOENT`.
Both failures showed up on the suite's first run.

**A stand-in with a looser contract than the real call is the one way a
stand-in can be worse than none** — it lets code pass here that would be wrong
on a machine. All eight primitives now answer with the kernel's numbers.

### And the measurement was hiding five functions

BG-197, and it is the fourth of a family. `measure-libc.py` drops symbols
starting with `__`, which is right for `__stack_chk_fail` and wrong for
`__isoc99_sscanf` — glibc's spelling of `sscanf`. Five library functions sat
behind that filter, and the number was wrong **in both directions**: `errno`,
the ctype table and `strtoul` are answered and were not counted; `sscanf` and
`assert` are not answered and were never reported.

Fixed by translating the spelling rather than widening the filter. The script
then refused to report anything until all three newly-visible names had a line
in its table, naming them — the guard added in v0.4.28 doing its job.

### Where it leaves things

| | before | now |
|---|---|---|
| call sites answered | 2,951 of 3,113 | **3,020 of 3,134** |
| desktop sources that build with no libc | 11 of 79 | **19 of 79** |

The eight new ones are almost exactly the browser's half of the desktop — the
HTML parser, the CSS parser, forms, HTTP. Not a coincidence: a parser is
strings and allocation and very little else.

Sockets is now the largest group left at 44 call sites, then the ten remaining
file calls at 31, then `sscanf` at 14.

---

## v0.4.29 — errno, and two numberings that meet in one place

One symbol, **43 call sites**, and the only group left that needed nothing at
all from the kernel: the kernel already answers with numbers, and turning a
number into a sentence is entirely this side's work.

### The values are Linux's, deliberately

A ReconOS system call answers a negative number of its own -- `SYS_ENOENT` is
-7 -- and the obvious thing would be to expose those. `userland/include/errno.h`
says why it does not, and the first reason is the one that would have bitten
soonest: **the desktop is compiled against both libraries at once, today.**
`scripts/check-userland.sh` builds individual sources of `src/` with these
headers while the rest of the program is still built against glibc's. `ENOENT`
being 7 in one translation unit and 2 in another is a fault that reads as a
filesystem fault.

The second is that it makes `strerror` testable at all. Every other function in
this library is held against the host's by asking both the same question, and
the question `strerror` is asked is a number.

So `libc/errno.c` is the single place the two numberings meet, and being the
only one is the point of it.

### The messages are glibc's, word for word

Not because the wording is specified -- it is not -- but because a message this
library invents is a message that differs from the one the same program prints
today on Linux for the same fault, and somebody will eventually compare two
logs. It also means the suite can hold all 52 of them to **equality** rather
than to "is it a non-empty string".

### Where it deliberately disagrees, and how that was found

The suite's first run failed on one case: it had picked 60 as an example of a
number neither library defines, and 60 is `ENOSTR` -- which glibc knows and
ReconOS does not, because there are no streams here. `errno.h` says outright
that a constant defined for a condition that cannot arise is a constant
somebody writes a branch for, and that branch is never taken and never tested.

That is not a fault, so it did not become a bug entry. It became an
**assertion**: `ENOSTR`, `ETIME`, `EDQUOT` and `ESTALE` are checked to read
`Unknown error N` here, with a note saying what glibc calls each. The
divergence is now recorded in the one place that cannot go stale.

### And the translation cannot go quietly out of date

The suite reads `userland/include/recon.h` -- **the kernel's own list** -- while
it runs, and compares the highest error number there with what the translation
covers. A lookup table's failure is going stale, and the cost here is specific:
an error the table has never heard of falls back to `EIO`, which is a plausible
wrong answer, and somebody goes to look at a disk that is fine.

Mutation-tested in both directions. Adding an error to the kernel's header:
*the kernel has 18 error numbers and the translation covers 17*. Adding one to
the table that the kernel does not have: the same sentence the other way round.

**1,435 checks, 0 failures.**

| | before | now |
|---|---|---|
| symbols answered | 77 of 132 | **78 of 132** |
| call sites answered | 2,908 of 3,113 | **2,951 of 3,113** |

162 call sites left. Files and directories is now the whole of the near-term
work at 96, then sockets at 44.

---

## v0.4.28 — the allocator

The last large piece of the C library, and the one every other piece was
waiting behind. Measured rather than guessed: **5 symbols, 430 call sites** --
more than two thirds of everything the desktop still could not link.

`userland/include/stdlib.h` used to open by saying there was no `malloc` in it,
so that a caller failed to *link* rather than getting a stub that returned
nothing and crashed an hour later somewhere else. That paragraph is gone.

### The memory comes from two function pointers

`malloc.c` names no system call. It asks a `struct recon_memory_source` -- take
a range, give one back -- and everything else is written against those.

This is the same split `userland/init/layout.c` uses for making directories,
and it is here for the same reason: **the part that can be wrong is the
allocator, not the call underneath it.** With the source behind a pointer the
whole file runs on the host against the library it replaces, and against three
sources no kernel can be made to be:

- one that can hand memory back,
- **one that cannot** -- which is the configuration ReconOS is actually in, so
  every scenario runs twice,
- and one that refuses after a while, which is how the out-of-memory path gets
  exercised at all.

Boundary tags, coalescing on both sides, free lists segregated by size, and a
region handed back when nothing in it is in use. An allocation larger than a
quarter of a megabyte gets a region of its own and returns the whole thing --
because KERNEL-WANTS measured a browser tab at 6.8 MiB, and a window where
twelve tabs have been opened and closed has lost eighty-one megabytes if
nothing is released.

### 11,506 checks, and the suite earned its keep on the first run

The heap is audited **after every operation**, not at the end: every block of
every region, footers against headers, each block's flag about its predecessor
against whether that block is really in use, no two free blocks side by side,
and the running counters against a fresh count. A shadow copy of what every
live block should contain is checked alongside it, because alignment and audit
would both pass an allocator that hands the same address out twice.

It found **BG-194** before the allocator had run anywhere else: in the smallest
block -- 32 bytes, a 16-byte header and a 16-byte payload -- the payload is
*exactly* the two free-list pointers, and the footer was being written into its
last eight bytes. Every smallest-size block on a free list had its `prev` link
overwritten by its own size. The fix is the layout every allocator of this
shape uses: the footer lives in the next block's header, and the flags move
into the low bits of the size.

That fault would have been invisible until it was a crash somewhere else
entirely, minutes later, with no allocator in sight.

### And two instruments that could not see the allocator

**BG-195.** `scripts/measure-libc.py` went on reporting 430 call sites
unanswered while the functions sat in the build, tested, next to it -- because
it read the objects of *one test target*, and the allocator has a suite of its
own. That is the third of a family: BG-182 counted from a list of expected
names, BG-185 corrected the list, and this one defined "the library" as
whatever one target had compiled. Each time the measurement took its idea of
the thing from the same place the thing came from.

Fixed by removing the name rather than correcting it: every object compiled
from `userland/libc/` anywhere counts, and the script now **refuses to report a
number when a library source has no object**, naming it. It caught one on its
first run.

**BG-196.** `check.sh` builds every suite twice, once sanitized and once the
way a release is -- and built the second list from the *desktop's* tests only.
The four suites under `userland/tests/` had never been built optimised: the C
library, the file layer, the maths, and the allocator. `-O2` is where strict
aliasing and pointer provenance start to matter, and an allocator is nothing
but pointer arithmetic across type boundaries.

It surfaced as a fix that would have been dead code, which is the part worth
keeping: the sanitized pass aborted on the `calloc` overflow check, because
under AddressSanitizer the host's `calloc` is ASan's and ASan kills the process
rather than returning NULL. Skipping the comparison under a sanitizer would
have meant skipping it everywhere. **34 suites became 40.**

### Where it leaves the library

| | before | now |
|---|---|---|
| symbols answered | 72 of 132 | **77 of 132** |
| call sites answered | 2,478 of 3,113 | **2,908 of 3,113** |

205 call sites left. The largest group is files and directories at 96, then
`errno` at 43 and sockets at 44.

### What it needs from the kernel, which is one call

`mem_recon.c` asks `SYS_MAP` for a range with **no file behind it** -- fd -1,
the spelling `docs/KERNEL-WANTS.md` proposes. Today the kernel finds no such
descriptor and refuses with EBADF, so `malloc` answers NULL and
`recon_malloc_stats().refusals` counts it. **The day the kernel accepts it,
this works with nothing rebuilt.**

No new system call number is taken here, deliberately: two sessions build this
kernel, and a number claimed in advance by the half that does not own `core/`
is a number claimed twice. That has happened, and it is in `docs/BUGS.md`.

The program on the disk links the allocator already, so the wiring is in place
and compiled -- and the freestanding build has to keep working for it, which is
what stops it drifting while it is only ever exercised on the host.

---

## v0.4.27 — the kernel starts the system instead of containing it

Said plainly, because it is the thing this version is about:

> *"The system and the kernel are two separate systems when they should be one
> and the same relying on each other."*

They were separate in a way that can be stated as a build dependency.
`recon_init` — the program a person sees when a ReconOS machine starts — was
**121 KiB of `.rodata` inside the kernel image**, pulled in by `.incbin`. The
installer wrote an EFI partition with a loader and a kernel, formatted a System
volume, and **put nothing in it**. The ten directories v0.4.26 added were
empty.

So changing a string on the first-boot screen meant rebuilding and reflashing a
**kernel**. Not a figure of speech: on real hardware, a stick, a reboot and a
firmware menu, to change a sentence in a program.

### What was missing was the join, not the parts

Almost everything this needed already existed and had never been connected:

| | |
|---|---|
| a kernel that runs a program from a path on the volume | `user_exec_path`, built, and passing a self-test on every boot |
| a standalone `recon_init.elf` | built, 126,880 bytes |
| a filesystem that can be written by the installer | `reconfs_mount`, `reconfs_create`, `reconfs_write_named` — all take a volume |
| a medium that carries a system | no |
| an installer that writes one | no |
| a kernel that looks for one | no |

Three of those are now yes.

**The medium carries `/reconos/init.elf`** — the first file on a ReconOS
install medium that is neither a loader nor a kernel.

**The installer writes it onto the System volume** as `/System/init.elf`. This
is the first thing the installer has ever written into a **ReconFS** volume;
every copy before it was FAT32 to FAT32, because everything before it was what
firmware reads.

**And the kernel asks the volume first**, falling back to the copy inside
itself and saying which it used. That line matters more than it looks: the
whole point is that the screen can now come from somewhere other than the
kernel, and a boot that does not say where it got its program cannot be used to
tell whether any of this works.

### Creating on a volume that is not the one you booted from

`rootfs_create_file` and `rootfs_create_directory` did exactly the right three
moves — walk to the parent, act, rewrite the chain to the root — against
`rootfs()`, which is the one volume an installer must never touch.

So the three moves moved down a layer into `reconfs_place_file` and
`reconfs_place_directory`, which take the volume as an argument, and the two
`rootfs_*` entry points became those calls with `rootfs()` filled in. An
extraction rather than a rewrite: the bodies are the bodies that were there, so
the suites that covered them cover them still — 67 self-tests, unchanged, on
the first boot after the move.

### The proof, and why it had to be built this way round

A claim that two things are separate can only be shown by changing one and not
the other. So:

1. Keep the kernel binary exactly as built.
2. Change a string in the **program** and rebuild it.
3. Build a medium carrying the **old kernel** and the **new program**.
4. Install from it and boot.

```
kernel kept:   402df0f6403460f0
program built: 666db5e4b023ed04

the system: /System/init.elf, from the volume
first screen [BUILT-WITHOUT-THE-KERNEL]: 1920 x 1200, 7680 bytes a row

is the kernel on that disk the one we kept?
  yes -- byte for byte the kernel built before the program changed
```

Yesterday that was impossible by construction.

### And the test asserts it, because the fallback is designed to hide it

`scripts/install-then-boot-test.sh` gained an eighth check: **the system it
runs came off the volume.** Without it the run passes either way — the kernel
falls back to its built-in copy deliberately and quietly, so an installer that
stopped writing `/System/init.elf` would still produce a machine that boots,
draws a screen and reports 67 self-tests passing, and the file would still have
printed 7 of 7 forever.

That is the shape KF-229 was: a check sitting on top of the fault it was
written to catch, unable to fail. Taking the program off the medium turns the
run red and names the reason — `the system: no /System/init.elf (-7), using the
copy inside the kernel` — while every other check still passes, which is also
the graceful fallback being shown to work.

### What is still in the kernel image

The copy. It is the fallback for a machine that has not been installed onto —
every blank disk in the verification rig, and any medium built before this
existed. Taking it out is a separate decision and wants a machine that can be
recovered without it.

---

## v0.4.26 — the volume has a shape, and a second boot finds it

Asked for directly: *"whats next? i would like the filesystem"*, after a night
in which the answer to *what is on the volume* was **nothing**. The installer
wrote a bootloader, a kernel and an empty formatted partition, and the first
screen said so honestly: `0 entries at the root of the volume`.

`include/recon_fs.h` has described the layout since v0.1.0 and nothing could
build it, because nothing in the system could create a directory.

### Ten directories, and the second boot is the requirement

`SYS_MKDIR` is the twenty-sixth system call, over a
`rootfs_create_directory(path, mode)` built on the `reconfs_create` that was
already there. `userland/init/layout.c` holds the ten paths -- `/System` with
Apps, Config, Themes, Icons, Modules and Logs under it, then `/Apps`, `/Users`
and `/Temp` -- parents before children, mode 0755.

`recon_init` lays them down on **every** boot, not only the first. That is the
requirement rather than a nicety: a first boot that lost power half way and a
second boot that finds everything already there are the same code path, and
one boot cannot tell them apart. So the program is asked both questions.
Installed onto a blank 8 GB disk from a real medium, then booted twice:

```
first boot    the volume: 10 directories, laid out just now
              storage: 12 entries at the root of the volume
              65 self-tests passed, 0 failed

second boot   the volume: 10 directories, all already there
              storage: 12 entries at the root of the volume
              65 self-tests passed, 0 failed
```

A reader that is not this kernel -- `scripts/reconfs-check.py`, against the
partition lifted out of the image -- says the same twelve names.

**The list is data and the directory-maker is a function pointer.**
`recon_layout_build(make, already_there, &report)` knows nothing about system
calls, so the whole arrangement -- order, refusals, a half-finished previous
boot, the report it produces -- is checked on the host in a millisecond by
`recon_init_layout_tests`, 100 checks, instead of by installing onto a disk.
The suite also reads `include/recon_fs.h` and compares the two lists **in both
directions**, so a path in the header and not the layout fails, and so does a
path in the layout and not the header. Mutation-tested three ways; all three
caught.

And it carries on past a refusal rather than stopping, naming the first one:
`7 of 10 directories -- /System/Apps refused (-9)` is a line somebody can act
on. `1 refused` is not.

### Four faults were in the way, and none of them was in the new call

The call worked on the first try. Making it work on a machine took the rest of
the day, and every one of these was invisible to every test in the tree.

**A directory took seventy seconds to create.** Not a hang -- 69, 74, 69 and 69
seconds, measured. The same disk image attached as virtio-blk instead of NVMe
took none at all, which is what said where to look. The NVMe driver polls for a
completion and yields between looks; the thread it yielded to was the boot
thread, which by then is running `for (;;) power_idle_wait()` and sleeps for up
to a second. **The scheduler already knew how to prevent this** -- `pick_next`
takes an idle thread only as a last resort and `idle_over_work` counts the
times that rule is broken -- and none of it applied, because the boot thread
was never marked as an idle thread. It is now, from the moment it stops doing
any work. KF-226.

**The root of a volume could not be listed.** `reconfs_walk_path` refuses a
path with no leaf, rightly; the two readers that should not care each had a
branch for the leafless case that the walk made unreachable. Nothing had ever
asked, because `recon_init` was the first caller in the system's life. KF-227.

**Every refusal from a listing reached a program as "the disk failed."** The
mapping function exists and vfs.c already used it twice; the listing had its
own two-case version. So the screen said *no volume this kernel can read* three
lines under *10 directories, laid out just now*, and both sentences came from
the same volume. KF-228.

**And five self-tests passed exactly once per volume.** Each creates a file
with a fixed name and none cleared it first, so the second boot of any
installed disk reported five failures on a machine with nothing wrong with it.
KF-229.

### The check that was written to catch that, and could not

The last one is worth its own paragraph, because it had been sitting inside
its own detector for a month.

`scripts/install-then-boot-test.sh` has a check called *"and they pass again,
on a volume already written to"*, whose comment says it is *where a test that
leaves a file behind shows itself*. It was asked of the BIOS boot -- and over
BIOS the kernel does not find the disk it booted from (KF-192, open), so that
boot's report reads `root : none found`, the five tests that need a volume say
so, and a boot that never touched a volume was counted as a pass. Measured
rather than reasoned: the BIOS log says `root : none found; file calls will
say so`; the UEFI log says `root : nvme0n1p3`.

The check now boots the target a second time under UEFI, and **fails loudly if
no volume is mounted** rather than passing, because the way this went wrong was
silence. Taking one of the five clears back out turns the run red and names
`files carry a mode`, which is how it is known to work. The number moved from
*59 passed* on a volume-less boot to *65 passed, second boot on this volume*.

### And the screen stopped guessing

`say_storage` counted newlines. `SYS_LIST` separates names with NULs and always
has -- `user.h` says so where it is declared. Finding no newlines it counted
zero, and a fallback two lines down turned that into `1`. A volume with twelve
names at its root reported **one entry**, which is a number that looks like a
measurement. A count of zero would have been obviously wrong; a count of one is
plausible, and this screen had only ever been looked at on machines with no
volume at all. BG-193.

The line above it now says *why* when it cannot answer, instead of concluding:
`no volume this kernel can read` is printed only for ENODEV, and every other
refusal prints its number. And the storage line goes down the serial cable
alongside the other two, because the one fact that turned out to be wrong was
the one that existed only in pixels.

### What this version is really about

Nothing above is a feature the user can point at. What changed is that a
ReconOS machine, installed from media onto a blank disk, now starts twice in a
row and is the same machine both times. Until today there had never been a
second boot.

---

## v0.4.25 — ReconOS is on the screen

Until this version every program that had ever run in ring 3 on the ReconOS
kernel was a self-test. `hello.S` proved the loader. `paint.c` proved that C
compiled against these headers runs here at all and that a framebuffer is
written through its **pitch** and not through width times four. Both end by
exiting with a code the kernel reads, which is what makes them tests.

**`userland/init/recon_init.c` is what somebody sees.** It asks `SYS_MACHINE`
what the machine is, `SYS_SCREEN` how the display is arranged and `SYS_LIST`
what is on the volume, draws a screen a person standing in front of a computer
that has just started can read, and stays up.

Booted in QEMU, the screen it drew said: 1 processor found and 1 in use;
511.8 MiB with 508.8 MiB free; `QEMU Virtual CPU version 2.5+`; 2560 x 1440 at
10240 bytes a row; and *no volume this kernel can read*, because no disk was
attached. Every one of those came out of the kernel through a system call. On
real hardware they become the machine's own.

### It is the first customer of the C library

Every string on that screen goes through `snprintf`. That is the point of the
last four versions: a library that passes 3.6 million comparisons against
glibc is a suite of functions, and a library something is **built on** is a
library. `recon_init` links `string.c`, `printf.c`, `ctype.c`, `stdlib.c`,
`stdio.c` and `syscalls.c` and allocates nothing, because there is still no
allocator -- every buffer is sized at compile time, the same discipline
`libc/stdio.c` already follows.

It needed one thing the two self-tests did not: **vectors**. `USERCFLAGS`
turns SSE off, which is right for the kernel and was inherited by the user
programs without anybody needing otherwise -- `paint.c` has no floating point
in it. `snprintf` does: `%f` exists, and on x86_64 a function taking a double
cannot be *compiled* without SSE whether or not anything calls it. Allowing it
is safe and not an assumption: `struct thread` carries 576 bytes of vector
state and `sched_switch` saves and restores it on every switch. KF-146 is the
entry about that buffer being sixteen bytes too small, which is a fault that
only exists because the state is real.

### The drawing is tested without a kernel, and that is deliberate

`userland/init/screen.c` takes a buffer, a description of how it is arranged
and some facts. It makes no system call and does not know one exists -- so the
host renders exactly the picture the machine will render, in a millisecond,
and `recon_init_screen_tests` checks it. **306,797 checks** across nine
resolutions from 320x200 to 4K.

Finding out what the first screen ReconOS ever draws looks like should not
require writing a stick, walking to a machine and turning it on.

**Every canvas in that suite has padding on the end of each row, and the suite
checks that nothing touched it.** This is the one thing a photograph cannot
catch: on a screen where bytes per row happen to equal width times four -- which
is most emulators -- a program that confuses the two draws a *perfect* picture
and shears on the first laptop. The real framebuffer in QEMU turned out to be
2560 x 1440 with 10240 bytes a row, where the two are equal; the padding is
what says the code would survive a screen where they are not.

### Three faults, and one of them needed an eye

**BG-190** -- the text drawer checked its text for null and not its canvas, and
segmentation-faulted on the suite's first run. Beside it, `panel_w - line * 2`
is unsigned, so a canvas narrower than its own margins gave four billion rather
than a negative, and the fill loop after it would have run until the machine
was switched off. A blank screen is a far better failure than one that appears
to have hung.

**BG-191** -- the panel was sized to the display and the content filled the top
third of it. Every check passed; it still looked wrong, and the only thing that
said so was looking at the rendering. Same lineage as BG-174.

**BG-192** -- a test that could not pass: the canvas is filled with a guard byte
to catch a stride fault, and the text tests asked whether pixels were non-zero.
It announced itself only because the answer was obviously wrong. Had the check
been the other way round it would have passed on a canvas where nothing was
ever drawn. **A test that cannot pass and a test that cannot fail are the same
fault from opposite sides, and only one of them tells you.**

### And the console still owns the screen

The first boot came back with the panel showing through a hole in the kernel's
boot log -- the conflict `docs/KERNEL-WANTS.md` has had an entry for since
`paint.c`. **Moving the call after the kernel's last message was not enough**,
and that is the part worth keeping: the console does not only draw when it is
handed something new, it repaints its window when it scrolls. One `kputs` after
the program started put the whole log back on top of a screen that had just
been drawn.

What works today is `main.c` starting the program as its last statement with
**nothing printed after it at all**, not even a success line. That is an
arrangement rather than a fix and it holds for exactly as long as there is one
program. It is written where somebody would look, because the ordering makes
the fault invisible rather than absent -- the next person to add a diagnostic
print after that line will not find out what they broke until they photograph a
machine.

### What it asks the kernel for next

A new entry, and a small one: **a program has to be inside the kernel image to
run at all.** `recon_init` is 121 KiB of `.rodata` in a 539 KiB kernel, carried
by `.incbin` beside the two self-tests. Changing a string on the first-boot
screen means rebuilding and reflashing a kernel.

Every piece of the alternative is already built -- `SYS_OPEN`, `SYS_READ` and
`user_elf_create` all exist and are exercised. What is missing is the few lines
that read `/System/init.elf` into a buffer and hand it to the loader instead of
handing it a pointer into `.rodata`. **That is much smaller than process
creation and worth separating from it**: a kernel that can start one named
program from a volume is a system somebody can install a new version of.

---

## v0.4.24 — the maths, and the difference between close and right

ReconOS could not draw a letter. Not for want of a font or a rasteriser: the
rasteriser is `third_party/stb_truetype.h` and it calls `sqrt`, `floor`,
`ceil`, `pow`, `fmod`, `acos` and `cos`, and on a kernel with no glibc under it
there was nothing to answer them. The maths library is not a calculator's
luxury; it sits between this system and its first glyph.

**Twenty functions, 3,613,874 checks against the host's, and no failures.**

### Two standards, because these functions are two kinds

**Eight have an exactly right answer and are held to bit-for-bit equality** --
`fabs`, `floor`, `ceil`, `round`, `fmod`, `ldexp`, `lrintf`, and `sqrt`. The
last is exact for free: IEEE 754 requires square root to be correctly rounded
and both architectures have the instruction, so `sqrt` here is `sqrtsd` and
nothing else. Verified in the object file rather than assumed.

Bit-for-bit catches what `==` cannot. `floor(-0.4)` is **-0.0**, and a caller
who divides by it gets negative infinity rather than positive. The two compare
equal.

**Twelve are approximations**, held to a bound in units in the last place --
and the suite **prints the worst error it found** for each rather than only
whether it stayed under the line, because a tolerance nobody looks inside is a
tolerance that can grow by a factor of a thousand and still pass. Measured:

| | ulp | | ulp | | ulp |
| --- | --- | --- | --- | --- | --- |
| `exp` | 1 | `sin` | 2 | `asin` | 3 |
| `log` | 3 | `cos` | 2 | `acos` | 3 |
| `log10` | 2 | `tan` | 4 | `atan` | 2 |
| `pow` | 17 | | | `atan2` | 3 |
| `cbrt` | 4 | | | | |

Every bound in the suite is that figure rounded up to the next even number.
They were set **after** the runs. Setting them first would have been setting
them from hope.

### Four faults, and every one was a wrong answer rather than a close one

**BG-186 -- the reduction subtracted the same piece of pi/2 twice.** 860 **billion**
ulp at three pi. `PIO2_1T` is the tail of `PIO2_1` and `PIO2_2` is the leading
part of that same tail; they are alternatives, not a chain. Fixing that left a
second fault underneath: `k * pi/2` is only exact while k has under twenty
bits, so everything past about 1e6 was still wrong and no test of small angles
could see it. The reduction now runs in **2,048 bits of 2/pi**, in integer
arithmetic where nothing rounds. `scripts/gen-two-over-pi.py` computes that
table and checks it against the published expansion before writing it;
`check.sh` regenerates and compares it, like the help pages.

**BG-187 -- a ten-bit hole between two doubles.** The reduction accumulates 126
bits and has to get them into doubles; taking the top half as
`(double)(acc >> 63)` **rounds**, so bits 53 to 62 were in neither piece. Only
visible near a multiple of pi, where those are the only significant bits there
are. Three pieces of 42 instead, each of which a double holds exactly. `sin`
and `cos` are 2 ulp out to **1e300** now, and the suite prints that sweep so
the limit is a measurement rather than a claim.

**BG-188 -- `pow(1, nan)` and `pow(-8, 3)`.** One to any power is one,
including to a power that is not a number; the check for a base of one came
after the check for a NaN, and the order is the whole of what decides it. And
`exp(3 * log 8)` returned 511.99999999999994, which is what somebody notices
first. A small integer power is repeated squaring now -- bounded at 8, because
squaring costs a rounding per doubling and at a power of 60 it was 33 ulp where
the logarithm was 17.

**BG-189 -- `cbrt` overflowed in its own last step.** `guess * (cube + 2x) /
(2cube + x)` multiplies before it divides, so the numerator at x = 1e300 is
1e100 times 3e300 and there is no such double. **Found by printing the
intermediates**, which mattered: the formula was right, and reading it again
would have kept saying so.

### And `pow` is honestly the weak one

Seventeen units where everything else is two to four, and it is inherent rather
than unfinished. `pow` is exp(y * log x), and exp turns an error in its
argument into the same *relative* error in its answer -- so log's own last bit
gets multiplied by y before it reaches the result. At y = 40 that was **576
ulp**. Carrying log to a hundred bits, in two doubles, took it to 17; the rest
would need exp carried the same way. Seventeen units is two parts in 1e15 --
the last two digits of sixteen, where a calculator shows fifteen.

### What it unblocked

`scripts/check-userland.sh` is at **11 of 11**, and the new one is
`src/recon_expr.c` -- the calculator's expression grammar, 459 lines, which
holds seventeen maths functions in a table of function pointers and therefore
could not compile until they existed. **ReconOS can do arithmetic with no glibc
underneath it.**

A sweep of all seventy-nine sources says where the rest stand, and the picture
is now one word wide: **fourteen are blocked on nothing but `malloc`,
`calloc` or `free`**. Three want `assert.h`, three want `errno.h`, four want
`dirent.h`, and the remainder want Wayland, DRM or xkbcommon, which are Linux
and always were.

---

## v0.4.23 — the calendar, and a measurement that asks the linker

### Dates

`userland/include/time.h` and `libc/time.c`: the two clocks, the calendar, and
`strftime`. **436,012 checks against the host's**, including a sweep of
`gmtime_r` every seven hours and thirteen minutes across two and a half
centuries in both directions, and every conversion `strftime` claims at every
buffer size around the answer.

The corpus is the dates a calendar is actually wrong on rather than a range of
plausible ones: the century that is divisible by four and is not a leap year,
the second either side of an epoch, the day a 32-bit `time_t` stops, year 1 and
year 9999. A sweep of ordinary timestamps agrees with anything.

**The two clocks stay two calls.** `clock_gettime(CLOCK_MONOTONIC, ...)`
reaches `SYS_TIME`, which never goes backwards and means nothing outside this
boot; `time()` reaches `SYS_WALLTIME`, which is the date and can jump. Neither
can reach the other, and an unknown clock is refused rather than answered from
whichever is nearer — a duration measured across a clock correction comes out
negative, and a negative duration is a number that gets used.

**`localtime_r` is `gmtime_r`, deliberately, and it costs nothing.** There is
no host to ask on this kernel and no zone database to read, so UTC is the only
true answer — and `src/recon_clock.c` already applies ReconOS's own zone to its
own civil-date arithmetic, under a comment from months ago saying exactly why
it does not call `localtime`. The difference is asserted in the suite rather
than skipped, along with `%Z` being `UTC` where the host's C locale says `GMT`.

**Two faults, and both were the standard's wording rather than the
reference's behaviour.** `%C` and `%F` were written as two- and four-digit
padded fields, because that is how POSIX describes them. The suite disagreed on
year 1, and a probe settled it: glibc pads neither, and floors `%C` rather than
truncating it, so year -1 gives `-1` where `-1 / 100` in C is `0`. The goal
here has never been to match a document; it is that the desktop prints the same
thing it printed on Linux yesterday.

**And the suite was checked against itself again.** Four faults introduced one
at a time — the century leap gone, the leap-day shift in `tm_yday` gone, the
weekday off by one, floored division truncated instead — caught at 7, 8143,
40111 and 120622 failures. The century leap is the interesting one: seven
checks out of 436,012, and every one of them is a date somebody put in the
corpus on purpose.

### A measurement that has never heard of the list

The coverage figure in v0.4.22 was a grep for a list of function names.
**BG-182 is the record of what that is worth**, and correcting it by hand was
not enough: the corrected list then missed `gmtime_r`, `localtime_r`, and
sixteen of the twenty floating-point functions the desktop references.

It could never have found `puts` at all. `src/main.c` calls `printf` with a
string literal containing no conversions in it, and the compiler rewrites that
into `puts` — so **the desktop needs a function whose name appears nowhere in
its source.** No grep can see that. Reading the file cannot see it.

`scripts/measure-libc.py` asks the linker instead. Every object file carries a
table of the symbols it needs and a table of the symbols it has; the difference
is the external surface, exactly, and `nm` reads both in one command. It gives
two numbers because there are two questions:

- **52 of the 132 C library symbols** the desktop references. That is the
  completeness measure: 80 missing symbols is 80 functions to write.
- **2,473 of 3,113 call sites** in `src/`. That is where the weight is — and
  the names it counts now come out of `nm` rather than out of a list.

Three functions came out of that first run and are written and checked:
`memmem` (`recon_html.c`, finding the end of a comment in a page that may hold
a zero byte), `strcasestr` (`recon_http.c`, spotting a chunked transfer), and
`puts`.

### Two more desktop sources build with no glibc under them

`src/recon_url.c` joins the nine, so `scripts/check-userland.sh` is at
**10 of 10**. `src/recon_access.c` turned out to want xkbcommon as well and is
in the group waiting on that.

It found something on the way: **`include/recon_fs.h` includes
`<sys/types.h>` and does not use it.** Every type in its thirty-odd
declarations is `size_t`, `bool` or `time_t`. The two files that actually want
`mode_t` both already include `<sys/stat.h>`. One line in a public header,
inherited by everything that reads it, and it was the whole of what stood
between `recon_access.c` and a freestanding compile.

### What is left, by what it needs

| | symbols | call sites |
| --- | --- | --- |
| an allocator | 5 | 430 |
| files and directories | 21 | 96 |
| sockets | 19 | 44 |
| an errno | 1 | 43 |
| loading a module at run time | 4 | 12 |
| processes and signals | 6 | 8 |
| floating-point maths | 20 | 5 |
| an environment | 1 | 2 |
| the rest of stdio | 3 | 0 |

The allocator is still the wall and is still first in `docs/KERNEL-WANTS.md`.
What the linker changed is the shape of the rest: **floating-point maths is
twenty functions**, not the four a grep found, and it is a self-contained
afternoon that needs nothing from the kernel at all.

---

## v0.4.22 — a C library of our own, checked against the one it replaces

Every library call the desktop makes today reaches glibc. When the desktop is
built for the ReconOS kernel there will be no glibc under it, and something has
to answer those calls. This version is that something: `userland/libc/`, a
freestanding C library, and the harness that proves it behaves the way the one
it replaces behaves.

**Measured now: 2,479 of the 3,089 library calls in `src/` are answered by
it.** Not chosen -- counted, and then *corrected*, which is the part worth
keeping: the first count listed the functions the library was expected to need
and counted those, which finds every call of a function on the list and none of
a function that is not. It reported 2,430 of 2,997 and was wrong in both
numbers.

What corrected it was taking glibc away -- `scripts/check-userland.sh`
compiles the desktop's own sources with `-nostdinc`, the compiler's own headers
and `userland/include`, and nothing else. Seven of the sixteen files the sweep
called ready did not build, and three functions turned out to be simply absent:
`strtok_r` on **44 call sites**, `strncat`, `strtoull`. All three are written
and checked against the reference now. The script is the fifth pass of
`scripts/check.sh`, so a header cannot drift away from the code meant to
include it again without something saying so. The top of that list is the shape of this system: `snprintf`
915 times, `strcasecmp` 357, `strlen` 211, `strcmp` 139. A desktop is mostly
text being compared and formatted.

### The test is the point, not the code

Writing `strlen` is an afternoon. Writing a `strlen` that behaves *exactly* as
the one it replaces, across every input the desktop will ever hand it, is the
part that can go wrong quietly -- and every one of these functions fails
quietly. A `strcmp` that returns the wrong **sign** still sorts, backwards. A
`snprintf` that rounds `%.2f` the wrong way at a half is wrong in a way nobody
writing a test from memory would think to check. Neither crashes. Neither
warns.

So there is no test here that says what these functions should do. Both
libraries are compiled into one program -- ReconOS's renamed by a
`-include prefix.h` so the two can coexist -- and every call is made twice and
the answers compared. **86,160 checks, and the reference is the referee.**

The corpus is deliberately the cases nobody writing from memory reaches for:
empty strings, overlapping ranges, embedded NULs, lengths of zero, bytes above
127, the most negative integer, a precision longer than the string, a width
shorter than the number.

### What it now covers

- **Strings and memory**, and `snprintf`/`vsnprintf` -- the most-called
  function in ReconOS, with flags, width, precision and length modifiers, and
  an unknown conversion written back verbatim rather than swallowed.
- **Character classes**, ASCII only and permanently so: ReconOS's text is
  UTF-8, where a non-ASCII character is two or more bytes, so "is this byte a
  letter" has no useful answer above 127 and a table that said yes would
  encourage the per-byte reasoning that breaks on the first accented name.
- **Numbers out of text** -- `strtol`, `strtoul`, `strtod`, `atoi`, `qsort`.
- **The file layer** -- `fopen` and the eleven stdio functions the desktop
  actually calls, on a fixed table of sixteen streams with their buffers inside
  them, because there is no allocator to put them anywhere else. The
  seventeenth `fopen` is refused cleanly, and the suite checks that it is.

### How the file half is testable at all

It should not be. The buffering is the interesting part, and the way to find
out whether `fseek` from the current position accounts for what is sitting
unread in the buffer would normally be to boot a kernel and look at the
consequences -- which is exactly the class of fault that does not announce
itself. A file read four bytes short looks like a file that is four bytes
short.

`userland/tests/hostsys.c` answers the five primitives `stdio.c` is built on
with POSIX calls instead of ReconOS system calls. So the same buffering code
that will run on the kernel reads a real file here, and is held against what
the host's stdio returns for the same file: the same bytes, the same counts,
the same position after every operation, at seven item sizes and nine buffer
sizes.

**And the suite was checked against itself**, because a file-layer suite that
passes on its first run deserves suspicion rather than satisfaction. Three
faults were introduced deliberately -- `fgets` dropping the newline it stops
on, `fseek` handing a relative offset straight to the kernel, `isprint`
accepting one byte too many -- and the suite caught all three, at 9, 40 and 1
failures. Then they were taken back out.

### Three faults, before any of it ran

All three are the same shape: the reference accepted something this refused,
and a refusal in these functions is not a failure -- it is a different number,
returned without complaint.

- **BG-179** -- `strtol("0b101", NULL, 0)` is 5 on every machine the desktop is
  built on today, because C23 added the binary prefix and glibc has shipped it
  since 2.38. This read 0 and consumed one character.
- **BG-180** -- `strtod("0x1F")` is 31.0 and the standard requires it. This
  returned 0.0. The hexadecimal path is now there and is the one exact path in
  the function: four bits a digit, so the mantissa is an integer and the
  scaling is by a power of two.
- **BG-181** -- `inf` and `nan` were read as nothing at all. A file of
  measurements round-tripping through ReconOS would have had its infinities
  quietly become zeros.
- **BG-182** -- `strtok_r`, `strncat` and `strtoull` were absent, and the
  coverage measurement could not see them: it counted the functions it already
  knew to look for, so the numerator and denominator were short by the same 49
  calls and the fraction looked right. **A measurement that shares its premises
  with the thing it measures can only agree with it.** Taking glibc away and
  asking a compiler was the instrument that disagreed.

**One fault was in the test rather than the library**, and it is worth
recording because the correction went the other way. The first run reported 254
disagreements on `toupper` and `tolower` for arguments below -1 -- the negative
numbers a signed `char` produces for every byte above 127. The reference maps
`-128` to `128`; this returns `-128` unchanged. The standard defines neither:
the argument has to be representable as an `unsigned char` or be `EOF`, and
those are not. So the test was asking a question that has no right answer and
believing the reply. It now holds the defined domain to exact equality and
**asserts ReconOS's deliberate answer separately, with the reason** -- above
127 there is no character to be the case of, and mapping the first byte of a
multi-byte character into the second half of a different one is the one outcome
that corrupts text rather than merely differing.

### Nine desktop sources build with no glibc under them

Not a claim -- a compile. `recon_ocr.c`, `recon_crypt.c`, `recon_smtp_message.c`
and six others build against these headers and the compiler's own, with every
system header directory removed.

The seven that do not each name something the port still needs, and only one of
those is the library's fault. Four reach `<xkbcommon/xkbcommon.h>` through
`recon_ui.h` -- a real Wayland dependency, and those files want their drawing
half separated from their compositor half before they can move. Two reach
`<time.h>` through `recon_fs.h` and `recon_cookie.h`, which is a
`userland/include/time.h` waiting to be written rather than a kernel ask: the
kernel has had both clocks since 8 September.

### What stops the rest being built on it

`malloc`. Of the 610 calls this library does not answer, **430 are the
allocator** -- `free` 311, `calloc` 80, `malloc` 34. Everything else left over
is small: 56 directory calls, 43 for `strerror`, 37 socket calls, 21 for time,
15 for the rest of stdio, 4 for processes, 4 for floating-point maths.

`strerror` is the interesting one of those, because it is not waiting on the
kernel: the kernel already returns negative error numbers and
`userland/include/recon.h` names them, so the table is ours to write. All 43
calls are in three files that are blocked on the allocator anyway, so it
waits.

`userland/include/stdlib.h` therefore has **no `malloc` declaration at all**.
A caller fails to *link* rather than getting a stub that returns NULL and a
crash somewhere else an hour later. The ask is written up in
`docs/KERNEL-WANTS.md`, first on the list.

---

**The count, which lived only in the version table until 15 September.**
**2,473 of the 3,113 library calls** in `src/` are answered by it -- counted
with `nm` over the objects rather than estimated -- and it is held against the
library it replaces by compiling both into one program and making every call
twice: **511,000 checks**.

Recorded here because the row in `docs/VERSIONS.md` was the only place those
three figures existed, and that row is an index line now. A measurement with
one home is a measurement that goes when the home is tidied.

## v0.4.21 — cookies, and the four things they are not allowed to do

Forms could sign you in and the answer came back with a session the next
request did not carry. Now it does: signing in on one page and following a link
on the next is one visit.

A cookie is a piece of somebody else's state kept on your computer and returned
to them on every visit. That is what makes a session survive a page, and it is
also the whole mechanism by which browsing is followed around -- **the two are
the same feature**, and there is no version of it that is only the first. So
the interesting content of `recon_cookie.h` is what it refuses, and none of the
four is a setting.

**One: a cookie belongs to the host that set it.** The `Domain` attribute --
which asks for a cookie to be sent to a whole family of hosts -- is honoured
only when it names the host itself, and anything wider is *narrowed* to the
host rather than refused, because narrowing is always safe.

Honouring it properly means knowing where the registrable part of a name ends,
that `example.co.uk` is a site and `co.uk` is not. That is not derivable from
the name; it is a list, and a *copy* of that list goes stale in the one
direction that matters -- a suffix registered after the copy was taken is one
this would treat as an ordinary domain, so `Domain=.something.new` would be
accepted and every site under it would share a cookie. **A check that quietly
weakens as a file ages is worse than one that was never there**, because
nobody is watching it. What the refusal costs is bounded and stated: a cookie
set on `example.com` is not sent to `www.example.com`.

**Two: only the document carries them.** A page's pictures and stylesheets are
fetched without cookies, always. This is enforced by the *shape of the call* --
`recon_http_get` takes the jar as an argument, so every fetch says at its call
site whether it is carrying the session, and two of the four say no. Proven
rather than asserted: a test server that counts image requests recorded the
picture being fetched twice, once before signing in and once after, and the
second carried nothing while the document beside it carried all three.

**Three: a `Secure` cookie is never set or sent over an unencrypted
connection.** The same rule, in the same shape, as refusing a redirect from
https back to http.

**Four: nothing is written to disk.** A stored session cookie is a key to
somebody's account sitting in a file. The place for a key here is the keyring,
which holds 512 bytes per secret and has no consent question in front of it
yet; both are on the list. Until then this says what is true -- signing in
lasts as long as the window.

`HttpOnly` is recorded and does nothing, because it hides a cookie from script
and there is no script. `SameSite` is recorded and does nothing because this
sends cookies on no cross-site request at all, which is stricter than
`SameSite=Strict` and is not a setting.

**A Cookies page**, in the menu beside History, listing every site, name,
**value**, path and expiry, with the narrowed ones marked -- and one entry that
forgets them all and says how many went. The values are shown because a viewer
that offers to tell somebody what is being sent on their behalf and then hides
the part that matters is not telling them anything.

**Three faults, all found before this had spoken to a server.**
[BG-176](BUGS.md#bg-176) is the one worth reading: the expiry parser returned 0
for a date it could not read, and 0 is a real date -- `Thu, 01 Jan 1970
00:00:00 GMT` is epoch zero and is **the commonest deletion header on the
web**. So signing out would have left somebody signed in, and it would have
looked exactly like being signed in.
[BG-177](BUGS.md#bg-177), a cookie name too long being stored under a name no
server ever set, from `-Wformat-truncation`. [BG-178](BUGS.md#bg-178), a
`Max-Age` of twenty digits overflowing the clock, from the sanitizer -- fixed
by clamping to four hundred days, which RFC 6265bis says a user agent must do
anyway.

**84 checks in `tests/test_cookie.c`**, and the fuzzer sends mutated
`Set-Cookie` headers to five hosts and five paths and checks no cookie ever
reaches a host that did not set one: **10,788 cases**, up from 9,862.

---

## v0.4.20 — forms, and they submit

The viewer could show a search box and could not search with it. That was the
largest thing missing from it by a distance: a browser you have to leave to go
and use a browser.

**Every control a page can have.** Text boxes, passwords drawn as dots,
checkboxes, radios grouped by name, `<select>` with a list that drops down over
the page, `<textarea>`, submit and reset buttons, and hidden fields that are
sent and never drawn. A control is a *run* in the document rather than a block
of its own, so it sits in the line where the page put it -- "Search for [ ] in
the menu" is one sentence with a box in the middle of it, which is what it is.

**A GET goes; a POST asks first.** That split is the whole of what survives of
the old rule that a viewer able to submit a form could change something on
somebody's server. A GET is a question: everything it says is in the address,
asking twice is asking once twice, and it is what every search box on the web
is. A POST is a statement -- not in the address, not in the history, and asking
twice may have done the thing twice -- so it says how many answers it is about
to send, to which host, and whether one of them is a password, and waits to be
told again. One dialogue between somebody meaning to sign in and somebody's
first click on a page they have not read.

**The rules about what is sent are HTML's, and each one has a page that breaks
without it.** An unchecked box is absent entirely rather than empty. A checked
box with no value of its own sends `on`. A menu sends its option's *value*, not
the words shown. Of all the submit buttons on a form, only the one pressed is
sent, which is how a page tells Save from Delete. Two controls sharing a name
send twice, in document order, because that is how a page sends a list.

**A control a stylesheet has hidden is still part of the form.** That changed a
rule this parser already had: inside something hidden, only the structure is
followed. Form elements are now the one exception, because what a hidden
control contributes is not a picture, it is a value -- and half the search
forms on the web carry a hidden field saying which section is being searched.
Measured on Wikipedia's article for *HTML form*: twenty-six controls, of which
six are its search forms and twenty are page furniture.

**Every control a pointer can reach, a keyboard reaches too.** Tab moves
through them, Space works whatever has the ring, Enter in a text box sends the
form it is in, and Escape puts a field back to what the page had. A first
version stepped only between text boxes, which reads as working until the form
has a box somebody has to tick.

**`recon_form.c` is its own file, and that is the point.** These rules decide
what leaves the machine, and testing them inside the browser meant linking a
compositor -- the same reason `recon_url.c` was split out of `recon_http.c`.
Forty-eight checks in `tests/test_form.c`, written against Wikipedia's search
form and the HTML5 standard's own example form rather than against markup
invented for the test. The fuzzer now sends every form of every mutated page:
**9,862 cases**, up from 7,556.

**POST in `recon_http`, and what a redirect does to one.** A 303 becomes a GET
with no body, and so do 301 and 302 -- they are *specified* to keep the method
and are implemented everywhere as though they were 303, and a client that
followed the specification alone would re-post an order to a server that was
redirecting to a receipt. 307 and 308 exist to say "again, exactly", and are
the only two where a body crosses a redirect.

**And a fourth pass in `scripts/check.sh`, for a fault that was nobody's
code.** `assets/help` is generated from this file by a script, and the rule was
to run it in the same commit -- which held until it did not.
[BG-175](BUGS.md#bg-175): the Help application inside a running v0.4.19 was
describing a system that ended at **v0.4.5**, and nothing said so. The check
regenerates into a copy, compares, and puts the tree back exactly as it found
it: a check that repairs what it is checking passes the second time it is run.

**Three faults, and how each was found.** [BG-174](BUGS.md#bg-174), a textarea
drawn three times too tall, straight through the buttons under it -- found in
the first photograph, and invisible to every number, because the page's height
was correct and only the drawing was wrong. [BG-173](BUGS.md#bg-173), Space on
a checkbox ticking it and then letting go of the focus, so the next Tab started
again from the top of the form -- found by asking the window where its focus
was after each key, because a ticked box looks the same either way.
[BG-172](BUGS.md#bg-172), a tooltip left explaining a button that had already
gone; the same fault as BG-089 one level down, and fixed the same way.

---

## v0.4.19 — a > inside an attribute is not the end of a tag

A sweep of twelve real sites, looking at *what came back* rather than at
whether anything crashed. Eleven were fine. Wikipedia's "Comparison of
operating systems" had `{{cite web|...}}` and `[[Lisa OS]]` in the middle of
it -- wiki source, in a rendered article.

**BG-171.** The parser found the end of a tag with `memchr` for the first `>`.
An attribute value may be quoted, and a quoted value may contain anything at
all -- including `>` -- and it is still inside the tag. Wikipedia carries the
wiki source of every reference in a `data-mw='{"parts":...}'` attribute:
single-quoted, holding JSON, and that JSON holds escaped HTML. The scan
stopped at the first `>` and rendered the rest of the attribute as page text.

**411 blocks before, 219 after.** Nearly half of what that article appeared to
say was attribute rather than content.

Nothing had caught it because every hand-written test uses well-formed markup
with short attributes -- that is what somebody writing a test writes. The
html5lib corpus did not catch it either: it tests tree construction, this
parser builds no tree, so it is only run for the invariants, and "the page
says something it should not" is not an invariant. It took loading real pages.

The sweep is worth keeping as a habit rather than a script. The other eleven
sites all rendered, and the RFC 2616 page hit the four-thousand-block ceiling
and **said so**, which is the behaviour that ceiling exists for.

---

## v0.4.18 — the package module splits, and the stub file goes

`recon_package.c` was one file doing two jobs. Reading a manifest and taking
digests of what it names is arithmetic on bytes; installing loads a shared
object into this process and rebuilds the icon cache.

The consequence was the same one `recon_http.c` had. Testing the first half
meant linking the second, so the signature suite needed a stub file that
**aborted on the module loader, the icon cache and the version comparison**
just to link -- five functions stubbed to make one file testable, which is the
shape of a file that wants splitting. That was said out loud in the stub file
when it was written, and this is that debt paid rather than carried.

`src/recon_manifest.c` is 559 lines that load nothing: no module loader, no
icon cache, no registry, no accounts. `src/recon_package.c` keeps the 802 that
install, upgrade, verify and remove. `src/recon_manifest.h` is what they share
-- the manifest's shape, the error buffer both report through, and the
allow-list question the installer asks.

The signature suite now links five files instead of eight, and the stub file
is deleted. It is worth being precise about what that buys: not tidiness. A
suite that can only be built by stubbing out the dangerous half is a suite
somebody will eventually stop maintaining, and the untested half is where the
fragment bug spent its life.

---

## v0.4.17 — tables have columns

A table was one line per row with the cells run together by spaces. You could
read it and you could not scan it: "0.4.13 8 September Package signing" is
three facts with nothing to say where one ends.

Two things had to exist first. **A row is a block of its own kind**, so a
viewer can tell which blocks belong to one table -- consecutive rows is what a
table is, once the tags are gone. **A cell marks the run it begins at** rather
than being a block itself: a cell that was a block would get its own line,
which is the one thing a table exists not to do.

That second one needed a change where it was easy to miss. Runs that look the
same and sit next to each other are merged into one, which is right everywhere
else and wrong across a cell boundary -- merging loses the boundary, and the
boundary is the only thing saying where a column ends. A table of plain
unstyled text, which is most tables, would have come back as a single run and
could not have been laid out at all.

A column's width is a property of the **table**, not of any row -- it is the
widest that column gets anywhere -- so it is measured once over every row
before the first is drawn. A table wider than the window has all its columns
scaled down together, which keeps their relative widths: the shape survives
even when the size cannot.

`<th>` records itself as level 1 on the row it is in, which is the field a row
otherwise has no use for, and that is the whole of what makes the top of a
table read as a heading rather than as more data.

**What this does not do**, said plainly: no colspan, no rowspan, no nested
tables, no borders, and a cell whose text is wider than its share overlaps
rather than wrapping. Wrapping inside a cell means a row of variable height,
which means measuring the height before placing the row, which is the whole
layout problem again one level down. Lining the columns up is most of what a
table is for and it is the part that can be had honestly.

---

## v0.4.16 — the fuzzer learns about stylesheets and addresses

`test_malformed` swept the ICO, MP4, HTML and expression parsers with
truncations, single-byte changes and random bytes. It did not sweep the two
parsers added since -- **CSS and addresses** -- and both take bytes straight
off a web server.

A stylesheet is as much somebody else's file as the markup is: fetched from
whatever host the page named, parsed, and then consulted for every element on
the page. An address is worse in one way -- it is the boundary between their
bytes and where this machine *connects to*.

The CSS feeder does not stop at parsing. It matches the sheet against a
three-deep element stack, which is the only thing a sheet is for and the path
where a bad rule actually gets walked -- a sheet that parses without crashing
and then reads outside itself on the first match is one a parse-only sweep
calls fine. It also feeds the same bytes to `recon_css_inline`, which is the
other way in and does not go through `recon_css_add` at all.

The address feeder checks what a caller then relies on: that the host and path
are terminated, that the path begins with the slash every caller assumes, that
the port is a port, and that **no fragment was left in the path** -- the path
is what goes in a request line, and a `#` in it is a byte sent to a server
that was never meant to leave the machine.

7,556 cases, no failures, under the address and undefined-behaviour
sanitizers. Nothing found, which is the ordinary result and is worth having
run.

---

## v0.4.15 — text-align, which needed a line before it could be placed

`text-align` had been read, recorded on the block, and then not drawn. The
comment beside the field said so and said why: centring a line means knowing
how wide it came out before placing its first word, and how wide it came out
is only known once the last one is placed. The flow put each word down as it
reached it, so by the time the width was known the words were already
somewhere.

So the words are held now, and the line is drawn when it ends -- at a wrap, or
at the end of the block. Each keeps the x it would have had on a left-aligned
line and the flush adds one offset to all of them. No second pass over the
document, no second idea of where a word goes, and **left alignment comes out
with an offset of zero, which is bit-for-bit what it did before**.

The link underlines are held with the words and shifted by the same amount. A
rule drawn at the unshifted position under a centred line is a rule sitting to
the left of the words it belongs to.

`justify` is not here. It means changing the space *between* words rather than
moving the line, which is a different operation on a different unit, and doing
it badly -- stretching the last line, or leaving rivers down the column --
looks worse than not doing it.

---

## v0.4.14 — the configuration that ships, built for the first time

Every build ReconOS makes for itself is Debug with no `-O`. `scripts/check.sh`
says so at the top and calls it out as a risk, and `scripts/package.sh` builds
Release -- so **the thing that ships is the one configuration nothing looks
at**. BG-170 came out of exactly that gap, so the rest of it got the same
treatment.

An optimised build turns on warnings a debug build cannot produce, because
`-Wformat-truncation` needs the optimiser's value-range analysis to see how
long a string can get. Twenty-two of them fell out, and three were real:

**A null reaching `%s` on the sign-in path.** `recon_users_login` looks the
account up a second time and did not check the result. It cannot be missing --
`recon_users_check` returns false when the lookup fails, and nothing changes
the account list in between -- but that is an invariant spread across two
functions and stated nowhere. It is checked now, because the day `check` grows
a cache or matches names by a different rule, this becomes a null dereference
found by somebody failing to sign in.

**Nine paths in the Recycle Bin built with `snprintf`, which truncates.** A
truncated path is not a shortened name for the same file, it is the name of a
different one -- and these are then removed, restored or written to. None is a
memory fault; the fault would be a restore that puts a file somewhere else, or
a purge that removes the wrong note, and neither would ever be traced back to a
path one byte too long. They go through one helper that refuses.

**And three more path builds elsewhere** -- a theme file, a track the player
opens, and a path handed to the module loader -- moved to `recon_fs_join`,
which was already the right answer and already refuses.

Seventeen truncation warnings remain, all of them in code building a string to
*display* rather than to open. Left rather than silenced, because a warning
that has been argued with in a comment is worth more than one suppressed.

---

## v0.4.13 — a package says who made it

Installing was trusting whoever handed the folder over. A package brings a
module, a module is a shared object loaded into this process, and a module in
this process can do everything ReconOS can do. The allow-list bounds where a
package may *place a file* and says nothing at all about what its code does
once running -- it was never meant to.

This was left on purpose until the keyring had been read, because adding a
second piece of unattended crypto before the first had been looked at would
have compounded the debt rather than reduced it. Joshua lifted that.

**ECDSA over P-256, SHA-256, from mbedTLS.** Ed25519 would be the better
choice and is not available: mbedTLS 2.x has Curve25519 for key agreement
only, which cannot sign. A correct P-256 signature is worth more than an
Ed25519 one this would have had to write itself.

**There is no random per-signature secret.** ECDSA's `k` is the whole of its
danger -- reuse it under one key for two messages and the private key falls
out with school algebra, which is how the PlayStation 3's signing key was
published, and a *biased* `k` is nearly as bad and much harder to notice.
mbedTLS is built with `MBEDTLS_ECDSA_DETERMINISTIC`, so `k` comes from the key
and the message by way of HMAC-DRBG (RFC 6979) and there is no randomness that
can fail quietly and take the key with it. Since that is a property of how the
library was built and not of this code, `recon_sign.c` **refuses to compile**
without it rather than falling back to the randomised form and being weaker in
a way nobody would see.

**What a signature covers is a digest list, not the manifest.** Signing the
manifest alone binds the *names* of the files and none of their contents,
which leaves the module free to be replaced -- and the module is the only file
that can do anything. So the signed bytes are one line per file, sorted, with
the manifest first. Sorted because the order a directory hands back its
entries is not a property of the package, and two machines must build the same
bytes from the same folder.

**And there is no `--force`.** A check that can be turned off is off on the day
it matters, and the person who would reach for the flag is exactly the person
being attacked. Somebody who means to install something unsigned signs it,
which is one command and is the point.

**A machine makes its own key on first run.** That is not a weakening -- the
key is generated locally and its private half is written mode 600 -- and it is
what makes the refusal actionable. A machine that trusts nothing installs
nothing, which is correct and is a terrible first experience if the way out is
a command nobody has a reason to know exists. Now `sign` always works, and the
refusal names it.

Three commands: `keys` lists what the machine trusts and whether it can sign,
`keys make` / `keys trust` / `keys distrust` change that, and `sign <folder>`
puts this machine's name on a package. Distrusting a key stops what it signed
from *installing*; it does not uninstall what is already there, and says so,
because that code is already on the disk and pretending otherwise would be
theatre.

**Two suites, and a note about the second.** `signatures` tests what
`recon_sign.c` decides rather than what mbedTLS does -- that a changed byte
stops verifying, that an untrusted key does not count, that a private key
cannot be added to the trust store, that a key name cannot escape the trust
directory. One of its checks is load-bearing in a way that is easy to miss:
signing the same bytes twice must give the same signature, which is the RFC
6979 property, and is what would notice if the `#error` above were ever
removed.

`package-signatures` tests that the signature covers the module and not only
the manifest. There was **no suite for packages at all** before this, which
meant the install path was the least tested thing in the system.

### And what the keyring review found

Reading outward from `recon_keyring.c` to what its callers do with what they
are handed. **The keyring itself holds up** -- see the commit for the full
list, but the short version is that every decision in it is the right one and
`recon_from_hex` even validates its length, so a truncated nonce is rejected
rather than zero-padded.

What did not hold is BG-170: two places cleared a secret with `memset` where
nothing reads the buffer afterwards, which makes the store dead and lets the
compiler delete it. It does. `scripts/check.sh` now fails on any `memset` of
something named like a secret, because no test and no sanitizer can see the
difference.

---

## v0.4.11 — History and Bookmarks, as pages

The History entry in the menu reported a count in the status bar, which is not
a history. Both are now **pages the browser builds and then reads**.

Not a panel, a dialog or a list widget, because a page is something this
program already does completely: lay out, scroll, colour to the skin, search
with the find bar, and -- the part that matters -- make every entry a link
that already works. A list widget would need its own layout, its own scrolling
and its own click handling, none of it shared with the three that exist. The
markup is generated and parsed rather than turned straight into blocks, for
the same reason: the parser is the one place that decides what a document is.

History is newest first, which is the order somebody looking for where they
just were wants, and the opposite of the order Back and Forward use. The page
you are on is marked rather than left as a link to itself.

**And it found a bug the moment it was looked at (BG-169).** The list said
`https://gaming.recontowers.com/example.com`. `go_typed` was resolving what
was typed against the page being read -- the same way a link on that page is
resolved. A link on a page is relative to it; **text typed into an address bar
is not**. So once you were on any page at all you could not reach a different
site by typing its name, only paths on the site you were already on.

It had been there since the address bar existed, and survived because every
test of it typed into a *fresh* tab, which has no page to be relative to. The
same shape as the last two bugs: a check written against the one case somebody
happened to try. A path or a fragment is still resolved against the page --
"/about" and "#notes" have nowhere else they could mean.

Worth recording how it was found: not by a test and not by reading, but by
building a screen that shows the data. A list of visited addresses is a thing
you look at, and a wrong one is obvious the moment it is on screen.

---

## v0.4.10 — a link to a place on a page goes there

"That points at a place on this page, which this cannot jump to yet" was the
viewer's answer to every `#anchor` on the web, including the one link whose
entire purpose is to be followed: "Skip to content", which exists for people
who cannot use a mouse.

Three pieces. **An address keeps what follows the hash** -- out of the path,
because that is not part of what is asked of the server, and out of the
formatted address, because two addresses differing only after the hash are the
same document and comparing them with the fragment in would fetch the page
again for every anchor on it. **A document records its named places**: every
`id`, and the block it lands on, which is the finest thing a viewer can scroll
to. **And the viewer arrives**, on the draw after the click, because where a
block *is* is only known during the pass that lays the page out.

A link naming a place the page does not have says so, rather than doing
nothing -- a link that appears dead is indistinguishable from one this has
failed to handle. A bare `#`, which pages use for links only script gives
meaning to, goes to the top, which is what a browser does.

**And the bug that was in the way (BG-168).** `href="#main"` parsed to an empty
fragment, because the fragment is pulled out by searching for a hash in the
*path* -- and the path that branch writes is the base's, whose own hash was
stripped when the base was parsed. Every link within a page named no place at
all, and the viewer correctly did what that means: went to the top of the page
you were already at the top of.

**There was no suite for reading an address, and there could not easily have
been one.** The parser sat in `recon_http.c` beside the fetching, so testing it
meant linking sockets, TLS, the registry and a Wayland event loop. An address
parser that cannot be tested without a socket is one nobody tests. It is
`src/recon_url.c` now -- 250 lines that open nothing -- and `tests/test_url.c`
covers it: full addresses, fragments, resolution against the page a link was
written on, and the fact that two places on one page format as the same
address.

Find on page also **goes to the match** now rather than marking one that may be
off screen, a third of the way down the window and only when it is not already
visible.

---

## v0.4.9 — a corpus nobody here wrote

Joshua added a checkout of mozilla-central alongside the Netscape one, for
features Netscape did not have. Measured before being used, the same as last
time: `parser/html` is 36 C++ files, `servo/components/style` is 277 **Rust**
files, `layout/` is C++, and `js/src` is 825 C++ files. ReconOS is C11. None of
it is takeable as code, for a different reason than Netscape's -- that tree was
too old, this one is in two languages this does not build.

The obvious portable thing was the named character reference table: the
specification has 2,231 and this has 24. So that was measured too, across six
real pages -- gaming.recontowers.com, recontowers.com, Hacker News, a Wikipedia
article, gnu.org and LWN. **1,251 named entities used, 100% of them already
covered.** The comment in the table calling its 24 entries "the fraction that
appears in prose" turned out to be exactly right, and importing the other 2,207
would have bought nothing.

**What the tree was actually worth is the html5lib tree-construction suite** --
1,556 pieces of markup, MIT-licensed plain data, written by people whose
purpose was to break HTML parsers. Their expected output is a DOM tree and this
parser deliberately builds none, so the answers cannot be compared. What can be
checked is everything that must hold whatever the answer is: that it returns,
that every block's runs are inside the run table, that every link index names a
link that exists, and that every byte of every run can be read back.

That is 14,204 checks, and it is the first suite in this project written by
somebody other than whoever wrote the code it tests -- which is the failure
BG-162 records.

**It found a bug on its first run (BG-167).** `add_rule` sets four of a block's
six fields, and the missing one is `source`, the index of the block's picture.
The array is `calloc`ed, so the value was not garbage -- it was zero, and zero
is not "no picture", it is "the picture at link zero". Every `<hr>` on the web
claimed to be an image whose address was the page's first link. Nothing acted
on it, because every reader checks the kind first; it was simply untrue, with
nothing arranged to notice. Nobody writes a test asking whether a horizontal
rule has a picture.

**And find on page was searching the screen rather than the page (BG-166).**
Shipped last version, and wrong from the start: the match test sat below
`put_word`'s off-screen early return, so the count was of matches already
visible. "1 of 2" meant two on this screenful, and searching for a word further
down found nothing at all. Every test of it had used a word visible in the
first screenful -- the same fault as BG-162, in a different place. Whether a
word matches is a fact about the document; everything below that return is
about the screen. **Next now takes you to the match**, a third of the way down
the window rather than at the very edge, and only when it is not already
visible.

`recon_html_run_count` and `recon_html_link_count` exist because the corpus
test needed to bounds-check an index and there was no way to ask how big the
tables were.

---

## v0.4.8 — the browser gets the rest of a browser

Asked for as "redesign the entire layout of the browser -- it doesn't have a
lot of buttons and features". It had three: back, forward and reload, with an
address bar. Everything below is what a window with one page in it turns into
when it can hold several.

**Tabs.** The structural change, and the one everything else sits on. A window
was a page: one document, one history, one set of fetches. It is now a window
that *owns* pages, and the split is exactly at the toolbar -- the tab owns the
document and the work, the window owns the chrome around it.

The part worth writing down is that **every fetch is handed the tab, not the
window**. A page finishing in a background tab lands in its own tab instead of
overwriting whatever is in front of you, and the same rule stops a background
page rewriting the address bar or the window's title. Twelve to a window, each
one heap-allocated -- an image table and a history is a quarter of a megabyte,
and twelve of those inside the window struct would be three megabytes of
window whether or not anybody opened a second tab.

Closing the last tab empties it rather than closing the window. A browser that
vanishes when you close a tab is a browser people lose work in.

**A toolbar with the controls a browser has.** Home, and a stop that is the
same button as reload -- one question, "is this page still coming?", whose
answer is never both, where two buttons would leave one of them always dead.

**A padlock that says only what it knows.** Closed for TLS, open for plain
HTTP, and absent before anything has loaded. It is deliberately not a claim
about the site: "the connection was encrypted" is the only thing this can
actually know, and dressing that up as "this page is safe" would be a lie told
by the browser rather than by the site. Drawn rather than typed, because the
system font has the arrows, the star, the house and the triple bar this
toolbar uses and does not have U+1F512 -- and a security indicator that is
sometimes invisible is worse than none, since its absence is what says "not
secure".

**Bookmarks**, kept in `/Users/Shared/Web/bookmarks.txt` as `url<tab>label` --
a tab because a title has spaces in it and an address does not, so the split
needs no quoting and no escape rules to get wrong. The star adds and removes:
one control answering one question, where a separate remove would be a second
control that is only ever right when the first is wrong. The bar turns itself
on when the first bookmark is kept, because a bookmark you cannot see is one
you will not believe was saved.

**Find on page**, with the matches marked *behind* the words rather than by
recolouring them -- a page already uses colour to mean things, and a match
that recolours a word competes with whatever the page was saying. The current
match takes the accent and the others a wash of it, so "which one am I on"
reads without counting. The count comes out of the same pass that draws the
page: counting separately means walking the document again with a second idea
of what a word is, and the two disagree the moment one is changed.

**Zoom**, per tab, 50% to 250%. It multiplies whatever the kind and the
stylesheet settled on rather than replacing it, so a heading at 150% is half
again as big as *that heading*. The percentage appears in the status bar only
when it is not 100%, and clicking it puts it back -- a number that is always
there is a number that never means anything.

**A menu**, and the keys: Ctrl+T, Ctrl+W, Ctrl+F, Ctrl+D, Ctrl+L, Ctrl+R,
Ctrl+plus, Ctrl+minus, Ctrl+0, F3, Escape. The shortcuts are read before any
field sees the key -- letting the address bar have Ctrl+T first would type a
"t" instead of opening a tab. Entries that cannot do anything are drawn and
greyed rather than hidden: a menu whose entries come and go is one nobody can
learn the shape of, and "why is this grey" has an answer where "where did it
go" does not.

The tab strip and the menu rows both go through `recon_widget` -- the tab look
and the menu highlight already existed there, which is what that layer is for.

---

## v0.4.7

**A page is drawn in its own colours (BG-164).** Every site came out in the
skin's black on the skin's white, whatever it had asked for, because
`background-color` was parsed and then dropped -- and because the value could
not have been read anyway. Pages do not write colours as colours any more:
gaming.recontowers.com's stylesheet has 157 custom properties, 378 uses of
`var()`, and one literal colour in 62 KB. So a reader without `var()` reads a
modern stylesheet and finds nothing.

Custom properties are now collected from `:root`, `html` and `body` in a first
pass over each sheet, and `var(--name)` and `var(--name, fallback)` resolve
against them in a second. Two passes, because a sheet may use a name above the
`:root` that defines it -- which is not rare once a bundler has concatenated
four files, and is not the author's choice when it happens.

**And every colour on the page is checked against the page's paper, not the
skin's.** That check has not been weakened and still has no way to be turned
off. It was asking the wrong question: a page colour was tested against the
skin's surface even on a page painting its own, which failed in both
directions -- a light heading rejected for being unreadable on white, then
replaced with a near-black that went on near-black paper. Text, links, list
markers, horizontal rules and the quotation bar all now ask about the surface
that is actually underneath them.

The address bar and the status bar stay the skin's whatever the page says. A
page that could repaint them could dress itself up as the browser.

**A background on a page with no `<body>` tag is found anyway (BG-165).** Both
`<html>` and `<body>` are optional in HTML. The paper was taken as the element
went past, so documents that omit it had their stylesheet read correctly and
nothing to apply it to. Found by a test, not by a site -- the page being
looked at writes its `<body>`, so the feature looked finished.

---

## v0.4.6

**A `<br>` ends the line, not the heading (BG-162).** `<br>` ended the open
block and opened a *paragraph*, so every heading, list item and quote on the
web with a line break in it lost its kind halfway through. Measured on
gaming.recontowers.com, whose masthead is `<h1>Games built to<br>mean
something.</h1>`: the first half came out at heading size and the second at
body size, in the middle of one sentence. There are now two helpers named for
what they do -- `break_block` starts a paragraph, `break_line` reopens the
block it just closed -- and the callers say which they mean.

**An image no longer draws over the status bar (BG-163).** The viewer decided
whether a picture was *visible* and then drew all of it, so one three hundred
pixels tall whose top sat ten pixels above the last visible row painted the
other two hundred and ninety over the bar below. The check was written for
lines of text, where being wrong by less than one line is invisible, and then
handed to something fifteen times taller. `recon_draw_image_clipped` takes the
band of rows it may paint; `recon_draw_image` is that same call with the
panel's own height as the band, so the two cannot drift apart.

---

## v0.4.5

**A span a stylesheet turned into a block starts a line.** The difference
between a `<span>` and a `<div>` is one property, and pages set it constantly.
Measured on recontowers.com: a card of five spans laid out with `display: flex`
-- an eyebrow, a name, a destination, a blurb, a caveat -- came out as one
run-on underlined sentence. This still cannot lay anything out; it can tell a
line from a paragraph, and that is most of the difference.

**And the things a browser hides that no stylesheet mentions.** None of this is
CSS -- it is the behaviour of the elements themselves, and a reader that only
reads stylesheets shows all of it. A `<template>` is never rendered, a
`<dialog>` is shown only when opened, `[hidden]` means exactly what it says,
and a closed `<details>` shows its `<summary>` and nothing else. The same page
was showing an accessibility panel in full: twenty lines of settings nobody had
opened. Fifty-five blocks to thirty-seven, and the page's actual content
appeared underneath.

**A boolean attribute means something by being written, not by its value.**
`<dialog open>` and `<div hidden>` have no value, and the attribute reader
looks for one -- so asking it got both backwards: an open dialog was treated as
closed and a hidden div as shown. `has_attribute` asks the question that was
meant, on a name boundary at both ends so `data-open` is not `open` and
`openable` is not either.

**A page that is not UTF-8 is read as what it is.** Everything downstream --
the parser, the font, the title bar -- assumes UTF-8, and the single-byte web
that is left is Windows-1252 in practice, whatever it says. A page in it read
as UTF-8 loses every accent.

Decided by **looking at the bytes rather than believing the header**. UTF-8 is
self-checking: a sequence that decodes cleanly essentially never means
anything else, so valid bytes are UTF-8 whatever the label says, and invalid
ones are Windows-1252. The other direction would break pages that work today,
because a page that *says* Latin-1 and is really UTF-8 is common. Overlong
forms and surrogates are refused as invalid -- both decode "fine" and neither
is valid, and accepting them is how one byte sequence comes to mean two things.

**A link is underlined once, not once per word.** The rule and the clickable
region were drawn by the function that draws one *word*, so a headline of eight
words got eight underlines with seven holes between them -- which reads as
damage. Invisible until a page with multi-word links: everything this was built
against had links of one or two, where the gaps pass for letter spacing. It
also put eight regions in a finite hit table where one would do.

**The web viewer reads stylesheets.** Not all of CSS -- a subset chosen for
one question: what makes a page readable rather than what makes it look the
way its author drew it. `display`, `visibility`, `color`, `background-color`,
`font-weight`, `font-style`, `font-family`, `font-size` and `text-align`,
under selectors of one compound each with descendants: `p`, `.footer`, `#nav`,
`p.lead`, `div.body p`.

**The one that matters is `display: none`.** Most of what makes a real page
unreadable in a structural reader is not layout -- it is the parts of the page
that were never meant to be seen at once. wikipedia.org's front page loses
eight blocks of language-name wall to its own stylesheet, which is exactly
what its own stylesheet was for.

**What it will not do is as deliberate as what it will.** A child, sibling,
attribute or pseudo-class selector is kept *out* rather than half-matched:
`a:hover` is not `a`, and a rule that fires when it should not is worse than
one that never fires, because the first hides text. `@media` is skipped whole
-- its condition needs a viewport this does not model, and a print stylesheet
applied to a screen is worse than neither. A colour name it does not know
leaves the colour unset rather than guessing at somebody's brand.

**A page's colour is checked against the skin's paper before it is used.**
Ignoring the page loses a distinction the document drew between its own parts;
obeying it blindly is worse -- a page written for white says `color: #f8f8f8`
for something it puts on a dark panel, and a reader that takes that draws white
on white. `recon_color_readable_on` again, the third place in this system to
need it. A link keeps the skin's accent whatever the page says: it is the one
colour here carrying a *meaning*, and a meaning whose colour changes per page
is one nobody can learn.

**Smaller as asked; larger by half of what was asked.** A page sets a large
size because it has a layout to fill -- columns, a sidebar, a header the text
sits beside. This has one column the width of the window, so the same number
is far more of the screen here. Taken whole, wikipedia.org went from thirteen
visible lines to nine. Smaller is left alone, because a page marking something
down reads the same in one column as in six.

**A page that links stylesheets is parsed twice**: once to find what it asks
for, and again when the answers are in. They are fetched in the order the page
named them, because order is half of the cascade, and the page is redrawn once
at the end rather than per sheet -- half a cascade is not a cascade, and a page
that moves under somebody reading it is worse than one that changes once. The
markup is held for that second pass and only up to two megabytes: holding a
ten-megabyte document to restyle it later is a cost every page would pay for
the benefit of a few.

**The web viewer shows pictures.** An `<img>` becomes a block carrying the
address and the alt text; the viewer resolves each address against the page it
came from, fetches the distinct ones **one at a time after the words are on
screen**, decodes them and draws them in the flow. A page is readable the moment
its text is there, and thirty sockets opened at once to decorate it would make
the words wait for the decoration. Each picture redraws the page as it lands.

**The alt text stays.** A block that keeps it degrades to exactly what this did
before when the picture does not arrive -- which is what alt text is for, and
means a failure is a sentence rather than a gap. A failure is also *remembered*:
without that, a picture the server will not give up is asked for again on every
redraw, which is a page that never stops loading.

A picture gets its own block rather than sitting inline, because a picture has
a height and a line of text does not. That splits a paragraph an image sits
inside, which is what every renderer without a real layout engine does. Never
enlarged past its own size, shrunk to the column when it is wider: a
2000-pixel photograph fitted to the width is the photograph, and a 16-pixel
icon blown up to it is not more of the icon. Bounded at 48 pictures and 8 MB
each, and `data:` addresses are skipped -- that is a decode, not a fetch, and a
page may carry megabytes of them.

**And the parser has a suite now**, which it did not have at all. That absence
is how BG-160 got in. Thirty-five checks over blocks, runs, images with one
half missing or both, and the title. The title check was confirmed by breaking
the fix: it reports one failure and names it.

**Two faults in the web viewer, found by pointing it at the internet.** The
window's title bar read *"Wikipedia Close"* -- `<title>` is not only the
document's, SVG uses it for the accessible name of a drawing, and the parser
turned collection back on at every one. The first wins now: which is the
document's cannot be told from the tag, so it is told from the order.

And the status line was drawn in a page's ink on the *bar's* colour -- correct
while every skin's bar was grey, invisible on Beacon's blue or Metallic in
garnet. It goes through `recon_color_readable_on`, which was written for the
clock and is the answer to this whole class.

**The two names no icon pack answered are drawn, in the Glass set's own
terms.** `application` is what a program without an icon of its own gets, and
it was the one thing left on a themed desktop still wearing the drawn blue
window -- a coloured picture sitting in a row of silhouettes that take the
skin's ink. `file-font` was left out on the rule that a wrong picture on a file
type is worse than a plain sheet, which is true and does not apply to a page
with a letter on it.

Both are ours: ninety-six pixels square, pure white, the drawing entirely in
the alpha channel, falling from 212 at the top to 130 at the bottom. That ramp
is what gives the set its lit-from-above look, and is why a new icon that is
merely also white does not belong to it.

**Two more of the Colored Glass tiles found homes.** The rising chart is the
Task Manager's -- Watchtower graphs what the machine is doing and shares that
name -- and the list of coloured markers moved to `file-data`, which is rows of
values and is what a data file is. That leaves seventeen unused, and they are
unused for one reason: they are pictures of things ReconOS has no page for. A
wallet, a weather forecast, a map. The mapping in
`scripts/install-colored-glass.py` says which, and what each one actually is.

**Why this is 0.4.5 and not 0.4.1.** Eighty-seven commits landed on top of the
v0.4.0 tag without the number moving, which is exactly what the entry below
this one was written to complain about -- it says a release numbered 0.3.1 for
seventy commits is not a patch release, and then the same thing happened again
underneath it. A version that only moves when somebody remembers is a version
that says nothing.

Five patch-sized rounds went by unnumbered: mail and the network page, the
widget layer, the icons, the scene layers, and the skins. So the number jumps
to where it would have been if each had been cut when it was finished. It stays
in 0.4.x on the author's call -- the network stack is what 0.5.0 is for, and
the web viewer cannot reach the internet until it exists.

Twenty-two faults were found and fixed in this stretch, BG-137 to BG-158, and
every one of them is in [BUGS.md](BUGS.md) and on the tracker.

**The Glass icon set fills nearly every name ReconOS asks by.** 198 pictures,
36 of them answering a ReconOS name. The Control Panel is one set now rather
than a mix: Accounts, Passwords, Appearance, Display Settings, Programs,
Modules, Network, Firewall, Storage, Disk Cleanup, Update, Troubleshoot,
Recovery and Registry all come from it.

`scripts/install-icon-pack.py` is what puts one in, and it handles what a
folder of downloads actually looks like -- several sizes of the same picture, a
`web` subfolder the browser dropped things into, `-liquid-glass` in some names
and not others, and `-2`/`-3` suffixes where the same file was fetched twice.
Adding a pack is dropping a folder next to the others and running it.

**The search box looks like the field it is.** It drew a caret only once
something had been typed, which is exactly backwards -- the caret is what says
*type here*, so showing it only afterwards shows it to the one person who no
longer needs it. Clicking the box did nothing because there was nothing for a
click to do: while the menu is open, nothing else in it takes typing, so the
keys were already going there.

It is drawn as the focused field it always is. Two pixels of border in the
selection colour and a caret that is always present -- the border says it from
across the menu, the caret from close up.

**A stage with no name says so.** `session` over the control socket reported
`(null)` after a run that was killed rather than stopped. Two stages had been
added to the enum and not to the table of names -- the same fault the comment
above that table already describes, and the fix written for it cannot prevent:
designated initialisers stop a name landing on the wrong stage and do nothing
about a stage with no name. Both named, the table is sized by the enum, and an
unfilled slot now prints `stage N, unnamed` instead of `(null)`. A hole cannot
be caught at compile time; it can be made to name itself.

**The account's picture in the Start menu is a button.** It goes to the page
about accounts, which was four clicks away through the Control Panel's front
page. The picture, not the row it sits in -- and with nothing added to announce
it: a picture of the account is already the most obvious thing on screen to
press to reach the page about accounts, and a second mark at the other end of
the row explaining that is one more thing to look at. It lights under the
pointer, which is how a pressable thing says so here.

**The Photos window's controls are along the top.** They were along the bottom,
which is where a *status* line goes -- and that row is not status, it is the
only way to do anything there. Every other window in ReconOS puts what you can
do above what you are looking at, and a picture is the one thing on screen big
enough that a row underneath it is genuinely far away.

**The Start menu's search does what the box under it says it does.** The
tooltip has always read "Programs, places, settings and help", and it looked
through programs and help. Two of four -- and the missing two are the ones
people actually go hunting for: nobody forgets where Notepad is, and everybody
forgets which page the firewall is on.

It now finds **settings** (every Control Panel item, opening straight at that
page), **places** (the account's folders, opening the Explorer there), **files**
in those folders, and the help. Typing `fire` puts *Settings: Firewall* at the
top with the firewall's own icon; typing part of a document's name opens it in
whatever opens it.

**Order is by how sure the answer is.** Programs, then settings, then places,
then files, then help -- because the help is the only one of these that matches
on *body text* rather than on a name. It sat above the files at first, and
searching `quar` put four help pages above the two files actually called
`quarterly-notes.txt` and `quarry.png`.

**Six files at most, one level deep.** This runs once per keystroke while
somebody is typing, so a search that walks the whole filesystem is a menu that
stops -- and a menu filled with eighteen files is one where the program they
were reaching for has been pushed off the bottom by its own documents.

**And pressing Enter on a result now opens the result.** There were two ways to
reach a row and two answers: clicking switched on what kind of thing it was,
and Enter called "open the application called this" whatever it was -- so Enter
on a help page asked for an application named `Help: Files`, logged an error and
did nothing. Invisible for exactly as long as a program was always the first
match, which is as long as programs and help were the only things that could
match. One function knows what opening a row means now.

**Beacon comes in olive, and there is a Metallic skin.** That era shipped its
bright blue with a muted yellow-green alternative and a brushed-metal one, and
the green is the half people remember choosing. Beacon offers Blue and Olive.
Metallic is new: opaque, gradient chrome, and eight metals -- Silver, Steel,
Gold, Bronze, Copper, Ruby, Garnet, Onyx.

**A tint is part of a skin now, not a palette bolted to the side of one.**
Fifteen hues in one row under every tintable skin would be a row where the two
Beacon has a use for are lost among thirteen it does not. And the *strength* is
per skin as well, which turned out to matter more: Glass's 150 is right for
Glass, where a tint is a mood. Beacon's two are styles -- the machine came in
blue or it came in olive -- so a 59% move produced neither, and Beacon plus
Olive came out a slate blue-grey nobody asked for. It goes all the way now.

**The selection follows the tint now, and always should have.** The menu
highlight, the list selection, the field selection and the desktop selection
were left out with the accent and the warning, on the rule that a colour
carrying a *meaning* should not change with the decor. That rule is right and
these are not it: the accent says "this is the important one" and the warning
says "this will lose something", and those are learned. A selection says "this
one, the one you are pointing at" -- and it says it by being different from its
neighbours, not by being blue. A deep garnet desktop was opening a grey menu
with a slate blue bar across the row under the pointer: one thing on screen
that had not been told what skin this is.

On a solid-tint skin they are shifted rather than repainted -- their own
lightness, hue only -- because they carry white text, and Silver's lightness
under white text is a row nobody can read.

**An icon that is a picture gets an outline on chrome.** A silhouette is drawn
in the skin's own ink and reads on whatever is behind it. A picture keeps the
colours it was made with and takes its chances: measured on the Photos window
under Metallic in garnet, a blue picture-frame icon on a dark red bar, and
worse on Classic, the same blue on navy. It now gets a one-pixel outline in the
ink the title text is written in -- a colour the skin already knows reads
there -- which says where the shape is without pretending to know what colour
the shape should be.

**A tint that keeps the base's lightness is a mood. Metal is not a mood.**
This is the change that made Metallic work, and it took being told twice: "the
colours don't have too much of a metallic feel -- it looks more like frosted
glass", and then "more like a solid colour with a shine to it".

Both true, and one cause. `recon_color_tint` moves a colour to the tint's hue
while keeping its own lightness, which is exactly right for glass -- the
palette's structure survives and the hue shifts. It is exactly wrong for metal.
Silver and ruby are not one colour at two hues; they are two *lightnesses*.
Holding ruby at a pale silver's brightness produces pink, every time, and
pastel pink on a soft ramp is a frosted pane.

So a skin can now say that its tint carries its own lightness --
`recon_color_tint_to`, targeting a lightness the caller chooses rather than the
base's. Metallic asks for the metal's own, offset by half of where the role
sits relative to the taskbar, so a title bar stays a shade deeper than the
strip below it and the ramps still have somewhere to go.

**And it paints far less.** In that mode the tint reaches the title bars, the
taskbar and the window edge, and nothing else. Menus, buttons and page bodies
stay silver, because they carry dark text and a role taking the metal's
lightness would take the ground out from under it. Coloured chrome round a pale
page is what that era actually did, and for the same reason.

**The shine is the ramp.** Roughly sixty levels across a twenty-eight pixel
title bar, where Recon's is ten -- the difference between a surface that is lit
and a fill that is slightly uneven, which is the whole of what "metallic" means
with two stops to spend. Ruby and Garnet were deepened as well: `A01830` and
`6E1832`, because in this mode the hue's own lightness is what the chrome
becomes.

**Smoked has pictures where Glass has silhouettes.** Thirty-one of the fifty
Colored Glass tiles, under the ReconOS name each one answers. Glass and Smoked
are the same idea light and dark, so they are the pair to show the two ways an
icon set can work: one colour taking the skin's own ink, or a picture keeping
its own colours on a chrome that suits them.

Every file in that pack is RGB with **no alpha channel at all**, so the rounded
tile sits on an opaque near-black rectangle -- a black square on the wallpaper
and a black square on the chrome. The corners are cut geometrically rather than
by colour, by flooding inward from the border: the tile's own darkest pixels
are the same near-black as the surround, so keying by colour eats holes out of
the middle of a dark icon, and only what is connected to the outside can be
removed.

**And a skin's icon set can be improved now, which it could not before.** "Write
it if it is not there" keeps a real promise -- a replaced icon stays replaced --
and quietly meant a better set never reached a machine that had run ReconOS
once. The drawn icons answered that with a generation and a per-file
fingerprint; a skin's set cannot, because there is nothing of ours to compare a
downloaded picture against. So it is one number for the directory. The rule
that falls out is worth stating: a skin's icon directory is ReconOS's to manage,
and the **shared** set is the one to replace a picture in -- which is also the
one every skin without an opinion falls back to.

The order of two calls is the whole arrangement: the coloured pack is installed
first and replaces, then Glass's is installed and only fills gaps.

**The skin list is four lists.** Twelve skins in one column, of which three
exist for colour blindness and two for reading, is a list where the ones
somebody is choosing between are outnumbered by the ones they are not. They
are all skins and they are not all the same kind of thing -- Deuteran is not an
alternative to Glass, it is an alternative to *needing* to tell red from green.

**Standard**, **Colour Blindness**, **Easier to Read**, **Your Own**. Which
family a shipped skin belongs to is written down rather than guessed from its
description, because it is a decision about what the skin is *for* and a rule
read out of prose breaks the first time somebody rewords it. Anything not named
lands in Standard, which is the right default: a new skin is a new look until
somebody says otherwise.

The filter changes which rows are drawn and nothing else. `Use This Skin`,
`Customize Skin` and the per-skin colour lookups all speak in whole-list
positions, so the rows map back through the family and the rest of the page
never learns a filter exists. Copying a skin moves the list to Your Own,
because closing the editor onto a tab the new skin is not in reads as the copy
having failed.

**The taskbar is opaque, and that is not a setting.** It took the skin's full
glass, on the reasoning that a strip of short labels survives being
see-through. Measured on Smoked it does not: the bar and the wallpaper behind
it landed sixteen levels apart. And a pale window sliding under it *lifted*
it -- the same strip read `1C1D2B` over the desktop and `2E2D47` over the
Calculator, which is what "the taskbar greys out when a window opens" is a
picture of. It is how you reach everything else, which is the same reason it
sits in its own scene layer above every window. Smoked's bar is darker than its
window frames now, too: on a dark skin the taskbar is the floor.

**An outline is about what is behind it.** The ring round a button was the face
mixed toward black and nothing else -- right when a button and its background
are close, which is the whole reason it exists (BG-141: Glass's keys are eight
levels from the panel they sit on). On Beacon the taskbar's buttons are a
hundred and forty-four levels from the bar, already unmistakable, and the same
rule put a neutral grey ring round each one. Against a saturated blue that
does not read as an edge, it reads as dirt. It now slides from the shaded tone
to a shadow of the background as the two separate.

**The clock picks ink that can be read.** `bar.text` is the label on a task
button *and* the clock written straight onto the bar, and Beacon's is a
near-black because that skin's buttons are pale. Both uses are legitimate and
one role cannot answer for both. `recon_color_readable_on` keeps the skin's own
choice wherever it works and otherwise takes the ink that skin writes on its
title bar -- still the palette's answer, not black or white. A test checks the
colours the clock will actually use, in every skin that ships.

**Recon is violet.** It was grey chrome with navy titles and an oxblood accent,
which is a good skin and is not this one. Recon Core is the hub of the story
this system is named after and the hub is violet -- it is on the wallpaper, it
is the tint the author reaches for, and the one place it was not was the skin
called Recon. The greys carry a little of it, because a pure grey beside a
violet title bar reads as two decisions.

**Two tests stopped depending on a colour.** They asserted that a skin file
which mentions one role inherits the rest, and checked that by comparing
against the old Recon accent written out as hex. Making Recon violet broke two
tests that are not about Recon. They ask the default skin now.

**The Appearance page stopped drawing its own buttons.** Each row shows a
sample in that skin's own numbers, and it had its own copy of the drawing and
its own copy of the radius cap -- including the paint-out-the-corners trick
fixed everywhere else as BG-142. `recon_fill_button_radius` is the real button
with the radius passed in.

**"Edit Colours" is gone unless there are colours to edit.** A built-in skin
cannot be changed, so the button was drawn greyed out on eleven of the twelve
rows: a control that has never once worked, sitting in a row of controls that
do. "Customize Skin" beside it is how a built-in becomes one that can be
edited.

**Three wallpapers the author made, and the two skins they were made for.**
`Constellation` is a star field behind frosted panes, `Millpond` is the same
idea in daylight -- glass over still water -- and `Meadow` is flat, bright and
green. Glass now opens on Millpond and Smoked on Constellation, because a
see-through skin makes the wallpaper part of the chrome: pairing them is not
decoration, it is the rest of the skin.

They arrived at 13760x7680 -- three hundred megapixels, twenty-two megabytes
between them, for a picture nothing draws above 1024x576. Cropped to the shape
the others are and scaled down to it. A repository is not the place to keep
resolution nothing can use.

Renamed, too: the files said `Desktop_Background_Constellations` and `DUCKS`,
which is where they came from rather than what they are, and the picker shows
the name.

**The taskbar is above every window, because it is not a window.**
Everything joined one scene tree, so what sat in front of what was a running
argument that whoever raised last won -- and focusing a built-in window raised
it over the taskbar, because the code that focuses a window has no reason to
know the taskbar exists. The shell re-raised itself in the path for *client*
windows and not in the path for its own, so the fault showed on the Control
Panel and the Calculator and not on anything a client opened, which is a good
disguise for a bug.

The fix is not another raise. It is four trees under the scene root --
background, windows, chrome, system -- and a node can be raised to the top of
its own tree and no further. Where something sits is now decided once, by which
tree it was created in, and the taskbar is not in the argument at all. It is
how you reach everything else; it is a different kind of thing from a window.

The top tree is what has taken the screen: the login screen, the security box,
a modal dialog and the dimmer that goes with it. Dimming everything and leaving
the taskbar bright would say the taskbar still works, and it does not. The
tooltip is up there too, because it explains whatever is in front and is never
in the way -- it takes no clicks and leaves on its own.

**Two of the five session marks were wrong, and one of them had never been
right.** The power symbol was a closed ring with a bar across it, which reads
as *no*. It was drawn as two rings, one with the gap from 340 to 360 and one
from 0 to 20 -- and two rings do not intersect, they add: between them they
drew the whole circle. The ring can now be given a gap that crosses straight
up, written the way it is said, from 340 round to 20.

Switch user was a person and a floating notch. The cut that separates the two
figures was a rectangle placed over the front one's corner, which took the
middle out of the *back* one's shoulders. It now cuts the front figure's own
shape, two pixels fat, so what survives is a line following its head and
shoulders -- which is the thing that says one person is standing behind
another.

Both found by photographing the five buttons at five times size. Neither is
visible at the size they are drawn, and both are obvious at that one.

**The power symbol was drawn into the pack rather than left out of it.**
Nothing in a folder of firewalls and spreadsheets means *power*, and it is the
one picture that had to exist. So it is ours: the same ring-and-stem everybody
already knows, drawn at ninety-six pixels and given the pack's own alpha ramp
-- white falling from about 212 at the top to 130 at the bottom, sampled off
`stop`, which is a solid shape and shows the ramp cleanly. That ramp is what
gives those icons their lit-from-above look, and matching it is the difference
between an icon that belongs to the set and one that is merely also white.
Nothing to license, because nobody else drew it.

**Three names are left drawn, on purpose, and the reasons are in the script.**
`application`, `file-video` and `file-font`, because a wrong picture on a file
type is worse than the plain sheet. And all four caption buttons, which is a
decision about the *set*: the
pack's maximize and minimize are a glyph inside a box and a caption button is
already a box, so at twelve pixels both come out a featureless blob, while its
bare X survives -- and one soft grey icon between two solid drawn ones looks
worse than three that are merely plain.

**The avatar caps were sized for eight pictures in a folder of forty icons.**
Thirty-two pictures, and a hundred and twenty-eight entries read from the icon
folder to find them -- which is a bound on the whole folder rather than on the
pictures in it, so every ordinary icon added ate into how many account pictures
could be found. At two hundred icons neither reached. Both raised, and the scan
moved off the stack.

**On the desktop, a picture beats a silhouette.** A silhouette is the right
thing on a toolbar -- small, one colour, on chrome whose colour it should
follow -- and the wrong thing on a wallpaper. A desktop icon is drawn four
times that size on a photograph nobody chose for it, and a shape with no detail
inside it comes out as a blob: the Recycle Bin was a black bin-shaped hole on a
pale wallpaper, where the drawn one has a lid, a rim and ribs. The desktop asks
the shared set for its picture first and falls back to the skin's silhouette
when there is none, because a silhouette beats a hole.

`recon_icon_draw_shared` is how it asks. The marker lives in the cache key
rather than beside it, since the two versions of one name are two different
pictures and a cache keyed on the name alone can hold only one.

**Smoked: the dark glass that was missing.** Every see-through skin was a pale
one, which is half of an idea -- glass is a *material*, not a brightness, and a
dark desktop that wanted a translucent title bar had nothing to choose. Midnight
is dark and deliberately flat; this is the other half.

Built from Glass by turning the palette over rather than by darkening it.
Darkening a light skin gives grey text on grey chrome, because a palette's
structure is which surfaces sit above which, and that structure inverts along
with the lightness. Slightly more solid than Glass at 200 against 210: dark
chrome has less contrast with a dark wallpaper to start with, so the same
transparency reads as further gone, and the number that looks identical on the
two skins is not the same number.

It shares Glass's icons rather than having a set of its own. They are
silhouettes coloured from the skin wherever they are drawn, so one set of files
comes out dark on Glass's pale chrome and pale on Smoked's dark chrome.

**Three Control Panel pages stopped borrowing.** Accounts showed the generic
application icon, Troubleshoot the terminal's, and Display Settings the
notepad's -- which Registry also showed, so two pages wore the same picture.

**The account pictures show the chosen one large.** A grid of thirty-two-pixel
discs is a grid of coloured dots to anybody who cannot see thirty-two pixels
clearly, and choosing from it means picking one and finding out afterwards.
Asked for in those words. It shows what is *chosen* rather than what is under
the pointer -- a preview that followed the pointer would be blank whenever
nobody was moving the mouse, which is exactly when they are looking at it --
with its name beside it, because a picture somebody cannot make out is not
helped by a larger copy of itself alone.

**The Explorer toolbar asks for icons and keeps its drawings as the fallback.**
Back, forward, refresh, home, rename, delete and restore take the skin's
picture where there is one; up and new-folder keep their drawn glyphs, because
no set seems to carry an up arrow and a new folder wants a folder with
something *added* to it. Not a switch between two toolbars: an icon that exists
should be used and one that does not should not leave a hole.

**Accounts and Troubleshoot stopped borrowing.** They showed the generic
application icon and the terminal's -- the nearest things to hand rather than
pictures of what those pages are. That works until an icon set arrives with the
right picture in it and nothing can ask for it, because the name says
"application" and means "the people who may sign in". The generated set draws
the same shapes under the new names, so a skin without an icon set looks
exactly as it did.

**A silhouette on the desktop gets the ring the label already had.** A desktop
icon sits on a photograph nobody chose for it, which is the problem the label
solved a while ago -- a halo of the shadow colour, drawn only when it earns its
place against the wallpaper actually behind it. A *silhouette* has that problem
worse than the label does, being one flat colour with no internal contrast to
fall back on: the bin drawn in Glass's near-black label colour vanished on a
dark wallpaper. The measurement that decides it is now asked once and answered
for both, because two separate decisions about the same square inch of
wallpaper eventually disagree, and the way that shows is a label with a halo
above an icon without one. A picture with colours of its own gets no ring -- it
carries its own contrast, and eight offset copies of a folder would be a smear.

**The login screen is a window like any other.** Its card rounds to the skin's
window corner, carries the skin's glass where the skin asks for it, and its
account tiles highlight rounded like every other selection. It was the first
thing anybody sees and the last flat rectangle in the system -- on a skin whose
whole idea is that surfaces are not solid, the sign-in screen was solid.

The card is drawn square and its corners are kept and put back, rather than
being drawn as a shape: every stage draws into that rectangle as though it
were square, and there are a dozen of them with early exits, so restoring once
at the end is exact where threading a curve through all of them would be a
great deal of arithmetic arriving at the same four corners.

**A skin can bring its own icons, and a silhouette takes the skin's colour.**
Two mechanisms, and the rule for the first is the whole rule: a skin uses the
icons in a directory named after it. `/System/Icons/Glass/` is the Glass
skin's, no manifest and nothing to keep in step, and anything that directory
does not have falls back to the shared set -- because a skin that silently
loses icons looks like the icons are broken rather than like the skin is
incomplete.

The second is for icon sets drawn as **silhouettes**: every visible pixel
white, the whole picture in the alpha channel. Detected rather than declared,
since the pixels already say what they are. ReconOS colours them where they
are drawn, so one file is a dark glyph on a light toolbar and a light one on a
dark title bar without being two files -- and the colour comes from the skin,
which is what makes an icon set part of a theme rather than a decoration on
top of one. `recon_icon_draw_in` takes the colour; `recon_icon_draw` uses the
surface's text colour, which is right for a menu, a list or a tile, and the
places where it is not -- the desktop, the taskbar, a title bar -- say so.

**The Glass skin has an icon set.** Eighty-three files, twenty-three of them
answering names ReconOS already asks for and the rest available as glyphs for
toolbars that currently draw their own. See THIRD_PARTY.md for where they came
from.

**Highlights are a step now, not a fraction of the way to white.** A constant
fraction is not a constant effect: mixing 110 of 255 toward white lifts a light
button by nine levels and a dark one by eighty-five, so every dark skin had a
pale grey bar across the top of every button. The taskbar had its own version
of the same thing -- a hardcoded `E8E8E8` along its top edge, which is a gentle
lift on a light bar and a white stripe the width of the screen on Midnight's
`24282E`. Both are derived from the surface now. Third time this shape of fault
turned up in one session; all three were written when every skin was grey.
BG-144.

**Fill and outline are one pass, so the outline stops being diluted.** Drawn as
two blends, the outline landed on a corner pixel the fill had already
part-covered, and so came out mixed with the *face* rather than with what is
behind the control -- a pale wedge in each corner, visible wherever a light
button sits on dark chrome. A corner pixel is three things at once and is
worked out as three. BG-145.

**A control is drawn as a shape, so its corners stop being a guess.** The
rounding worked by filling a square rectangle and painting the corners back out
with a colour the caller *believed* was behind them. Wrong over a gradient
always -- a graded surface is a different colour on every row and one flat
colour cannot be all of them -- and the title bar is graded on several skins,
so every close, maximize and minimize button had a wedge of the wrong colour in
each corner. Nothing paints over the corners now, so nothing has to know what
is under them. BG-142.

**Selections and hovers round like everything else.** Around thirty plain
rectangles -- the selected file on the desktop, the chosen tile in the Control
Panel, the row under the pointer in a list, the menu entry being pointed at --
each written where it was wanted, so when the rest of the system learnt to
round, they did not. One routine owns it now, and uses the same radius a button
of that size gets. Two compositing faults surfaced doing it, both invisible
until something anti-aliased was drawn onto the desktop: blending onto a
*transparent* pixel kept the destination's alpha and so produced nothing at
all, and the source colour's own alpha was ignored, which put a bright rim
around a translucent highlight. BG-143.

**Every button has a boundary now, in its own colour.** `recon_draw_bevel`
carried a comment reading *"Fixed highlight and shadow for now; a skin would
supply these."* Nothing ever did, so every button in the system was edged in
the same two greys whatever it was painted in. Worse, a 95 bevel is a lighting
effect and reads as an edge only while the button sits between its own
highlight and the surface behind it -- true for as long as every skin was grey.
On Glass the Calculator's keys are `E8EBF5` and the panel they sit on is
`F0F2F8`, eight levels apart, so the lit half landed lighter than the
background and the top and left of every key stopped existing. Two edges out of
four. A boundary is not a lighting effect: it is a one-pixel outline in the
shaded tone, on all four sides, following the corner, derived from the button's
own colour so a skin made this afternoon gets a correct edge without naming
one. BG-141.

**The icons are drawn at four times the size, because that is where the
blockiness was.** They were 32 by 32, and `recon_draw_image` averages when it
shrinks an image and takes the nearest pixel when it grows one -- which is
right for pixel art, and wrong for an account picture the login screen draws at
seventy pixels, where every pixel of the drawing becomes a 2.25-pixel block.
Redrawing at 32 would not have helped; the fault was not in the drawing. At 128
every size ReconOS asks for is a shrink, and the averaging that already existed
handles all of them. Round things are round now: a disc drawn at 128 and
averaged down has a soft edge, where the same disc drawn at 32 has a staircase.

**And they can be improved, which they could not before.** The rule was "write
an icon if it is not already there", which keeps a real promise -- a replaced
icon stays replaced -- and quietly meant an icon *improved* here never reached
a machine that had run ReconOS once. The set was not a default, it was a
one-time imprint. Each icon now carries the generation that wrote it and a
fingerprint of what was written, and is brought up to date only when the
generation has moved on and the file is still byte-for-byte ours. `icons
refresh` does that on demand; `icons replace` overwrites the lot, its own word
because it is the one that can lose something.

**Eighteen more account pictures, and the flame became a campfire.** Eight was
not a choice, it was a shortage. The flame was described as looking odd, and
the reason is that a bare tapering blob is not a picture of anything -- so
whoever looks at it supplies the nearest thing, and the nearest thing is a
campfire. Given that everybody was going to read it as one, it should be one:
two crossed logs give the flame something to be *on*. Four of the new ones were
redrawn after being looked at rather than reasoned about -- a gear with four
short teeth is a crosshair, a symmetrical taper is a grain of rice and not a
leaf, a fourteen-row lighthouse is a spool of thread, and three arcs computed
by walking x and solving for y flatten into a palm tree. An account that had
chosen a picture since renamed follows the rename rather than silently falling
back to its initial.

**A widget layer, and it is the answer to why nothing reacted to the
pointer.** Every application in ReconOS drew its own buttons -- a fill, a
bevel, a label, a hit region and a tooltip, five or six lines at a time,
around two hundred times across this repository. Writing that out is not the
problem; what it means is. A behaviour that has to be written two hundred
times is a behaviour that gets written zero times, and so nothing anywhere
highlighted under the pointer, nothing sank when pressed, and the close button
on every window did not go red on approach the way close buttons have on every
desktop for twenty years.

`recon_widget` is the place those answers now live. An application says *this
is a button, here, called that, and pressing it means this*, and is told
nothing about bevels, corner radii, hover tints or hit regions. A control is in
one of four states -- normal, hot, active, disabled -- and the panel knows
which without the application being asked.

It carries its own version, separate from the module ABI, because the two ask
different questions: `RECON_MODULE_ABI` asks whether a module can be loaded at
all and refuses a mismatch, while `RECON_WIDGET_VERSION` asks whether its
controls will match the rest of the screen and reports one. A module a version
behind still draws working buttons; a module that will not load draws nothing.
`docs/WIDGETS.md` records what each version means.

Adopted across the system in the same release: window frames for built-in and
client windows alike, the taskbar and its Apps button and pager, the start
menu and the five buttons that lock and restart the machine, the login screen,
the question dialog, the security box, the file dialog, File Explorer,
Watchtower, Photos, Notepad, the Calendar, Mail, the Player, the browser, Help,
the Control Panel and the Calculator -- which, being a module, is also the
first thing to be checked against the version it was built for.

**Held and hovered is what counts as pressed.** Press a button, think better of
it, slide off it, let go -- and nothing happens, which is what every desktop
does and this one could not. The caption buttons act on the release now rather
than the press, which is also why they never appeared to do anything before:
the window was closed or minimized before the frame showing the button sunk had
been drawn.

**Close goes red under the pointer**, and so does anything else that throws
something away: End Task in Watchtower, Delete in the File Explorer, Remove on
a mail attachment. Under the pointer and not at rest -- a close button that is
red all the time is a close button somebody stops seeing.

**A disabled control stops answering clicks, in one place.** It keeps its
region and its tooltip, so pointing at it can still say why it is unavailable,
and it no longer lets the click fall through to whatever is behind it. Saying
`disabled` is now the whole of saying it: several applications had been
remembering a second guard in their own click handler, and one of them had
forgotten.

**Rounding is the default.** `metric.corner` went from 0 to 5 and
`metric.button-corner` from 0 to 4, and the ceiling on the button radius from 8
to 12. Zero was chosen when Beacon and Glass were the only skins that rounded
anything, which left nine of the eleven built-in skins drawing square buttons
not because anything had decided they should but because nobody had written a
number for them. Classic, Reading and Contrast now say zero out loud, each for
a reason that is written down next to it.

**Glass's six colours are reachable from the Control Panel.** They have existed
since v0.4.0 with no way to reach them but the Terminal --
`recon_tint_available` was written for "a page that should not offer a choice
that cannot be made" and nothing had ever asked it. Glass is the only skin with
`metric.tintable`, so the row appears under the skin list only when Glass is
the skin on screen.

Shown rather than named: six swatches, the one in use drawn pressed, with its
name in words beside them -- a swatch says the colour and not its name, and the
name is what the Terminal and the help use.

**The Appearance window's stale skin name is fixed, and not with a pointer.**
Renaming in the Skin Editor left the Appearance window still naming the old
skin, because they are separate windows with separate state. A pointer between
two things that can each be closed first would be a worse defect than a stale
line, so the line notices instead: `recon_theme` already counts its own
changes, and a status that names a skin is dropped when that count moves.
Dropped rather than corrected -- it was true when it was said, and there is
nothing to replace it with until somebody picks a row again.

**A fourth hit-id hazard, and this file had already written the warning.** The
click ladder is descending and every test in it is an unbounded `>=`; the
comment at the top says an id belonging to a base above the one being tested
"is answered by the wrong branch and vanishes without a trace". The tint base
is the highest in the file and the check went in near the bottom, so every tint
click set a time zone instead. Nothing failed and nothing was logged. A
photograph found it; re-reading the code I had just written did not.

**A skin can be renamed and deleted from the page it is edited on**, which
was the last thing in that editor that meant opening the file by hand. Two
buttons on a second row labelled *The skin itself*, because they act on the
skin rather than on the chosen line -- five buttons together would have made
Delete look like something that happens to `title.active`.

Renaming moves three things at once, and that is the whole of why it is one
function: the file's name, the name written inside it, and what the account
remembers. Any two of them leaves a skin that half exists -- a file whose name
and contents disagree loads under neither, and a rename the account has not
been told about works until the next sign-in and then looks undone.

It refuses a built-in, an empty name, a name with a slash (it becomes a file
name, and `../Config/system` would write over the machine's settings), a name
another skin has, and renaming a skin to what it is already called -- that last
because the work removes the old file after writing the new one, which with
both names the same would delete what it had just written. **Changing only the
capitalisation is allowed**: a skin is found without regard to case, so
`testing` and `Testing` are one skin but two files, and the old file still goes.

Deleting asks first and says both things -- that the file goes and cannot be
brought back, and, when it is the skin in use, that the default comes back with
it. Then the editor leaves editing, rather than sitting there with every button
writing to a file that is not there.

Twenty-four checks, and the two that matter -- that the old file is gone, and
that the account followed the rename -- confirmed by removing each in turn and
watching the right checks fail.

**And a second hand-assigned id collision.** The rename field took
`HIT_FIELD_BASE + 8`, which the firewall's custom-rule name already had.
Different pages, so no click could reach both, but the handler for one was
about to run for the other. There is a map of which offsets are taken beside
the definition now, so the next one is chosen by reading rather than guessing.

**A ramp can be set, not only removed.** The list already showed one as
`E8E8EC to D4DAE2`; the field is now filled with exactly that, and exactly that
can be typed back -- one text in both directions.

That round trip is what lets **one colour mean flat** with no separate control
to say so. A field holding the whole value is one somebody replaces rather than
edits part of, so typing `AABBCC` over `E8E8EC to D4DAE2` has said what the
role should be; keeping the old far end would leave a colour nobody asked for
and nobody typed. Same reasoning as `metric.buttons` reading its three names.

`Remove Ramp` stays as a one-click shortcut, but the reason it used to give --
that there was nothing to type for "no gradient" -- is no longer true, and the
comment saying so has gone.

The notation moved into `recon_theme`, where it belongs (it is the skin file's,
not the Control Panel's) and where it can be tested without a window. The parse
got strict on the way: the old one stopped at the first space and returned what
it had, so **`AABBCC junk` was accepted as `AABBCC`** with the junk silently
dropped. Twenty-one checks; the one that matters is that one colour means flat,
confirmed by making it keep the ramp and watching that check fail.

And the field's label said *"Empty leaves it alone."* It never did -- an empty
field has always been refused as not a colour. A label describing behaviour the
code does not have is worse than no label, because somebody clears the field on
the strength of it.

**The skin editor sets the four measurements -- all ten of them.** They sit
under the colours in the same list, because they are the same kind of thing: a
line in a skin file. Down walks off the last colour onto the first measurement
rather than stopping there.

Three are questions rather than amounts and one is a bit set, and the list says
so in words. `metric.buttons` reads `close maximize minimize` rather than `7`,
and typing those words back in any order sets it -- with the maximize button
disappearing from every window on the desktop as the sentence appears.
`icon-gloss`, `tintable` and `buttons-left` read `yes` or `no`.

**A number out of range is refused, not clamped.** `recon_theme_set_metric`
clamps, which is right for a file being read and wrong as the only answer a
person gets: 4000 silently becoming 48 tells somebody their number worked. The
field names the range before anything is typed, and 4000 comes back as
"'4000' is not a number from 18 to 48".

**"Use Default" takes the line out of the file** rather than writing the
default into it -- confirmed by reading the file afterwards, where
`metric.buttons` is absent rather than set to 7. A skin that says nothing
follows the default if the default moves; one that asks for that number does
not. The list marks which is which and the button greys itself on a row that is
already following the default.

The wording and the parsing went into `recon_theme` rather than the Control
Panel, for the reason `recon_expr` is not in the Calculator: it is the skin
file's own vocabulary, and there it can be tested without a window. Twenty-nine
checks, and the one that matters -- that the range is refused rather than moved
-- confirmed by making it clamp and watching two fail.

Found by photograph rather than by reading: the first version let
`close maximize minimize` run underneath the word `default`, because it had
been given the column width a colour needs and a colour is eight characters.

**The sweeps run without being remembered.** `.github/workflows/sweeps.yml`
builds and runs every suite sanitized, then again the way `scripts/package.sh`
builds, and runs the analyzer as a job of its own. The second pass is the
BG-134 lesson: a containment check passed the sanitizers, the analyzer and
every suite, then aborted on the first path it resolved in an optimised build,
because every build this project makes for itself is Debug and the one that
ships is not.

**What made it possible was fixing a claim this file has made for a year.**
Every suite was written to need no window -- that is stated all over
`CMakeLists.txt` -- but it was true of the *code* and not of the *build*:
`pkg_check_modules(WLR REQUIRED wlroots)` ran before anything else, so
configuring at all wanted wlroots 0.17.1 and no other machine could run a
single suite. `-DRECONOS_TESTS_ONLY=ON` configures the twenty-five against
stock libraries. wlroots is needed only by the compositor; two suites want
wayland-server for an event loop, which every distribution has.

**And ctest was running eleven of them.** The other fourteen -- the
malformed-input sweep, the firewall, the keyring, the error catalogue among
them -- were built and never registered. "100% tests passed, 0 tests failed out
of 11" was true, and meant less than half. All twenty-five are registered now.
That is the exact shape of green this project keeps writing tests against,
sitting in its own build file.

Rehearsed rather than pushed and watched: the machine this is written on is
ubuntu-24.04 with mbedTLS 2.28, the same as the runner image, so both
configurations were built and run before the commit. 25/25 sanitized in 45
seconds, 25/25 optimised in four.

**What the optimised pass reports.** 330 warnings from twelve distinct sites --
ten in `recon_fs.c`, one each in `recon_users.c` and `recon_theme.c`, every one
`-Wformat-truncation`, none of them visible to any build this project makes for
itself because GCC needs the optimiser's value ranges to see them. `snprintf`
truncates rather than overflows, so none is a memory fault; but a path silently
cut is the thing this system refuses everywhere else, so they are counted in
the run summary and left as work. Counted rather than gated, because failing on
twelve pre-existing warnings would have made it red on its first run, and that
is how somebody learns to ignore a red build.

**The Terminal answers questions.** `help password` used to say "No command
named 'password'" -- true, useless, and said by a system with three pages about
passwords that had just declined to mention them. It now names what the help
has, six pages at most, because a word like "file" is in most of the document
and a terminal answering with forty lines has not answered anything.

**And the answers are in a better order, everywhere.** Adding the second caller
exposed how `recon_help_search` ranked things: `password` returned six pages of
which three were release notes, and `skin` put "Files" above "How it looks".
Two signals decide it now -- a page named after the word beats one that merely
mentions it, and a subject page beats a release note. The second needed nothing
added, because `make-help.sh` already writes the two kinds as `help-NN.txt` and
`changes-NN.txt`. Release notes are ranked below rather than dropped: one about
the thing being asked about is a reasonable answer when there is nothing
better. The Start menu leads with Passwords now rather than Accounts.

**"Open with" looks inside the file now.** `recon_sniff` reads a file's first
bytes and says what format it is in -- PNG, JPEG, GIF, BMP, ICO, WAV, MP3, MP4,
zip, gzip, PDF, a program, a web page, or plain text. Evidence rather than a
guess: a file beginning with the eight bytes PNG defines is a PNG.

It does not change what `recon_props_claims` decides. Only a program knows what
it will open and nothing here asks it, so the declared extension list still
decides. What changed is what the warning says, and that was the more useful
half. It used to read "Photos does not say it opens files like 'notes.txt'. It
will probably show something that is not readable." For a PNG somebody had
named notes.txt, every word of that was wrong. It now says the file is really a
PNG image, which Photos does open, and that it is the name that is wrong rather
than the choice -- worked out by asking the extension machinery a different
question: not whether the program opens files *called* .txt, but whether it
opens files that really *are* a PNG.

The intermediate version was worse than either end and is worth recording:
bolting the new sentence onto the old one produced a warning that contradicted
itself in consecutive lines. There is one sentence now, decided in one place,
and File Explorer and the desktop share it -- they had been carrying the same
two sentences written twice, with only one copy improved.

Text and unrecognised formats produce nothing at all, deliberately. A warning
that grows a line every time is one people stop reading, and text has no
signature to find in the first place.

**And two things the suite caught that reasoning had not.** A sentence
beginning "BM" was read as a bitmap, because "BM" is the whole of that format's
signature and two ordinary letters are not a signature -- it needs the reserved
bytes and the DIB header size to corroborate it. And six bytes of 0xFF passed
as plain text, because the check allowed any byte above 127 on the grounds that
other alphabets are full of them. Right instinct, too generous: high bytes are
now allowed as valid UTF-8 *sequences*, which keeps every real alphabet and
rejects the runs of high bytes that ordinary binary is made of. 0xFF is not a
legal UTF-8 byte at all.

`recon_fs_read_head` came out of it too -- the first bytes of a file, rather
than `recon_fs_read` pulling an entire film into memory to look at eight of
them.

**How far the parameter runs is somebody's to set.** Two fields on the
grapher's button row in parametric mode, holding expressions rather than
numbers -- `2pi`, `-pi/2` and `3` all work, and the defaults are written
`-2pi` and `2pi` because that says what the range is in a way `-6.283185`
does not. Tab reaches them. The x range is no longer drawn in that mode: it
describes the view, which the plane already shows, and leaving it there got it
clipped to "x -2.50 to ...".

A bound that does not evaluate falls back to the default and says so under the
buttons, rather than quietly drawing a right-looking curve over the wrong
range. Reversed bounds are swapped without comment, because from and to
describe an interval and not a direction here.

That empties the grapher's list of named gaps: the parameter range, the file
picker and the decimal comma were the three, and all three are done.

**Every file dialog was putting a hole through the window behind it.** The
dialog dims what is behind so it reads as a question, and `dim` is a
translucent black -- but it was laid down with `recon_fill_rect`, which writes
the colour rather than blending it. So the content area got a 60%-opaque black
*pixel* instead of a dimmed form, and the compositor blended the window over
the desktop through it: the wallpaper and whatever windows were behind showed
through the middle of an opaque application.

Notepad and Mail had it too, for as long as they have had dialogs. It surfaced
because a picker was added to the Calculator and the first photograph showed
What's New through the middle of it -- and the first guess, that the new code
had broken the Calculator's drawing, was wrong. Notepad did the same thing.

`recon_fill_rect` is right to write: a Glass window frame is drawn translucent
*on purpose*, so the compositor blends the window over the wallpaper, and
making fills blend would have made the frame opaque and the skin pointless.
Making a window see-through and veiling something inside it are different
operations, so there is now a `recon_blend_rect` for the second. BG-136.

**A data file can be chosen instead of typed.** A Choose button on each of the
three rows in the grapher's data mode. It costs the path field some width
rather than costing the form a row -- a fourth row would come off the plane's
height, and the plane is what the mode is for.

Two things came out of building it. The first attempt drew the button off the
right-hand edge, on the assumption that the space parametric mode uses for its
second field was spare in data mode; an unpaired field takes the whole row, and
a photograph said so in one step. And `RECON_FILEDLG_HIT_BASE` turned out to be
sitting exactly on top of the Calculator's own hit ids -- 1500, with a comment
claiming that was four hundred and ninety more than any application used, while
the Calculator ran to 2007. Harmless only because it had no dialog to put up.
The base is 3000 now and `RECON_FILEDLG_IDS_BELOW` asserts it, so the next
application to grow past it fails to compile rather than behaving strangely.
Confirmed by putting the old value back and watching the build fail.

**A file that writes 3,14 is no longer read as the point (3, 14).** The
grapher's data mode used `strtod`, whose decimal point is a dot, always. Handed
a file written where the convention is a comma that is not a refusal and not a
mangled number -- it is a *different number*: `3,14 2,71` reads as x = 3, the
comma is skipped as a separator, y = 14. Two valid numbers, no bad line
counted, a point five hundred times too high.

Every other way that reader can fail announces itself. This one produced a
wrong picture indistinguishable from a right one, which is the failure the
whole mode was designed against.

The fix asks the file rather than the machine, which is the part worth keeping.
A machine's language setting says how the person at the keyboard writes
numbers; the file was written by somebody else. A row votes for the comma only
if it is two whitespace-separated tokens each holding one comma and no dot;
any row with a dot, or using a comma to separate, votes against; and the comma
wins only if it has votes and nothing contradicts it. Mixed files are read the
ordinary way rather than guessed at, plain integers vote for neither, and when
the comma does win the plot says so underneath rather than leaving the
interpretation invisible.

**And the reader moved out of the Calculator to be testable.**
`src/recon_data.c`, for the same reason `recon_expr` is not in there: it
touches no screen and can be asked questions with known answers. The suite is
the twenty-fourth, and its first check names the wrong answer out loud -- with
the sniff deleted on purpose it reports "first point came back as (3, 14); it
should be (3.14, 2.71)" rather than a bare mismatch. A passing test that has
never been seen to fail is not a test yet.

**Every day from 1970 to 2100, turned into a number and back again.** 47,847
of them, which takes no measurable time. The eight dates the clock suite
already checked are well chosen -- 2000 is a leap year, 2100 is not, and both
are there -- but a list of examples cannot cover the arithmetic *between* them:
a month length wrong by a day in one month of one year is invisible to it and
obvious to a walk.

Nothing was wrong, and the walk was then shown to be capable of saying so: the
century rule was removed from `recon_clock_epoch_of` on purpose, and it named
the date -- "2000-03-01 is weekday 3 after 5". **The eight examples did not
fire**, because they only test one direction, and the break was in the other.
Two functions can agree with each other and both be wrong; two functions that
disagree cannot hide it from a round trip.

The walk also counts, so a month of the wrong length that the round trip agrees
with still shows up: the weekday has to advance by exactly one, every day,
forty-seven thousand times.

**A setting with a leading space lost it at the next start** (BG-135). The
registry is `key = value` lines and the reader trims what comes after the `=`
-- which takes the format's own space, and any the value began with. The
escaping handled a backslash, a newline and a carriage return, and nothing
else.

That is the worst shape of bug this file could have: the setting works all
session and is different after a restart, so what somebody blames is the
setting rather than the store underneath it. **Trailing spaces survived**,
which is why nothing had noticed -- the half somebody would think to try is
the half that worked.

A space or tab at either end is now written as a backslash and the character,
which needs nothing added to the reader: its last case already copies whatever
follows a backslash, so files written by this version read on an older one and
the other way round. Only the first and last characters, so an ordinary value
still reads as itself when somebody opens the file, which its own header
invites them to do.

Found by a round-trip test of eighteen awkward values -- an equals sign, a
hash, quotes, tabs, high bytes, a trailing backslash -- stored and read back
once from memory and once after a restart. Four of them changed.

**The grapher plots data from a file**, which was the last of the four shapes
it was meant to ask about. Two numbers a line, separated by whitespace or a
comma, `#` and blank lines skipped; three files at once, drawn as marks with a
line through them. Both, because points alone hide how far apart two readings
are and a line alone makes measurements look like a function.

**The decision that took the time was what happens to a line that is not two
numbers.** It is counted, and the count is drawn beside the plot -- "54 points.
6 lines were not two numbers and were left out." A plot that silently drops the
rows it could not read is a picture of a data set nobody has, and it looks
exactly like a picture of the right one; so does one that stops at the first
bad line. Neither refusing nor ignoring: draw what was read and say what was
not. The four-thousand-point ceiling reports itself the same way.

Not sorted by x, deliberately: a data plot is often a path -- a position over
time, a hysteresis loop -- and sorting one draws something that never happened.

**The view fits itself to the data** when a file is read, and only then, so
panning afterwards is not undone on the next frame. A file of readings between
0 and 1000 in a window ten units wide shows nothing at all, and what somebody
concludes from that is that the mode does not work.

The first version of it drew nothing: the loop that draws curves still ran each
field through the expression grammar before reaching the data branch, and a
path is not an expression. Every file was skipped and then reported as having
no numbers in it, which is what a file that was never opened looks like from
the other end.

**Raising a fault cannot raise a fault.** Recording one writes through the
filesystem, and the filesystem is a place faults come from, so a raise can
reach a raise -- unbounded, and the last thing the machine would do is lose the
record of what went wrong. The inner one now goes to stderr and returns. A stop
deliberately does not clear the flag: nothing after it returns to ordinary
running.

**VT-B003 and VT-B004 have sites**, which the containment work gave them.
B-003 is raised when a name resolves outside the filesystem -- and
**deliberately not** when a path merely climbs out with `..`. That refusal is
an ordinary mistake in a relative path, `recon_fs_resolve` runs on every file
operation in the system, and a line per attempt is a log somebody can fill on
purpose. The signal worth keeping is the rare one, and burying it under the
common one loses both. There is a check for that: fifty refused `..` paths
write nothing.

Thirty-four of the forty-three can now happen, up from thirteen.

**A link inside the filesystem read a file outside it** (BG-133). `normalize()`
splits a path on separators, drops `.`, pops on `..` and refuses to go below the
root -- complete against anything *written* in a path, and the twenty escape
shapes in the new sweep all come back refused or land inside. It says nothing
about what the resulting name turns out to be on the host, and a symbolic link
inside the tree pointing at `/etc` normalises perfectly and lands outside.

Measured rather than argued about: the test made a link at `/Temp/way-out`
pointing at `/etc/hostname` and asked `recon_fs_read` for it. It returned
thirteen bytes of `/etc/hostname`.

ReconOS cannot make a link -- there is no command for it -- but the host root is
an ordinary directory and `cp -a` of a tree containing one brings it along;
`scripts/look.sh` copies the filesystem that way on every run. A containment
rule that holds only while nobody copies anything in is not one.

`recon_fs_resolve` now resolves the deepest part of the host path that exists
and refuses anything not under the root, which also catches a link used as a
directory halfway along. **What it costs, measured:** a resolve goes from about
150 ns to 6-10 microseconds, which in the system is 100 ms once, on the very
first start, while the whole tree is written -- 216 ms to 317 ms. Every start
after that is 92 ms either way.

**And that fix aborted every optimised build** (BG-134), which was found by
benchmarking it. `realpath` wants a `PATH_MAX` buffer and was given a
`RECON_PATH_MAX` one; with `_FORTIFY_SOURCE` on, glibc kills the process. It
was also declared implicitly, because glibc puts `realpath` behind
`_XOPEN_SOURCE` rather than `_POSIX_C_SOURCE` -- so it returned `int` and the
result was a truncated pointer. Twenty-three suites, both sanitizers and the
analyzer all said it was fine. `scripts/check.sh` now runs every suite twice,
the second time built the way a release is.

**Watchtower said "0 shown" above the applications it was showing** (BG-132).
The footer's count came from a value the *draw* sets and the line was built on
the timer, which runs first -- so the number was the previous frame's, and on
the first frame it was zero. It corrected itself a second later, which is why
it had survived: the wrong number is only up while somebody is still looking at
the window they just opened.

Found by photographing the whole desktop at the end of the night to check that
eight thousand lines of change had not broken anything visible. Nothing was
broken; this was.

**Four more codes, and one taken out again.** VT-J001 when a program will not
open -- which is silent from the outside, since something is clicked and
nothing happens, and every way in goes through the one line that now reports
it. VT-F003 when a name will not resolve, because "Mail could not connect" and
"this machine cannot look up any name" are different problems with the same
symptom. VT-F002 when a connection fails. VT-E003 when a folder is not a
package.

VT-E003 was raised in the shared manifest reader first, and `install` on a
folder that is not a package put **two identical lines** in the log for one
action -- once from the install and once from the read that builds the message
about it. Installing is the thing that failed; looking is not. Moved.

**VT-E005 was wired and then taken out.** It says "a program could not be
removed", and the only failure path in uninstalling is the receipt not being
there -- which means *nothing called this is installed*, a different thing. A
code on the wrong condition sends somebody looking up a removal that never
started, and is worse than a code nothing raises, which is the whole argument
this project makes about unreachable ones. Uninstalling is written not to fail,
so there is no site; the roadmap says so.

Thirty-two of the forty-three can now happen, up from thirteen.

**A program asking for a help page that is not there says so** (VT-M002, a new
code). The check existed and ran only when somebody pressed F1, for the window
in front -- so a program naming a page nobody wrote stayed invisible until the
one person who needed help happened to be in it.

`recon_help.h` said the check ran "at startup", which it never could and never
did: the topic is declared in an application's impl, and an impl is only
reachable once a window is being made from it. That is the earliest moment
there is, and it is where the check runs now. The header says what happens.

Nothing fires today -- every application's topic exists -- which was checked by
opening all eleven of them and then by pointing Notepad at a page that does not
exist and watching VT-M002 arrive. There were once five of these at the same
time: Web and Mail both asked for "Networking", which has never existed, and
Photos, the player and the Calendar each asked for "Writing", which exists and
is about Notepad.

Twenty-eight of the forty-three codes can now happen, up from thirteen.

**The error catalogue is checked by something other than reading it.**
`recon_error.c` was linked into eight test targets and exercised by none of
them, which coverage said and reading did not -- and eleven codes had just been
wired into the system on the strength of reading.

`include/recon_errors.def` states three rules at the top and nothing enforced
any of them. **Two of the three the compiler already enforces**, which was
found by breaking them rather than by assuming either way: the macro builds an
enumerator out of the area and the number, so a repeated pair is a
redeclaration and the build stops. That is written into the test, because a
check claiming to be load-bearing and not being is worse than no check.

What the compiler does not catch, and breaking them proved this does: an area
letter outside the listed areas -- `VT-I001` compiles perfectly and reads as
VT-1001 on a screen somebody is squinting at because the system has stopped --
and a code with an empty summary, which is a code somebody looks up and finds
blank.

Also checked: that raising one reaches the log with the detail *and* what the
code means, that a second is added rather than replacing the first, that a
fault leaves nothing for the next start to report, and the whole of A-005 --
including that a caught crash takes precedence over it, which had only been
verified by hand.

**One damaged byte in the firewall's rules file turned the firewall off**
(BG-131). The switch was read as "yes or on means yes, anything else means no",
so `on = yqs` meant off -- silently, and looking exactly like somebody having
turned it off. The two defaults had the same fault in the more dangerous
direction: an unreadable `default out` became **allow**.

Damage to that file did not make the firewall complain. It made it weaker,
quietly, in the direction nobody would choose -- which is the failure
`recon_firewall_init`'s own note calls worse than having no firewall at all.
Missing was handled; damaged was the case the sentence actually describes.

A value that is neither yes nor no now leaves the setting where the built-in
defaults put it, and **VT-H003 is raised** -- a setting was ignored, which is
what that code is for and which had no site anywhere until now. An explicit
`on = no` still works, and there is a check for that too, because the fix could
have made the switch unusable without anybody noticing.

Found by writing the first test that reads that file, which
`scripts/coverage.sh --zero` had just named as four functions nothing runs.

**`scripts/coverage.sh`, and the two things it found immediately.** A fourth
question for the tools: the warnings, the sanitizers and the analyzer look at
code, and this looks at the *tests* and says which parts of the code they never
touch.

It exists because twice in one night a test passed while testing nothing. Two
files came back at zero:

* **`recon_firewall.c` had no test suite at all** -- a security component,
  compiled into the network target as a dependency and never once called. That
  is the worst shape a gap can have: the file appears in a test target's source
  list, so it looks tested from every angle except the one that counts. It has
  a suite now, and what is tested is the *answer* rather than the bookkeeping --
  order deciding, a rule that is off not being consulted, a rule for one program
  not answering for another, the firewall being switched off allowing everything
  **and saying so**, and a rule that ships not being removable. Fifty-six
  checks. The ordering rule was then inverted on purpose to confirm the suite
  notices, and it named exactly the four checks about order.
* **`recon_titlebar.c` had none either**, and it holds the rule this project
  states most loudly: close is always there and always outermost. That had been
  proved by writing a skin that asks for no buttons and photographing the close
  button it got anyway -- one skin, needing a display. It is now proved for all
  eight sets of buttons on both sides, without a screen, in a millisecond.

**And that test found BG-130 on its first run.** A window's title had eight
pixels less room with the buttons on the right than on the left, because the
left-hand branch left a margin between the title and the buttons and the
right-hand one did not. The right is the default. A long title is clipped at
exactly that width, so the "..." finished against the button rather than short
of it. Nobody had noticed by looking, which is what eight pixels on one edge is
like.

**The coverage number is the most any single suite runs, not the union**, and
the script says so at length. gcov given every `.gcda` for a file linked into
fourteen targets reports the sums -- `recon_fs.c` came out as "21% of 10920
lines" when it has 780 -- so both columns were wrong, and the first version of
this script printed them.

**The grapher draws parametric curves**, which completes the three shapes of
question it was always meant to ask. x and y are each a function of `t`, three
curves at once, and the pair sits on one row rather than under each other -- so
parametric has three rows like the other two modes and the plane keeps its
height. Six rows over a small plane is a form with a graph attached.

Both halves are needed. A curve with only its x filled in is not drawn, because
half a pair is not a point -- and drawing the x against a y of zero would be a
line nobody asked for that looks like an answer.

The parameter runs from -2pi to 2pi, which is a choice rather than a
consequence: there is no natural range for a parameter the way there is for an
angle. It is the one that closes `(cos t, sin t)` and gives `(t, t^2)` both arms
rather than half of one.

**Polar and parametric now share their drawing**, because they were the same
problem: a path walked by a parameter, several of whose steps land in one column
of pixels and many of which land in none, and which crosses itself as a matter
of course. Only the arithmetic that turns a step into a point differs.

**`3t` means 3 times t.** So do `2pi`, `2(x+1)`, `(x+1)(x-1)` and `3sin(x)`.
The grapher's hint had to read `sin(3*t)`, which is teaching somebody the one
form that does not work.

The rule is a line: a value has just finished and the next character begins
another one. What that deliberately excludes is as much of the point:

* **`2 3` is still a mistake.** Nobody writes six that way, and letting
  implicit multiplication swallow it turns a typo somebody can see into an
  answer they cannot question.
* **`xt` is still one name.** There is exactly one variable, so it was never
  going to be a product -- and reading it as one would make every misspelled
  function name silently become a multiplication.
* **`2e3` is still two thousand, and `2e` is two times e.** Scientific notation
  and "two times e" are the same characters, and this settles it without a rule
  of its own: the number reader is greedy about exponents that are *valid*, so
  it takes all of `2e3` and stops at the `e` in `2e`. Both answers are the ones
  somebody typing them meant, and there is a test saying so, because the next
  person to read that file will wonder.

**The grapher draws polar curves.** A second mode beside "y of x": the
expression is the distance from the origin and the angle sweeps round, so
`sin(3*t)` is a three-petal rose, `1+cos(t)` a cardioid, and `t/3` a spiral --
three at once, in their own colours, like everything else this grapher draws.

A mode rather than a flag, because it is a different question and not a
different way to draw. An ordinary curve has one point per column of pixels and
cannot double back; a polar one has a point per step of angle, several of which
land in the same column and many of which land in none, and it crosses itself
as a matter of course. It keeps the one thing that matters from the other loop:
an angle where the expression has no value breaks the stroke rather than being
joined across, which is the same lie in a different coordinate system.

Four turns rather than one, because a spiral is a polar curve and `r = t` over
a single turn is a comma.

**Switching modes keeps the expressions and resets the view.** Opposite
decisions for one reason. ±10 is right for `x^2` and draws every ordinary polar
curve as a thumbnail; ±2.5 is right for a rose and shows almost nothing of a
parabola -- so carrying the view across would make the mode look broken
whichever way you switched. The text stays because seeing the same three lines
mean something else is the quickest way to understand what the mode is.

**`recon_expr` will call its variable something else**, which is what made the
above possible without a small lie. There is still exactly one variable; only
its spelling changes, so a polar field can be a function of `t` rather than of
an `x` that happens to be an angle. `x` stays accepted everywhere, and the
grapher names the other spelling `t` under *both* modes -- the first version
named it per mode, and switching back from polar left three fields reading
"'t' is not something this knows", which is true, useless, and reads as the
mode having broken what was typed.

The order matters and there is a test for it: a caller who renames the variable
to `e` gets the constant, because `e` silently ceasing to mean 2.718 inside
every expression is a far worse surprise than a rename that did not take. That
test failed first -- the code and its own header disagreed -- which is how they
came to agree.

**A package can be upgraded.** Installing over an existing one used to be
refused outright, so the only way to a newer version was remove-then-install
with whatever that loses.

Its own verb -- `upgrade` in the Terminal, and a question in File Explorer when
you install something already installed. Not an install that quietly replaces
things: this is the only operation here that has to remove something in order
to succeed, and that is worth asking for on purpose.

**Strictly newer, and the old one is kept until the new one works.** Every file
the installed version placed is moved aside, the new package is installed over
the gap, and the old files are deleted only once the install has succeeded and
the module has loaded. Anything that fails puts them back, reloads what was
there, and says so with VT-E004.

The obvious implementation is uninstall-then-install, and it is wrong in a way
worth naming: an upgrade that fails halfway that way has removed a program that
worked and put nothing in its place. Somebody who was trying to get a newer
version now has no version -- which is worse than what they started with, and
worse than being refused.

The same version is refused (remove it first, if reinstalling is what you
want), and so is an older one, because a downgrade is a different decision and
should not arrive through a button labelled "upgrade". A version that cannot be
read is refused too: this removes a working program, and "probably newer" is
not a property to do that on.

Settings the old install wrote are left alone. It only wrote them where there
was no value, so they are the ones somebody may since have changed.

**Two of File Explorer's dialogs were reading a field that had already been
cleared** (BG-129), including one written the same night and tested -- on the
desktop's copy of the same menu, which keeps its answer differently. The
feature worked on one of the two menus that are supposed to be the same menu.
Found by a truncation warning about something else.

**ReconOS can be asked to stop** (BG-128). Nothing handled SIGTERM, SIGINT or
SIGHUP, so every ordinary way of stopping a program -- a service manager, a
logout script, a terminal closing, `kill` with no arguments -- ended the
process outright and skipped every line of the teardown. The control socket
stayed in the filesystem. The keyring's key, derived from somebody's password,
was left in freed and unscrubbed memory, which is the exact case the lock at
shutdown exists for. From the inside, every deliberate stop was a crash.

It had no symptom until something was added that could see it, which is the
part worth writing down.

**And what was added is that ReconOS can now tell a crash from a power cut**
(VT-A005). A marker is written at startup and removed at a clean shutdown, so
finding one means the last run never reached its own end. The crash handler
already covered the faults that can be caught; this covers the ones that
cannot. `kill -9` now produces, on the next start, "The last run ended
unexpectedly", naming the time and version of the run that died -- and the
login screen shows it, once, the same way it shows a caught crash.

Where the two overlap, the specific one wins: a caught crash names the actual
fault, and reporting both would put two screens in front of somebody for one
event with the vaguer one second.

**Eleven error codes that were documented and unreachable now happen.**
Twenty-four of the forty-two are raised, up from thirteen.

None of them was a matter of finding the failure and adding a line. Every one
was a place where a failure was already being handled and never said:

* **The renderer was used without being checked.** A NULL one does not fail
  where it is created -- it fails four calls later inside wlroots, in a crash
  with no code on it and nothing to look up. VT-D001, and it stops.
* **A keymap that would not build was passed straight to wlroots.** That
  leaves a keyboard attached and silent, which looks exactly like a keyboard
  nobody plugged in. VT-K001.
* **A registry that would not save returned false to callers that do not
  check.** The quietest fault in the system: the change appears to take, works
  all session, and is gone at the next start -- so the setting looks broken
  rather than the disk. VT-H002.
* **A refused sign-in was recorded nowhere at all.** One is somebody
  mistyping; forty overnight is the only trace this machine would keep of
  somebody trying. VT-C003, a NOTE, with the name and never the password --
  and there is a check in the account tests for both halves of that, because
  the line could otherwise be deleted and every other test would still pass.
* Also VT-C001 (the account list), VT-C004 (a password change that rolled
  back, which is invisible precisely because the rollback worked), VT-H001,
  VT-M001 (the help, which is where the explanation of every other fault
  lives), VT-D003 (a capture fails a frame after anybody was told it started),
  VT-E002 (a module built for a different ReconOS -- "interface 3; this is 4"
  is true and means nothing without the page behind the code), and VT-G003 (a
  firewall rule that is in force now and will not be after a restart).

**A twentieth test suite, for input that is wrong.** Every other suite here
asks whether a decoder gets the right answer from a good file. This one asks
what it does with a bad one -- which is the question that matters for anything
a person can be sent.

The ICO decoder, the MP4 demuxer, the HTML parser and the expression grammar,
each fed the same three kinds of wrong: truncated at every length, one byte
changed at every position to three different values, and random bytes behind a
real header. 5,388 cases, all deterministic, because a finding that cannot be
reproduced has not been found.

A truncated file has no right answer, so "wrong answer" is not what is being
looked for. What is: a read outside the buffer, a loop that does not end, and
**an offset handed back that points outside the input** -- the same bug one
step earlier, where the decoder has not crashed but has told the media player
exactly where to.

Nothing failed. That is worth having, and it is worth less than the thing that
happened on the way to it.

**The first MP4 fixture tested nothing, and reading it would not have shown
that.** It was a header and an empty `moov`. The bounds check in
`recon_mp4_sample` was deleted on purpose to see whether the suite would
notice; it reported five thousand cases and no failures, because the fixture
never produced a sample and the check was never reached. The fixture now
carries a real sample table -- `stts`, `stsc`, `stsz`, `stco` and four samples
in an `mdat` -- and the same deletion is caught and named: "MP4 with byte 276
set to 0xFF -- a sample points outside the file".

**stb_image's implementation moved out of `main.c`** into `src/recon_stb.c`. A
single-header library needs exactly one file to define its implementation, and
that file had been the one with the entry point -- so a test target that wanted
a decoder had to link the compositor. Every test target in this project exists
to need no display.

**A third sweep, with the static analyzer** -- `scripts/analyze.sh`, after the
warnings and the sanitizers. It walks paths nothing has ever run, which is
where the other two cannot look.

Three things worth changing, none of which a test could have found:

* **Notepad cleared a struct that owns a pointer.** Correct on every path that
  reaches it, and correct for a reason two functions away that nothing stated
  at that line. It frees first now; `free(NULL)` costs nothing and the
  argument stops being needed by whoever adds a fourth way to get there.
* **Creating an account trusted a loop to find a free slot**, because the count
  said there was one -- an invariant kept by hand in five places. If it ever
  slipped this was a memset through NULL, while creating an account, which is a
  bad moment for a crash with nothing to read afterwards. It says what is wrong
  instead.
* **Three test allocations never checked their malloc.** A test binary that
  segfaults reports nothing at all -- not a failure, not which check it died
  on. The sanitized build allocates several times what the ordinary one does,
  and it is the sanitized build that runs before a release.

The two findings that are not real -- a file descriptor handed to the Wayland
event loop, which closes it somewhere the analysis cannot see -- are listed by
file and line in the script rather than turned off with a flag. A tool whose
output nobody reads has stopped being a tool, and the way that happens is one
harmless entry at a time.

**The Apps menu's search box finds help pages, not only programs.**

Type a word and matching pages appear under the matching programs, marked
"Help:". Clicking one opens Help at that page. Under the programs and never
instead of them -- somebody typing "mail" wants Mail, and four pages about mail
above it would have made the search worse at what it was already good at.

Four at most, because the menu is a fixed height and a word like "file" is in
most of the document; a menu that grew to fifty-one rows has stopped being one.
The rest is what opening Help is for.

The search is `recon_help_search`, which reads the pages off disk and needs no
window -- so the next place that wants to answer a question rather than list
commands costs a caller rather than a subsystem.

**Choosing a program that does not open that kind of file asks first.**

"Open with" offers every program that reads files, which is deliberate --
narrowing it to the ones claiming the extension would leave a menu with one
entry and nothing to choose between. So the list stays wide and the warning
does the work: pick Notepad for a `.png` and it says so, names both ends, and
says that the choice applies to every file of that kind rather than this one.
It is a warning and not a refusal, because opening a picture in Notepad to look
at its header is a real reason to do it.

Both menus, the desktop's and File Explorer's, because they mean the same thing
and two answers to one question is two places for them to disagree. The check
itself is `recon_props_claims`, in `recon_props` where the rest of "what is
this file and what opens it" lives.

**A dialog long enough to fill itself read a line that was never written**
(BG-127). `dialog_wrap` returned one more than it had written whenever the
message overflowed, and the drawing read a row past the end of the array. The
warning above is the first message in the system long enough to reach it. The
count is clamped, the ceiling is ten rather than six, and -- the part that
matters more than the number -- **the last line now ends in "..." when there
was more**, so a dialog that is short and a dialog that lost its point look
different. That is the same fault as the ceilings swept earlier in this
release, in the one place that had been missed.

**Passwords can be kept, and there is a page that shows what is kept.**

Mail's sign-in screen now offers "Remember this password". Ticking it and
connecting successfully puts the password in a keyring; opening Mail after that
fills the field in and leaves a Connect button.

What is behind the tick box is the point. The password is encrypted with
AES-256-GCM under a key derived from your account password with PBKDF2 at
sign-in. **That key is never written anywhere.** Signing out, locking the
screen and shutting down all erase it, and everything kept becomes unreadable
until somebody signs in again. A copy of the disk is a copy of ciphertext.

Three details that are not decoration:

* **The keyring's salt is not the login salt.** The accounts file already
  stores PBKDF2(password, login_salt) so a password can be checked; deriving
  the key with the same salt would make that stored value *be* the key, and
  anybody who could read the accounts file could read every secret without
  knowing the password. The keyring hashes a tag of its own with the login salt
  first, so the two derivations are independent.
* **A fresh random nonce for every write.** GCM with a repeated nonce under one
  key is not weakened, it is broken. There is a test whose whole job is to
  write the same secret twice and check the two files differ.
* **The entry's name is authenticated.** A ciphertext cannot be moved from one
  name to another, so somebody who could write the file could not slide the
  mail password onto a name a different program reads.

**Control Panel -> Passwords** lists what is kept -- names and which program
asked, never values -- and forgets one. `keyring` does the same from the
Terminal. There is no call, no command and no page that prints a secret back,
which is deliberate: no window shows one, so anything that did would be the
only way in the system to get a password out in plain text.

**What it does not do, said plainly.** It does not protect against somebody who
is signed in. The key is in this process's memory while the session is
unlocked, and every module is loaded into this process, so any module can ask
for any secret. There are no separate address spaces to hide a key in without a
kernel. It protects a stolen disk and a stolen backup, and `recon_keyring.h`
says so rather than implying more.

**Text that has to explain something now wraps instead of being cut off.**
`recon_draw_paragraph` draws across as many lines as it needs and returns its
height. `recon_draw_text` clips with an ellipsis, which is right for a window
title in a taskbar button and wrong for a sentence -- the Date and Time page
had an explanation reading "Its own co...", and a note beside it asking for
exactly this. Two pages use it so far; the rest can move as they are touched.

**The look harness can sign in with a real password.** `scripts/look.sh
--password X` sets a known password on every account in its throwaway copy of
the filesystem and types it at the login screen, instead of clearing the
password as it always has. Without it none of the above could be photographed:
an account with no password has no keyring, so every screen that offers to keep
a secret correctly hides the offer, and the harness could only ever see the
version where nothing is there.

**A skin can replace the window buttons' glyphs with pictures.** Put a file at
`window-close`, `window-maximize`, `window-restore` or `window-minimize` in
`/System/Icons` and it is drawn instead of the shape.

There is none by default and none is written, so almost every system draws the
rectangles it always has -- which is right: these have to be there before a
font has loaded and before any file has been read, and a title bar that could
fail to have a close button on it is not a title bar.

The file has to exist before it is used, so a skin that supplies one of the
three gets the drawn version of the other two. It cannot produce a missing
button, only a differently drawn one. Both frames use it -- the built-in and
the client's -- because a skin whose close button is a dot everywhere except on
client windows is exactly the near-miss `recon_decor`'s own header warns
against.

Now that a package can place files, a skin that changes them is a package.

**Control Panel → Programs → File Types** lists the kinds of file somebody has
chosen a program for, and "Use the usual program again" undoes one. An
association set months ago was visible only by right-clicking a file of that
kind and looking for the mark -- a setting you cannot find is a setting you
cannot undo.

On the Programs page rather than a page of its own, because it is about which
program opens what and that is the page about programs.

One round went into a constant standing for two slightly different things. The
registry's listing takes a prefix and checks that what follows it is a
separator or the end, so `"open-with/"` matches nothing -- the character after
it is the dot of the extension. The slash belongs to the key being built, not
to the name of the section. The page listed nothing while the keys were plainly
in the file.

**The compositor was run under the leak checker for the first time**, rather
than only the test suites -- which cover what can be tested without a screen,
and a panel is not one of those things.

`recon_shell_create` makes nine panels and four timers. `recon_shell_destroy`
freed six and three. The tooltip, the All Programs list, the screen blanker and
the slide timer were missed, which is what a list of nine calls that has to
match a list of nine calls somewhere else invites.

**The timer is the serious one and it is not a leak.** A timer left on the
event loop still points at a shell that has been freed, and `on_slide_tick`
dereferences it on its first line. The shell is destroyed by
`services restart Shell` as well as at shutdown, so restarting the shell while
a window was sliding would have written into freed memory. Nobody has hit it
because a restart takes a deliberate command and a slide lasts a quarter of a
second -- the kind of window that stays open for years and then closes on
somebody once.

The checker's total for a session that opens every application and shuts down
cleanly went from **8,097,016 bytes to 4,245,584**, and the only ReconOS frame
left is `wlr_renderer_autocreate`.

Worth recording alongside: **resident memory was flat across thirty shell
restarts both before and after the fix.** The allocator does not hand the pages
back, so this class of bug is invisible from outside the process, and measuring
RSS -- the obvious thing to reach for -- would have said there was nothing
wrong. A checker that looks inside is the only thing that finds it. BG-126.

**A graph can be saved as a picture.** A Save button writes the plane into
Pictures as a PNG, named for the moment it was taken -- a grapher that saved
over `graph.png` would lose the one somebody kept.

The pixels are the ones the window last drew, cropped to the plane, rather than
a second drawing of the same thing into a buffer. A second drawing is a second
copy of every rule about where a curve goes, and the two would drift: the saved
picture would stop being a picture of what was on screen, which is the one
thing it has to be. `recon_panel_read` is new and does exactly that -- a
rectangle of a panel, refusing rather than clamping when the rectangle is not
inside it, because a smaller picture than was asked for is a bug that looks
like a feature.

The save happens at the end of a draw rather than at the click, because a click
has no panel to read from -- and because it makes the picture one of the frame
the person was looking at when they pressed the button.

**A sweep for the whole class: every fixed ceiling, and what it does when it
is reached.** Three of them were dropping data and saying nothing, which is the
same failure three times in three subsystems:

- The help's page cap, at 512 lines, which was hiding two thirds of the change
  log. BG-123.
- The web viewer's six ceilings, which are *correct* -- a page is somebody
  else's file -- and were quiet about it. BG-124.
- File Explorer's folder listing, at 512 entries, which said "512 items" about
  a folder of six hundred. The number that did not fit was already known and
  was being clamped away on the line that knew it. BG-125.

And two caps added earlier the same night had the same fault: a package
manifest silently dropped a seventeenth `place` line. Those refuse the install
and name the limit now, because installing most of a package and reporting
success is how a missing file turns up as something not working weeks later.

The rule this leaves behind: a ceiling is often the right answer, and being
quiet about reaching one never is.

**And the web viewer stopped cutting pages off in silence.** Found by sweeping
for the shape BG-123 had: a fixed ceiling that drops data quietly.
`recon_html.c` has six of them, and unlike the help's they are *right* -- a
page is somebody else's file and can be any size, and a reader that grows to
fit whatever it is handed is one a hostile page can exhaust. The comment beside
them already said as much.

What was wrong was the quiet. A page cut off at four thousand blocks looks
exactly like a page that ended, and nobody scrolls to the bottom of a document
to check whether it finished. Every ceiling records being reached now, and the
status line says the rest is not shown. BG-124.

**The help search finds the word inside the page, not only the page.** Typing
narrows the topic list, and the page that opens is scrolled to where the word
actually is, with every line carrying it marked. A search that filters forty
topics to one and then shows the top of it has done half the job: on a long
page the word may be nowhere on screen, and the answer looks like the search
was wrong.

That found **BG-123**, which is worse than the feature is good. A page held
`char lines[512][200]` and every loop filling it stopped at 512. **One version's
change log is about 73 KB**, which wraps to well over a thousand lines — so the
bottom two thirds of it could not be reached by scrolling, the "N more lines
below" note counted only as far as the cap, and nothing said so. Anybody who has
opened the change log has been reading a third of it.

It also cost a hundred kilobytes per page whichever page it was, twice over, for
a two-line topic and the whole change log alike. The lines are allocated and
grown as they fill now: no cap, and a short page costs what a short page costs.

Nobody scrolls to the bottom of a change log to check it ends where it should —
the failure looks exactly like the document being that long. It took a feature
that *jumps* to a place, and then did not, for the missing part to become
visible.

**Three startup failures report the code that was written for them.** The error
catalogue defines forty-two codes and, counted for the first time, thirty-two
of them were raised by nothing. Some of that is deliberate -- a code is never
reused, so reserving one ahead is fine -- but three were describing failures the
system genuinely hits and reported as log lines instead:

- **VT-A001**, no usable font. `recon_shell_create`'s return was not checked at
  all, so a machine with no font carried on with a NULL shell and fell over
  further in, complaining about whatever dereferenced it first.
- **VT-A003**, no display to draw on, for a backend that will not start.
- **VT-A004**, the Wayland socket could not be created.

A log line saying "failed to create Wayland socket" is true and is not
something anybody can look up. Forced the failure by making XDG_RUNTIME_DIR
unwritable and watched `VT-A004` come out with the catalogue's own description
of the three usual causes.

Twenty-nine codes are still raised by nothing, which is now a counted number
rather than an unexamined one.

**The help was audited against the system.** Everything it claims that can be
checked from outside was checked by driving the desktop and looking: the five
power buttons are named as the help names them, Alt+Tab moves the focus,
Ctrl+Alt+Del opens the security box, Print Screen writes into Pictures, all
thirteen Terminal commands it lists exist, Alt+1..4 switches desktops and
Shift+Alt+N brings the window along, and every one of Notepad's eight editing
shortcuts does what the Writing page says — Ctrl+Z really does undo a word at a
time, and Ctrl+H really is the same bar with a second field.

Two things came out of it:

**The Desktops page said "There are four."** It has been possible to turn them
down to one from the taskbar since v0.3, and the page did not mention it — nor
that turning them off brings every window on the other three to the one that is
left, sliding in from where it was. Written down now.

**BG-122**, found in the photograph taken to prove a *different* sentence true.
Notepad's Replace bar drew a caret in both its fields, and so did the Control
Panel's Add Account form and its Registry key-and-value form. Each has two
flags for one fact — a `..._focused` boolean deciding where keys go, and
`recon_edit.active` deciding where the caret is drawn — and only the first was
kept up to date. BG-118 fixed the drawing, which was enough for every window
with one field and could not be enough for one with two.

The order caught me: the first attempt set the focus *before* starting the two
fields, and `recon_edit_begin` sets `active` — which is how a single-field
window gets a caret without asking. The photograph after the fix looked
identical to the one before it, and each line read correctly on its own.

**A package can ship a file and a setting, and can be nothing but files.** Two
manifest lines, each repeatable:

    place   = aurora.png /System/Wallpapers
    setting = notes/wrap on

**Where a package may write is an allow-list, not a check on what is
forbidden.** Icons, wallpapers, themes, fonts, sounds -- the directories that
exist to hold content -- and nowhere else. Not `/System/Config`, which holds the
accounts file. Not `/System/Modules`, which is loaded at startup. Not `/Apps`,
because a package that could drop a second thing into the directory scanned at
startup could bring code it did not declare.

The shape matters more than the list: a list of *forbidden* places is one
somebody has to keep complete, and the day it is missing an entry is the day a
package writes there. A list of permitted places is wrong in the safe
direction -- a package that wanted somewhere new fails to install, and somebody
reads the comment.

**A setting is a default, not an override.** A key that already has a value is
somebody's choice, and an install that overwrote it would be an install
rearranging their desk. Only what an install actually wrote goes in the
receipt, so removal takes back exactly that.

**And a package no longer has to bring code.** It required a `module`, which
made a package the wrapper for a program and nothing else -- so a wallpaper
pack or a set of skins had no way to be one, even though placing files is
exactly what they are for. The rule now is that a package must bring
*something*: code, or at least one file. A manifest with neither describes
something that would do nothing on installing and nothing on removal.

Checked against the case that matters, which is two packages naming the same
file. The second claims nothing, because what it named was already there;
removing it leaves the first package's file and setting alone; removing the
first takes back exactly its own; and the wallpaper that shipped with ReconOS
is untouched throughout. A package that tried to place a file into
`/System/Config` was refused with the directory named.

**A skin can move the title bar's buttons, and ask for fewer of them.** Two
metrics: `metric.buttons-left` puts them on the left, and `metric.buttons` is a
sum -- 1 close, 2 maximize, 4 minimize -- so a skin can have just a close
button, or close and minimize, and the icon and title take back the room the
missing ones would have used rather than leaving a hole.

**Close cannot be taken away.** The metric's range refuses zero, and
`recon_titlebar` puts the close bit back regardless of what arrives -- both,
because a range is a promise about what a skin *file* may say and the other is
a promise about what is *drawn*, and only the second is a guarantee. A skin is a
file somebody downloads; a window nobody can close by mouse is not a look
anybody chose. Checked by writing a skin that asks for zero buttons and
photographing the close button it got anyway.

Close is outermost on whichever side they are -- nearest the right edge on the
right, nearest the left edge on the left. The button somebody reaches for
without looking is the one in the corner, and a layout that put maximize there
would close windows by accident from the other direction.

**The layout is worked out in one place now, for both frames.** ReconOS draws
two title bars -- one around its own windows and one around a client's -- and
each worked out its own. They used different insets and different gaps, so a
client window's buttons sat two pixels from a built-in window's. `recon_decor`'s
own header argues against exactly that: *"a client window that looked nearly
like a ReconOS window would be worse than one that plainly does not; near-misses
read as a fault rather than as a difference."* Both ask `recon_titlebar` now,
which is worth more than the two pixels -- it means a skin that moves the
buttons moves them on every window, not on the windows whoever made the change
remembered to look at.

**`scripts/check.sh` runs everything under the sanitizers.** Address and
undefined behaviour, on all eighteen suites -- reading past an array, a double
free, use after free, a shift wider than a word, signed overflow, a misaligned
load. Every one of those is a bug that passes on the machine it was written on
and fails somewhere else.

All eighteen were clean the first time, which is worth saying precisely because
it makes the next thing the script says worth believing.

**The compiler was never asked what it thought.** There were no warning flags
in `CMakeLists.txt` at all -- not through v0.1, not through v0.4 -- so the whole
project had been building at gcc's default level, which says almost nothing.
Switching on `-Wall -Wextra` for the first time turned up four real things in a
few minutes:

- **Two mail header chains written as `a() || b() || c();`** -- an expression
  statement whose value is discarded. It works, and it is exactly the shape a
  real mistake takes, and the compiler cannot tell the two apart. Said as an
  `if` now.
- **A click compared across two different enums.** `event->state` is wlroots'
  `enum wlr_button_state` and it was being tested against Wayland's
  `WL_POINTER_BUTTON_STATE_RELEASED`. They share values today and are under no
  obligation to tomorrow -- the day they stopped, every click in the system
  would be read as the wrong half of a press.
- **Two switches over an enum that had quietly stopped covering it**, so they
  had stopped being able to warn about the next value anybody added. Both now
  name the values handled elsewhere, so the checking continues.
- **Six functions nothing called.** One was `cycle_focus`, the old Alt+Tab,
  left behind when `recon_shell_cycle_windows` replaced it -- dead code that
  looks like a working feature is the kind that costs somebody an hour. The
  others were thin wrappers and an orphaned declaration. One of them,
  `are_twins`, went with a table nothing read: the OCR built a bitset of every
  confusable *pair* at O(n^2) mask comparisons and only ever consulted the
  derived "has any twin at all". The finer question is a real one and is named
  in the code where the answer is still being computed and thrown away.

None of those was going to be found by reading. The build is clean at
`-Wall -Wextra` now, with `-Wunused-parameter` deliberately off -- a callback
signature is fixed by whoever calls it, so a handler ignoring an argument is
the normal case, and twenty-two of those would bury the next real one.

`third_party` is included as a system directory, which is what makes the rest
readable: `stb_truetype.h` alone contributed ninety-odd "defined but not used"
warnings, because a header-only library is mostly functions a program does not
call. The six real ones were hidden in that ninety, which is the whole argument
for the distinction.

**A file type can be chosen, not only inherited.** Right-clicking a file offers
"Open with" for every application that opens files, with the one that would
open it marked. Choosing one remembers the choice for that extension and opens
the file with it -- both, because choosing a program from a menu means "this
one, and from now on", and an Open-with that opened once and changed nothing
would have to be used every single time, which is the thing somebody reached
for the menu to stop doing.

It is offered in File Explorer as well as on the desktop, because the same file
right-clicked in two places should offer the same things. That needed one thing
an application could not do before: mark the entry that is in force. The shell's
own menus have had marks since the clock grew a twelve-hour and a twenty-four-
hour entry; an application's menu could not say it, so a menu an application
built that offered several answers to one question had no way to show which
answer was current.

A choice is a *user's*, so it lives in their settings and beats everything an
application declares -- it is the only rule in `recon_props_opener` that came
from a person rather than from a deduction. "Use the usual program" appears
only when there is a choice to undo, and clears it rather than setting a
different one, which is the same rule the presets follow everywhere else: what
shipped cannot be deleted, what you added can.

The key is the extension lowercased, so choosing a program for a photograph
applies to the next one even if the camera shouts its file names. A chosen
application that has since been uninstalled falls back to the declared answer
rather than leaving a file type pointing at nothing.

Menus hold twenty-four entries now rather than sixteen, and an overflow
complains in the log. The "Open with" list is as long as the number of
applications that open files, which grows every time somebody installs a
module -- a menu that silently stopped at its limit would lose Properties, and
nothing on screen says a menu is short.

**A client window gets its own icon.** A Wayland client hands its compositor an
`app_id` and never a picture, so every client window wore the same generic
icon -- honest, and also a taskbar where six different programs look identical.

The note this replaces said that guessing an icon from a reverse-DNS string
would be wrong more often than right, and that is true of *guessing*. What is
done instead has three rules and every answer has to be confirmed against
something that exists: a mapping written down in `/System/Config/app-icons`, an
application ReconOS has registered under that name, or the last dotted part of
the app_id **if an icon by that name is actually there**. The third looks like
guessing and is not -- it cannot produce a wrong picture, only a right one or
none, because the file has to exist before it is used. The failure mode is "no
better than before", which is the only failure mode worth having.

The written-down mapping comes first, because it is the one rule that is a
statement rather than a deduction: it is how to say "no, this client is not the
thing its name reduces to". It is created once with the format in it and never
overwritten, unlike the help pages -- a line added by hand that the system
replaced on the next boot would be a file nobody edits twice.

Checked by handing the compositor three app_ids and looking at what it drew:
one taken from the mapping file, one resolved to a registered application, and
one that reduces to nothing, which correctly kept the generic icon.

**The grapher draws three curves and can be moved about.** A grapher exists to
compare -- "is x^2 above or below 2^x" is the question, and answering it by
typing one, looking, typing the other and remembering is not answering it. Three
fields, each with a swatch in its curve's colour, so the picture says which
line is where and the swatch says which field drew it.

The colours are the skin's own readout accent with its hue rotated a third and
two thirds of the way round, which keeps the skin's character and keeps the
*lightness* -- the half of a colour that decides whether it can be seen at all.
Three fixed colours would be legible on some skins and invisible on others.

**The wheel zooms about the pointer, and the expressions are remembered.**
Zooming about the middle instead means finding a feature, dragging it to the
centre, zooming, and finding it has moved again -- the arithmetic that keeps
the value under the pointer where it is is one line, and it is the difference
between a grapher somebody explores with and one they fight. The wheel and the
buttons use the same factor, so one notch out undoes one notch in exactly.

The three expressions are written to the user's settings as they are typed
rather than when the window closes -- a window that saves on close loses
everything if it is never closed politely, and the whole point of keeping these
is that somebody spent a minute getting one right.

**And the view can be dragged.** It was always centred on the origin, which
makes a grapher that can only look at one place: every interesting part of
log(x) is to the right of it, and zooming in on something near x=10 moves it
off the screen. The plane comes with the pointer the way a map does, the centre
at the moment the drag began is remembered rather than accumulated so the point
under the pointer stays under it, and Reset puts back the place as well as the
span.

The grid is drawn outward from where zero actually is and clipped, rather than
counted from the middle of the box. Counted from the middle it would slide half
a line at a time as the plane moved under it, which looks like the grid being
wrong rather than like the view moving.

BG-121 is the hour that went into the first version of the drag: `motion`
changed the view and did not ask for a redraw, so the numbers moved, the state
was right, and the picture went on showing where it used to be. It looked
exactly like the drag not being delivered. Two `fprintf`s settled it in one
run.

**Help can be searched.** A box above the topic list, filtering on titles and
on the text of every page. Both, because the change log's topics are titled
with version numbers -- a title-only search would find nothing in two thirds of
the document -- and because somebody typing the name of a page they already
know they want should get it.

The corpus is about a hundred kilobytes across forty-odd files, read once and
kept, because a search box that reads forty files per letter typed is a search
box that feels broken. Ctrl+F puts the caret in it, the same key that finds
text in Notepad. Escape empties it and gives the whole list back. Typing while
a page is open does not move off that page unless the search filters it away,
so looking something up does not lose the place of somebody who was reading.

**And F1 now opens the page it says it will.** The help has been promising that
it opens "at the page about whatever you are looking at", and it kept that
promise for four applications out of ten. Web and Mail both asked for a page
called "Networking" that has never existed, so F1 from either left whatever was
already showing; Photos, the player and the Calendar each asked for "Writing",
which exists and is about Notepad -- which is worse, because it looks like an
answer.

Seven pages written that did not exist, every application pointed at its own,
and a topic that does not exist is now said out loud in the log and falls back
to the beginning rather than being passed over quietly. The name is declared
beside the application and the pages are written somewhere else, and nothing
made the two agree; this is what makes the disagreement visible. BG-120.

**An application says what it opens, and a module can say it too.** The answer
to "what opens a .png?" was a list of extensions inside one function in the
system -- a long way from the application that opens one, and with no way for a
module to answer it at all. A module could bring an application that reads a
format, and the file would sit on the desktop correctly named and correctly
drawn, and double-clicking it would do nothing.

Registering an application and registering what it opens are now the same act.
A module's claim beats the system's own default, because somebody who installs
a picture editor and finds pictures still opening in the viewer has installed
something that does not work. Turning an application off in the Control Panel
hands its file types straight back -- an "off" that leaves the associations
behind is not off -- and every application's row now says what it opens, because
"this program will now open your pictures" is a consequence of installing
something rather than a detail.

The one association that stays dynamic is the player's: what it opens is
whatever can be decoded, and that changes when a module brings a decoder.
Written down once it would be a list that is wrong from the moment somebody
installs anything.

**Every built-in lost its version number to that change, for about an hour.**
The field went into the middle of a public struct and the twelve built-in
registrations were positional, so each version string slid one place and
`version` became NULL. It compiled without a warning; the only sign anywhere
was a dash in one column of `apps`. The version is the number the whole
applet-update decision rests on. The table uses named fields now. BG-119.

**Mail can attach files.** Up to eight, twelve megabytes together, chosen with
the same file picker Notepad uses. The message becomes multipart/mixed; without
an attachment it is written exactly as it always was, one plain part with no
boundary anywhere in it -- which is the point rather than an optimisation,
because every mail reader in the world handles the simple shape better than the
complicated one.

**The boundary is checked, not assumed.** This is the one genuinely dangerous
part of multipart and it is dangerous quietly: a boundary that also occurs
inside the message splits it there instead, so the letter arrives cut in half
-- or somebody who can put text in the body ends the message early and appends
parts of their own. The usual answer is a long random string and the argument
that a collision is unlikely, which is fine against accident and worthless
against somebody who has read the source. So the string is built, searched for
in everything it will separate, and rebuilt with a different number if it is
found. Certain rather than probable, for the cost of one pass.

A filename is a header value twice over, so it can carry the same attack a
subject can and one more: it is written inside quotes, and a quote in it ends
the value early. Line breaks, quotes and backslashes are refused.

The files are read at Send, not when they are chosen. A file picked at nine and
sent at eleven should be the file as it is at eleven -- and reading it then is
the only way to notice it has been deleted since, which is a thing to say out
loud rather than to send an empty part about.

**Mail takes Cc and Bcc.** Three address fields, each a comma-separated list,
and one rule that is the whole reason there are three of them rather than one:
**every address reaches the server, and only To and Cc reach the message.**

Getting that backwards is the failure that has embarrassed real mail software
repeatedly. The letter still sends, still arrives, and quietly hands every
recipient the list of people who were meant to be hidden -- and nothing about
it is visible from the sender's side. So the guarantee is built as something
`recon_smtp_compose` *cannot* do rather than something it remembers not to:
there is one function that writes an address header, it is handed the name to
write, and Bcc never calls it.

A letter addressed only in Cc, or only in Bcc, is a real letter and is sent --
with no To header at all, because there is honestly nobody to put in it. A bad
address anywhere in any of the three lists stops the whole send and the message
says which one: a letter that reached four of five people and reported success
is worse than one that failed, because nobody goes looking for the fifth.

**`state` says who has the input.** The login screen takes every pointer event
while it is up, which is correct -- but `apps <name>` would still open a window
and `state` would still call it open and focused. It is neither, and that cost
an afternoon of measuring a Calculator whose buttons were fine. `session` knew
all along, which was not enough: it has to be said by the command somebody
actually runs. BG-117.

**One caret, in the field being typed into.** Every text field drew one at
once, and every field with a default value drew it selected -- so the Mail
setup screen showed six carets and two highlights, none of which meant
anything. `struct recon_edit` has carried the flag that answers this the whole
time and the drawing never read it. BG-118.

## v0.4.0

**Why this is 0.4.0 and not 0.3.1.** It was numbered 0.3.1 for seventy commits
and seventy-six entries in this file, and a patch release is not that. The
number is supposed to say what changed: sound, video, a codec registry, a
theme protocol for clients that are not part of this program, mail that can
send, an expression grammar with its own tests, and a filesystem call that
creates a private file rather than tightening one afterwards. None of those is
a fix to 0.3.0 — each is a thing 0.3.0 could not do at all.

Noticed by the user, not by the project, which is the part worth writing down:
nothing here counts commits, so "in progress" stayed true for as long as
somebody kept typing under it.

**The Calculator opens at a size it can be used at.** Six mode tabs on one
row, each sized to its own label; a keypad whose columns divide the width
exactly rather than throwing the remainder away; labels centred by the line's
own height instead of by a constant that only worked at one font size; and
keys drawn with the button edge, so they round with the skin and read as forty
buttons rather than one slab with lines scored in it.

The window opened at 430 wide with a minimum of 520 — a size the resize code
would refuse to let anybody choose. That is fixed in `recon_appwin_create`
rather than in the Calculator: an opening size below the minimum is raised to
it, for every application, because it is a mistake any of them can make and
none of them can see. See BG-116.

**`scripts/look.sh` photographs the desktop from a script.** Every "does it
look right?" question in this project has been settled by a picture, and
getting to one takes four steps that are not obvious — two of which cost an
afternoon each. The login screen takes the whole screen's pointer input, so a
window opened while it is up reports itself as focused and cannot be clicked;
and `capture` resolves its path inside the ReconOS filesystem rather than the
host's, so an absolute host path reports success and writes nowhere useful.
The harness signs in first, to a copy of the filesystem with the password
removed, and refuses to continue if it did not reach the desktop.


**ReconOS makes a sound.** `recon_audio` is a new boundary of the same kind as
the filesystem and the network: one file knows how sound reaches hardware and
nothing above it does. It speaks ALSA rather than a sound server, deliberately
— ALSA is the lowest thing on Linux that is still an interface, so the header's
shape is close to the shape a real driver has, and replacing it when there is a
ReconOS driver is replacing a file rather than rethinking an idea.

It is **pulled, not pushed**: the device asks for samples rather than being
handed them. A device runs at its own rate and wants a fixed number of frames
at fixed moments, and an interface where the caller decides when to write makes
the caller responsible for a clock it does not own.

Without a sound library at build time, or a sound card at run time, every
function is still there and says there is no audio. That is the same code a
machine with no card takes, which is the path that otherwise gets tested least.

**Codecs are a registry, and the registry is the deliverable.** A decoder says
which extensions it handles and how to recognise its files, and everything
above asks rather than knowing a list — so a module can bring a decoder and
nothing in the player changes. Contents are checked before the name: a file's
bytes are what it is, and its name is what somebody called it.

ReconOS ships the ones it can honestly write. **WAV** is written here, because
it is a header and then the samples: 8, 16, 24 and 32-bit, integer and float,
mono and stereo. **MP3** is minimp3, which is a format parser and therefore on
the permitted side of `THIRD_PARTY.md`'s line. Writing an MP3 decoder here was
considered and rejected — the format is a hundred pages of psychoacoustics, and
one that is ninety-five per cent correct does not sound nearly right, it sounds
broken.

Every decoder is checked against ffmpeg, sample for sample, on real music
rather than a tone. All four WAV widths match **exactly**; MP3 is off by at
most one across eight million frames, which is rounding. That comparison found
a real fault: the float conversion scaled by 32767 instead of 32768 and
truncated instead of rounding, so every sample came out fractionally quiet and
biased towards silence.

**MP4 files play their sound**, and the split that makes that work is the
answer to what a codec pack is here.

ReconOS demuxes the container itself. An MP4 is a tree of boxes and three
run-length tables saying where each compressed piece lives — that is structure,
and structure is what this system writes for itself. Verified by pulling every
audio frame out of a real video and comparing it byte for byte with the frames
ffmpeg extracts from the same file: **all 3307 of them match exactly.**

The *decoding* is a module. `CodecPack.rts` wraps libavcodec to register an AAC
decoder, and it is genuinely optional — built only where libavcodec is present,
removable afterwards, and the desktop plays WAV and MP3 without it. A codec
pack that cannot be taken off again is not a pack, it is a decision somebody
made for you.

Which means a file that cannot be played says **which decoder is missing**:
*"that file holds H.264 video and AAC sound. There is no AAC decoder installed,
and nothing here shows video yet."* That sentence is the entire reason the two
halves are separate, and it was wrong until it was tested — `open()` returned
NULL for three unrelated reasons with no way to tell them apart, so the player
guessed, and guessed "or it is damaged" about a perfectly good file.

**Video plays.** H.264 and H.265 decode through the codec pack; everything
around them is ReconOS's own, and the split is deliberate rather than
convenient. libavcodec is handed compressed bytes and gives back three planes.
It is not asked to demux, to convert colour, to scale, or to decide when a
picture should be shown -- it can do all four, and all four are structure or
arithmetic, which are the things this project writes for itself. Keeping the
borrowed part exactly the size of the thing that justifies it is what keeps the
justification checkable: "libavcodec, because H.264 is seven hundred pages" is a
claim somebody can weigh, and "libavcodec" is not.

**Colour conversion carries the two things that are usually guessed.** Which
coefficients (BT.601 or BT.709) and which range (0-255 or the broadcast 16-235)
are read from the file and only guessed when the file does not say -- and then
guessed the same way everything else guesses, by picture height. Treating studio
range as full range gives grey blacks and washed-out whites, which is the
commonest way a home-made player looks subtly wrong and never looks like a bug.

**Converting and scaling are one pass, and there are two resamplers.** Going
straight from the planes to the window's size means the colour conversion runs
once per pixel that will be *seen* rather than once per pixel in the file: a
1080p frame in a 400-pixel box is nine tenths less work, every frame. And the
resampling changes with direction -- interpolation when enlarging, an average of
every source pixel when reducing. That second one was not a preference. Bilinear
reads four pixels, which is most of the source at 1:1 and a twelfth of it at
3.4x, and what gets thrown away comes back as aliasing that crawls from frame to
frame. Measured against ffmpeg: 5.05 mean difference per channel with bilinear,
3.59 with the average, and 1.15 at native size where neither runs.

**The sound leads and the picture follows.** The device is the clock, as it has
been since sound arrived -- it consumes samples at a fixed rate whether or not
anybody is watching, and asking how many it has played is a measurement rather
than an estimate. Pictures have no such thing, so a frame that arrives late is
*dropped* rather than shown late. A file with no sound track runs on wall time,
which is a worse clock and is still a real one; counting frames and assuming
each took as long as it should have is not.

**Seeking starts from the last frame that stands on its own.** Video frames are
mostly descriptions of how they differ from earlier ones, so landing anywhere
and decoding forward gives a second of coloured smears. The sync-sample table
says where the self-contained frames are; everything between there and the
target is decoded for what it teaches the decoder and never shown.

**Verified against ffmpeg on a real file, frame by frame**, at five scales and
seven seek targets rather than one of each -- which is what caught both faults.
The full accounts are BG-097 and BG-098, and the short version of each is worth
carrying: a test whose inputs are all round numbers is testing round numbers,
and a comparison against a reference decoder proves agreement about the one file
it was run on.

**A Media Player** with a playlist, transport, a draggable position bar and its
own volume — applied to the samples rather than to the device's mixer, because
turning the whole machine down when somebody wants one track quieter is
reaching past your own edges. The position is asked of the device rather than
counted, since what has been handed over is up to a buffer ahead of what
somebody is hearing.

**Every kind of file has its own icon.** They are all the same sheet with one
mark — a level meter for sound, film perforations and a play triangle for
video, a horizon for a picture, angle brackets for markup, a brace for
structured text, a letter for a typeface, a banded box for an archive. One
icon for every file says nothing, and saying what a thing is before its name is
read is most of what an icon is for. The Type column and the desktop ask the
same question the explorer does, so a file looks the same everywhere.

**A window frame you can see through, and a skin built on it.** A skin can now
say how solid the chrome is, and *Glass* says 210 of 255 -- see-through enough
that the wallpaper moves under a title bar, solid enough that a filename stays
readable over a photograph.

The mechanism is one pass over a finished rectangle rather than an alpha on
every fill. The title bar is drawn exactly as it always was, by code that has
not changed, and then faded once at the end -- so its text, its icon, its
buttons and its rounded corners all become see-through together and none of them
had to learn a new rule. The alternative was touching every drawing primitive in
the system, where getting one wrong leaves an opaque patch inside a translucent
bar.

**Premultiplied, which is the part that fails invisibly.** Wayland's ARGB8888
has the colour channels already scaled by the alpha, so half-opacity white is
half-grey. A version that set only the alpha byte would give chrome that is
see-through *and* too bright -- which reads as a deliberate glow rather than as
a fault, and would be found by somebody wondering why the glass looks lit from
inside. It is pinned by a test rather than by looking at it, in a file that
needs no compositor to run.

**The taskbar and the Apps menu take it too, and the menu takes half.** At the
same opacity as a window frame, the menu put "Recon Core" directly on top of
another window's "Line 1, Column 1". That is not a tuning problem: a title bar
carries one short label, and a menu is a column of a dozen somebody is scanning.
Halved rather than given a second setting, so a skin still says how much glass
it wants once and the rule that follows is a sentence -- chrome you read a list
from gets half. The tooltip and the dim behind a dialog stay solid, for the same
reason stated the other way round.

**And a second icon set, lit.** Every generated icon is written twice: flat, and
again into `/System/Icons/Glossy` with a curved-glass treatment -- a vertical
ramp that makes a flat shape read as curved, the lower arc of a large ellipse
centred above the icon for the specular, and one lighter row along the top of
the shape for the rim. It follows the icon's own alpha rather than a rectangle,
which is the whole difference between a glossy icon and an icon with a glossy
box behind it.

Two sets rather than one treatment applied everywhere, because the flat idiom is
what Classic and Recon are *for* and 95 did not gleam. Two sets rather than a
gloss applied on the way to the screen, because these are files precisely so any
one of them can be replaced -- and a gloss at draw time would be applied to the
replacement too.

A skin says which it wants with `metric.icon-gloss`, and the glossy set falls
back to the flat one rather than switching to it: an icon that only exists flat,
because somebody added or replaced it, still appears under a glossy skin. And
the icon cache now watches the theme's generation counter, because which file a
name resolves to is no longer decided by the name alone -- BG-089's shape
exactly, caught before it shipped this time.

**Screenshots read now.** A whole 1280x720 desktop -- windows, their menus, the
status bar and the clock -- comes back as 101 of 125 marks at confidence 97,
where a day earlier it came back as nothing.

The missing stage was in front of the others. Finding lines by looking for rows
with no ink is right for a picture that is only writing and wrong for a screen:
a window border puts ink on every row it spans, so the display collapsed into a
few enormous bands and every mark in one was a blob.

**The first guess about why was wrong, and measuring said so immediately.** I
assumed the wallpaper was being thresholded as ink and drowning everything. A
whole desktop measures 1.9% ink -- *less* than a crop of plain text at 3.9%. The
threshold was never involved. It was geometry.

`recon_ocr_regions` cuts the picture on whitespace, alternating between rows and
columns and recursing into each piece: a screen into windows, a window into its
bar and its contents, the contents into paragraphs. **A cut has to be wide
relative to the piece being cut**, which is what stops the recursion running
past a paragraph and separating the words in it -- a fixed number of pixels
cannot do that, because the gap between two words at forty point is wider than
the gap between two paragraphs at eight.

**And a line drawn across a whole block is a place to cut, not something to
read.** That is what gets inside a window at all: its border is one column of
ink on every row, so no run of blank will ever be wide enough there. Text never
spans its own extent; a frame, a rule and an underline always do. Only where the
block is over 48 pixels, because at the bottom of the recursion a block is a
single line and a tall letter genuinely does span it.

Smaller blocks then exposed the other end of BG-100. The taskbar clock read as
`9 / 5 / 2 0 2 6`: its gaps are one and two pixels, and two is twice one, so the
proportional rule found a word break between every character. That fault was a
threshold taken from the band's height alone; its fix took one from the
distribution alone. Both halves were needed -- **one pixel of difference is not
evidence at any size**, and a quarter of the height is the floor under it.

**Photos saves what it opens.** It could read seven picture formats and write
none of them, so a JPEG stayed a JPEG. **Save as PNG** writes the picture out
beside the original, under a name nothing else has -- a converter that
overwrites is a converter that loses the thing it converted.

PNG out and nothing else, deliberately. The direction people want is almost
always this one: a photograph arrives compressed and is wanted lossless to work
on. Offering the other direction would mean a button that costs a little of the
picture every time somebody presses it.

The whole feature was the join between two things that already existed -- the
decoder Photos opens files with, and the encoder written for screenshots -- and
the interesting part was what checking it found. **The encoder had never been
tested.** It had been looked at, on screenshots, by a person deciding they
looked right; a picture can have red and blue swapped or an alpha channel
quietly flattened and still look right at a glance. The new suite encodes with
the writer and decodes with stb_image, so what is being checked is agreement
with a different implementation by a different author.

That found a refused encode leaving the caller's length variable untouched. No
live caller was bitten -- each starts its own at zero and checks the pointer --
which is exactly why it was worth closing: the next caller is the one that
reuses the variable, and a stale length beside a NULL pointer is an overrun
waiting for a skipped check.

Checked on a running system as well as in memory. A 3900-byte JPEG opened in
Photos, the button pressed, a 7485-byte PNG beside it; both then decoded by
ffmpeg, which wrote neither. 375 of 79800 samples differ and **every one of
them by exactly one step** -- stb_image and libjpeg rounding the IDCT
differently, within what the JPEG standard permits. The conversion moved
nothing.

**A Read Text button in Photos.** It reads the picture on screen, writes the
text to a new file in Documents named after the picture, and opens it in
Notepad. Verified end to end on a running system: 46 of 46 marks, confidence
100, a file on disk with a provenance line at the top of it.

**Three of the four outcomes write nothing, and that ratio is the feature.**
The only case that saves without asking is the one the engine itself calls a
reading. A picture where marks were found and none could be named says so and
saves nothing -- a file whose entire contents are replacement characters is not
a result. A guess is held and asked about, with the real numbers, and
**deliberately without showing a sample of the text**: an excerpt reads as
evidence, and the whole reason to ask is that the engine cannot tell whether it
is. There is no setting to skip the question and no remembered answer, because
it was a guess every time.

The provenance line goes on **every** file, not only the doubtful ones. A note
that appears solely on uncertain output makes its absence a claim of
confidence, and absence is erased by one edit.

**The reading happens on a timer rather than on the click.** It is synchronous
and the desktop is single-threaded, so doing it inside the click handler would
freeze with the *previous* status showing -- the one moment the machine is busy
would be the one moment it had not said so. Forty milliseconds is enough for
"Reading." to reach the glass first. Above twelve megapixels it asks before
starting, and that threshold comes from measurement rather than caution: a
1920x1080 screenshot reads in about 40ms and a 2400-pixel page in about 100.

**And the header now says what this cannot read.** A screenshot of a *whole
desktop* comes back as almost nothing, and it is worth stating plainly because
it is the picture people will try first: `recon_ocr_lines` treats a row with any
ink as part of a line, which is true of a cropped page and false of a screen
where window borders put something on nearly every row. The display collapses
into a few enormous bands and every mark in them is a blob, correctly refused.
Fixing it needs a stage that finds text regions before finding lines, and that
stage does not exist yet. Until it does, this reads a picture of some text and
not a picture of a screen.

**Reading text out of a picture, finished.** On a real screenshot of ReconOS's
own text -- through the compositor and a PNG encoder, not a buffer the test drew
for itself -- it reads **52 characters of 52, refuses nothing, and reports a
confidence of 100.** The premise held.

The matching is baseline-anchored template comparison. Each line's size and
baseline are fitted from all its marks at once, because no single mark contains
either -- an 'o' does not know where the baseline is and a 'T' does not know the
x-height. Then every candidate has exactly one place it could sit, so three
comparisons on position and width discard about nine tenths of the alphabet
before a pixel is looked at. **Those three comparisons are what separate o from
O from 0, and a comma from an apostrophe** -- and they are the payoff for
refusing to normalise each mark into a common box, which is the standard move
and which throws away exactly the height and width those pairs differ by.

**The engine measures whether it can tell two letters apart, rather than
assuming.** For every pair of candidates at a size it records whether they are
the same shape, and refuses to name either when they are. In DejaVu Sans, `l`
and `I` score exactly 1000 against each other at 10, 12, 14, 16 and 20 pixels.
That is not a similar shape; it is the same shape, and picking one would be
right about half the time and look identical to being right.

Two faults found before shipping, both by looking rather than reasoning:

The same letter came out **9x10 from the page and 7x9 from the rasteriser** --
the mark systematically fatter, and every score sitting at 850 where it should
have been a thousand. The two sides were cut at different thresholds: the page
by Otsu, which on black-on-white lands near 200, and the candidate at a flat
128. The mirrored cut is exact rather than tuned -- a page pixel is ink when its
value is at or below the threshold, and coverage `c` is drawn as `255 - c`, so
the candidate cut is `255 - threshold`.

And it read `"the lazy dog"` as `"the Iazy dog"`, which is the one thing the
design exists to prevent. The twins rule should have caught it and did not,
because it compared the winner only against the runner-up. Fixed to refuse any
candidate with a twin -- which is what `recon_ocr_match.h` already promised, and
the header being stronger than the code is the worse direction for those two to
disagree in.

**Reading text out of a picture, begun.** ReconOS has no word processor and
cannot yet run anybody else's, so a screenshot of a document is currently text
that cannot be got at. That makes this worth more here than it would be
elsewhere.

**It is not a research project, and the reason is that ReconOS draws its own
text.** General optical character recognition -- photographs, perspective,
handwriting, unknown faces -- is a field. But most of what anybody wants read is
text that was *rendered*: a screenshot, a saved page, a scan of ordinary print.
A system with a font rasterizer can produce the shapes it is trying to
recognise, which turns "what letter is this" into a comparison against shapes it
can draw on demand. That is the whole idea and it is also the whole limit.

Three of the five stages are built and tested: deciding which pixels are ink,
finding the bands text sits in, and splitting a band into marks. All three are
arithmetic on a bitmap, so they need no font and are tested without one -- on
images built a rectangle at a time, because an OCR test that runs on a
photograph can only be checked by reading its output, which means it passes
whenever the output looks plausible.

Two things settled early because they are the honest part rather than the last
part. The threshold is chosen from the image by Otsu's method rather than fixed,
and it also decides **which way round the page is** -- light text on dark is as
ordinary as dark on light in a screenshot, and reading it backwards does not
fail: it finds the *gaps* between letters, which are marks, in rows, of
plausible size, and produces a confident answer made entirely of holes. And the
result carries a confidence and a count of marks it could not name, because a
reader that cannot say "I am guessing" is one that lies on every picture it was
not built for.

**The matching is not written**, and there is deliberately no whole-picture
entry point yet. A function that finds marks and can name none of them works and
returns nothing, which is a worse thing to ship than a header saying which half
is finished.

**The glass can be a colour.** Six tints -- Blue, Amber, Rose, Jade, Violet,
Graphite -- chosen beside the skin rather than as skins of their own. Eleven
skins times six colours is sixty-six entries in a list somebody has to read,
every one differing from its neighbours in a single respect.

**A tint keeps the lightness and moves only the hue**, which is the whole
design. A palette's *structure* is in its lightness -- which surfaces sit above
which, which text reads against which -- and only its appearance is in its hue.
Recolouring by rotating hues changes what a skin looks like; doing it the other
way round changes whether it works. So the tint is used as a hue reference: a
colour at the tint's own lightness comes out as the tint, darker ones as darker
versions, lighter ones as lighter. Pinned by a test across the whole range,
because the two sides of that calculation are different code and an error in
either is invisible from the other side of the branch.

**Only the chrome moves, and only a skin that opts in.** Text roles are excluded
so a tint can never be the reason a label became hard to read; the accent, the
selection and the warning colour are excluded because they mean something, and a
meaning that changes hue with the decor is one nobody can learn. And a skin has
to say `metric.tintable` -- which the colour-vision skins never will, because
their palettes are chosen so particular pairs stay distinguishable and moving
the hues would undo exactly that, silently, to whoever picked the colour.

The first six screenshots of six different tints came out pixel-for-pixel
identical. The setting was stored, the lens was applied at the point a colour is
asked for, and nothing had asked for a colour since -- `recon_shell_restyle` was
missing. A change nothing is told about is a change that did not happen, which
is the same shape as the icon cache keyed on a name after the name stopped
deciding the file.

**A `dialog` command on the control socket**, which exists because of how the
change below was tested. Getting a dialog on screen to look at cost four
attempts at aiming a click, and every one failed silently -- a click that misses
a close button by three pixels does nothing, reports nothing, and is
indistinguishable from a dialog that never opened.

The useful half is not `dialog ask`. It is the bare `dialog`, which prints what
is being asked and **where each button is**, so nothing driving the desktop from
outside has to guess a coordinate. Every guessed coordinate is a test that can
fail for a reason unrelated to what it tests.

`dialog press <label>` answers by name, and does it by clicking the button at
its real position rather than by calling the answer callback -- calling the
callback would test the callback, and what is worth testing is that the button
is where it is drawn and that its hit region agrees. `dialog ask` is gated
behind `RECONOS_ALLOW_SPAWN` like `raise`, because it puts a question on screen
that nothing asked.

**Glass reaches the dialogs, and stops at one of them.** The context menu and
All Programs take the same half-strength glass the Apps menu does. A dialog
takes it *only on its title strip*, which is the rule window frames already
follow: the bar that says what this is fades, and the part somebody has to read
does not.

That was found by looking rather than by reasoning. Faded whole, the delete
confirmation had the file list's selected row -- a solid blue bar -- running
directly behind the words "Move 'notes.txt' to the Recycle Bin?". Legible, and
not what a question about somebody's file should look like.

**The security box stays solid, and the skin gets no say.** It asks somebody to
approve something they cannot undo, and it dims the whole desktop behind itself
so that being asked is unmistakable. See-through would work directly against
what it is for: "it looked like part of the window behind" is the beginning of
every story about somebody approving the wrong thing. Same rule as there being
no switch to turn a safety check off.

**ReconOS has its own Wayland protocol.** The applications that ship with it
read the palette directly because they are in this process; one that arrives
later is a client in its own process and cannot. The decision recorded in
`docs/APPLICATIONS.md` is that installed applications become clients, so this is
on the path to that rather than beside it.

Without being told, a client can guess and be the one window that does not
match, ship its own theme and be the one window that does not change when the
desktop does, or read the registry behind the compositor's back — which works
until the format changes and is not a boundary at all.

**What it does not do is say how to draw.** No frame, no button shape, no font.
A client that wants to look like it belongs uses the colours; one that wants to
look like itself ignores them, and the test client keeps its fallback colours to
prove that ignoring them is allowed.

**The first version of the test client passed and proved nothing.** It recorded
the palette and stopped, so the client was told the skin had changed and went on
showing white content inside a dark frame. Everything about the protocol worked;
the claim was that a client *following* it ends up the right colour, and that
was false. A test that stopped at "the bytes arrived" would have called it a
pass. Measured properly: the pixels inside the client's window are `#FFFFFF` on
Recon and `#1E2024` on Midnight — exactly the two surface colours the compositor
said it had sent.

**The Calculator's sixth mode.** Graphing was the one of six never built, and it
needed an expression evaluator first — the Calculator is button-driven and had
no way to read one.

**Two things make a grapher honest rather than merely working**, and both are
about refusing to draw a line that is not there. A column where the function has
no value breaks the stroke, so 1/x is two branches rather than two branches
joined by a vertical wall down the y axis. And a jump wider than the whole
window between neighbouring columns is a break rather than a slope, because no
honest curve crosses the window in one pixel of x — get that wrong and tan(x) is
drawn as a row of walls.

That is why the evaluator has **three results rather than two**. Dividing by
zero and the root of a negative are not errors: the expression is fine and has
no value at that x. A grapher told "error" gives up on the whole curve because
of one point.

**Pixels are square**, with the height following from the width and the shape of
the box. Letting them differ draws a circle as an ellipse, which is a quieter
lie of the same kind. The cost is that sin(x) looks flat at this scale — because
it is, and a grapher that silently stretched it would be answering a question
nobody asked.

**My test was right and the code wrong**, for a change: `-2^2` came to 4. A power
binds tighter than a minus on its left and looser than one on its right, and the
paragraph at the top of the file said exactly that while the grammar underneath
did the opposite.

**STARTTLS, with the upgrade required.** Port 587 works now, so a provider that
offers only STARTTLS is usable. It was deferred on purpose — the easy half is
connecting and the hard half is the one that ships a system sending passwords in
the clear.

**Three things something in the middle can do, and all three are refused.**

It can *delete* STARTTLS from the server's offer, and a client that carries on
has sent everything in the open because one line was removed. So the upgrade is
**required**: no offer means the session ends. A server that genuinely cannot
encrypt and a middle that stripped the offer look identical from the client's
side — and that is the point, because a client able to tell them apart is one
that could be argued out of encrypting.

It can answer *"ready to start TLS"* and put more commands in the same packet.
Those bytes arrived before anything was proved, and a client that reads them
takes an attacker's commands as though they came from inside the encrypted
session. That is how STARTTLS has been broken in real mail clients. Everything
buffered is discarded the moment the upgrade starts.

And it can lie in the first EHLO. Nothing learned before the upgrade survives
it — EHLO is sent again afterwards, so what the server can do is heard from the
party whose certificate has been checked.

**Tested against a server built to attack it** rather than one built to work,
because the well-behaved path ends at a certificate a fake server cannot produce
and everything worth checking happens before that. The no-offer server saw
exactly `EHLO reconos` and nothing else; the injecting server saw `EHLO reconos`
and `STARTTLS` and nothing else. No AUTH, no MAIL FROM, no letter, in either.

**BG-113 came out of the same fake server, and is not about SMTP.** A server
that accepts and then says nothing left the client waiting forever: the connect
deadline was dropped the moment the socket connected, and *connected* and
*usable* are the same moment for a plain stream and not for an encrypted one.
Every outgoing TLS stream had it — reading mail, the web viewer, checking the
time.

**Mail can send.** Both halves of an account on one form, because reading and
sending are separate protocols on separate servers and the same account to the
person filling it in. The sending half is allowed to be empty — somebody who
only wants to read should not be stopped at a field asking for a server they do
not have.

**Write is reachable without connecting first.** Sending and reading are
different servers, and requiring a successful connection to one before a letter
can be handed to the other is a coupling with nothing behind it: a mail server
being down should not stop somebody writing.

The body is the first multi-line field in the system, so `recon_edit` gained a
flag — Enter puts a newline in rather than finishing, on that field only. There
is no way to send from the keyboard, deliberately: a letter should not leave
because somebody finished a line. And no wrapping, stated rather than faked — a
long line runs off the edge and is still there; wrapping means deciding where
words break and mapping the caret through it, which is a text engine rather than
a text box.

**A failure leaves the letter exactly as it was.** A failure that also loses what
somebody wrote is two failures, and the second is the one they remember.

**BG-112, found by filling the form in and noticing the port had gone.**
`recon_edit_begin(&field, field.text, …)` hands `snprintf` a source and a
destination that overlap — undefined, and here an empty string. Not new: the
mail form has moved focus that way since it was written, and the Web viewer's
address bar did it on a click, so **clicking the address bar cleared the address
it was showing**. Unnoticed in both places because a field about to be typed
into looks the same cleared as selected. The form only exposed it by gaining two
fields with defaults worth keeping.

**SMTP, the protocol underneath it.** It could receive on two
protocols and not send at all. This is the transport and the message; the window
to write one in is next.

Encrypted from the first byte, with no plaintext path in the file and no setting
that produces one — and **the cost of that is named**: a provider offering only
STARTTLS on 587 is a provider this cannot send through. STARTTLS is absent
because it means starting in the clear and asking to be upgraded, and `recon_net`
has no way to upgrade a stream, deliberately. Building it has to make the
upgrade *required*, because the easy half is connecting and the hard half is the
one that would ship a system sending passwords in the open.

**The rules live in their own file so they can be tested without a network
stack**, and they are where the mistakes are. A newline in a subject is how one
message becomes two — whoever wrote it gets to add headers, and what arrives is
not what was on screen. Refused at composing as well as at checking, because
those are separate entry points and a caller could reach the second without the
first. The *sender's* address is checked too: it comes from the settings rather
than the message, so it is the one nobody thinks about.

A line that is exactly a dot ends a message, so one inside a letter is doubled.
A body with no final newline gets one, or the dot that follows lands on the end
of a sentence. And a message too long for the buffer is refused whole — a
truncated one is still valid SMTP, so it would be accepted, delivered, and
arrive missing the end with nobody told.

base64 went in beside hex in `recon_crypt`, checked against RFC 4648's vectors —
not mine, so agreeing with them is evidence about the encoder rather than
evidence I wrote the test and the code the same way.

**Photos can make a picture a different size.** Half and Double rather than a
box to type a size into — a dialog taking two numbers has to explain what
happens when they do not match the picture's shape, and the honest answer (the
shape is kept and one of them is ignored) means the second box was never real.

Nothing is written. What is on screen changes and the file does not, so a resize
is something to look at before deciding to keep, and keeping it is **Save as
PNG**, which already refuses to overwrite. That gave Photos unsaved state for
the first time, so the title bar carries the same star Notepad uses —
deliberately not a different sign.

**One resampler, where there were two halves and a gap.** I said this would be
small because `recon_video` already area-averages down and interpolates up.
That was wrong: `recon_video_render` takes YUV planes and cannot be pointed at a
photograph. What existed was a private RGBA shrinker in the wallpaper loader
that could only make things smaller, a good two-way resampler welded to video,
and nothing at all in the application where somebody would ask.

`recon_image` is the shared one, and it picks its resampler **per axis** — a
picture made narrower and taller at once wants an average across and an
interpolation down, and one rule for both gets one of them wrong. Tested against
properties rather than a fixture, because a fixture for a scaler is a picture
the scaler made and proves only that it agrees with itself.

**The clock's menu opens the page, not the panel it lives on.** Also two things
the screenshot caught that the change itself caused: the header said "Central
(UTC-6)" while the row highlighted underneath was Eastern, because the
summer-time hour was being folded into the match and Central-plus-an-hour is
Eastern's offset. And the note explaining why there are no buttons to set the
clock ran off the end of the window as "Its own co...".

**The clock tells the right time.** It was an hour slow: Central selected, the
host reading 4:25 am, ReconOS reading 3:25 am. The page said plainly that
nothing here follows daylight saving — true, and it did not stop the clock being
wrong for most of the year for most of the people in the list.

The original reasoning is not reversed. A rule engine for the world's
daylight-saving legislation is a database with politics in it, revised by
parliaments with no interest in this clock, and getting it wrong twice a year is
worse than not having it. So **summer time is a switch rather than a rule**, and
it starts from what the host thinks: `tm_isdst` says whether summer time is in
force and the offset says by how much, so the two come apart into a standard
zone and a switch without any rules being carried. Written with standard time
arithmetic rather than `tm_gmtoff`, which is a BSD extension C11 does not have —
a system that intends to run on its own kernel should not lean on what glibc
adds to a standard structure.

**The time zone list had a scrollbar and no way to move it.** Twenty-six zones,
about nine rows of room, seventeen unreachable. The note directly above the
fault describes the fault: it explains that every list in Appearance takes the
wheel now, because one of them *"said 'scroll for the rest' under a list that
could not be scrolled — a page telling somebody to do something it would not let
them do."* The same sentence was true one page over.

**And checking against a time server told people to edit the Registry**, which
is not a setting, it is an instruction to go around one. It is a button now.

**Windows arrive from the desktop they came from.** The first thing in ReconOS
that moves on its own. Turning four desktops into one moves windows nobody asked
to have moved, and a thing that happens without being explained looks like a
fault — so they slide in from the side they were on. Desktop 3 is to the right
of desktop 1 on the pager, so its windows come in from the right; the direction
is the explanation.

The offset is a **display** offset and never touches the window's position. A
window animating in from off the right edge is, as far as everything else is
concerned, already exactly where it belongs — hit testing, snapping, the taskbar
and the clamp that keeps a title bar reachable all go on working on a position
that never moved. Ease-out cubic in integer arithmetic, a quarter of a second,
and the last step sets zero explicitly so the end of an animation is the same
pixel as no animation at all.

**Desktop labels read on the wallpaper they are on.** Reported from a
screenshot. Three faults stacked: the shadow was drawn at one offset, so seven
of a glyph's eight edges sat on the picture; the shadow roles carry alpha and
were written into a premultiplied buffer unscaled, which is see-through and too
bright; and a ring drawn where it is not needed blends with the label's own
antialiased edges and costs the glyph weight for nothing.

So the ring goes all the way round, premultiplied, and is drawn **only when it
is far enough from the wallpaper to be separating anything**. The wallpaper's
lightness is measured into a 16×9 grid where the picture is decoded — a grid
rather than one number, because a wallpaper is often pale at the top and dark at
the bottom and an average of that is wrong in both halves.

**The measurement agreed with the code and disagreed with the eye, and the eye
was right.** The contrast ratio came back 17:1 for a label that was barely
readable, because it was answering "is there a dark pixel near a light one" —
true throughout. What had changed was the weight of the strokes.

**Four desktops, or one, per account.** Somebody who does not use four desktops
should not have four buttons taking up the corner of their taskbar. Right-click
the bar and choose.

Off is genuinely *one desktop*, not four with the buttons hidden, and that
distinction is the whole design. Hiding the pager while leaving Alt+2 working
would let somebody arrive on a desktop with nothing on screen saying where they
are and no button to come back with — a worse place to be than a taskbar with
four squares on it. Everything that can reach another desktop was already
guarded by one count, so making that count answer the setting turns the buttons,
Alt+1..4 and Alt+Shift+1..4 off together, with no second rule to fall out of
step with the first.

**And everything comes to the desktop you are standing on.** You do not move:
once there is one desktop it is the one you were already on, so nothing jumps
and nothing has to be gone looking for. The setting and the windows move in one
function rather than the registry being written at the call site — after the
switch there is nothing left to reach a stranded window with.

A screenshot caught the bug the numbers could not. Every reading said "1 of 1"
and every one was correct, while the corner still held a single square marked
**1** — the drawing loop is bounded by the count, so answering the setting drew
one button instead of none. A button saying which of your one desktops you are
on is the exact clutter the setting exists to remove.

**A put-away window's button recedes into itself.** The last thing asked for on
the taskbar and not built. `minimized` reached the drawing code, was cast to
void, and then reached exactly one thing six lines later -- the bevel direction.
So a put-away window and a background window differed by one pixel of light and
one of shadow, on a 28-pixel button, at the bottom of the screen.

There are three states, and there are now two signals, so every pair differs by
at least two things:

| | fill | bevel | contents |
|---|---|---|---|
| focused | active | pressed | full |
| open, behind | plain | raised | full |
| minimized | plain | pressed | **washed** |

The contents wash towards the button's **own fill**, which is the original idea
corrected rather than a new one. It used to be the whole button filled with the
bar's colour so a put-away window sank into the bar, and that inverted on the
skin whose bar is deep blue and whose buttons are near-white. Receding into its
own button is right on every skin because the *surface* defines the direction --
and it is the only such rule a skin cannot defeat, because every other way of
saying "less prominent" needs two colours to stay apart and a skin may put them
anywhere.

**Measured on the same button, open and then put away**, so nothing but the
state differs. All eleven skins land between 0.574 and 0.588 against an
arithmetic prediction of 0.569.

**Three instruments were wrong before that number was right**, and the code was
right every time. Mean colour per button compared against two *other* buttons
as a noise floor — a button is mostly fill, and the floor was measuring
different words and icons. The pointer was in the photograph, its tooltip and
cursor sitting on the very button being measured, which made three skins report
a put-away button with *more* ink than an open one. And the taskbar was assumed
four pixels higher than it is, so the window straddled the bevel — which flips
with the state, and swamped what it was meant to measure around.

A fourth found nothing wrong with the code and something wrong with the
measurement: one skin read 0.74 where ten read 0.577, the gradient looked like
the culprit, the fix changed the number by nothing at all, and the fault was
that a graded button's own spread was being counted as contents. A wash cannot
remove that, because washing a colour towards itself does nothing.

**The wash strength's ceiling was measured with this system's own reader.**
Washed past about 140 of 255 the title stops being separable from the button at
all — the engine finds zero marks where it found nine or twelve, on every skin.
It is set to 110.

**A menu on the clock.** Clicking it opened the Control Panel at its root: the
right application at the wrong page, and four more clicks for somebody who only
wanted to stop reading fourteen thirty. It now offers the two things people
want from a clock in a corner — which way it writes the hour, and how to get at
the rest.

Both choices are shown and the one in force is **marked**, rather than one entry
that toggles. "Show am and pm" says what will happen and not what is happening,
so the state would have to be read off the clock itself — which is the thing
somebody was looking at when they could not tell. It writes the setting the
Control Panel writes; two places that can change one thing and two records of
what it is set to is how a preference starts disagreeing with itself.

**The clock goes in the corner, and gains the date.** The clock and the desktop
pager were the wrong way round. That is a mistake rather than a preference: the
corner is where a clock goes, and four numbered squares sitting in the place a
clock goes are four squares somebody has to look at twice to identify.

The clock showed the time and not the date, so the corner answered half of what
people look there for. The full date is four times too wide for a taskbar, so
this writes a short one from the same numbers, on a second line, in a smaller
face. Two lines do not fit a 28-pixel button at the bar's own size, and a clock
is glanced at rather than read -- competing with the window titles beside it
would be wrong even if it fitted.

**Twelve-hour time needed nothing built.** `clock/twenty-four-hour` already
existed, `recon_clock_short` already honoured it, and the Control Panel's Date
and Time page already toggled it; the new date line follows whatever the time
line is doing. Clicking the clock still opens the Control Panel, though at the
root rather than at Date and Time -- that needs the panel to accept which page
to show, and is not done.

**And the pager rounds.** It drew a plain bevel where every other button goes
through `recon_draw_button_edge`, so it stayed square under every skin. Those
four were the last square things on a desktop whose windows and task buttons
have rounded since v0.2.10 -- and being in the corner is exactly where that
gets noticed.

Checked by clicking rather than by reading the diff: pager button 3 selects
desktop 3, button 4 selects desktop 4, and the clock still opens the Control
Panel from its new position. Moving a button and breaking its click is the
whole risk in that change.

**Four wallpapers made for this system rather than drawn by it.** The
four that existed are two colours, a ramp and some stars, generated at first run
-- the right default, and not artwork. Glass is see-through *to* the wallpaper,
which makes the wallpaper matter more than it did before it existed.

*Aurora* is installed at first run alongside the drawn ones, into the same
directory, listed by the same scan, chosen the same way. It also cannot be
deleted, and not because anything new says so: removal already refuses anything
with no recorded origin, and only a wallpaper somebody *added* has one. The
preset rule fell out of the existing design rather than needing a second one.

How it was made is written down in THIRD_PARTY.md, because "made for ReconOS"
and "drawn by hand" are different claims and only the first is true.

**No backdrop blur, and that is the honest gap.** Real Aero blurred what was
behind the glass, which is what let it be far more transparent than this is. A
window's buffer cannot see what is under it, so blurring would mean either
sampling only the wallpaper -- correct over the desktop and a lie over another
window -- or reading back the composited scene every frame. Neither is worth
doing badly, so the opacity is set where it is legible without it.

**A web viewer.** Not a browser, and it is called a viewer everywhere: HTTP
and HTTPS, HTML structure, links, back and forward — and no CSS, no
JavaScript, no images, no forms. That boundary is stated rather than
discovered, because "a viewer for simple pages" and "a browser" are a weekend
and a decade apart.

Structure decides appearance, since there is no stylesheet: a heading is large
because it is a heading. A page whose layout lives entirely in CSS renders as
its underlying structure — readable for a well-written page, a column of text
for a badly-written one. That is the honest failure rather than a hidden one,
and a page that builds itself with JavaScript says so instead of showing blank.

`text/plain` is read as text and not as markup, so an RFC or a README keeps its
own line breaks and its own column alignment. A large fraction of what is worth
reading is a plain file, and running one through an HTML parser eats every `<`
in it.

A redirect may go from http to https and **may not go the other way**. A server
answering an encrypted request with "now ask me again in the clear" is broken
or hostile, and following it would silently undo what the encryption was for.

An image becomes its alt text, which is what alt text is for. A link is
underlined as well as coloured, because a link that is only a different colour
is invisible to a reader who cannot see that difference.

It opens a file from this machine as well as one from the network. `.html` and
`.htm` open in the viewer; `.xml`, `.json`, `.csv`, `.ini` and `.conf` open in
Notepad, which shows them as what they are. XML is deliberately *not* given to
the viewer: it reads HTML's tag vocabulary, so an XML document would come out
as its text with every tag silently dropped — which looks like a viewer that
works rather than one that does not understand the file.

The Type column in File Explorer knows about all of them now. It had been
saying "File" for a JPEG that Photos would open perfectly happily, which is the
column disagreeing with the rest of the system about what it is looking at.

The layout runs while the page is drawn, and the link regions are registered as
the words are placed — so what is clickable is, by construction, exactly what
was drawn. A separate layout pass is a second set of arithmetic that can
disagree with the first, and the disagreement shows up as a link a few pixels
from where it looks.

---


## v0.3.0

**Mail.** IMAP and POP3, both over TLS, both through the encrypted streams
above — so the certificate of whatever answers is checked before a password
goes near it. There is no unencrypted option for either. Those ports exist,
the passwords they carry are readable by anything in between, and a switch for
it is a switch somebody eventually flicks.

Both protocols, because they answer different questions. POP3 collects: it
downloads what is waiting and the mail is yours, on one machine. IMAP reads:
the mail stays on the server and this is a view onto it, so the same account
opened from two machines shows the same thing.

**ReconOS never deletes on the server, on either protocol.** `DELE` is not
sent, and the IMAP fetches use `BODY.PEEK` rather than `BODY`, so opening the
list does not mark forty messages read. A young mail client with a bug that
alters somebody's mailbox is a young mail client nobody uses twice, and there
is no undo on the far end.

**The password is not stored.** It is asked for when a connection is made and
kept in memory until the window closes. That is an inconvenience and it is the
honest position: the registry would mean anything that can read a file can read
somebody's mail password, obfuscating it is worse because it looks like
protection, and doing it properly needs a keyring — a key that exists only
while somebody is signed in — which is a subsystem rather than a field. It is
written down as work to do. The window says so on the screen where the
password is typed, because somebody typing one into a new program is entitled
to know what happens to it.

It does not send. SMTP is a separate protocol with its own ways of losing a
message, and this does not pretend otherwise.

**A clock, bottom right.** It shows the time, and asking it shows the date,
the day, the zone and the region. It keeps twenty-six zones, in minutes rather
than hours, because three-quarters of an hour is a real offset that half the
world's software still cannot represent. It can be set to check an NTP server
and will report what the server said — and will not set the clock from it,
because a machine that quietly moves its own clock is a machine whose logs
cannot be trusted about when anything happened.

The tick is on the minute boundary, not once a second. A clock showing minutes
that redraws sixty times for each one is fifty-nine drawings nobody asked for.

**Photos and a Calendar.** Photos shows one picture at a time, fitted to the
window and never enlarged past its actual size, on a dark mat so a bright
image is not sitting in a bright frame. The Calendar is a month grid with
what is on each day; the entries are a text file, so they can be read and
edited by anything, including a person with a Notepad.

**The Calculator has five modes.** Standard, Scientific, Programmer, Date and
Convert. Programmer works in whole numbers rather than doubles, because a
calculator showing you a bit pattern has to be exact about all sixty-four of
them and a double is exact to fifty-three. Convert covers twelve families with
factors that are exact where an exact value exists — an inch has been exactly
0.0254 metres since 1959, and writing 0.0254001 would be inventing a
disagreement with the rest of the world.

Currency is deliberately absent. Every other family is a ratio fixed by
definition; an exchange rate is a fact about this afternoon, and a calculator
that shipped with one baked in would be confidently wrong about the one
question where being wrong costs money.

**Applets can be updated on their own.** A system application is no longer
welded to the release that shipped it. Install a module registering a name
that is already taken, at a higher version, and it takes the name; the one it
displaced is kept and comes back if the replacement is removed. An update you
cannot back out of is not an update.

The built-in applications carry the system's version as their own, which gives
this a property worth knowing about: when ReconOS is updated past an installed
applet, the built-in wins again and the installed one steps aside. An applet
update applies until the release that catches up with it.

Versions are compared as numbers and not as text, because the obvious
implementation says 1.10 is older than 1.9 — correct for nine releases of
anything, and wrong on the tenth in the direction where an update system
refuses the update it exists to install.

**The terminal is fixed-width, and has colours.** The interpreter has always
written its tables with `%-20s` and the terminal had always thrown that work
away; in a proportional face the columns wandered by a character or two on
every row. It uses a fixed-width font now, so `apps` and `ls` and `firewall`
read as tables.

Failures are drawn in red. The text stays plain and what each line *means* is
carried beside it, because the interpreter's output goes two places — a window
that has colours and a socket that has none — and an escape code in the middle
of the text would be something the socket has to know to strip.

Four schemes: Recon, which follows the system skin and is still the default;
PowerShell, which is that console's blue and near-white exactly; Green Screen;
and Paper. `scheme` lists them and `scheme <name>` chooses, and the choice
survives a restart. They are presets and cannot be removed.

The one colour not copied faithfully is PowerShell's error red, which on that
blue is close to unreadable and is the single most complained-about thing
about the console. Being faithful to a mistake is not a service to the person
reading it.

**The network port is encrypted.** Remote access over TCP 7420 speaks TLS. The
key is offered over the encrypted channel rather than in front of it, and
every warning saying otherwise has come down.

There is no certificate authority and no expiry theatre. A machine that owns
itself has no upstream to ask for an identity, so it asserts its own: a
self-signed certificate made the first time the port opens. What stands in for
an authority is the **fingerprint** — `remote` prints it, and so does Control
Panel → Network — which the client pins on first connect and checks from then
on. That is how SSH does it, for the same reason: a chain answers "did
somebody vouch for this name", and nobody has vouched for this machine.
Pinning answers "is this the same machine as last time".

**Outgoing connections are encrypted too**, and this is the opposite problem
with the opposite answer. Listening, the identity question is "is this the same
machine as last time", and pinning a self-signed certificate answers it
honestly. Connecting out, the question is "is this really imap.example.com" —
which is exactly what a certificate authority is for, and somebody *has*
vouched for that name. So going out verifies: a chain to a trusted root, and
the hostname checked against the certificate.

There is no switch to turn that off. A verify-off switch is a switch that ends
up on, and the failure it causes is silent — an encrypted connection to
whoever answered, which is not the same thing as an encrypted connection to
who you asked for.

When it refuses it says *which* check failed. "Certificate error" leaves
somebody with three very different possibilities and no way to tell them
apart: a wrong clock, a short bundle, or somebody sitting in the middle. The
last of those is the reason the code exists and it gets its own sentence.

The trusted roots are copied into `/System/Config` on first use from wherever
the host keeps its bundle — borrowed once, owned afterwards, the same as the
icons. Nothing at runtime looks at a host path, so the day this boots on its
own kernel the roots are already a file it owns.

Applications get this as an **encrypted stream**, handshaked a step at a time
between turns of the event loop rather than inline — a handshake is several
round trips to somebody else's network, and doing it in one go would freeze
the screen for as long as they took to answer. A stream's `opened` does not
fire until the handshake finishes, so nothing can write a password into a
socket that has proved nothing.

A refused certificate is its own outcome and not "unreachable". They mean
opposite things to the person reading them: "your mail server is unreachable"
sends somebody to check their connection, when the machine is reachable and
what is wrong is a clock, a missing root, or something answering in its place.

The port still ships closed and the firewall still decides whether it may open
at all. Encryption removes one reason it is off by default; it does not make
opening a port to the world a default.

**Screen resolution**, the last row on Display Settings that was not built.
The page lists the sizes a display offers and sets one, and a display that
cannot be changed says so rather than offering a control that could only fail
-- which is every nested and headless backend, where ReconOS is whatever size
the window is.

How it is built matters more than that it is. Nothing above
`include/recon_display.h` knows wlroots exists: the page asks ReconOS what the
screen can do, and ReconOS asks wlroots today and its own kernel later. The
same shape `recon_volume_*` already uses for three directories that will one
day be partitions. The point is that swapping what is underneath is one file
rather than every page that ever asked a question.

Phase 2 has also started. The kernel, in `kernel/`, boots on x86_64 and
aarch64 under BIOS, UEFI and device tree, reports the firmware underneath it,
and manages physical memory. It is built by its own Makefile against no libc
and no wlroots. It runs nothing of the desktop yet, and this file will say so
plainly when it does.

The kernel now also **arranges things to happen later**: a timer wheel, so
anything can ask for a callback or a thread can sleep without a processor
spinning for it, and a worker thread so an interrupt handler can hand off work
it must not do inline. Device interrupts have moved off the 8259 -- which has
one output, wired to one processor -- onto the **I/O APIC**, which can send them
to any of them; the switch is verified against the clock and reverted if the
tick stops, rather than trusted. Message-signalled interrupts are composed and
programmed, and no driver asks for one yet, which is said rather than implied.

**The kernel has a virtual filesystem.** Programs open things, hold them, read
and write and seek through them, and close them -- and what is on the other end
can be a file on the volume, the console, a pipe, or something in `/dev`, with
nothing above the interface knowing which. That unblocked five things at once:
pipes between programs, memory two programs can both reach, a mapping filled
from a file, `/dev/null` and friends, and **a program loaded from a volume**
rather than from inside the kernel image.

The scheduler also stopped handing a thread to another processor before the one
it was leaving had finished with it. That had been possible since the second
processor was woken and showed up as a kernel panic whose link register held a
marker word written by the page allocator's own concurrency test -- the reaper
had freed a stack that was still being stood on, and the allocator had handed
the page straight to something that wrote to it.

And it now expects a much larger machine than the one it is tested on. The
processor ceiling is **256** rather than 8, x2APIC is implemented so identifiers
above 255 can be addressed at all, and on aarch64 a processor's identity no
longer comes from one MPIDR affinity field -- which two processors on a
two-socket board would have shared. **None of that is tested above 32
processors**, and both the code and the bug register say so.

---

## v0.2.17

**An application can be turned off.** Programs → System Apps → Disable. It
stays registered and listed there, marked, and is offered nowhere else: not in
the menus, and not openable by any route. The list of turned-off names lives
in the system registry, so it survives a restart. This is distinct from
removed on purpose -- a built-in cannot be removed, it is compiled in, and "I
do not want this and cannot delete it" is a real thing to want.

**Repair says what it found.** For a program installed from a package, the
receipt names every file the install placed and each is checked. For one that
arrived as a bare `.rex` in /Apps, whether that module is still there and
loaded is the whole question. Neither puts a file back: that needs the package
it came from, and nothing keeps one, which is worth saying rather than having
a button that quietly does nothing.

**Install a Program opens the File Explorer.** It was a path to type. The same
right-click that adds a picture or a font now installs a program, on `.rex`,
`.rts` and `.rpk` -- three ways of saying "take this file into the system",
said one way.

**Storage and Disk Cleanup line up.** Figures are drawn from their right-hand
edge, because sizes are read against each other and that comparison is made on
the digits. A share bar's track is a groove rather than an empty text field,
and a category holding nothing draws no track at all. The volume selector is
the same tab bar Appearance and Network use -- these were buttons, and
choosing which of three spaces you are looking at is choosing a view, not
acting.

**One click highlights, everywhere.** Three pages forced a selection into
existence every frame, so they opened with the first row lit and the buttons
that act on it already armed. Switching lists, switching hives and removing a
row all reset to row zero, which is a real row -- so a Remove button came back
armed against whichever neighbour slid up into the gap.

**Tooltips, on anything with a clickable region.** A control says what it is
when somebody stops on it for half a second. The Control Panel's fourteen
icons carry one -- which is where the line of description under each name
went, because that line had to fit under an icon and so was cut off mid-word.
So do the window buttons, whose middle one says which of its two meanings it
currently has; the taskbar's window buttons, which lose their titles as more
windows open; the pager; and the Apps button.

**Network is four sections**: Status, Adapters, Data Used, Applications. It
was one page holding the machine's name, every interface, the gateway, every
resolver, the last test and two buttons, which fitted only because this
machine has two interfaces.

Adapters is the hardware question: every interface, and for the one picked,
its address, netmask, state and kind. Data Used is what each has carried since
it came up, with a total that leaves loopback out because loopback never left
the machine. Applications is which programs may open a connection, and Allow
and Block -- the sharing decision somebody actually gets to make, given
ReconOS has no stack of its own to share. Status keeps the rest and gains a
Firewall button, because the firewall belongs to the network and reaching it
meant going back to the front page to find a second icon.

**Fonts can be chosen and installed.** Display Settings lists what is
installed, where each came from, and which one is being drawn with. Installing
one is a right-click on any `.ttf`, `.otf` or `.ttc` anywhere -- the way a
picture becomes the background. It installs without switching to it: changing
every letter on the desktop without being asked is a different act from being
asked to keep a file.

**Wallpapers can be added and removed.** A picture anywhere becomes the
background from its right-click menu, and joins the list. The list says where
each one came from, so two folders that both hold a Sunset.png can be told
apart. One somebody added can be removed; one that ships cannot, and neither
can the one currently showing.

**New Skin**, which does not need a skin selected first. That is the whole
point of it: Customize Skin copies the row you are pointing at, and somebody
who wants to make their own had to work out that pointing at somebody else's
was how. It asks which of three starting points -- light, dark or high
contrast -- because a skin has forty-eight roles and every one has to hold a
colour, so there is no blank to start from.

**One click chooses, two acts**, in the lists that were doing both at once.

**Reading is Display Settings**, which is what the page is. Screen resolution
is still the one thing on it that is not built.

**About is System Information**: what the machine is, what ReconOS is, and
what is underneath. Three groups, kept apart on purpose, because the third is
what explains how the first is readable at all. The processor and its core
count are read from the host rather than described in the abstract.

**Programs splits into Installed and System Apps.** They are different things
and the buttons that apply to them are different buttons.

**All Programs is a fly-out**, beside the menu rather than replacing what is
in it. Anything in it can be pinned to the Start menu from its right-click
menu, and anything pinned can be unpinned the same way.

**Disk Cleanup**, as its own Control Panel item: a space to clean, categories
with sizes and counts and a tick box each, View Files, and Clean Up System
Files. Each row says what it costs to tick it, because a size is not a
consequence. Cleaning removes rather than binning -- moving scratch into the
bin would free nothing -- and the question it asks first says so.

**Storage is three spaces**: System, Programs and User, each measured on its
own and each with its own recycle bin. Deleting routes itself -- a file goes to
the bin belonging to the space it came from -- and emptying one names the space
it is emptying, because deleting a document and deleting a system file are not
the same act.

Real partitions still need a kernel. This is the layer above them, and it is
the part that has to be right before anything can be moved onto a partition
later.

**The screen blanks when nothing is happening.** A timeout on the Power page
-- never, 1, 2, 5, 10, 15, 30 minutes or an hour -- and an option to ask for
your password when it wakes. It covers the screen rather than switching the
display off, which would need a kernel, and the page says so.

The key or click that wakes it is spent on waking: somebody coming back to
their desk presses a key to see what is there, and typing that key into a
document they cannot see yet is not what they asked for.

**The firewall can be changed from its page.** Add Rule offers nine presets --
web server, mail, a database, a run of the ports games take, and the two blunt
ones that refuse everything in or out -- because most people adding a rule want
one of those. "Something Else" takes a name, a port or a range, and three
buttons that cycle through direction, protocol and what to do. Remove Rule too.

**A search box in the Start menu.** Typing there has narrowed the list since
v0.2.15 and nothing said so, which meant nobody found out. There is a box in
the footer now with "Search programs" in it when it is empty.

**Icons** for Appearance, Programs, Modules, Network, Firewall and Recovery,
which had been sharing one generic red square. Update is the last one without
its own.

**The Control Panel is icons.** Fourteen of them, each with a line under it
saying what it is for. Clicking one opens it in a window of its own, named for
the item and stepped clear of whatever opened it -- so a wallpaper and a set of
colours can be worked on side by side. They are still the Control Panel: one
application, one entry in the menus, one window per item and no more.

**Appearance is three sections**: Themes, Colours, Wallpapers. Each gets the
whole window, which is why the wallpaper list shows all five now instead of the
two it had room for when the skins were above it.

Choosing a skin no longer puts it on. Clicking down the list to read the
descriptions restyled the desktop nine times on the way; there is a **Use This
Skin** button, and the row says which one is in use.

**Customize Skin**, not Copy This Skin. It asks first, then takes a name and a
line describing it, then opens the editor **as its own window** -- because
changing a colour is something you do while looking at the result, and an
editor covering the thing it is changing was the worst place to put it. The
desktop, the skin list and the colours are all on screen at once, and a colour
changes under all three.

Four faults underneath, all of which had to be fixed for any of it to work:
the shell held eight windows and quietly dropped the ninth; every window of an
application shared one remembered position, so they opened on top of each
other; the title bar drew the application's name while the taskbar drew the
window's; and a click that opened a window left the keyboard talking to the
window that was clicked. BG-065 to BG-068.

**Services.** The parts of ReconOS that run are a list now, in Watchtower
beside Applications and Processes: the desktop shell, the control socket,
remote access, the firewall and networking. Each one says whether it is
running, stopped, or failed and with which error code, and how many times it
has been started this run -- one means a normal system, more than one means
somebody has been repairing something. Start, Stop and Restart. `services` in
the Terminal is the same list and the same registry.

Multitasking used to be a Control Panel page describing behaviour nobody could
change. It is a service now, which is what it always was.

**The desktop shell can be restarted.** It rebuilds the taskbar, the desktop,
the menus and the window management, and leaves your application windows open
-- so a taskbar can be repaired without costing you the document you were
writing. Whoever is signed in stays signed in: restarting the shell is a
repair, not a sign-out.

Two faults found by doing that, both of which had been waiting:

The UI font belonged to the shell and was freed with it, while every surviving
window still held the pointer. The first frame after a restart was a crash
inside the glyph rasteriser. The font belongs to the system now, loaded once
per size for the whole run.

Registering a built-in application twice was refused as a name collision, so a
restarted desktop had no Notepad and no File Explorer while the old windows
were still on screen. A built-in re-registering itself is an update in place;
a module trying to take a built-in's name is still refused.

**Bugs have numbers.** Every fault found in ReconOS -- sixty-two of them so
far -- is written down in `docs/BUGS.md` with an ID (`BG-001` upward), what it
actually was, how it surfaced, who found it, and what was done about it. The
ones found before today were numbered retroactively from the commit history.
They are GitHub issues too, so the record is public and dated.

---

## v0.2.16

**Errors have codes now.** When something goes wrong ReconOS names it —
`VT-A001`, `VT-G005` — so it can be written down and looked up. `errors` in
the Terminal says what a code means, `errors log` is what has happened on this
machine, and `docs/ERRORS.md` is the whole list for when you have a code and
not a working machine.

Three kinds: **STOP** (the system cannot continue and shows a screen with the
code on it), **fault** (something failed and the rest carried on), and **note**
(written down, nothing broke). If a run stops, the next start says so once,
with the code.

**A firewall.** Control Panel → Firewall, and `firewall` in the Terminal. It
decides what ReconOS itself opens and accepts: outgoing allowed, incoming
blocked, and a list of rules where the first match decides. Nine rules ship —
the incoming ones written down and switched off, so opening one is a switch
rather than remembering a port number. It is not the host's firewall and does
not touch it.

**The startup screen checks the system** instead of only counting it. Every
folder the system needs, the skins, the accounts, the programs, the firewall —
each line says what it found, and says the code when what it found is wrong.
Missing folders are rebuilt rather than only reported.

**Remote access, two ways.** Over SSH by forwarding the control socket, which
is encrypted and needs nothing from ReconOS; or over TCP 7420 with a key, which
is off by default and says plainly that the key crosses the network in the
clear. The firewall has to allow it either way.

## v0.2.15

**Help.** A new application, beside the Control Panel in the Start menu, with
a page for each part of the system and this change log underneath. The text is
written out of the system's own files every time it starts, so help describing
a version that is no longer running cannot survive an update.

**What changed, after an update.** The first time an account reaches the
desktop on a new version, a window says what that version brought, with an OK
button. Each account is told once. The whole log is in Help at any time,
back to the first version.

**The Control Panel and the Start menu have their own icons**, instead of
sharing the plain window that stood in for everything without one.

**Storage says where the room went.** The page used to be four notes about
things that do not work. It now measures what ReconOS owns — the system, the
programs, each account by name, the scratch space and the Recycle Bin — and
can empty the bin. How much room is left is still the host's answer, because
there is one filesystem and no volume layer under it.

**The Recycle Bin from the Terminal.** `bin` lists it, `bin <name>` puts
something in, `bin restore <name>` takes it back out, and `bin empty` clears
it. Before this the bin could only be reached from the File Explorer.

**Text that is not plain English draws properly.** Accents, dashes,
quotation marks and other alphabets used to be dropped silently — a sentence
would arrive with a hole where its punctuation should be. They are read as
UTF-8 now, in file names as well as in the help. A character the font has no
drawing for shows as an empty box rather than as nothing. A keyboard laid out
for a language with accents in it can type them, and Backspace removes a whole
character rather than a piece of one.

**The Update page says what version this is and what it brought**, with a
way through to the whole change log. It used to be three notes about things
that do not work; one of them stopped being true when the change log arrived.

**Notepad can replace.** Ctrl+H opens the find bar with a second field:
*Replace* changes the match in front of you and moves to the next, *All*
changes every one. Both undo.

**Programs written for Wayland get a ReconOS title bar.** A program that
was not written for ReconOS used to arrive with a frame of its own, in
somebody else's colours, with its own buttons. Now it is framed the way every
other window is — same colours, same buttons, same corners — and can be
dragged, resized, minimized, maximized and closed like one. A program that
insists on drawing its own frame still may; it is not given a second one.

**F1 opens the help about whatever you are looking at.** From Notepad it opens
Writing; from a Control Panel page it opens the page about that page.

**Three quiet bugs, found by clearing the compiler's warnings.** The File
Explorer's folder drop-down could point at the wrong place for a deeply nested
folder; a wallpaper or account picture with a very long file name was listed
but could not be loaded; and a file with a very long name could be moved to
the wrong place on its way to the bin. The build now compiles with nothing to
say, so the next warning will be one somebody notices.

**Skins can be written here.** *Copy This Skin* on the Appearance page writes
the chosen one out under a name of your own; *Edit This Skin* lists every
colour with a swatch and lets you change it. The change is immediate and is
saved as you make it. The ten that ship are built in and cannot be edited —
copy one first. `theme copy <name>` does the same from the Terminal.

**Type in the Start menu to find something.** The list narrows to names
containing what you typed; the arrows move the highlight and Enter opens it.
Escape steps back one thing at a time. The menu took no keys at all before
this, not even Escape.

**Every button is the same shape.** The rounded corners a skin asks for were
written in three places and every button that had not been rewritten stayed
square. On Beacon that meant two shapes of button in one window.

## v0.2.14

**Programs arrive as packages.** A `.rpk` is a folder with a manifest in it,
so a program can bring an icon and other files rather than being one file of
code. Installing writes a receipt naming everything it placed, and removing
takes back exactly that. The receipt is written before the program is loaded,
so one that refuses to load can be rolled back instead of leaving files
nothing knows about.

**Find in Notepad.** Ctrl+F opens a bar above the status line. It ignores
case and wraps around the end of the document.

## v0.2.13

**Removing an account can keep or delete its files.** They are two decisions,
and only one of them can be undone.

**The Start menu's power buttons are icons**, in the bottom right. The name of
whichever one you point at appears on the left.

**Buttons have rounded corners** where the skin asks for them, and the startup
screen is slower, because it went past faster than it could be read.

## v0.2.12

**A text clipboard.** Cut, copy and paste work in Notepad, in every text field
in the system, and into the Terminal. Before this, text could not be moved
from one place to another at all.

**Notepad can select text** — with the mouse, or Shift and the arrow keys.

## v0.2.11

**Notepad can undo**, by word rather than by keystroke, and has an Edit menu
so the shortcuts are findable.

## v0.2.10

**Skins can change the shape of a window**, not only its colours: the height
of a title bar, the thickness of its border, how far its corners are rounded,
and how big its buttons are.

## v0.2.9

**Desktop icons can be dragged**, and stay where they are put.

**Files open when you click them.** A text file opens in Notepad. Before this,
nothing in the system opened a file by being clicked.

## v0.2.8

**Properties** tells you what something is: its name, kind, size, where it
lives and when it last changed. It had been in the menu and greyed out since
the menus existed.

## v0.2.7

**Four desktops.** Alt+1 to Alt+4 switches between them; the numbers on the
right of the taskbar do the same. Alt+Shift and a number takes the current
window with you.

## v0.2.6

**Windows snap to the edges.** Drag one to the left or right edge for that
half of the screen, or to the top to fill it.

**Alt+Tab works again.** It had quietly stopped working when ReconOS began
drawing its own windows.

## v0.2.5

**The registry can be changed from the Control Panel**, not only read. Saving
redraws the system, so a setting takes effect while you watch.

## v0.2.4

**Skins can be installed.** `theme install` takes a skin file and adds it to
the list; `theme remove` takes it away.

## v0.2.3

**Gradients.** A skin can ask for a surface to fade from one colour to
another, which is what the Beacon skin needed to look like the era it is
reaching for.

**A startup screen**, with the Recon Towers mark and a count of what the
system found as it came up.

## v0.2.2

**All Programs** at the foot of the Start menu lists everything installed. The
column above it is what this account opens most.

## v0.2.1

**Connections that carry data**, with a rule about which programs may open
one. **Screen capture** on Print Screen. **Installing and removing programs.**
**Wallpapers the system draws for itself.**

## v0.2.0

**The network, seen but not implemented.** ReconOS has no kernel, so it has no
network stack of its own; it reports the host's, and says so on screen.

## v0.1.3

**Choose an account, then sign in** — two screens rather than one, so nothing
about how an account signs in is shown before one is chosen.

**Updates announce themselves** on the first start after one.

## v0.1.2

**Setup that looks like it belongs to something**, with the Recon Towers mark
and a picture of each skin. **Account pictures.**

## v0.1.1

**One account at a time, properly.** Each account gets its own settings, its
own folders and its own windows.

**A Control Panel** with the shape of the whole system in it, including the
parts that are not built, each saying what has to exist first.

## v0.1.0

The first version that was a usable desktop: it sets itself up on first run,
asks who you are on every run after, and gives that person a desktop of their
own. A taskbar, a Start menu, windows, a file explorer, a terminal, a task
manager, Notepad and a calculator.
