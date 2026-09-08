# Bugs

Every fault found in ReconOS, what it actually was, how it surfaced, and what
was done about it.

This exists because a list of commits is not a track record. A commit says
what changed; it does not say that something was broken, that somebody hit it,
or that it is fixed now. Sixty-odd faults have been found in this system so
far and until today none of them had a name. They do now.

## How a bug is named

`BG-` and a number, assigned in the order the fault was **found**:

```
BG-001 ... BG-999
```

Numbers are never reused, never renumbered, and never retired. A closed bug
keeps its number forever, because the number is how the fix is referred to
afterwards — in a commit, in a change log, in a conversation six months from
now about whether this has happened before.

BG-001 through BG-059 were assigned retroactively on 4 September 2026, from
the commit history, and are in the right order relative to each other. From
BG-060 on, a number is taken when the fault is found.

### Why not lettered, like the error codes

The [error codes](ERRORS.md) are `VT-A001`, `VT-G004` — a letter for the area
of the system, then a number. That is right for them and wrong here.

An error code is a **category**. It is written into the source once and raised
every time that kind of thing goes wrong, on any machine, forever, so it wants
grouping: all the storage faults together, all the network faults together.

A bug is an **event**. It happened once, in one place, and was fixed. Grouping
events by subsystem sounds tidy and buys nothing: nobody ever needs "all the
bugs in the display layer" as a contiguous range, and the moment one bug spans
two subsystems — as several below do — the letter has to be guessed at. So:
one sequence, in the order things were found, and the area is a label rather
than part of the name.

## What each entry records

- **Found in** — the version that was running when it surfaced.
- **Found by** — who or what surfaced it. `Joshua` means it was reported from
  actually using the system, which is how most of the interesting ones arrive.
  The rest name the instrument: a screen capture, the compiler, injected
  input, a measurement.
- **Was** — what was actually wrong. Not the symptom: the cause. A register
  that records symptoms is a register nobody can learn anything from.
- **Fixed in** — the version, and the commit.

Bugs still open say **Open** and why.

## Where this is tracked

Every bug here is also a [GitHub issue](https://github.com/neogentrics/ReconOS/issues),
titled with its `BG-` number and labelled `bug` plus its area. The issue is
where discussion happens; this file is the durable record. If they disagree,
this file is wrong and should be corrected — the issues carry the timestamps.

Features, patches and releases are tracked the same way: see
[Labels](#labels) at the foot of this file.

---

## Open

None. Every bug below was found and closed; the list is kept in full
rather than pruned, because a register that only shows what is currently
broken says nothing about the work.

---

## Fixed

### BG-105 — A register that says nothing until the port is already running

- **Found in** v0.0.12, by the kernel session, on the one boot path where no
  firmware had touched the hardware first.
- **Was** AHCI's port signature register does not describe the socket. It holds
  the first four bytes the attached device *sent*, in a frame — so it means
  nothing until the port is receiving them, and reads as all ones before that.
  The driver read it early and rejected a plainly attached disk as "not a disk".
- **Why it survived** three of the four boot paths had firmware that had already
  started every port, so reading it early worked.

  **This is the third failure shape's nastier sibling, and it is worth
  separating from its parent.** The ordinary third-shape bug is *correct code on
  hardware that cannot show the fault* — answered by running it somewhere it can
  fail. This one is different: the code was wrong from its first line, and
  something else was quietly doing the setup that made wrong code work.

  Not *nothing to run on* but **something else was covering for it**, and that is
  worse in two ways. The covering party is invisible from inside the code — the
  driver cannot tell that the port it is reading was started by somebody else.
  And the covering gets *more* reliable over time rather than less, because
  firmware improves. A fault of this kind becomes harder to find every year it
  survives.

  The remedy is the same in form and stricter in practice: the rig has to keep a
  path with **nothing helpful on it**. `scripts/verify-kernel.sh` now keeps a
  boot with no firmware at all and one with no disk at all, and requires that a
  run given a disk says it found one. The last of those caught something on its
  first run.

  The desktop's equivalent gaps, named so they are a list rather than an
  omission: everything here still runs on one screen size, one renderer, one
  account, one build configuration — and, now, always with a font present.

### BG-104 — An unimplemented region reporting exactly four gigabytes

- **Found in** v0.0.12, by the kernel session, the moment the sizes were
  printed.
- **Was** sizing a PCI base address register fills the high half with ones so
  that one code path can serve both register widths. For an unimplemented
  register that produced a reported size of exactly four gigabytes.
- **Why the finding matters as much as the fault** nothing had gone wrong yet.
  A four-gigabyte region never fits the window, so it was never placed, and the
  bug had sat through two commits doing no damage.

  It became obvious the instant the value was printed rather than reasoned
  about: **a host bridge with six four-gigabyte regions is not a thing.** No
  amount of reading the sizing arithmetic says that; one line of output says it
  immediately, because the reader knows what PCI devices look like and the code
  does not.

  Both of this session's OCR faults were found the same way and neither would
  have been found by reading. A letter that came out 9x10 from a page and 7x9
  from the rasteriser that drew it is a sentence; the antialiasing argument
  behind it is a paragraph nobody would have got right.

### BG-103 — A 64-bit base address restored only in its low half

- **Found in** v0.0.12, by the kernel session.
- **Was** sizing a 64-bit PCI base address register requires writing all ones
  and reading back, then restoring what was there. Only the low half was
  restored. The size came out right; the base then read as
  `0xFFFFFFFF_FE000000`, which the direct map turned into a non-canonical
  pointer and the processor into a general protection fault three functions
  later.
- **Why it was hard to see from the crash** the fault surfaced three functions
  away from the register that caused it, in code that was correct.

### BG-102 — A clock test that fails for two hours out of every twelve

- **Found in** v0.3.1, at ten o'clock at night, by running the suite for an
  unrelated reason.
- **Was** the twelve-hour clock must never show a zero hour, and the check was
  `strstr(twelve, "0:") == NULL`. That substring also occurs in `"10:"`.

  So the test failed at ten in the morning and ten at night, and passed at the
  other twenty hours of the day. It had been in the tree since the clock was
  written and had never once been run inside either of those hours.

- **Fixed by** asking about the field instead of about a substring. The hour is
  the start of the string, so a zero hour is a leading `'0'`.
- **Why it matters more than a one-character fix** nothing about this test was
  meant to depend on the time. It became time-dependent by looking for a digit
  and a colon rather than for the thing it meant, and a suite that is green
  twenty hours a day is worse than one that is red: it teaches everybody that a
  red run is a fluke.

  **A test that reads a formatted string with `strstr` is testing the
  formatting, not the value.** The general form is worth carrying, because it is
  the same shape as BG-100 one layer up: both checks were written against an
  example of the output instead of against the property.

### BG-101 — The engine named an 'l' as an 'I', which is the one thing it must
not do

- **Found in** v0.3.1, before the matcher shipped, by reading a real screenshot
  rather than the sample the test draws for itself.
- **Was** the whole point of the design is that a wrong answer is worse than no
  answer. It read `"the lazy dog"` as `"the Iazy dog"`.

  There is a rule for exactly this. Two candidates that score 985 or more
  against each other are recorded as twins -- the same shape in this face at
  this size -- and a mark matching one is refused. Measured rather than
  assumed: in DejaVu Sans `l` and `I` score **exactly 1000** at 10, 12, 14, 16
  and 20 pixels. They are not similar. They are the same shape.

  The rule did not fire because it only ever compared the winner against the
  *runner-up*. `I` won, something else placed second, and nothing looked at `l`
  at all.

- **Fixed by** refusing any candidate that has a twin, rather than one whose
  runner-up happens to be its twin.
- **Why it survived** `include/recon_ocr_match.h` already stated the stronger
  rule -- *"at or above RECON_OCR_TWINS they are one shape here, and no mark
  will be named either of them"*. The header was right and the code was weaker
  than its own documentation.

  **That is the worst direction for those two to disagree in.** A comment that
  overstates what the code does is not a stale comment, it is a promise the
  reader checks against and stops verifying. BG-087 and BG-094 are the same
  hazard with the error in the other direction; this one is more dangerous,
  because a weaker implementation behind a stronger promise fails only on the
  inputs nobody wrote the test for.

  And the test could not have caught it: the sample string was chosen with no
  `l`, no `I` and no `1` in it, deliberately, so that a first test would not
  depend on a face-specific difference. That reasoning was sound and it also
  removed the only characters that could have exposed this.

### BG-100 — One pixel of ascender decided whether a line kept its words

- **Found in** v0.3.1, within an hour of the code being committed, by running
  it on a screenshot of ReconOS's own text instead of on rectangles.
- **Was** the width a gap had to reach before it counted as a space was a
  quarter of the text band's height.

  Three lines were read. The second and third came out perfectly — nineteen
  marks in groups of 5, 4, 3, 4, 3, which is exactly *"jumps over the lazy
  dog"*. The first split almost every letter into its own word.

  The difference between them was one pixel. Line one's band is 11 rows tall,
  so its threshold was 2; line two's is 12, so its threshold was 3. At
  14-pixel text the ordinary gap between two letters is 2.

- **Fixed by** measuring the thing being split. In a line of rendered text the
  gaps fall into two populations — between letters, and between words — and the
  letter gaps are the large majority. So the median gap *is* a letter gap, and a
  word gap is a multiple of it. When no gap is much wider than the median, the
  line has no word breaks at all, which is the right answer for `0123456789` and
  is one that no fixed threshold can give: a threshold admitting a word break in
  one line invents them in another.
- **Why it survived the tests** the synthetic test used a 20-pixel band with
  2-pixel letter gaps and a 30-pixel word gap. That passes at **any threshold
  between 3 and 29**. The test was not weak by accident — it was built to check
  that wide gaps and narrow gaps are told apart, and it checks exactly that.

  **A test built from a made-up example tests the example.** The rectangles were
  drawn with the gaps a person would draw to make the two cases obvious, and
  obvious cases are the ones no threshold gets wrong. What the fault needed was
  a *typical* case — gaps 2 and 5 rather than 2 and 30 — and nobody invents a
  typical case, because typical is the thing you have to go and measure.

  The general form, and the reason this register keeps saying it: the synthetic
  test pins the arithmetic and the real input finds the fault. Neither replaces
  the other, and a project with only the first has a suite that goes green on
  a system nobody could use.

### BG-099 — The position bar never moved on its own

- **Found in** v0.3.1, by noticing that the video's position advanced and the
  MP3's did not, in the same run, reading the same clock.
- **Was** the player asked the sound device where it had got to only while it
  was drawing, and nothing made it draw. The clock was correct the whole time
  and the number on screen was however old the last redraw was.

  It had never been visible because the window is redrawn whenever anything
  happens to it — a click, a move, a focus change, a menu opening somewhere
  else. During use that is often enough that the bar looks like it updates
  erratically; during an automated test, where nothing touches the window, it
  never updates at all.

- **Fixed by** running the timer whenever anything is playing rather than only
  when there is a picture, at a quarter of a second for sound and sixty times a
  second for video, and redrawing only when the second actually changes.
- **Why it survived** it predates video by several versions and was never
  looked for, because the failure of a clock display is indistinguishable from
  a clock that is not running — and the clock genuinely had not been running
  once before, for a different reason, which had been found and fixed. A second
  fault behind a first one that produced the same symptom.

  The thing that surfaced it was **two things that should have agreed and did
  not**: the same window, the same clock, two files, two different answers. That
  is worth more than either observation alone, and neither was being looked at
  on purpose — the video was being checked and the MP3 was the control.

### BG-098 — Seeking worked at every whole second and nowhere else

- **Found in** v0.3.1, by a seek test that happened to include two times with a
  decimal point in them.
- **Was** seeking sets a line before which decoded frames are used to prime the
  decoder and not shown — everything between the last keyframe and the target
  exists only so the target has something to have been predicted from. That
  line was set to *the time that was asked for*.

  A frame is not a moment, it is an interval. The frame containing 33.7 seconds
  starts at 33.667, so refusing to show anything before 33.7 refused that frame
  — and the next one does not begin until after 33.7 either. The seek decoded
  correctly, threw the answer away, and produced no picture at all.

- **Fixed by** setting the line at the target frame's own start time, read from
  the sample table, rather than at the time the caller named.
- **Why it survived** every seek to a whole number of seconds worked perfectly,
  because whole seconds land on frame boundaries at 25 and 50 frames a second.
  Five of the seven times in the first version of the test were round numbers,
  and all five passed.

  **A test whose inputs are all round numbers is testing the arithmetic of round
  numbers.** The habit worth keeping is not "add a decimal" — it is to ask, for
  any test that passes, which property of the *inputs* it might be relying on.
  Round numbers, powers of two, square pictures, even dimensions, and one item
  in the list are the same mistake wearing five hats.

### BG-097 — Every reordered video would have played at the wrong times

- **Found in** v0.3.1, before it shipped, by checking which boxes the test file
  actually contained instead of assuming the ones being read were the ones that
  mattered.
- **Was** frame times were taken from `stts`, which gives the time a frame is
  *decoded*. The time a frame is *shown* is that plus the offset in `ctts`, and
  `ctts` was not being read at all.

  The two are the same number only for a file with no B-frames. A B-frame is
  predicted from a picture that comes after it, so it has to be decoded after
  that picture and displayed before it — which is exactly what the offset
  records. Without it, a reordered file plays its frames in the right order at
  the wrong moments.

- **Fixed by** reading `ctts` and adding the offset, clamped at zero for a file
  whose first frames carry negative offsets.
- **Why it nearly survived** the test file has no `ctts` box. Every check
  passed, against ffmpeg, frame by frame, at five scales — because the file
  does not reorder, and on a file that does not reorder taking decode times as
  display times is correct.

  **A comparison against a reference decoder proves agreement about the file it
  was run on, and nothing else.** The thing that found this was not a better
  comparison; it was looking at what the file *was* — and the first scan
  reported every box absent, including `stts`, which every MP4 must have. An
  answer that says a mandatory thing is missing is not a finding, it is a broken
  instrument: `moov` was at the end of the file and the scan read the first
  eight megabytes.

### BG-096 — A video track's parameter sets vanished, eight bytes at a time

[#261](https://github.com/neogentrics/ReconOS/issues/261)

- **Found in** v0.3.1, by a test written for the MP4 demuxer, one check after
  the check it was written for.
- **Was** the fixed part of an MP4 video sample entry is seventy bytes. The
  demuxer skipped seventy-eight -- eight too many, which lands inside the
  `avcC` box that follows it. The scan for `avcC` therefore started in the
  middle of one and found nothing, and the track reported no setup data at all.

  Everything either side of it was right: the dimensions, the frame count, the
  duration, the sample table, and every audio frame matching ffmpeg's byte for
  byte. It would have looked like a working demuxer until the first thing that
  tried to decode video -- at which point there would have been no parameter
  sets to start from, and H.264 without its SPS and PPS does not fail loudly,
  it produces nothing.

  The count is written out in the comment now rather than stated: two
  pre-defined, two reserved, twelve more, width, height, two resolutions, four
  reserved, a frame count, a thirty-two byte name, a depth, a final
  pre-defined. Seventy. A number that can be recounted is one somebody can
  check; a number that is asserted is one they have to trust.
- **Fixed in** v0.3.1, and the test now checks that a video track carries its
  parameter sets rather than only that it has a size.

### BG-095 — Page tables twenty-four bytes off a page boundary

[#262](https://github.com/neogentrics/ReconOS/issues/262)

- **Found in** v0.0.10, by the kernel session, after moving three scratch words
  into the boot region.
- **Was** the processor ignores the low twelve bits of every pointer to a page
  table. Nudging the boot region by three words pushed the x86 page tables
  twenty-four bytes off a page boundary — so the entries were written where the
  code put them and read from where the hardware rounded to.

  Everything about the code was correct. The alignment was structural, nothing
  checked it, and the kernel printed nothing at all.

  **The same family as BG-096's `avcC` offset**, and worth reading beside it:
  in both, a number that describes *where something is* was wrong, everything
  that used the number was right, and the result was silence rather than an
  error. An offset has no natural place to be validated — it is not a value
  anything computes twice — so a wrong one produces a system that reads
  plausible garbage and carries on.
- **Fixed in** v0.0.10. The tables are aligned explicitly, and the alignment is
  asserted rather than arranged: an arrangement holds until somebody moves
  something, which is exactly what happened here.

### BG-094 — Secondary processors erased the page tables they were sharing

[#263](https://github.com/neogentrics/ReconOS/issues/263)

- **Found in** v0.0.10, by the kernel session, on four processors — and not on
  two.
- **Was** every aarch64 secondary processor zeroed the shared page tables in
  order to rebuild them identically. The comment above it said that was
  harmless, on the grounds that the result was the same either way.

  The result was. The *interval* was not. Between the zeroing and the rebuild
  the other processors' translations do not exist, and a processor that takes
  an instruction fetch in that window is simply gone. It failed about a quarter
  of the time on four processors and never on two.

  **A comment and the code under it disagreed, and this time the comment was
  wrong** — the mirror image of BG-087, where the comment described the correct
  design and the code did something else. Both are the same underlying hazard:
  the comment is what the next reader checks against instead of the behaviour,
  so a confident wrong one is worse than none.

  Worth recording in the author's own account: the comment was written
  deliberately, while looking straight at the code, and it said *"harmless: it
  rebuilds them identically"*. That is a true statement about the end state.
  The bug lives in the interval, and the sentence never mentions one.

  **A comment that reasons about the destination will always miss a bug that
  lives in the journey.** Worth carrying because it is checkable: when a
  comment explains why something is safe, ask whether it describes a *result*
  or a *duration*. Concurrency, interrupts and reentrancy all live in the
  second, and none of them are visible from the first.
- **Fixed in** v0.0.10. A secondary uses the tables that already exist rather
  than rebuilding what is already correct.
- **Why it is its own shape.** Not "true when computed, and nothing arranged to
  notice when it stopped being true"; not "correct until a second caller
  arrived". This one was wrong from the first line it was written, on every
  machine that could expose it — and the machine it was tested on could not.
  See the note at the end of this file.

### BG-093 — The aarch64 exception vectors destroyed the first argument

[#259](https://github.com/neogentrics/ReconOS/issues/259)

- **Found in** v0.0.10, by the kernel session, at the moment user mode needed
  it.
- **Was** each vector slot began `mov x0, #index` to record which exception had
  been taken, and only then saved the registers. `x0` was already gone.

  Harmless for as long as every exception was a fault: a fault handler wants to
  know *which* fault, and nothing it does depends on what the faulting code had
  in its first register. A system call is the opposite — `x0` is the first
  argument, and it was being overwritten by the vector number before anything
  could read it.

  **The same shape as BG-091 two entries down, in a different language.** A bug
  that sat correctly in shared code because the only caller happened not to
  exercise the broken part, and that became visible the instant a second caller
  arrived. Neither was findable earlier except by having had the second caller,
  which is worth saying plainly: some bugs have no earlier moment at which they
  could have been caught.
- **Fixed in** v0.0.10. The frame gained a slot for the vector number, and
  `x0` is saved before anything writes to it.

### BG-092 — A page table that was right, over memory that was not

[#260](https://github.com/neogentrics/ReconOS/issues/260)

- **Found in** v0.0.10, by the kernel session, when a second user program was
  loaded.
- **Was** `vm_map` never invalidated the TLB when it replaced a mapping that
  was already live.

  Invisible for nine checkpoints, and not by luck: every mapping the kernel had
  ever made was made *once*, at an address nothing had touched, so there was
  never a stale translation to leave behind. The second user program was mapped
  at the same virtual address as the first — and read and ran the first
  program's page.

  **Correct page tables, wrong memory.** Everything the kernel could inspect
  about its own state was right; the processor was working from a copy of an
  answer that had been true earlier. That is the same shape as BG-089's tooltip
  and BG-084's page table, and it is the third instance on the kernel's side:
  something true when it was computed, with nothing arranged to notice when it
  stopped being true.
- **Fixed in** v0.0.10, on both architectures, invalidating only on
  replacement — a flush on every map would be correct and would also throw away
  the translations of everything else on every allocation.

### BG-091 — The taskbar kept the title a window had when it opened

[#258](https://github.com/neogentrics/ReconOS/issues/258)

- **Found in** v0.3.1. **Found by** the web viewer, in a screenshot: the title
  bar read "In defense of simple architectures" and the taskbar button under
  it still read "www.rfc-editor.org", which was the page before.
- **Was** `recon_appwin_set_title` redrew the window and nothing else. The
  taskbar is the shell's, and the shell redraws it when the window list or the
  focus changes -- neither of which a rename is.

  **Latent for as long as it existed, and not by luck.** The only thing that
  had ever changed a title was Notepad, showing the file being edited -- and
  that changes at the same moment the file is opened or saved, which is a
  moment the window is being redrawn anyway for other reasons. The web viewer
  changes its title on every page, with nothing else happening, and the fault
  became visible immediately.

  Worth keeping for that: a bug can be sitting in a shared function for months
  because the only caller happens to do something else that hides it. The
  second caller is what finds it, and there is no way to have found it sooner
  except by having had one.
- **Fixed in** v0.3.1. Setting a title refreshes the shell as well as the
  window. `recon_shell_refresh` already existed and is documented as "call
  when the window list or focus changes" -- a title is part of what that list
  shows, so this was a missing call rather than a missing mechanism.

### BG-090 — A password erased with memset, which a compiler may delete

[#257](https://github.com/neogentrics/ReconOS/issues/257)

- **Found in** v0.3.0, hours after it shipped. **Found by** the kernel session,
  from the other direction entirely: they were describing what a keyring would
  need from a kernel and named "memory that is actually erased when the session
  ends" as the hard one, because zeroing a buffer is a store nothing reads
  afterwards and a compiler may legally remove it.

  They were right, and it was already in my code.
- **Was** the Mail session and the Mail window both hold a password, and both
  did `memset(p, 0, sizeof(*p)); free(p);` on the way out, with a comment
  saying the memory was cleared because freed memory keeps what was in it.

  Those stores are never read. The standard permits deleting the call as dead,
  and at higher optimisation levels compilers do -- which produces a program
  whose source says the password was erased and whose binary leaves it in the
  heap for whatever gets that memory next. It has a number: CWE-14.

  **It had not actually been deleted.** `objdump` on the object file showed the
  `memset` call still there, immediately before the `free`. So this was not a
  live fault; it was a security property that happened to hold because of the
  flags this project builds with, and would have stopped holding on the day
  somebody added `-O3` -- silently, with nothing to notice.

  Worth recording for that reason rather than in spite of it. "Correct in this
  build" and "correct" are different claims, and the gap between them is where
  this kind of fault lives. Checking the binary was also what turned a guess
  into a fact in either direction: had it been removed, the comment would have
  been a lie for as long as it had been there.
- **Fixed in** v0.3.0. `recon_secure_erase` writes through a volatile pointer,
  which the standard does not permit to be elided, and the three places that
  held a secret use it. Verified in the object file: the call is there, before
  the free.

### BG-089 — A tooltip kept showing over the window that opened on top of it

[#256](https://github.com/neogentrics/ReconOS/issues/256)

- **Found in** v0.3.0. **Found by** looking at a screenshot taken to prove
  something else: the Programs page had the Control Panel's "What is
  installed" lying across the middle of it.
- **Was** `tip_under` asks every window front to back and stops at the first
  one the point is inside, so a tip cannot come from a window behind another
  one. That part was right and had already been fixed once.

  What was wrong is that it was only ever *asked* when the pointer moved. The
  pointer is not the only thing that moves. Open a window under a stationary
  cursor and the tip belonging to the window now behind it carried on showing,
  drawn on top of the window in front of it — and stayed until somebody moved
  the mouse.

  **A correct answer, cached against the wrong event.** The tip depends on two
  things, where the pointer is and what is under it, and only one of them was
  treated as able to change.
- **Fixed in** v0.3.0. The same question is asked again, with the pointer where
  it already is, whenever a window is opened, raised or closed.

### BG-088 — Installing an older applet deleted the newer one that refused it

[#255](https://github.com/neogentrics/ReconOS/issues/255)

- **Found in** v0.3.0. **Found by** the test written for applet versioning,
  on its first run, one step after the step it was written to prove.
- **Was** applet versioning added a rule: a module registering a name that is
  already taken wins only if its version is higher. That rule worked. What had
  no rule at all was *un*registering.

  The loader calls a module's `unload()` when its `load()` returns false, so a
  module that changed its mind can tidy up whatever it registered first. A
  module that lost the version comparison takes exactly that path — and its
  `unload()` calls `recon_unregister_app("Notepad")`, which removed whichever
  Notepad was registered. Not its own. The winner's.

  So installing an applet *older* than the one running silently uninstalled
  the running one, and reported only "declined to load". Installing a second
  older one then removed the built-in as well, and the system had no Notepad
  at all — no error, no log line, just a name that had quietly stopped
  existing.

  The shape is worth naming: **the guard was on acquiring the name and not on
  releasing it**, and a caller who fails the guard is handed the release path
  as its consolation. Every check that decides who owns something needs its
  mirror image, or the losing branch becomes the exploit.
- **Fixed in** v0.3.0. A module may only unregister what it registered:
  the module whose `load()` or `unload()` is running is named while it runs,
  and `recon_unregister_app` refuses a name belonging to somebody else. Fixed
  at the registry rather than at the loader, so it holds however unload comes
  to be called.

### BG-087 — The font cache's fallback claimed "nearest" and returned "first"

[#254](https://github.com/neogentrics/ReconOS/issues/254)

- **Found in** v0.3.0. **Found by** the kernel session, indirectly: they asked
  whether anything on this side pools or caches, and whether it gives the
  *pages* back rather than merely the bytes. Going to look answered a
  different question.
- **Was** `recon_font_system` caches six sizes. Its out-of-slots path carried
  a comment saying it returns the nearest size already loaded -- and returned
  `g_system_fonts[0]`, the size loaded *first*, which is the nearest only by
  coincidence.

  Latent until the same session's own change made it reachable: Notepad gained
  a text size somebody can walk from 9 to 32, so the seventh distinct size in
  a session would have come back as whatever the splash screen happened to
  want, silently, with the window reporting the size it had asked for.

  Two faults, and the second is the more interesting: **a comment and the code
  under it disagreed, and the comment was the correct design.** Code that does
  not do what the line above it says is worse than code with no comment,
  because the comment is what the next reader checks against instead of the
  behaviour.
- **Fixed in** v0.3.0. The fallback is actually nearest, and says which size
  it substituted. Notepad shares the system's font only at the size everything
  else draws at, and loads its own at any other -- so a window resized away
  from the default is not competing for cache slots with the chrome.

### BG-084 — A page table stopped being reachable at the instant it was installed

[#251](https://github.com/neogentrics/ReconOS/issues/251)

- **Found in** kernel 0.0.6. **Found by** booting it: a page fault whose
  faulting address was the root table's address plus the index being read.
- **Was** every pointer to a page table had been obtained through the map the
  kernel was handed on entry. Installing the kernel's own map replaced that
  world with one where only the kernel image is identity mapped -- and the
  root table is not in the kernel image, so the next read of it faulted. The
  fault address said so exactly: `CR2=0x5800` for a root at `0x5000` and index
  256, which is one of the rare faults that can be read straight off.

  The general shape, and it is the same as BG-081 one level up: **a structure
  that describes memory has to be reachable in the world it describes.** A map
  that cannot be read after it is installed is a map that can only be built,
  never maintained.
- **Fixed in** kernel 0.0.6. The tables are reachable through the map they
  themselves establish.

### BG-085 — Supported is not enabled, and CPUID answers the wrong one

[#252](https://github.com/neogentrics/ReconOS/issues/252)

- **Found in** kernel 0.0.6. **Found by** two of the four boot paths working
  and two not: the UEFI ones ran, the ones through our own trampoline faulted.
- **Was** bit 63 of a page table entry is the no-execute bit **only once
  `EFER.NXE` says so**. Until then it is a *reserved* bit, and setting a
  reserved bit does not mean "this page is executable" -- it means every
  access through that entry faults, whatever else the entry says. Firmware
  enables NXE for itself, so the UEFI paths inherited it; our own trampoline
  had never had a reason to.

  Worth recording as a pattern rather than as a fact about NX, because the
  fault is in the question: **CPUID reports that a feature is _supported_. It
  does not report that it is _enabled_.** Code that reads the first as the
  second is correct on every machine that happened to enable it already --
  and when the thing doing the enabling is firmware, that is every machine in
  a firmware-based test matrix.

  This shape exists on the desktop side too. `recon_procinfo` reads
  capabilities out of `/proc`, which is a place that answers "what does this
  processor have" and not "what is currently switched on".
- **Fixed in** kernel 0.0.6. NXE is enabled before any entry sets bit 63.

### BG-086 — The direct map described a hole, twice the size of the machine

[#253](https://github.com/neogentrics/ReconOS/issues/253)

- **Found in** kernel 0.0.6. **Found by** the author, who had written BG-082
  three days earlier and did not recognise it in a different coat.
- **Was** the direct map was built as one span from zero to the highest
  address in the memory map. This machine's map ends with twelve gigabytes of
  reserved space near the 1TB mark, so that span was 524,288 entries and 4MB
  of page tables, almost all of it describing memory that is not there.
  Region by region it is 29 pages.

  Recorded because it is the *second* time the same assumption cost something,
  by the same author, within a week: **a memory map is not a range.** It is a
  list of regions with holes between them, and the holes can be larger than
  the machine. BG-082 was that assumption in a count and this is that
  assumption in an extent.

  Two of a kind is worth more written down than one, which is the whole
  argument for a register that nobody prunes.
- **Fixed in** kernel 0.0.6. The map is built region by region.

### BG-082 — A memory-map limit derived from three machines, wrong the first time our own loader ran

[#249](https://github.com/neogentrics/ReconOS/issues/249)

- **Found in** kernel 0.0.5. **Found by** running the new UEFI loader against
  OVMF, and a warning that had been added defensively and had never fired.
- **Was** `BOOT_MAX_REGIONS` was 64, with a comment reasoning that real
  machines report well under thirty regions. That was true of every machine
  tested when it was written. Booting through our own loader, OVMF reported
  **86**; the kernel silently kept 64 and dropped 22, then reported 487MB
  usable where the truth was 505MB.

  The general shape is the part worth keeping: **firmware fragments its memory
  map as it allocates, so the region count reflects how much work the firmware
  did before handing over, not how much memory the machine has.** A limit
  derived from observing three machines is a guess wearing evidence's clothes,
  and it fails the moment the thing doing the handing-over changes -- which is
  exactly what replacing GRUB was.

  It was caught only because a warning existed for a case nobody expected to
  happen. Silently keeping 64 of 86 is the failure that produces a wrong
  number rather than an error.
- **Fixed in** kernel 0.0.5. The limit is 256, and the warning stays, because
  the next thing to hand over a memory map will not be OVMF either.

### BG-083 — The same range of memory, claimed twice, with nothing saying which claim won

[#250](https://github.com/neogentrics/ReconOS/issues/250)

- **Found in** kernel 0.0.5. **Found by** reading the loader's own memory map
  output, where one range appeared under two names.
- **Was** a range could be described as loader memory *and* as kernel image,
  both truthfully, and the printed map showed both with nothing to say which
  described what the memory actually is now. Underneath, the carve-out only
  subtracted non-usable from usable, so overlapping claims of the same kind
  resolved by whichever happened to be applied last.

  This generalises past the kernel and is worth writing down as a pattern: two
  true statements about one thing, with no rule for which is operative, is not
  a display problem. It is a missing decision, and printing both is what a
  program does when it has not made one.
- **Fixed in** kernel 0.0.5. An explicit priority: kernel over bad over ACPI
  over reserved over bootloader over usable. The more specific claim wins, and
  the map shows one answer per range.

### BG-081 — The page allocator counted a gigabyte of nothing as used

[#234](https://github.com/neogentrics/ReconOS/issues/234)

- **Found in** kernel 0.0.3. **Found by** the allocator's own self-test, run at
  boot on real hardware rather than on the build machine.
- **Was** the physical page bitmap was indexed from physical address zero. On
  x86_64 that is invisible: RAM starts near zero, so the first bit and the
  first page are the same thing and nothing looks wrong. On aarch64 RAM starts
  at 1GB, so every bit below that stood for an address that is not memory --
  262,433 pages reported as used that were never pages at all.

  The shape of this is worth keeping: an assumption that is *true on the
  machine you develop on* and false on the other one, in code written to be
  portable. `make check-portable` catches machine-specific code in `core/`; it
  cannot catch machine-specific *arithmetic* that compiles everywhere.
- **Fixed in** kernel 0.0.3. The bitmap is based at the lowest usable address
  rather than at zero.

### BG-080 — A font inside ReconOS could be set and would never load

[#233](https://github.com/neogentrics/ReconOS/issues/233)

- **Found in** v0.2.17. **Found by** screen capture, testing the new font
  picker: the page said which font was on and every letter on the screen was
  still the old one.
- **Was** `recon_font_load` opens the file itself, so it wants the path the
  *host* keeps it at. `recon_access_apply` passed it whatever the setting held
  -- and a font installed into ReconOS is named by its place inside ReconOS,
  which is not that. Everything above the loader reported success: the key was
  written, the page named the font, and `recon_font_reload` was documented to
  leave the old typeface in place when a file cannot be read, which is exactly
  what it did. The terminal's `access font /System/...` had the same fault and
  nobody had tried it, because until this version there was nowhere inside
  ReconOS for a font to be.
- **Fixed in** v0.2.17. `recon_access_apply` resolves the setting to a host
  path before loading, and passes anything that does not resolve through
  untouched so a host path somebody typed still works. The Control Panel
  checks through the same resolution, so what it reports is the loader's
  answer rather than a second opinion about a path the loader never sees.

### BG-078 — A dialog asked for four buttons and silently got three

[#231](https://github.com/neogentrics/ReconOS/issues/231)

- **Found in** v0.2.17. **Found by** screen capture, building the New Skin
  question: the dialog came up with Light, Dark and High Contrast and no
  Cancel.
- **Was** `recon_shell_ask` clamped to `RECON_DIALOG_BUTTONS_MAX`, which was
  three, by cutting the tail. The tail is the way out: the contract is that
  callers put the safe answer last and Enter and Escape both choose it. So a
  question with one button too many did not lose an answer -- it lost its
  escape hatch, and Escape silently started confirming instead of declining.
  Three had been enough for every question there was, which is why a cap that
  cannot be right had never been wrong.
- **Fixed in** v0.2.17. The maximum is four, and truncation now drops from the
  middle: the last slot always keeps the caller's last button. Losing an
  answer is visible; losing the safety is not.

### BG-079 — A chosen wallpaper outlived the skin change it was promised to

[#232](https://github.com/neogentrics/ReconOS/issues/232)

- **Found in** v0.2.17. **Found by** screen capture, testing New Skin: the
  system went dark and the desktop stayed light.
- **Was** the Wallpapers page says a picture chosen there "stays until the
  skin changes again". `recon_wallpaper_current` preferred the account's
  choice over the skin's suggestion unconditionally, so the first picture
  anybody chose was the last one they would ever see -- no skin could put its
  own on again. The page and the code had disagreed since wallpapers became
  choosable, and the page is the promise.
- **Fixed in** v0.2.17. `recon_wallpaper_set` records the skin in force
  alongside the picture, and the choice applies only while that skin is still
  on. Changing the skin is also somebody saying what they want, and it is the
  more recent of the two.

### BG-062 — What's New reopens after a shell restart

[#2](https://github.com/neogentrics/ReconOS/issues/2)

- **Found in** v0.2.16. **Found by** injected input, testing BG-061.
- **Was** the "what changed in this version" notice fires from the same code
  path a sign-in runs, and restarting the desktop shell runs that path again.
  Somebody who has not yet dismissed the notice gets it raised in front of
  them a second time. Harmless, and wrong: a shell restart is a repair, not a
  new session, and it should not put a window in front of what somebody was
  doing.
- **Fixed in** v0.2.17. `adopt_signed_in_user` takes an `arriving` flag. A
  sign-in passes true; a shell restart passes false, and only the true case
  raises the notice. The notice stays due either way -- it is still unread,
  and it will be shown the next time somebody actually signs in.

### BG-063 — The first row on the Services tab was drawn through the header rule

[#81](https://github.com/neogentrics/ReconOS/issues/81)

- **Found in** v0.2.17. **Found by** screen capture: the tab had been built and
  never looked at.
- **Was** `recon_draw_text` takes a **baseline**, not a top edge.
  `draw_service_rows` computed `ry + (ROW_HEIGHT - ascent) / 2`, which is a top
  edge, so every row sat about ten pixels high and the first one had the column
  header's rule through the middle of it. Every other row-drawing function in
  the file already used `ry + (ROW_HEIGHT + ascent) / 2 - 2`.
- **Fixed in** v0.2.17. The same formula as its neighbours.

### BG-064 — Clicking a row on the Services tab selected nothing

[#82](https://github.com/neogentrics/ReconOS/issues/82)

- **Found in** v0.2.17. **Found by** injected input, in the same look as
  BG-063: the row did not highlight and the button stayed on Start.
- **Was** the row-click handler has a branch per tab and Services had none, so
  a click fell through to the process branch. That looked the row up in the
  process list -- the wrong list -- and set `selected_pid` rather than
  `selected_row`, which is what the Services tab reads. With nothing ever
  selected, Start, Stop and Restart could not act on anything.
- **Fixed in** v0.2.17.

### BG-060 — The Appearance page lists two wallpapers out of five

[#1](https://github.com/neogentrics/ReconOS/issues/1)

- **Found in** v0.2.16. **Found by** Claude, reviewing the page after Joshua
  asked for it to be split into sections.
- **Was** the skin list and the wallpaper list were on one page, and the skin
  list took `(height - y) / ROW_HEIGHT` rows — everything left. With ten skins
  installed there was room for two wallpapers out of five, and the other three
  were drawn past the bottom edge where nothing could see them. Adding Copy
  and Edit buttons between the two lists is what pushed it over.
- **Fixed in** v0.2.17, `4f84508`. Appearance is three sections and each has
  the whole window, so neither list can be squeezed by the other growing. Not
  fixed by arithmetic: an arithmetic fix would hold until the next thing was
  added between them.

### BG-065 — The shell would hold only eight windows

[#147](https://github.com/neogentrics/ReconOS/issues/147)

- **Found in** v0.2.17. **Found by** reading `struct recon_shell` while making
  the Control Panel open a window per item.
- **Was** `struct recon_appwin *apps[8]`. Seven built-ins and a Calculator is
  eight, so a ninth window was refused — and refused quietly, from the
  application's point of view: the window was built and drawn and had no
  taskbar button, took no clicks, and could not be reached by Alt+Tab. It
  looked like a window and behaved like a picture.
- **Fixed in** v0.2.17, `4f84508`. `RECON_SHELL_WINDOWS_MAX`, thirty-two,
  named rather than written into one array declaration — the second array that
  had to agree with it was the one that would have been missed.

### BG-066 — Every window of one application shared a remembered position

[#148](https://github.com/neogentrics/ReconOS/issues/148)

- **Found in** v0.2.17. **Found by** three Control Panel windows opening at
  exactly the same coordinates, three times running, after being told to
  cascade.
- **Was** `geometry_key` built the registry key from `win->impl->title` — the
  *application's* name. That is the same as the window's name for an
  application with one window, which until now was all of them. Every Control
  Panel item is built from one impl, so all fourteen shared a single saved
  position: they opened on top of each other, and moving any one of them wrote
  that position for all the rest.
- **Fixed in** v0.2.17, `4f84508`. The key comes from the window's own title,
  falling back to the application's.

### BG-067 — The title bar drew the application's name, not the window's

[#149](https://github.com/neogentrics/ReconOS/issues/149)

- **Found in** v0.2.17. **Found by** screen capture: the taskbar button said
  Firewall and the window's own title bar said Control Panel.
- **Was** `recon_appwin_set_title` existed and the taskbar read it. The title
  bar read `win->impl->title` directly. A header comment three functions away
  claimed both read through `recon_appwin_title` "so they stay in step", which
  was true of one of them.
- **Fixed in** v0.2.17, `4f84508`.

### BG-068 — A click that opened a window left the keyboard behind

[#150](https://github.com/neogentrics/ReconOS/issues/150)

- **Found in** v0.2.17. **Found by** injected input: the new window arrived in
  front and `state` still reported the old one focused.
- **Was** after offering a click to an application, the shell raised and
  focused the window that had been clicked — unconditionally, including when
  handling the click had deliberately focused something else. A Control Panel
  tile opened its window, focused it, and had focus taken straight back to the
  tile that opened it.
- **Fixed in** v0.2.17, `4f84508`. The shell notes which window held focus
  before the click, by identity rather than by index, and only focuses the
  clicked one if the application did not move focus itself.

### BG-069 — The issue script made 126 duplicate issues

[#211](https://github.com/neogentrics/ReconOS/issues/211)

- **Found in** v0.2.17. **Found by** the run's own output: entries it had
  created an hour earlier came back as new.
- **Was** `scripts/make-issues.py` reads the existing issues with
  `subprocess.run(..., text=True)`, which decodes using the platform's
  preferred encoding. On Windows that is the locale codepage, not UTF-8, so
  every em dash in a title — and every title in the register has one — came
  back as mush. No title ever matched, every entry looked new, and the script
  made a second copy of the whole register. Twice.
- **Fixed in** v0.2.17. `encoding='utf-8'` on every `gh` call, and a check
  that refuses to run at all if the listing comes back with no `BG-` titles in
  it: the failure is silent by nature, so the guard has to be about the shape
  of the answer rather than about the error that was never raised. The 126
  duplicates were deleted, and every bug kept its original issue number.
- **Note** the register exists to show that faults get found and fixed. A
  tool that fills it with noise is worth an entry of its own.

### BG-070 — Nothing in the Help window was clickable

[#212](https://github.com/neogentrics/ReconOS/issues/212)

- **Found in** v0.2.17. **Found by** Joshua: *"now nothing in the window is
  clickable. You can't even scroll."*
- **Was** the region that lets the sidebar take the mouse wheel covers every
  row in it, and was registered *after* them. The last region added wins, so
  it swallowed every click on the list: the topics were drawn, highlighted
  under the pointer, and could not be chosen. The window frame kept working,
  which is what made it look like only the inside was broken.
- **Fixed in** v0.2.17, `b9745f9`. A region that exists to catch what the
  others miss goes down before them, not after.
- **Note** introduced by the fix for BG-047, which added the scrolling. The
  fix for one fault is where the next one comes from more often than is
  comfortable.

### BG-071 — Markdown subheadings appeared with their hashes

[#213](https://github.com/neogentrics/ReconOS/issues/213)

- **Found in** v0.2.16. **Found by** screen capture, while confirming BG-070:
  the page read `### Making a skin of your own`.
- **Was** the help is written as Markdown and read from it, and the reader
  drew every line as plain text. Nobody writing the help asked for the hashes
  to appear on screen.
- **Fixed in** v0.2.17, `b9745f9`. A line beginning with hashes is drawn as a
  subheading, in the accent colour, with the hashes taken off.

### BG-072 — "Scroll for the rest" above a list that could not scroll

[#214](https://github.com/neogentrics/ReconOS/issues/214)

- **Found in** v0.2.17. **Found by** Joshua: *"under appearance or colors, it
  says scroll for the rest, but you can't scroll."*
- **Was** `panel_scroll` handled the skin editor and the registry and nothing
  else. The Colours section printed a line telling somebody to do something
  the page would not let them do, which is worse than not offering it.
- **Fixed in** v0.2.17, `b9745f9`. Every list in Appearance takes the wheel,
  clamps where it is drawn rather than where the wheel is turned, and shows a
  bar when there is more than fits. Themes and Wallpapers got it at the same
  time: neither overflows today and both will.
- **Note** it survived because nothing could turn the wheel from outside, so
  nothing tested it. `ui scroll` exists now. This is BG-038 again.

### BG-073 — An application's icon did not follow its windows

[#215](https://github.com/neogentrics/ReconOS/issues/215)

- **Found in** v0.2.17. **Found by** Joshua: *"Control panel's icon isn't
  system wide. It's only in the start menu."*
- **Was** two separate faults with one shape. The Control Panel was registered
  with `RECON_ICON_CONTROL_PANEL` in the Start menu's table and
  `RECON_ICON_SYSTEM` in its own window description, so the menu drew the
  sliders and the title bar and taskbar drew a generic square. And a window
  opened for a Control Panel item carried the Control Panel's icon rather than
  the item's — fourteen taskbar buttons nobody could tell apart.
- **Fixed in** v0.2.17, `b9745f9`. `recon_appwin_set_icon` alongside
  `recon_appwin_set_title`, each page window carries its own, and the title
  bar and taskbar both read through `recon_appwin_icon` so there is one answer
  rather than two that can disagree.
- **Note** the same shape as BG-067, a day apart: a thing with two names is a
  thing that will eventually be two different things.

### BG-074 — Every folder in the Start menu opened the same folder

[#222](https://github.com/neogentrics/ReconOS/issues/222)

- **Found in** v0.2.17. **Found by** Joshua: *"if I click documents, it should
  be loading me into... the documents folder. Instead, it just takes me to the
  user folder."*
- **Was** `recon_fs_user_dir` returns a pointer into one shared static buffer.
  The menu built the path, then called `recon_shell_open_named` on the next
  line to make sure the File Explorer existed — and building the explorer
  calls `recon_fs_user_dir(NULL)` to decide where to start, overwriting
  `/Users/Joshua/Documents` with `/Users/Joshua` before the pointer was ever
  read. Every place opened the account's own folder.
- **Fixed in** v0.2.17, `7426d91`. The path is copied into a local before
  anything else runs.
- **Note** the fault is not the shared buffer, which is a reasonable thing for
  a path accessor to have. It is holding the pointer across a call that could
  reach the same accessor. Worth checking the other callers for the same
  shape.

### BG-075 — Clicking the Start menu's search box closed the menu

[#223](https://github.com/neogentrics/ReconOS/issues/223)

- **Found in** v0.2.17. **Found by** Joshua: *"you can't click in it. When I
  click on it, it just closes the app's menu."*
- **Was** the box had a hit region and nothing handled it, so a click on it
  fell through to "somewhere in the menu that is not an entry", which closes
  the menu. Typing had always worked; what was missing was the box not
  throwing the menu away when somebody did the obvious thing and clicked it
  first.
- **Fixed in** v0.2.17, `7426d91`. The box takes the click and keeps the menu.
  Its phantom text was also drawn in the disabled ink — the colour of a thing
  that cannot be used — which on a warm skin made the box read as switched
  off; it is the dim surface ink now.
- **Note** introduced with the box itself, one commit earlier. A region added
  without a handler is a control that looks alive and is not.

### BG-076 — The firewall's rule list could not be scrolled

[#224](https://github.com/neogentrics/ReconOS/issues/224)

- **Found in** v0.2.17. **Found by** Joshua: *"Doesn't have the scroll
  ability, so you can't scroll through the different rules."*
- **Was** the same gap as BG-072, in the one list that had not been swept:
  `panel_scroll` did not know about the firewall page. With nine rules it
  happened to fit; the moment more shipped it did not.
- **Fixed in** v0.2.17, `7426d91`. It scrolls, with a bar, and the list
  follows the selection only when the selection moves — following it on every
  draw pinned the list and made the wheel do nothing at all, which was the
  first attempt.

### BG-077 — Nothing in the Start menu could be right-clicked

[#230](https://github.com/neogentrics/ReconOS/issues/230)

- **Found in** v0.2.17. **Found by** Joshua: *"You can't right click them. If
  you try to right click, it right clicks the desktop in the background."*
- **Was** `recon_shell_handle_right_click` closed the Start menu as its first
  act, before testing where the click had landed. So a right-click on an
  application fell through to whatever was behind the menu -- the desktop --
  and the desktop's own menu appeared instead.
- **Fixed in** v0.2.17, `e045523`. The menu is tested first, and closing it
  happens only once nothing in it has claimed the click. A right-click on a
  gap in the menu now leaves the menu open, which is the same fault in a
  smaller place.

### BG-001 — The screen stayed blank

[#3](https://github.com/neogentrics/ReconOS/issues/3)

- **Found in** v0.1.0 (pre-release). **Found by** running it: the compositor
  started, took the display, handled input, and drew nothing.
- **Was** `wlr_scene_attach_output_layout()` keeps scene outputs positioned in
  step with the layout; it does not create them. Without a scene output,
  `wlr_scene_get_scene_output()` returned NULL every frame and the frame
  handler returned early — silently, which is precisely why the fault was
  invisible.
- **Fixed in** v0.1.0, `cd6f9c3`. A scene output per output, plus reporting
  the failure instead of returning early in silence.

### BG-002 — The render loop never started

[#4](https://github.com/neogentrics/ReconOS/issues/4)

- **Found in** v0.1.0 (pre-release). **Found by** the same investigation as
  BG-001.
- **Was** nothing asked for the first frame. Every later frame is scheduled by
  the one before it, so with no first frame the loop never began.
- **Fixed in** v0.1.0, `cd6f9c3`. An initial frame is scheduled once the
  output is up.

### BG-003 — The screen rendered as bands of stale image

[#5](https://github.com/neogentrics/ReconOS/issues/5)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua, on the Hyper-V VM.
- **Was** partial redraws assume the driver hands back a buffer still holding
  the previous frame. `hyperv_drm` does not, so every region the scene
  considered unchanged was never filled in.
- **Fixed in** v0.1.0, `7263845`. Existing damage is widened to the whole
  output before committing — widened, never created, because marking the
  output damaged unconditionally makes every frame commit and every commit
  schedule another, which is a compositor redrawing forever at full speed.
  `RECONOS_PARTIAL_DAMAGE=1` opts back in where buffers are preserved.

### BG-004 — The wallpaper was rescaled on every frame

[#6](https://github.com/neogentrics/ReconOS/issues/6)

- **Found in** v0.1.0 (pre-release). **Found by** measurement, while
  investigating BG-003.
- **Was** a 4256×2832 image resampled to screen size during compositing, in
  software, sixty times a second.
- **Fixed in** v0.1.0, `7263845`. Downscaled once at load to the size it will
  be drawn at, so compositing copies rather than resamples. Idle RSS fell from
  59MB to 16MB. This is why the background is built after the first output
  reports its resolution rather than at startup.

### BG-005 — The cursor swallowed every click

[#7](https://github.com/neogentrics/ReconOS/issues/7)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua: windows could not be
  dragged, resized, or closed, while typing worked fine.
- **Was** the pointer was a red rectangle in the scene graph, raised above
  everything else. Hit testing asks for the topmost node at the pointer's
  position, so the answer was always the cursor. No window was ever found,
  pointer focus was cleared on every motion, and no client received a mouse
  event. Typing still worked because keyboard focus is assigned when a window
  maps and does not depend on hit testing — which is what made the fault look
  like a mouse problem rather than a hit-testing one.
- **Fixed in** v0.1.0, `4cce403`. wlroots' own cursor, drawn on the pointer's
  layer rather than in the scene, so it cannot intercept input.

### BG-006 — Black rectangles flickered over the shell's own windows

[#8](https://github.com/neogentrics/ReconOS/issues/8)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua. Worst on a maximized
  Watchtower, and visible with nothing moving on screen.
- **Was** every buffer handed to the scene graph pointed back at the panel's
  single pixel block. The compositor may still be reading a committed buffer
  while the next frame is drawn into that same memory, so frames were
  overwritten mid-read.
- **Fixed in** v0.1.0, `e0b7fc5`. A committed buffer owns its pixels. The cost
  is a copy per commit, which is nothing next to how rarely a panel commits.

### BG-007 — Resizing a panel freed pixels that committed buffers still held

[#9](https://github.com/neogentrics/ReconOS/issues/9)

- **Found in** v0.1.0 (pre-release). **Found by** the investigation into
  BG-006: maximizing made the flicker dramatically worse, and maximizing
  resizes.
- **Was** a use after free. The panel's pixel block was freed outright on
  resize while committed buffers still referenced it.
- **Fixed in** v0.1.0, `e0b7fc5`, by the same change: a buffer that owns its
  pixels cannot have them freed underneath it.

### BG-008 — Apps, then Shut Down, closed the menu and did nothing

[#10](https://github.com/neogentrics/ReconOS/issues/10)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua, pressing it.
- **Was** the apps menu is raised above every window, but windows were offered
  clicks first. A maximized window covering the same pixels swallowed the
  click meant for the menu.
- **Fixed in** v0.1.0, `e0b7fc5`. Click routing follows what is drawn on top
  of what.

### BG-009 — Built-in windows were pinned above every other window

[#11](https://github.com/neogentrics/ReconOS/issues/11)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua: a client window
  could never be brought in front of Watchtower.
- **Was** built-in windows were raised to the top of the scene and kept there.
- **Fixed in** v0.1.0, `c0a361e`.

### BG-010 — Watchtower's Processes tab listed everything on every tab

[#12](https://github.com/neogentrics/ReconOS/issues/12)

- **Found in** v0.1.0 (pre-release). **Found by** reading the window.
- **Was** the process list was not filtered by which tab was showing.
- **Fixed in** v0.1.0, `c0a361e`.

### BG-011 — Clicking a window brought a different one forward

[#13](https://github.com/neogentrics/ReconOS/issues/13)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua: clicking the
  Calculator where it overlapped Watchtower raised Watchtower.
- **Was** built-in windows were offered clicks in the order they were
  *created*. Watchtower was created first, so it answered for every click
  landing inside its rectangle, including clicks on windows stacked above it.
- **Fixed in** v0.1.0, `1b0d820`. The scene graph is asked what is actually on
  top. "Contains this point" is a different question from "is on top here".

### BG-012 — A terminal would never minimize

[#14](https://github.com/neogentrics/ReconOS/issues/14)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua, pressing the button.
- **Was** the shell was offered every click before client windows were
  considered at all, so a built-in window underneath the terminal took the
  click and the taskbar branch was never reached. The log confirmed minimize
  was never called.
- **Fixed in** v0.1.0, `1b0d820`.

### BG-013 — Frames were corrupted on a machine with no GPU

[#15](https://github.com/neogentrics/ReconOS/issues/15)

- **Found in** v0.1.0 (pre-release). **Found by** measurement, after four
  reasoned explanations turned out to be wrong. Dumping panel pixels and
  capturing composited frames showed both were already correct, which placed
  the fault below compositing.
- **Was** with no render node, the OpenGL path renders through a software
  implementation and then copies the finished frame back out for the display
  to scan. That copy was corrupting frames.
- **Fixed in** v0.1.0, `5d38fa3`. The pixman renderer rasterizes straight into
  the display's buffer, so the copy never happens. Selected automatically when
  the machine has no render node; an explicit `WLR_RENDERER` still wins.
- **Note** this is the bug that changed how ReconOS is debugged. Four wrong
  explanations in a row were all reasoned from the code. The fix came from
  looking at pixels. `capture` and the `look.sh` harness exist because of this
  entry.

### BG-014 — The Applications tab listed processes

[#16](https://github.com/neogentrics/ReconOS/issues/16)

- **Found in** v0.1.0 (pre-release). **Found by** reading the window: it was
  the Processes tab twice.
- **Was** the Applications view enumerated processes rather than windows.
- **Fixed in** v0.1.0, `b3ceff4`.

### BG-015 — Focus had two owners that disagreed

[#17](https://github.com/neogentrics/ReconOS/issues/17)

- **Found in** v0.1.0 (pre-release). **Found by** code review while adding
  per-window memory.
- **Was** both the compositor and the shell tracked which window had focus,
  and nothing kept them in step.
- **Fixed in** v0.1.0, `1ab1143`. One owner.

### BG-016 — The compositor repainted on damage that had already been drawn

[#18](https://github.com/neogentrics/ReconOS/issues/18)

- **Found in** v0.1.0 (pre-release). **Found by** measurement: an idle desktop
  was not idle.
- **Was** repaints were triggered by leftover damage rather than by anything
  changing.
- **Fixed in** v0.1.0, `6c6fb0e`.

### BG-017 — Every context menu entry had silently done nothing, ever

[#19](https://github.com/neogentrics/ReconOS/issues/19)

- **Found in** v0.1.0 (pre-release). **Found by** injected input — pressing
  the buttons rather than reading the code.
- **Was** the button handler closed the menu *before* dispatching the click,
  so `context_open` was already false when the shell looked at it and the
  branch that turns a click into a choice never ran. Rename, Delete, Cut,
  Copy, Paste, New Folder, New File and New Shortcut had never been reachable
  since menus were added. The menu appeared, the click landed, the menu
  closed, nothing happened.
- **Fixed in** v0.1.0, `8b4ebc3`.
- **Note** this fault survived for as long as it did because there was no way
  to press a button without a person doing it. `recon_inject_pointer/button/
  key`, the `ui` command and the `state` command were all built here, and
  found BG-018 through BG-021 the same afternoon.

### BG-018 — Renaming was destroyed in the same breath it was started

[#20](https://github.com/neogentrics/ReconOS/issues/20)

- **Found in** v0.1.0 (pre-release). **Found by** injected input, immediately
  after BG-017 made Rename reachable at all.
- **Was** every context action ends with a shell refresh, which reloads the
  desktop, and the reload cancelled any rename in progress.
- **Fixed in** v0.1.0, `8b4ebc3`. A rename is carried across a reload by name.

### BG-019 — Renaming a file selected its extension too

[#21](https://github.com/neogentrics/ReconOS/issues/21)

- **Found in** v0.1.0 (pre-release). **Found by** injected input.
- **Was** the whole name was selected, so the first keystroke took the
  extension with it.
- **Fixed in** v0.1.0, `8b4ebc3`. The stem is selected and `.app` survives,
  with Ctrl+A, shift-free arrow keys and Backspace behaving accordingly.

### BG-020 — Right-clicking the taskbar did nothing unless you hit a button

[#22](https://github.com/neogentrics/ReconOS/issues/22)

- **Found in** v0.1.0 (pre-release). **Found by** injected input.
- **Was** the taskbar answered right-clicks only on a window button. Empty bar
  did nothing, which reads as broken.
- **Fixed in** v0.1.0, `8b4ebc3`. Empty bar offers Task Manager, Show Desktop
  and Refresh.

### BG-021 — Opening Watchtower opened the Calculator

[#23](https://github.com/neogentrics/ReconOS/issues/23)

- **Found in** v0.1.0 (pre-release). **Found by** injected input.
- **Was** applications were opened by position in a list. The index for
  Watchtower pointed at the Calculator, and any application failing to
  construct would have shifted every index after it.
- **Fixed in** v0.1.0, `8b4ebc3`. Applications are found by name.

### BG-022 — The file dialog filled the window it belonged to

[#24](https://github.com/neogentrics/ReconOS/issues/24)

- **Found in** v0.1.0 (pre-release). **Found by** screen capture.
- **Was** the dialog was sized to its parent rather than to its contents.
- **Fixed in** v0.1.0, `05799c4`.

### BG-023 — The desktop was empty

[#25](https://github.com/neogentrics/ReconOS/issues/25)

- **Found in** v0.1.0 (pre-release). **Found by** Joshua: no icons.
- **Was** the marker recording that default shortcuts had been written lived
  in `/System/Config`. When user folders arrived the shortcuts moved to the
  user's folder, but the marker did not, so a system that had already written
  them once never wrote them to the new location.
- **Fixed in** v0.1.0, `c7fbde2`. The marker belongs to the user and lives in
  their folder.

### BG-024 — ReconOS refused to start if its root was not writable

[#26](https://github.com/neogentrics/ReconOS/issues/26)

- **Found in** v0.1.0 (pre-release). **Found by** running it somewhere else.
- **Was** an unwritable root was treated as fatal.
- **Fixed in** v0.1.0, `a15ea26`. It falls back to a writable root.

### BG-025 — The setup screen offered eight skins out of ten

[#27](https://github.com/neogentrics/ReconOS/issues/27)

- **Found in** v0.1.0. **Found by** counting them on screen.
- **Was** the grid drew a fixed eight.
- **Fixed in** v0.1.1, `0fd4f66`.

### BG-026 — One account could read another account's files

[#28](https://github.com/neogentrics/ReconOS/issues/28)

- **Found in** v0.1.0. **Found by** code review of the new account boundary.
- **Was** writing into another account's folder was refused, but
  `recon_fs_list` and `recon_fs_read` were not, which is most of the way to no
  protection at all.
- **Fixed in** v0.1.1, `07ee8e8`. Both refuse, tested including the case where
  one account's name is a prefix of another's. The system's own folder stays
  readable — an account has to be able to load its settings.
- **Security.**

### BG-027 — The address bar's drop-down appeared behind things

[#29](https://github.com/neogentrics/ReconOS/issues/29)

- **Found in** v0.1.1. **Found by** Joshua, using the File Explorer.
- **Was** the drop-down was built as a panel, so it took part in scene
  stacking as an ordinary surface rather than as a menu.
- **Fixed in** v0.1.3, `b4560cd` and `f2690d6`. It is a menu.

### BG-028 — The refresh arrow was not redrawn

[#30](https://github.com/neogentrics/ReconOS/issues/30)

- **Found in** v0.1.1. **Found by** screen capture.
- **Was** the button's own redraw was never triggered after the first draw.
- **Fixed in** v0.1.3, `f2690d6`.

### BG-029 — Lock offered every account on the screen it put up

[#31](https://github.com/neogentrics/ReconOS/issues/31)

- **Found in** v0.1.1. **Found by** Joshua, locking the session.
- **Was** anybody could sign in over a locked session, which is the one thing
  locking exists to prevent.
- **Fixed in** v0.1.3, `a0dee0c`. Locked to whoever locked it: their account
  and no other, the arrow keys cannot step off it, and the check is made when
  signing in rather than only when drawing — a lock that is only a picture is
  not a lock. Switch User ends the session first, and that screen still offers
  everybody, because then there is nothing left to protect.
- **Security.**

### BG-030 — Double-clicking the Recycle Bin did nothing

[#32](https://github.com/neogentrics/ReconOS/issues/32)

- **Found in** v0.1.1. **Found by** Joshua, double-clicking it.
- **Was** the desktop's "what does opening this mean" switch had no case for
  the bin, while the context menu's Open — a second copy of the same logic —
  did.
- **Fixed in** v0.1.3, `a0dee0c`. One copy; the other calls it.

### BG-031 — Icons fell apart when shrunk

[#33](https://github.com/neogentrics/ReconOS/issues/33)

- **Found in** v0.1.1. **Found by** screen capture at a small icon size.
- **Was** nearest-neighbour sampling on the way down.
- **Fixed in** v0.1.3, `db72de7`.

### BG-032 — The control socket was open to every account on the machine

[#34](https://github.com/neogentrics/ReconOS/issues/34)

- **Found in** v0.2.1. **Found by** code review.
- **Was** `/tmp/reconos.sock` was created with default permissions. Anything
  that can write to that socket can drive the whole desktop.
- **Fixed in** v0.2.2, `f239432`, mode 0600.
- **Security.**

### BG-033 — Desktop labels were white text on a light wallpaper

[#35](https://github.com/neogentrics/ReconOS/issues/35)

- **Found in** v0.2.1. **Found by** screen capture, immediately after the
  Beacon skin was added.
- **Was** desktop label colour was fixed rather than asked of the skin.
- **Fixed in** v0.2.2, `58cf832`.

### BG-034 — The Start menu did not redraw itself

[#36](https://github.com/neogentrics/ReconOS/issues/36)

- **Found in** v0.2.1. **Found by** Joshua: the menu showed stale contents.
- **Was** the menu panel was drawn once and never invalidated when what it
  listed changed.
- **Fixed in** v0.2.2, `fd2654b`.

### BG-035 — Every gradient role showed the wrong label

[#37](https://github.com/neogentrics/ReconOS/issues/37)

- **Found in** v0.2.2. **Found by** screen capture, while adding gradients.
- **Was** the label lookup was offset by the roles that had no gradient.
- **Fixed in** v0.2.3, `6d694d4`.

### BG-036 — Two source files contained raw NUL bytes where `'\0'` was meant

[#38](https://github.com/neogentrics/ReconOS/issues/38)

- **Found in** v0.2.3. **Found by** the compiler: *null character(s)
  preserved in literal*.
- **Was** `recon_net.c` and `recon_theme.c` each had a literal NUL byte inside
  single quotes, from an edit that lost a backslash passing through a shell.
- **Fixed in** v0.2.4, `27cc5f8`.
- **Note** this keeps happening — it happened again on 4 September 2026 while
  fixing BG-061. Patches are written to a file and run by path now rather than
  piped through a shell heredoc, which is the only reliable fix.

### BG-037 — A previous fix claimed a bug that was never a bug

[#39](https://github.com/neogentrics/ReconOS/issues/39)

- **Found in** v0.2.3. **Found by** re-reading `f239432` while fixing BG-036.
- **Was** `f239432` claimed that comparing the gateway against a raw NUL byte
  meant a machine with no gateway did not print "(none)". That claim is wrong:
  a character constant holding a single NUL byte has the value 0, exactly as
  `'\0'` does, so the comparison had always been correct. The compiler warns
  about the form, not about a change in meaning. The warning was read and the
  conclusion assumed.
- **Fixed in** v0.2.4, `27cc5f8`, by correcting the claim rather than leaving
  it standing.
- **Documentation.**

### BG-038 — No system shortcut could be driven from outside, and Alt+Tab had stopped working

[#40](https://github.com/neogentrics/ReconOS/issues/40)

- **Found in** v0.2.5. **Found by** building the means to test shortcuts, then
  using it.
- **Was** two faults. Injected keys went straight to the shell, bypassing the
  compositor's shortcut handler entirely — so Alt+Tab, Alt+Q, Ctrl+Alt+Del and
  Print Screen had never once been tested. Sending one the route a real key
  takes immediately found that Alt+Tab cycled the compositor's list of
  *client* windows. That was correct when the only windows were clients, and
  became a shortcut that did nothing the moment ReconOS drew its own: a
  desktop with a Notepad and a Terminal on it has no clients in that list.
- **Fixed in** v0.2.6, `380b996`.
- **Note** a shortcut nothing can press is a shortcut nobody notices the loss
  of.

### BG-039 — A clean build had 36 warnings

[#41](https://github.com/neogentrics/ReconOS/issues/41)

- **Found in** v0.2.6. **Found by** the compiler, after a claim that the build
  was warning-free turned out to be true only of the one file being rebuilt.
- **Was** among them: five `implicit declaration of strcasecmp` — `recon_users.c`
  and `recon_session.c` called it without `<strings.h>`, so the compiler was
  assuming a signature. It happens to work on this target and is undefined
  behaviour everywhere. Four were paths built with `snprintf` from a folder
  and a name where the two together can exceed the buffer; a truncated path is
  not a shortened name, it is a different file.
- **Fixed in** v0.2.7, `666ee5e`.

### BG-040 — The roadmap claimed a warning-free build that did not exist

[#42](https://github.com/neogentrics/ReconOS/issues/42)

- **Found in** v0.2.6. **Found by** BG-039.
- **Fixed in** v0.2.7, `edd4653`.
- **Documentation.**

### BG-041 — An unfocused window's title was drawn in the focused colour

[#43](https://github.com/neogentrics/ReconOS/issues/43)

- **Found in** v0.2.7. **Found by** a screenshot that appeared to show a
  window with an empty title bar. That turned out to be occlusion — but
  looking into it found this underneath.
- **Was** `RECON_THEME_TITLE_TEXT_INACTIVE` was defined, answered by all ten
  skins, and read by nothing. On skins whose two title bars are near-identical
  greys it looked fine, which is why it survived; on Beacon it is white on
  light blue.
- **Fixed in** v0.2.8, `14ab3db` and `c3bbf62`.

### BG-042 — The accessibility contrast test measured the wrong bar

[#44](https://github.com/neogentrics/ReconOS/issues/44)

- **Found in** v0.2.7. **Found by** BG-041.
- **Was** title text was measured against the *active* title bar for both
  states, so the unreadable combination was never tested.
- **Fixed in** v0.2.8, `14ab3db`.

### BG-043 — The font drew no dashes

[#45](https://github.com/neogentrics/ReconOS/issues/45)

- **Found in** v0.2.15. **Found by** screen capture of the Update page.
- **Was** the glyph cache held 32..126 only, and an em dash is not in that
  range. See BG-044, which is the same fault seen properly.
- **Fixed in** v0.2.16, `020af35`.

### BG-044 — Any text that was not ASCII drew nothing at all

[#46](https://github.com/neogentrics/ReconOS/issues/46)

- **Found in** v0.2.15. **Found by** every em dash in the help coming out as a
  hole in the sentence.
- **Was** text was walked a byte at a time and glyphs were cached only for
  32..126, so a UTF-8 sequence was three or four bytes each of which drew
  nothing. Not a box, not a question mark: nothing. The typeface had the
  glyphs the whole time. The first attempted fix folded the punctuation to
  ASCII on the way into the system, which was a patch on the symptom.
- **Fixed in** v0.2.16, `189aa0f`. The walk decodes UTF-8, and the glyph cache
  has a second half for what is above the ASCII range.

### BG-045 — Deleting one byte of a two-byte character broke the field

[#47](https://github.com/neogentrics/ReconOS/issues/47)

- **Found in** v0.2.15. **Found by** typing an accented character into a name
  field and pressing Backspace, after BG-044 made such characters visible.
- **Was** the caret stepped by byte. Backspace over a two-byte character left
  a broken sequence — a name that had been typed correctly and could no longer
  be read. `recon_edit_key` also accepted only ASCII, so a keyboard laid out
  for a language with accents in it could not name a file the system would
  happily have stored.
- **Fixed in** v0.2.16, `0f295b9`. The caret is still a byte offset, because
  that is what the text is; arrow keys and Backspace step over whole
  characters by walking off the continuation bytes.

### BG-046 — The Help window could not be closed or moved

[#48](https://github.com/neogentrics/ReconOS/issues/48)

- **Found in** v0.2.16. **Found by** Joshua: *"None of the buttons work in the
  top right corner for the help window, so you can't close it once you press
  F1."*
- **Was** `help_draw` called `recon_hit_clear()` *after* the window frame had
  registered its own buttons, wiping every hit region the frame had just added
  — the close button, the minimize and maximize buttons, and the title bar
  that a window is dragged by.
- **Fixed in** v0.2.16, `7d7e36c`. The same fault was in the What's New window
  and was fixed with it.

### BG-047 — The Help sidebar could not be scrolled

[#49](https://github.com/neogentrics/ReconOS/issues/49)

- **Found in** v0.2.16. **Found by** Joshua: *"there's no scroll in the
  listing panel, no scroll bar."*
- **Was** around forty topics in an eighteen-row panel, with no scroll offset
  and no bar. Most of the help was unreachable.
- **Fixed in** v0.2.16, `7d7e36c`.

### BG-048 — "N more lines below" was drawn over the last line of text

[#50](https://github.com/neogentrics/ReconOS/issues/50)

- **Found in** v0.2.16. **Found by** Joshua: *"the text at the bottom is
  jumbled."*
- **Was** the visible-line count was computed before the heading had taken its
  room, and without reserving a line for the footer that says there is more.
- **Fixed in** v0.2.16, `7d7e36c`.

### BG-049 — F1 did not close the Help window

[#51](https://github.com/neogentrics/ReconOS/issues/51)

- **Found in** v0.2.16. **Found by** Joshua: *"Pressing F1 again doesn't close
  the help app. It just resets it back to the top."*
- **Was** F1 always opened help on whatever was in front, and opening help
  when help is in front reopened it at its first topic.
- **Fixed in** v0.2.16, `7d7e36c`. F1 with help in front closes it.

### BG-050 — Opening the Calculator crashed the whole system

[#52](https://github.com/neogentrics/ReconOS/issues/52)

- **Found in** v0.2.16. **Found by** Joshua, twice: *"I tried to open the
  calculator app, and it crashed it. The whole system just crashed."*
- **Was** v0.2.15 added a `const char *help;` field to
  `struct recon_appwin_impl` without bumping `RECON_MODULE_ABI`. A
  `Calculator.rex` built before that change passed the version gate with a
  struct one field short, and `recon_appwin_create` read `impl->help` past the
  end of it. SIGSEGV.
- **Fixed in** v0.2.16, `d61a9a3`. ABI bumped to 2, with the rule that was
  missed written into the header beside it.
- **Note** proved rather than assumed: the ABI was reverted to 1, the system
  rebuilt, `apps Calculator` run, and the segfault reproduced before the fix
  was claimed.

### BG-051 — A stale module was never replaced by the one that shipped with the build

[#53](https://github.com/neogentrics/ReconOS/issues/53)

- **Found in** v0.2.16. **Found by** BG-050: the mechanism that should have
  prevented it.
- **Was** shipped modules were installed only if absent, so an out-of-date
  `.rex` in the filesystem outlived every rebuild.
- **Fixed in** v0.2.16, `d61a9a3`. Shipped modules are compared byte for byte
  and replaced when they differ.

### BG-052 — The control socket aborted on the connection after a `quit`

[#54](https://github.com/neogentrics/ReconOS/issues/54)

- **Found in** v0.2.16. **Found by** the next connection failing, while
  building remote access.
- **Was** a use after free. `handle_line` closed the client from inside
  itself, freeing the struct the read loop was standing on.
- **Fixed in** v0.2.16, `abb00f4`. `handle_line` returns a verdict and the
  caller closes.

### BG-053 — The remote key was echoed back as a failed command

[#55](https://github.com/neogentrics/ReconOS/issues/55)

- **Found in** v0.2.16. **Found by** authenticating over the network port.
- **Was** the key line was consumed by the authentication check and then also
  handed to the command interpreter, which did not recognise it and said so —
  printing the key into the transcript.
- **Fixed in** v0.2.16, `abb00f4`.
- **Security.**

### BG-054 — The Terminal was documented as unavailable when it was installed

[#56](https://github.com/neogentrics/ReconOS/issues/56)

- **Found in** v0.2.16. **Found by** running `weston-terminal` to check, after
  the roadmap had recorded it as a blocker for some time.
- **Was** a documented blocker that had never been verified.
- **Fixed in** v0.2.16. Corrected in the roadmap.
- **Documentation.**

### BG-055 — `recon_spawn` had never been executed

[#57](https://github.com/neogentrics/ReconOS/issues/57)

- **Found in** v0.2.16. **Found by** adding the `spawn` command, which called
  it for the first time.
- **Was** code written, compiled, shipped and never once run. It did not work.
- **Fixed in** v0.2.16.
- **Note** one of several this session. Building the instrument that can reach
  a code path is how the code path gets found to be broken; see also BG-017
  and BG-038.

### BG-056 — `recon_help_show_topic` had never been executed

[#58](https://github.com/neogentrics/ReconOS/issues/58)

- **Found in** v0.2.16. **Found by** wiring F1 to it.
- **Was** as BG-055.
- **Fixed in** v0.2.16, `75c331c`.

### BG-057 — CMake added a source file to every target that mentioned another

[#59](https://github.com/neogentrics/ReconOS/issues/59)

- **Found in** v0.2.16. **Found by** the build: test targets acquired the
  firewall and everything it needs.
- **Was** a `sed` over `CMakeLists.txt` matched every target listing
  `recon_fs.c` rather than the one intended.
- **Fixed in** v0.2.16, `5d395f1`. Trimmed to the main target plus
  `recon_net_tests`.

### BG-058 — The stop screen was unreadable at a glance

[#60](https://github.com/neogentrics/ReconOS/issues/60)

- **Found in** v0.2.16. **Found by** Joshua, on seeing the first one.
- **Was** the error screen used the ordinary window palette, so a system that
  had stopped looked like a system that was fine.
- **Fixed in** v0.2.16, `ed41702`. Purple — the Recon Towers hub colour — with
  the code in amber, and a counter while it collects what it knows.

### BG-059 — Killing the build by pattern killed the shell running the command

[#61](https://github.com/neogentrics/ReconOS/issues/61)

- **Found in** v0.2.16. **Found by** the shell dying.
- **Was** `pkill -f "build/ReconOS"` matched its own invoking command line.
- **Fixed in** v0.2.16. The harness kills by recorded PID.
- **Note** a tooling fault rather than a ReconOS one, recorded because it cost
  an afternoon twice.

### BG-061 — Restarting the desktop shell segfaulted the system

[#62](https://github.com/neogentrics/ReconOS/issues/62)

- **Found in** v0.2.16. **Found by** injected input, testing the new Services
  tab: `services restart Desktop shell` with a Notepad open, three times out
  of three.
- **Was** two faults, one behind the other.
  1. The UI font was owned by the shell and freed with it. Application windows
     deliberately outlive a shell restart — that is what makes restarting the
     shell a repair rather than a loss — and every one of them holds that
     font pointer, several caching a copy of their own. The first frame after
     the restart was a segmentation fault inside the glyph rasteriser, five
     frames deep in stb_truetype.
  2. Registering a built-in application twice was refused as a name collision.
     A second shell registers the same seven built-ins, so a restarted desktop
     had no Notepad and no File Explorer while the old windows were still on
     screen — a desktop you cannot open anything from.
- **Fixed in** v0.2.17, `c0a43bc`. The font is the system's, loaded once per
  size for the whole run and freed after nothing is left that could draw. A
  built-in re-registering itself is an update in place; a module taking a
  built-in's name is still refused.
- **Note** found under gdb only after the harness was made to sign a user in
  first — without a signed-in account the restart took the other branch and
  did not crash, which is why the first three gdb runs looked clean.

---

## Labels

The same register covers everything else that happens to this system, because
"what changed and why" is one question:

| Label | For |
|---|---|
| `bug` | A fault. Titled `BG-nnn — …`. |
| `security` | A fault with a security consequence. Always also `bug`. |
| `regression` | Worked before, does not now. Always also `bug`. |
| `feature` | Something the system cannot do yet. |
| `patch` | A correction that is not a fault — wording, documentation, a claim. |
| `release` | One per version, closed when it ships. |
| `blocked-on-kernel` | Real, specified, and waiting on Phase 2. |

Areas, matching the [error code](ERRORS.md) letters where they apply:
`startup`, `storage`, `accounts`, `display`, `programs`, `network`,
`firewall`, `settings`, `applications`, `input`, `skins`, `help`, `build`,
`docs`.

## Adding a bug

1. Take the next number. The highest in this file is the last one used.
2. Write the entry — **Found in**, **Found by**, **Was**, and either
   **Fixed in** or **Open** and why.
3. Add its area to the `AREA` table in `scripts/make-issues.py`.
4. Run the script, which opens the issue with the same title and body:

   ```
   python scripts/make-issues.py --dry-run
   python scripts/make-issues.py
   ```

   Entries that already have an issue are left alone, so running it again is
   safe. One with a **Fixed in** line is created and then closed.
5. Paste the issue link under the heading, and reference the number in the
   commit that fixes it.

The **Was** field is the one that matters. A register full of symptoms is a
list of complaints; a register full of causes is something to learn from.

## Two things this register has learned about finding things

**The disassembly disagreeing with the theory is a result, not a null result.**
Three times now this project has gone to an instrument expecting to confirm
something and learned something else instead — the flickering that led to the
renderer, a kernel label the compiler had deleted rather than misplaced, and a
`memset` that was still there when it was expected to be gone. In the last of
those the instrument said "you are wrong about the bug", and the honest
response was to look harder rather than to be relieved: what it actually found
(BG-090) was a security property holding by accident of build flags, which is
worse to own than a fault, because a fault gets found.

**Some entries here never misbehaved.** A bug whose entire lifetime is silent —
including the moment it starts being real — has no failing behaviour to notice
and no test that can go red. Those belong in the register precisely because
nothing else would ever record them.

## Three ways a fault stays hidden

Named because the same shapes keep arriving, from both halves of the project,
and having a name for one makes the next one findable.

**Something was true when it was computed, and nothing arranged to notice when
it stopped being true.** A TLB entry, a tooltip cached against pointer motion,
a font cache answering from a stale slot, hardware mapped at an address read
once at boot, a `memset` the compiler removed because nothing observable
depended on it. These are quiet because *nothing watches*. There is no moment
of becoming wrong for anything to catch.

**It was correct until a second caller arrived.** A window title that redrew
the window and not the taskbar, because the only caller that ever changed a
title also redrew everything. Exception vectors that clobbered the first
argument register, harmless while every exception was a fault that did not need
it. These are quiet because *nothing asks* — and there is no earlier moment at
which they could have been caught, except by having had the second caller.

**It was correct on the machine it was tested on.** Secondary processors
erasing page tables they shared, which failed a quarter of the time on four
processors and never on two. The fault is real from the first line; what is
missing is the rig. These are the most dangerous of the three, because the
green result is not a false negative — the test genuinely passed, on a machine
where the bug cannot happen.

The third shape has a practical consequence the other two do not: **it is
answered by hardware, not by care.** Reading the code again finds nothing, and
the only thing that helps is running it somewhere it can fail.

### What that makes a verification rig for

`scripts/verify-kernel.sh` runs two, four and eight processors, both firmwares,
and two processor models. Not because eight is more thorough than four —
because **a fault of the third kind is invisible below some particular size,
and there is no way to know in advance which size.** Four processors found the
shared page-table erasure; two never would have, and the difference between
them is not thoroughness, it is whether the bug can exist at all.

Which reframes what a rig is doing. **It is not measuring quality. It is
manufacturing the conditions under which a class of bug can exist**, so that
there is something to be caught.

That distinction has a direction, and it is the useful part. A rig built to
*measure* adds cases resembling the ones it already has. A rig built to
*manufacture conditions* adds cases that are unlike them: a different processor
count, a different firmware, a different optimisation level, a machine with no
sound card, a compiler at a different `-O`.

The desktop's own rig has the same gaps, named here so they are a list rather
than an omission: everything runs on one screen size, one renderer, one
account, and one build configuration. Each of those is a condition that has not
been manufactured yet, and BG-090 is what the last of them already cost.

### BG-106 — A panel in front could not stop a tooltip search it had no answer for

- **Found in** v0.3.1. **Found by** opening the taskbar clock's new menu: the menu
  appeared under a pointer that had not moved, and the clock's own tooltip
  carried on showing across it.
- **What it actually was** not the stale-tip fault it looked like. `tip_under`
  asks each panel in front for a tip and *falls through* when it has none, so a
  menu with no tooltip under the pointer handed the question down to the taskbar
  underneath it — which answered.
- The rule was already written down eight lines below, over the window loop:
  *"Stopping matters more than finding: a blank patch of the window in front is
  not a hole to read the one behind through."* The panels above that loop were
  not following the rule stated for the loop.
- The reason they could not is that `recon_panel_tip_at` returns false for two
  different facts — "the point is not in this panel" and "it is, and there is
  nothing here" — and the caller cannot tell them apart. `recon_appwin_tip_at`
  already distinguishes them with an `owned` out-parameter; panels had no
  equivalent until `recon_panel_contains`.
- **The shape it shares with [BG-089](#bg-089--a-tooltip-kept-showing-over-the-window-that-opened-on-top-of-it)**
  is the second half only. BG-089 was a stale answer nothing rechecked, and its
  fix — `tip_recheck` — is called when a window opens and was not called when a
  menu does. That gap is closed too. But `tip_recheck` alone would not have
  fixed this, because the search it re-runs was giving the wrong answer.
- **Fixed in** v0.3.1. The panels in front are now checked in the order they are
  drawn, each stopping the search if the point is inside it.

### BG-107 — Desktop labels were unreadable on half the wallpapers

- **Found in** v0.3.1. **Found by** the user, in a screenshot: the Recycle Bin's
  label on a pale wallpaper, barely legible.
- **What it actually was** three faults stacked, and fixing any one alone made
  it worse rather than better.
- The label was drawn with a shadow **one pixel down and to the right**, under a
  comment saying the shadow kept it readable over any wallpaper. One offset
  protects one of the eight directions a glyph has an edge in. The other seven
  sat directly on the picture.
- The shadow roles carry alpha — `C0` in most skins, `C8` in the rest — and
  `recon_draw_text` writes the colour straight into the buffer at full coverage,
  **alpha byte included**. Wayland's ARGB8888 is premultiplied, so an unscaled
  colour with alpha under it renders both see-through and too bright. That is
  the fault `recon_panel_fade`'s own header warns about, met here for the same
  reason: chrome over a wallpaper.
- And a ring is not free. Drawn under the label it blends with the label's own
  antialiased edges, so adding one where it is not needed **costs the glyph
  weight and returns nothing**. A ringed dark label on a pale wallpaper came out
  thinner and greyer than the unringed one — while measuring 17:1, because the
  dark ink against the pale ground was never the problem.
- **The measurement agreed with the code and disagreed with the eye**, and the
  eye was right. The contrast ratio answered "is there a dark pixel near a light
  one", which was true throughout; what had changed was the weight of the
  strokes. Settled by magnifying the labels rather than by the number.
- **Fixed in** v0.3.1. The ring goes all the way round, premultiplied, and is
  drawn only when it is far enough from the wallpaper to be separating anything.
  A white ring on a pale wallpaper separates nothing — and in that case the ink
  is the dark half of the pair, which needs no help. The two go together,
  because every skin pairs a light colour with a dark one: if the ring has
  vanished into the wallpaper then the ink is the one that has not.
- The wallpaper's lightness is measured into a 16x9 grid where the picture is
  decoded, rather than one number for the whole screen. A wallpaper is very
  often pale at the top and dark at the bottom, and an average of that is a
  figure that is wrong in both halves.

### BG-108 — Changing the wallpaper left the desktop labels chosen against the old one

- **Found in** v0.3.1. **Found by** the harness for [BG-107](#bg-107--desktop-labels-were-unreadable-on-half-the-wallpapers),
  which set a skin and then a wallpaper — and every measurement came back
  describing the skin's own picture rather than the one on screen.
- **What it was** `recon_background_reload` replaced the wallpaper and damaged
  the screen, but never redrew the desktop. That was harmless while a label's
  colours came only from the skin. It stopped being harmless the moment they
  came from the wallpaper.
- Not a cosmetic lag: swap a dark wallpaper for a pale one and every label keeps
  the white ink that was right a moment earlier and is now invisible, until
  something unrelated happens to redraw the desktop.
- **The shape worth keeping** is that this bug was *created* by BG-107's fix and
  found by BG-107's harness within the hour. A value that used to depend on one
  thing and now depends on two needs every path that changes the second thing to
  know that. Nothing warns you; the old paths keep compiling.
- **Fixed in** v0.3.1.

### BG-109 — The clock was an hour slow for half the year, in half the world

- **Found in** v0.3.1. **Found by** the user, looking at it: Central selected,
  the host reading 4:25 am, ReconOS reading 3:25 am.
- **What it was** the zone list carried standard offsets and nothing applied
  summer time, which the page said plainly — *"Nothing here follows daylight
  saving"*. Stating a limitation does not stop it being wrong; it was an hour
  out for most of the year for most of the people in the list.
- The original reasoning holds and is not reversed here: a rule engine for the
  world's daylight-saving legislation is a database with politics in it,
  revised by parliaments with no interest in this clock, and getting it wrong
  twice a year is worse than not having it.
- **Fixed in** v0.3.1 by making summer time a **switch** rather than a rule, and
  starting it from what the host thinks. `tm_isdst` says whether summer time is
  in force and the offset says by how much, so the two come apart into a
  standard zone and a switch without any rules being carried.
- Written with standard time arithmetic rather than `tm_gmtoff`, which is a BSD
  extension C11 does not have — a system that intends to run on its own kernel
  should not lean on what glibc adds to a standard structure.

### BG-110 — The time zone list had a scrollbar and no way to move it

- **Found in** v0.3.1. **Found by** the user, trying to scroll it.
- **What it was** `panel_scroll` had a branch for Appearance, Display, Network,
  Firewall, Programs and the Registry, and none for Date and Time. Twenty-six
  zones, about nine rows of room, and seventeen zones that could not be reached
  — with a scrollbar beside them saying they were there.
- **The note directly above the fault describes the fault.** It explains that
  every list in Appearance takes the wheel now, because one of them "said
  'scroll for the rest' under a list that could not be scrolled -- a page
  telling somebody to do something it would not let them do." The same
  sentence was true one page over, and the fix had not been generalised.
- **Fixed in** v0.3.1.

### BG-111 — A first account started on UTC on a machine that was not

- **Found in** v0.3.1, while fixing [BG-109](#bg-109--the-clock-was-an-hour-slow-for-half-the-year-in-half-the-world).
- **What it was** the zone defaulted to UTC, which is right nowhere and reads
  as a fault everywhere. Adopting the host's zone was written into
  `recon_clock_init` — and the page still came up on UTC+00:00 on a machine six
  hours from it, with the code meant to prevent that already running.
- The zone is a **per-account** setting, and the clock starts before anybody has
  signed in. The hive being written was not the hive that would be read.
- **Fixed in** v0.3.1 by adopting at sign-in, which is the first moment there is
  an account to give it to. Only where no zone has been chosen — including
  where somebody chose UTC on a machine that is not on it.

### BG-112 — Moving focus between form fields emptied the field arrived at

- **Found in** v0.3.1. **Found by** filling in the mail setup form and noticing
  the port had gone. Tabbing past a field cleared it; the two ports on that form
  arrive with sensible defaults and lost them to being tabbed over.
- **What it was** `recon_edit_begin(&field, field.text, false)` — a field handed
  its own buffer as the text to start from. That is `snprintf` with a source and
  a destination that overlap, which is **undefined**, and on this library
  produces an empty string.
- Not a new fault. The mail form has moved focus that way since it was written,
  and the Web viewer's address bar did the same thing on a click and on Ctrl+L,
  so **clicking the address bar cleared the address it was showing**. It went
  unnoticed for the same reason in both places: a field you are about to type
  into looks the same whether it was cleared or selected, and both of these
  fields are usually typed into straight away. The mail form only exposed it by
  gaining two fields with defaults worth keeping.
- **The intent was right** and is kept. Selecting the text on focus is what
  makes the first keystroke replace a default rather than append to it.
- **Fixed in** v0.3.1 by `recon_edit_focus`, which does what
  `recon_edit_begin` did minus the copy — and the copy was the whole fault.

### BG-113 — A TLS handshake that stalled was never timed out

- **Found in** v0.3.1, while building STARTTLS. **Found by** a fake SMTP server
  that speaks the plain half of STARTTLS and cannot speak TLS — so it accepts,
  answers, agrees to upgrade, and then falls silent. The client waited forever
  with "Sending…" on screen and the button disabled.
- **What it was** the connect deadline was dropped the moment the socket
  connected, which left the handshake unguarded. `connected` and *usable* are
  the same moment for a plain stream and are not for an encrypted one, and the
  timeout was written for the first meaning.
- **Not specific to SMTP.** Every outgoing TLS stream had this: reading mail,
  the web viewer, checking the time. A server that accepts and says nothing is
  also what a failing middlebox looks like, so it is not an exotic case.
- **Fixed in** v0.3.1. The deadline now survives until the connection is usable
  rather than until the socket is open — dropped immediately for a plain
  stream, and at the end of the handshake for an encrypted one. An upgrade
  starts a fresh one, because the original was correctly dropped when the plain
  connection came up.

### BG-114 — Nothing in All Programs would open

- **Found in** v0.3.1. **Found by** the user: clicking any entry in All Programs
  closed the Start menu and launched nothing.
- **What it was** the All Programs list is a **separate panel** sitting beside
  the menu, and the code that handled a click on one of its rows was nested
  inside the branch that tests whether the click landed *in the menu*. So it
  never ran. A click on a program row was not in the menu, was not in any
  window, and fell through to "clicked outside, close the menu".
- **The third time this shape has appeared** in as many days: the compose
  window's fields were numbered above the message rows, which are matched with
  an open-ended `>=`; the graph's controls were numbered above the date fields
  the same way. Here the broader case was a *panel* rather than a range of ids,
  which is why remembering the first two did not prevent it.
- The pattern to watch for is not "check ids in the right order". It is **a
  specific case tested inside or after a general one that already claims the
  event.**
- **Fixed in** v0.3.1. The list is offered the click before the menu it hangs
  off, which is also the order they are drawn in.

### BG-115 — Right-clicking in the Start menu closed the menu

- **Found in** v0.3.1. **Found by** the user: *"you can't even see what you're
  right clicking to do."*
- **What it was** the right-click handler closed the menu before showing the
  context menu — deliberately, and wrong. What was left was a small menu
  offering "Open" and "Unpin from the menu" floating on an empty desktop, with
  nothing on screen saying which program it was about.
- **A menu about a thing has to be shown beside the thing.** The context menu is
  raised above the Start menu, so there was never a reason to close it.
- **Fixed in** v0.3.1. Both right-click paths — the pinned list and All
  Programs — leave the menu where it is.
- **The harness reported this as still broken after it was fixed**, because
  Escape clears the search box without closing the menu, so the test's blind
  click on Apps shut it and the right-click landed on the desktop, which
  answered with its own menu. The test was measuring the desktop and reporting
  on the Start menu. It now checks the menu is open rather than assuming it.

### BG-116 — The Calculator opened smaller than its own minimum size

- **Found in** v0.3.1. **Found by** the user: *"They're not big enough, or the
  calculator window is not big enough, and the text isn't... [cen]tered
  aligned. If you look at the buttons, the text is messed up. Also, the buttons
  aren't exactly unique. They just kinda exist up there."*
- **What it was** four separate faults in the same window, which is why one
  round of fixing it did not look like a fix.
- **The opening size was below the minimum size.** `recon_appwin_impl` carries
  both `default_width` and `min_width`, set a line apart, and nothing made them
  agree. The Calculator's default was 430 and its minimum 520, so it opened at
  a width the resize code would refuse to let anybody choose: six mode tabs
  wrapped onto a second row on a window nobody had touched, and the keypad was
  as cramped as the wrapping left it. **A previous attempt raised `min_width`
  and changed nothing on screen**, because the minimum is the floor for
  dragging and has no say in where a window starts.
- **Fixed in** v0.4.0 in `recon_appwin_create`, not in the Calculator: an
  opening size smaller than the minimum is raised to it. It is a mistake any
  application can make and none of them can see.
- **The keypad stopped short of the right margin.** A key width of
  `(width - gaps) / columns` throws the remainder away, and six columns of it
  threw away up to five pixels — so the grid ended at a different place in
  every mode and the rightmost column was visibly narrow. Each edge is now
  divided out of the whole span, which spreads the remainder a pixel at a time
  and lands the last key exactly on the margin.
- **The labels sat high in their keys.** The baseline was
  `(height + ascent) / 2 - 2`, which centres correctly only when the descent
  happens to be four pixels. It is now centred by the line's own height.
- **The keys were drawn with a bare bevel**, so they did not round with the
  skin and read as one slab with lines scored in it rather than as forty
  buttons. They use `recon_draw_button_edge` now, like the tabs beside them.

### BG-117 — Every click was ignored while the login screen was up

- **Found in** v0.4.0. **Found by** the look harness, which opened the
  Calculator, clicked "7", and photographed a display still reading zero.
- **What it is** the login screen takes the whole screen's pointer input, as it
  should. What is wrong is not the behaviour but what the system reports about
  it: `apps <name>` opens a window while the login screen is up, and `state`
  then lists that window as *open* and *focused*. It is neither — nothing can
  reach it.
- **Fixed in** v0.4.0, in the reporting rather than the behaviour: the
  behaviour is right, and only what the system said about it was misleading.
  `state` now opens with a line saying who has the input, and marks the focused
  window "not reachable" while the login screen holds it. It was recorded as
  "not fixed" first and fixed an hour later in the same session; this line
  replaces that one, because a bug record that says something is open when it
  is closed is the same kind of lie the bug itself was.
- **What it cost** three rounds of measuring a window that could not be clicked,
  and one wrong conclusion — that the mode tabs did not work — that was a
  property of the harness and not of the Calculator. `scripts/look.sh` now signs
  in before it does anything else, and refuses to continue if it did not reach
  the desktop.

### BG-118 — Every text field on a form drew a caret at once

- **Found in** v0.4.0. **Found by** a screenshot taken for something else: the
  Mail setup screen, photographed to check the new Cc and Bcc rows, showed a
  caret in Server, Username, Port, Sending server, Sending port and Your
  address simultaneously — and both port numbers highlighted as selected.
- **What it was** `recon_edit_draw` drew the caret and the selection
  unconditionally, without asking whether the field was the one being typed
  into. `struct recon_edit` has carried an `active` flag the whole time and the
  drawing never read it.
- **A caret is the answer to "where does the next key land?"**, and six answers
  is no answer. The selection is worse: a highlight says the next keystroke
  will *replace* that text, and on five of those six fields it would not.
- **Fixed in** v0.4.0. Both are drawn only when `active` is set. Safe without
  touching any of the twelve windows that draw fields, because both ways of
  starting to edit — `recon_edit_begin` and `recon_edit_focus` — already set
  it, so the seven windows with a single field each keep their caret without
  having been changed.
- **Why it survived this long**: every window that has *one* field looked
  correct, and those are most of them. It only reads as wrong on a form, and
  the two forms in the system are the Mail setup screen and the compose window
   — both of which were being looked at for whether the fields were in the
  right order rather than for what was drawn inside them.

### BG-119 — Every built-in application lost its version number

- **Found in** v0.4.0. **Found by** running `apps` on the control socket
  immediately after adding a field, for no reason except to see whether
  anything had moved. Every built-in showed `-` in the version column.
- **What it was** `struct recon_app_registration` gained an `opens` field, and
  it was added in the middle. The twelve built-in applications were registered
  with **positional** initialisers, so every `RECONOS_VERSION` slid one place
  into `opens` and `version` became NULL. Each built-in then claimed to open a
  file type called `"0.4.0"` and had no version of its own.
- **The version is not decoration.** It is the number the whole applet-update
  decision is made on: an installed applet takes a name by being newer, and
  gives it back when the system catches up. With every built-in at NULL, any
  applet claiming any version would have displaced any built-in — and no
  built-in could ever have taken its name back.
- **It compiled without a warning**, because both fields are `const char *`.
  The only visible sign anywhere in the system was a dash in one column of one
  command.
- **Fixed in** v0.4.0. The built-in table uses designated initialisers, so a
  field added anywhere in the struct cannot silently shift another. The struct
  is public and will gain another field; this is what stops it happening twice.

### BG-120 — Five applications' F1 opened the wrong page, or none

- **Found in** v0.4.0. **Found by** searching the help for "Networking" while
  testing the new search box, and getting no results — for a topic two
  applications name.
- **What it was** `recon_appwin_impl` carries a `help` field naming a topic,
  and the topics are written in `docs/HELP.md`. Nothing made the two agree.
  - **Web** and **Mail** both asked for a page called **"Networking"**, which
    has never existed. F1 from either left whatever page was already showing.
  - **Photos**, **Media Player** and **Calendar** each asked for **"Writing"**,
    which exists and is about Notepad. That is worse: it looks like an answer.
  - **Watchtower** asked for "Programs", which is about installing software.
  - The **Calculator** named no topic at all.
- **The help itself made the promise**: *"F1 opens this help at the page about
  whatever you are looking at."* It kept it for four applications out of ten.
- **Fixed in** v0.4.0, in three parts:
  - Seven topics written that did not exist: Pictures, Sound and video, The
    web viewer, Mail, What is running, The Calculator, Dates.
  - Every application pointed at its own page, the Calculator included.
  - **A topic that does not exist is now complained about in the log and falls
    back to the first page**, which is the other half of what the help
    promises. Verified by pointing Photos at a page called "No Such Page",
    watching the error appear, and watching Help open at "Getting around".
- **The check is at the point of use, not at startup**, because an
  application's window is built the first time it is opened — checking every
  topic at boot would mean building every window at boot, which is the idle
  weight this system exists to avoid. It also catches topics set at runtime,
  which is how the Control Panel names a page per section.

### BG-121 — Dragging the graph changed everything except the picture

- **Found in** v0.4.0. **Found by** dragging the new panning and watching
  nothing move.
- **What it was** `motion` changed the view and did not ask for a redraw. A
  click is redrawn by the shell, because a click may have changed something;
  pointer motion is not, because most motion changes nothing and redrawing
  every window on every pixel of movement is a compositor that never idles.
- **It looked exactly like the drag not being delivered at all**, which is the
  expensive part. The obvious suspects are all in the input path — is the hit
  region there, does a press reach the application, is motion delivered while a
  button is held — and all three were fine.
- **Settled by instrumenting rather than reasoning about it**: two `fprintf`s,
  one on the press and one on the motion, and the log said `panning=1` with
  coordinates arriving. The state was right and the screen was stale, which
  narrowed it to one line.
- **Fixed in** v0.4.0. `recon_appwin_refresh` from the motion handler. The
  window pointer is now kept for that reason and the reason is written beside
  it, because a handler that changes something on motion is rare enough that
  the next one will make the same mistake.
- **The same shape as a note already in `recon_help_show_topic`**: "the page
  was chosen and the window went on showing the one before it -- which looked
  exactly like the topic not being found". Twice now, in two files, with the
  same symptom and the same cause.

### BG-122 — Two more forms drew a caret in both their fields

- **Found in** v0.4.0. **Found by** auditing the help against the system —
  photographing Notepad's Replace bar to check the sentence *"Ctrl+H is the
  same bar with a second field"*, which is true, and noticing both fields had a
  caret in the picture taken to prove it.
- **What it was** BG-118 again, in three windows the first sweep did not reach.
  Each has **two flags for one fact**: a `..._focused` boolean that decides
  where keys go, and `recon_edit.active` that decides where the caret is drawn.
  Only the first was kept up to date.
  - Notepad's **Find and Replace** bar.
  - The Control Panel's **Add Account** form — a name and a password.
  - The Control Panel's **Registry → Add** form — a key and a value.
- **Fixed in** v0.4.0. One small function per form sets both `active` flags
  from the one that was already the answer, called wherever that flag changes.
- **And the order matters, which cost a round.** The first attempt at the
  account form called it *before* `recon_edit_begin` on the two fields —
  and `recon_edit_begin` sets `active`, which is how a window with a single
  field gets a caret without asking. So the caret went back on both, the
  photograph looked identical to the one before the fix, and the code read
  correctly from either line on its own.
- **Why the first sweep missed these**: BG-118 was fixed by making the *drawing*
  ask, which was right and sufficient for every window with one field — most of
  them. A window with two fields needed the second half, which is somebody
  telling the fields which one it is, and there was no way to find those except
  by looking at each form.

### BG-123 — The change log was cut off at 512 lines and did not say so

- **Found in** v0.4.0. **Found by** searching the help for "STARTTLS", getting
  the change log as the only result, and landing on the top of it. The search
  was working; the word was not among the lines the page had kept.
- **What it was** `struct page` held `char lines[512][200]` and every loop that
  filled it stopped at 512. A page longer than that ended there, and the "N more
  lines below" note counted only as far as the cap — so the bottom two thirds of
  one version's change log could not be reached by scrolling, and nothing on
  screen suggested there was anything to reach.
- **v0.4.0's own entry is about 73 KB**, which wraps to well over a thousand
  lines. Anybody opening the change log has been reading a third of it.
- **A hundred kilobytes per page, whichever page it was.** A two-line topic and
  the whole change log cost the same, because the array was sized for the worst
  case and there were two of them — the help window and the What's New notice.
- **Fixed in** v0.4.0. The lines are allocated and grown as they fill, so there
  is no cap and a short page costs what a short page costs. Grown rather than
  counted first, because counting means walking the text twice with the same
  wrapping rules, and two implementations of one rule is how they come to
  disagree about where a line breaks.
- **Why it took a search feature to find it.** Nobody scrolls to the bottom of a
  change log to check it ends where it should; the failure looks exactly like
  the document being that long. It took a feature that *jumps* to a place, and
  then does not, for the missing part to become visible.

### BG-124 — The web viewer cut a long page off and said nothing

- **Found in** v0.4.0. **Found by** sweeping for the shape BG-123 had, which is
  a fixed ceiling that drops data quietly. `recon_html.c` has six of them.
- **The ceilings are right and are not the bug.** A page is somebody else's file
  and can be any size; a reader that grows to fit whatever it is handed is one a
  hostile page can exhaust. Truncating is the correct answer and the comment
  beside them already said so.
- **The silence was the bug.** A page cut off at four thousand blocks looks
  exactly like a page that ended, and nobody scrolls to the bottom of a document
  to check whether it finished. The status line said "4000 blocks, 453 KB" as
  though that were the whole of it.
- **Fixed in** v0.4.0. Every ceiling records that it was reached and the status
  line says the rest is not shown.
- **The first attempt reported nothing, and the test caught it.** Five of the
  six ceilings were marked and the sixth — `close_block`, which is the path
  every ordinary paragraph closes through, and therefore the only one a long
  page reaches — was not. The edit had been written; the script that made it
  aborted on a later assertion before writing the file, and reported the
  earlier change as done. **A page of five thousand paragraphs stopped at four
  thousand and still said nothing**, which is what said so.

### BG-125 — A folder with more than 512 things in it looked complete

- **Found in** v0.4.0. **Found by** continuing the sweep that produced BG-123
  and BG-124: every fixed ceiling in the system, and what it does when reached.
- **What it was** File Explorer read a folder into a 512-entry array and then
  wrote `if (count > ENTRIES_MAX) count = ENTRIES_MAX;`. A folder of six
  hundred files showed five hundred and twelve and said "512 items", which is
  a sentence that is true about the window and false about the folder.
- **The number that did not fit was already known and thrown away.**
  `recon_fs_list` documents that it returns the total and writes as many as
  fit; the line above was the only place that total existed, and it was being
  clamped away on the same line.
- **Fixed in** v0.4.0. The status line says "512 items ... and 88 more this
  window cannot show", as an error rather than a note — a list that is short
  and looks complete is the kind of wrong somebody acts on.
- **Two caps added earlier the same night had the same fault**, in the package
  manifest: a seventeenth `place` line and a seventeenth `setting` were
  silently dropped. Those refuse the install and name the limit instead, which
  is the rule everywhere else here — a thing that will not fit is said about,
  not shortened. Installing most of a package and reporting success is how a
  missing file turns up as something not working weeks later.

### BG-126 — The shell forgot three panels and a timer when it was destroyed

- **Found in** v0.4.0. **Found by** running the *compositor* under the leak
  checker rather than only the test suites. The suites cover what can be
  tested without a screen; a panel is not one of those things, so nothing had
  ever looked.
- **What it was** `recon_shell_create` makes nine panels and four timers.
  `recon_shell_destroy` freed six panels and three timers. The tooltip, the
  All Programs list, the screen blanker and the slide timer were simply
  missed — which is what a list of nine calls that has to match a list of nine
  calls somewhere else invites.
- **The timer is the serious one, and it is not a leak.** A timer left on the
  event loop still points at the shell that has been freed, and `on_slide_tick`
  dereferences it on the first line. The shell is destroyed on
  `services restart Shell` as well as at shutdown, so **restarting the shell
  while a window was sliding would have written into freed memory.** Nobody has
  hit it because a restart takes a deliberate command and a slide lasts a
  quarter of a second — which is exactly the kind of window that stays open for
  years and then closes on somebody once.
- **Fixed in** v0.4.0. All nine panels and all four timers, in the order they
  are created, so the two lists read side by side.
- **Measured, not assumed.** The leak checker's total for a session that opens
  every application and shuts down cleanly went from **8,097,016 bytes to
  4,245,584**, and the only ReconOS frame left in the report is
  `wlr_renderer_autocreate`, which is wlroots holding something to exit.
- **Resident memory was flat across thirty shell restarts both before and
  after**, which is worth recording because it is why this was never noticed:
  the allocator does not return the pages, so the leak is invisible from
  outside the process. A checker that looks inside is the only thing that finds
  this class.

### BG-127 — A long enough dialog read a line that was never written

- **Found in** v0.4.0. **Found by** writing a message long enough to fill the
  dialog, which nothing had done before: the warning about opening a file with
  a program that does not claim it has to say what will happen *and* how to
  undo it, and that is the first message in the system past four lines.
- **What it was** `dialog_wrap` fills `lines[DIALOG_LINE_MAX][128]` and returns
  how many it wrote. Its loop increments `count` and then breaks when
  `count >= DIALOG_LINE_MAX`, so on a message that overflows it leaves the
  loop with `count == DIALOG_LINE_MAX` and returns `count + 1` -- one larger
  than the array the caller passed in. The drawing then reads `lines[i]` for
  `i` up to and including `DIALOG_LINE_MAX`, **one row past the end**, and
  draws whatever was on the stack there.
- **Why nothing had seen it.** Every message in the system was short enough.
  The ceiling was raised from three to six once already, for the properties
  box, and the off-by-one came along with the code that raises it -- a bound
  that is only wrong past the bound is invisible until something reaches it.
- **Fixed in** v0.4.0. `count` is clamped back into the array before anything
  indexes with it, and the ceiling is ten.
- **And the reason it was reachable at all is recorded separately:** the
  overflow was silent. A message longer than the dialog lost its tail and
  looked complete, which is the same fault BG-123 through BG-125 were about.
  The last line now ends in "..." when there was more, so a dialog that is
  short and a dialog that is missing its point look different.

### BG-128 — ReconOS could not be asked to stop, only killed

- **Found in** v0.4.0. **Found by** adding the marker file that tells a crash
  from a power cut, and then finding it left behind after every ordinary run
  of the look harness. The harness stops ReconOS the way everything stops a
  program: `kill`, which is SIGTERM.
- **What it was** Nothing handled SIGTERM, SIGINT or SIGHUP. The default for
  all three is to end the process immediately, so **every ordinary stop skipped
  every line of the teardown.** A service manager stopping ReconOS, a logout
  script, a terminal closing, `kill` with no arguments, and the test harness
  all did the same thing a crash does.
- **What was skipped.** `recon_control_destroy`, so the control socket stayed
  in the filesystem for the next run to trip over. `recon_shell_destroy` and
  every other destructor. `recon_keyring_lock`, so the key derived from
  somebody's password was left in memory that is freed and not scrubbed --
  which is the exact case the lock at shutdown was added for. And, once it
  existed, the marker: so the next start reported a crash for a stop that was
  perfectly deliberate.
- **Why nothing had noticed.** Every one of those is invisible from outside.
  The process ends either way, the exit status is the same to a shell that is
  not looking, and a socket left behind is unlinked by the next start before
  it binds. The bug had no symptom until something was added that could see it.
- **Fixed in** v0.4.0. `wl_event_loop_add_signal` for all three, calling
  `recon_quit` -- the same function Alt+Q calls, deliberately, so there is one
  way out rather than two and the rarely-run one cannot rot. Through the event
  loop rather than `signal()`, because a signal handler may call almost nothing
  and `wl_display_terminate` is not on that list; Wayland catches it and
  delivers it between two iterations of the loop, where there is no
  restriction.
- **Measured.** `kill -TERM` now logs "asked to stop (signal 15)" then
  "shutting down", exits 0, and leaves the logs directory empty.
  `kill -KILL` -- which nothing can catch -- leaves the marker, and the next
  start writes `VT-A005  The last run ended unexpectedly` naming the time and
  version of the run that died.

### BG-129 — Two of File Explorer's dialogs read a field that had been cleared

- **Found in** v0.4.0. **Found by** a `-Wformat-truncation` warning, which was
  about something else entirely: `question_target` had just been grown from a
  name to a path, and `explorer_answer` copies it into a name-sized local.
  Following that warning to see whether the local mattered is what showed that
  the field it copies *from* is emptied on the next line.
- **What it was** `explorer_answer` copies `question_target` into a local and
  then calls `cancel_delete`, which sets `question_target[0] = '\0'`. Two
  handlers -- the Open With warning and the new upgrade offer -- read
  `ex->question_target` directly rather than the copy, so both received an
  empty string.
- **What it did.** "Open Anyway" on File Explorer's Open With warning set the
  opener for a file called "", which is no file, and opened nothing. The
  desktop's identical menu was fine, because the shell keeps its answer
  differently -- so the feature worked when tested and did not work in the
  other half nobody photographed.
- **Both were written the same night as the bug.** Which is the point worth
  keeping: the desktop path was driven end to end with the look harness and
  passed, and that was taken as the feature working. It was the feature
  working *on one of the two menus that are supposed to be the same menu*.
- **Fixed in** v0.4.0. Both read the copy. The copy is path-sized, and the
  comment above it says what it is for, since the reason cannot be seen from
  either handler.

### BG-130 — A window's title had eight pixels less room on one side than the other

- **Found in** v0.4.0. **Found by** the first test ever written for
  `recon_titlebar`, which `scripts/coverage.sh` had just reported as one of two
  files no suite runs a line of.
- **What it was** With the buttons on the left, the title started an `inset`
  clear of them. With the buttons on the right -- which is the default and what
  every skin that ships uses -- its room ran up to the leftmost button's edge
  with no gap at all. So the two sides gave the title different room, by
  exactly one inset, for no reason anybody had chosen.
- **What it did.** A title longer than the room is clipped at exactly that
  width with an ellipsis, so on the default side the "..." finished against the
  button rather than a margin short of it.
- **Why nobody had noticed.** Eight pixels on one edge, on the side almost
  nobody compares against the other, in a layout that has to be *read* to see
  the difference: the two branches are forty lines apart and each is correct on
  its own terms.
- **Fixed in** v0.4.0. The right-hand branch leaves the same `inset` the
  left-hand one does. The test asserts the two sides give the title the same
  room, which is the property rather than the arithmetic.

### BG-131 — One damaged byte in the rules file turned the firewall off

- **Found in** v0.4.0. **Found by** writing the first test that reads the rules
  file, which `scripts/coverage.sh --zero` had just named: `parse_rule`,
  `action_from`, `protocol_from` and `add_rule`, all at zero. The rules file is
  the one part of a firewall somebody edits by hand, which makes it the part
  most likely to arrive damaged.
- **What it was** The switch was read as
  `g_on = (value is "yes" or value is "on")`. Everything else is therefore
  **no**. A single byte damaged anywhere in that value -- `on = yqs` -- turned
  the firewall off, with nothing said and nothing to see: the Control Panel
  shows the switch as off, which looks exactly like somebody having turned it
  off.
- **The same shape, twice more, in the more dangerous direction.**
  `default in` fell back to block, which is safe. `default out` fell back to
  **allow**. So damage to this file did not make the firewall complain, it made
  it weaker, quietly, in the direction nobody would choose.
- **It contradicts the module's own note.** `recon_firewall_init` says, at the
  top: "A firewall that fails open because its file is missing is worse than no
  firewall, because it looks like one." Missing was handled -- the defaults are
  in memory before the file is opened. Damaged was not, and damaged is the case
  the sentence describes.
- **Measured, not reasoned about.** The test wrote `on = yqs` and printed the
  switch it got back: OFF. That line is still in the test, printing what it
  reads, so the next person to change this sees the answer rather than the
  assertion.
- **Fixed in** v0.4.0. A value that is neither yes nor no leaves the setting
  alone -- at whatever the built-in defaults put there a moment earlier, which
  is a working set by construction -- and **VT-H003 is raised**, which is what
  that code is for and which had no site anywhere in the system until this.
- **And the other half is checked too**, because the fix could have broken it
  without anybody noticing: an explicit `on = no` still turns the firewall off.
  A switch nobody can turn off is not a switch.

### BG-132 — Watchtower said "0 shown" above the applications it was showing

- **Found in** v0.4.0. **Found by** photographing the whole desktop at the end
  of the night to check that eight thousand lines of change had not broken
  anything visible. Nothing was broken; this was.
- **What it was** The footer's count came from `rows_matching`, which is set by
  whichever tab *draws* its rows. The line was built in `sample()`, which runs
  on the timer **before** the draw -- so the number shown was the previous
  frame's, and on the first frame it was whatever the struct was created with.
  Opening Watchtower showed **"0 shown" above seven applications**.
- **It corrected itself after a second**, at the next timer tick, which is why
  it had survived: the wrong number is only on screen while somebody is still
  looking at the window they have just opened.
- **Fixed in** v0.4.0. The line is built after the rows and before the footer
  that reports them, which is the only order in which the number can be the one
  on screen. `sample()` still builds it too, so a tick with no redraw keeps the
  processor and memory figures moving.

### BG-133 — A link inside the filesystem read a file outside it

- **Found in** v0.4.0. **Found by** writing an exhaustive escape sweep for
  `recon_fs`, after `scripts/coverage.sh` put that file at 46% -- the largest
  untested surface left, and the one that owns containment.
- **What it was** `normalize()` splits a path on separators, drops `.`, pops on
  `..` and refuses to go below the root. That is complete against anything
  *written* in a path, and the twenty escape shapes the new sweep tries all
  come back refused or land inside. **It says nothing about what the resulting
  name turns out to be on the host.** A symbolic link inside the tree pointing
  at `/etc` normalises perfectly and lands outside.
- **Measured, not argued about.** The test made a link at `/Temp/way-out`
  pointing at `/etc/hostname` and asked `recon_fs_read` for it. It returned
  thirteen bytes of `/etc/hostname`.
- **How one gets in.** ReconOS cannot make a link -- there is no command for
  it. The host root is an ordinary directory, though, and `cp -a` of a tree
  containing one brings it along; `scripts/look.sh` copies the filesystem that
  way on every run. A containment rule that holds only while nobody copies
  anything in is not one.
- **Fixed in** v0.4.0. `recon_fs_resolve` now resolves the deepest part of the
  host path that exists and refuses anything that does not land under the root
  -- which also catches a link used as a directory halfway along, not only as
  the last name. The root is resolved once at startup because the root itself
  may sit under a link: `/tmp` is one on several systems.
- **What it costs, measured.** A resolve goes from about 150 ns to 6-10 us --
  forty to seventy times. In the system that is 100 ms once, on the very first
  start, while the whole tree is being written: **216 ms to 317 ms**. Every
  start after that is 92 ms either way, because there is nothing left to write.
- **And the fix had a bug of its own, in the same line, twice.** See BG-134.

### BG-134 — The containment fix aborted every optimised build

- **Found in** v0.4.0, in the change that fixed BG-133, before it shipped.
  **Found by** benchmarking the cost of that change, which meant building it
  at `-O2` for the first time.
- **What it was** `realpath`'s second argument must be at least `PATH_MAX`
  bytes -- 4096 on Linux -- and it was given a `RECON_PATH_MAX` buffer, which
  is 1024. That is not a truncation. With `_FORTIFY_SOURCE` on, which is any
  optimised build on this distribution, glibc checks the size and **kills the
  process**. ReconOS aborted on the first path it resolved.
- **Every check this project has said it was fine.** Twenty-three suites, the
  address and undefined-behaviour sanitizers, and the static analyzer, all
  clean -- because `build.sh`, `check.sh`, `analyze.sh` and `coverage.sh` every
  one of them pass `-DCMAKE_BUILD_TYPE=Debug`, and Debug is not fortified.
  **`scripts/package.sh` builds Release**, so the one configuration that ships
  was the one configuration nothing ran.
- **`realpath` was also declared implicitly**, because glibc puts it behind
  `_XOPEN_SOURCE >= 500` rather than the `_POSIX_C_SOURCE` this file asked
  for -- so it returned `int` and the result was a truncated pointer. The
  compiler said so and the tests did not.
- **Fixed in** v0.4.0. `realpath(path, NULL)` allocates what it needs, which
  is POSIX and has no size rule to get wrong, and the file asks for
  `_XOPEN_SOURCE 700`.
- **And `scripts/check.sh` now runs every suite twice**: once sanitized, once
  built the way a release is. Putting the small buffer back makes six suites
  abort in the second pass while the first stays clean, which is the shape of
  the whole class.

### BG-171 — Half of a Wikipedia article was the contents of an attribute

- **Found in** v0.4.19. **Found by** a sweep of twelve real sites, looking at
  what came back rather than at whether it crashed. Wikipedia's *Comparison of
  operating systems* had `{{cite web|...}}` and `[[Lisa OS]]` in the middle of
  it -- wiki source, in a rendered article.
- **What it was** the parser found the end of a tag with `memchr` for the
  first `>`. An attribute value may be quoted, and a quoted value may contain
  anything at all including `>`, and it is still inside the tag.

  Wikipedia carries the wiki source of every reference in a
  `data-mw='{"parts":...}'` attribute: single-quoted, holding JSON, and that
  JSON holds escaped HTML with `>` in it. The scan stopped at the first one,
  decided the tag had ended in the middle of an attribute, and rendered the
  remaining several hundred bytes as page text.
- **How much of the page it was.** 411 blocks before, **219 after** -- so
  nearly half of what that article appeared to say was attribute rather than
  content.
- **Why nothing had caught it.** Every hand-written test used well-formed
  markup with short attributes, because that is what somebody writing a test
  writes. The html5lib corpus did not catch it either: it tests tree
  construction and this parser builds no tree, so it was only ever run for the
  invariants -- and "the page says something it should not" is not an
  invariant, it is a fact about a particular page. It took loading real ones.
- **Fixed in** v0.4.19. `tag_end` tracks the quote character and handles both
  kinds. A quote of the other kind inside a value is ordinary text --
  `alt="it's fine"` is one apostrophe and no quoting problem.
- **And the first fix was wrong, which is worth recording.** It ended a quote
  at a newline, on the reasoning that a newline inside quotes usually means an
  unclosed one and that this would stop a malformed tag eating the page. That
  guard caused exactly what it was written to prevent, on the second page it
  was pointed at: gaming.recontowers.com carries an SVG colour-matrix filter
  whose `values` attribute is a matrix written over five lines, so the closing
  quote looked like an opening one and the tag ran on for three and a half
  thousand characters -- far enough to swallow the `<details>` after it, whose
  contents then appeared in full because the runaway attributes happened to
  contain the word "open". Caught by looking at the user's own site rather
  than at the block counts, which had gone *up* and could have read as the fix
  working. An unclosed quote now falls back to the first `>`, which is where a
  scanner that ignored quotes would have stopped: wrong about one tag instead
  of about the whole document.

### BG-170 — Two secrets were erased with `memset`, which the compiler deletes

- **Found in** v0.4.12. **Found by** the keyring review Joshua asked for --
  reading outward from `recon_keyring.c` to what its callers do with what they
  are handed.
- **What it was** two places cleared a secret with `memset` and nothing read
  the buffer afterwards, which makes the store dead and lets the compiler
  remove it. It does.
  - `recall_password` in `recon_mailwin.c` -- the **decrypted mail password**,
    on the stack.
  - `recon_tls.c` -- the machine's **TLS private key**, cleared immediately
    before `free`. `free` does not read what it is handed and the compiler
    knows it, so a clear-then-free is the textbook case.
- **Measured rather than argued.** A function that fetches a password, copies
  it out and memsets its buffer compiles at -O2 to the two calls and a return,
  with **no zeroing at all**; the same function using `recon_secure_erase`
  keeps the volatile loop. That function exists in this project for exactly
  this reason, and its header says so.
- **Why nothing caught it.** Every build ReconOS makes for itself is Debug
  with no `-O`, where nothing is eliminated, so the disassembly looked
  correct. `scripts/package.sh` builds Release -- **so the thing that ships is
  the configuration where the erase disappears.** The sanitizers cannot see it
  either: there is no invalid access, no leak and no undefined behaviour. The
  program is correct; it simply does not do what the line was written to do.
  This is the same shape as the `memset` found by objdump earlier in this
  project's history and recorded in the method notes: **a security property
  holding by accident of build flags.**
- **Fixed in** v0.4.12, and `scripts/check.sh` now fails on any `memset` of
  something named like a secret -- the only place the difference between the
  two calls is visible, since no test and no sanitizer can see it.

### BG-169 — Typing an address went to a path on the site you were reading

- **Found in** v0.4.11. **Found by** the History page, on its first look: the
  list said `https://gaming.recontowers.com/example.com`, which is not
  anywhere anybody had asked to go.
- **What it was** `go_typed` resolved what was typed against the page being
  read, the same way a link on that page is resolved. A link on a page *is*
  relative to it. **Text typed into an address bar is not.** So once you were
  on any page at all, you could not reach a different site by typing its name
  -- only paths on the site you were already on. Typing
  "news.ycombinator.com" on another site fetched
  `thatsite.com/news.ycombinator.com` and reported, accurately, that there is
  no page at that address.
- **How long it had been there.** Since the address bar existed. It survived
  because every test of it typed an address into a *fresh* tab, which has no
  page to be relative to -- so the wrong path was never taken. The same shape
  as BG-166 and BG-162: a check written against the one case somebody
  happened to try.
- **Fixed in** v0.4.11. A path or a fragment -- "/about", "#notes" -- is
  resolved against the page, because there is nowhere else those could mean.
  Everything else is somewhere to go.
- Worth noting where it was found: not by a test and not by reading, but by
  building a screen that *shows the data*. A list of visited addresses is a
  thing you look at, and the wrong one is obvious the moment it is on screen.

### BG-168 — Every link to a place on the same page went to the top

- **Found in** v0.4.10, while building the jump those links needed. **Found by**
  clicking "Skip to content" on gaming.recontowers.com and arriving at the top
  of the page it was already at the top of.
- **What it was** `recon_http_parse_url` pulls the fragment out by searching
  for a hash in `out->path`. For a relative link that branch works, because the
  hash is still in the text it copied. For a link that is *only* a fragment --
  `href="#main"` -- the branch copies **the base's** path, whose own hash was
  stripped when the base was parsed. So the search found nothing, every such
  link parsed to an empty fragment, and the viewer did the correct thing for a
  link that names no place: went to the top.
- The fault is that the fragment was being taken from the wrong place. It is
  in the *text*, and only sometimes also in the path.
- **Why there was nothing to catch it.** There was no suite for reading an
  address at all, and there could not easily be one: the parser lived in
  `recon_http.c` beside the fetching, so testing it meant linking sockets,
  TLS, the registry and a Wayland event loop. **An address parser that cannot
  be tested without a socket is an address parser nobody tests.**
- **Fixed in** v0.4.10. The fragment is taken from the text in that branch.
  The parsing moved to `src/recon_url.c`, which opens nothing and links
  nothing, and `tests/test_url.c` covers it -- including the case above, which
  fails on exactly two claims when the bug is put back.

### BG-167 — Every `<hr>` claimed to be a picture at the page's first link

- **Found in** v0.4.9. **Found by** the html5lib corpus, on its first run, from
  cases like `<!doctype html><hr><frameset>`.
- **What it was** `add_rule` is the one place that writes a block without going
  through `close_block`, and it set four fields of six. The missing one was
  `source`, the index of the block's picture. The block array is `calloc`ed, so
  the field was not garbage -- it was **zero**, and zero is not "no picture", it
  is "the picture at link zero". Every horizontal rule on the web said it was
  an image whose address was the first link on the page.
- **Why nothing had noticed.** Every reader of `source` checks the kind first,
  so the wrong value was never acted on. It sat there being wrong and costing
  nothing, which is this project's most common shape of fault: a thing that is
  untrue with nothing arranged to notice. A hand-written test would not have
  found it either -- you do not write a test asking whether a horizontal rule
  has a picture.
- **Fixed in** v0.4.9. The entry is cleared and then filled, rather than the
  fields this happens to care about being set and the rest left as they were --
  because the next field added to that struct would have been missed here too.

### BG-166 — Find on page searched the screen, not the page

- **Found in** v0.4.9, shipped in v0.4.8. **Found by** searching
  gaming.recontowers.com for a word further down than the first screenful and
  being told it is not on the page.
- **What it was** the match test was written into `put_word` *below* its two
  early returns -- the one for the measuring pass, and the one for a word that
  is off-screen. Whether a word matches is a fact about the document and
  everything below those returns is about the screen, so the count was of the
  matches already visible: "1 of 2" meant two on this screenful, and a word
  below the fold found nothing at all.
- It also broke the jump, and in a way that hid itself: the position recorded
  for the current match could only ever be a position already on screen, so
  "go to the match" had nothing to go to and correctly did nothing.
- Every test of it had used a word visible in the first screenful, which is
  the same fault as BG-162 in a different place -- **a check written against
  the case you happened to look at**.
- **Fixed in** v0.4.9. The match is settled at the top of the function, before
  anything is allowed to return; the highlight, which is about drawing, stays
  where it was.

### BG-165 — A background on a page with no `<body>` tag was never found

- **Found in** v0.4.7. **Found by** the test written for BG-164, whose markup
  was `<style>...</style><p>hello</p>` -- no `<body>`, because nobody writes
  one in a test.
- **What it was** the paper was taken as the `<body>` element went past. Both
  `<html>` and `<body>` are optional in HTML and plenty of real documents omit
  them, so on those pages the stylesheet was read correctly and then there was
  nothing to apply it to.
- Worth keeping as a bug rather than folding into BG-164, because the test
  found it and the site did not. gaming.recontowers.com writes its `<body>`,
  so the feature looked finished; the case that does not exist on the one page
  being looked at is exactly the case a test is for.
- **Fixed in** v0.4.7. When no `<body>` went past, the sheet is asked what one
  would have got -- the same question, put to a page that never wrote it down.
  A real `<body>` still answers first, because its answer includes its `style`
  attribute and a synthetic query cannot see one.

### BG-164 — Every site was drawn in the skin's black on the skin's white

- **Found in** v0.4.6. **Found by** loading gaming.recontowers.com, a
  near-black site, and getting a white page.
- **What it was** two faults that looked like one.

  `background-color` was parsed by the CSS engine and then dropped: nothing
  recorded it and nothing drew it. And underneath that, the engine could not
  read the value anyway -- the page says `background: var(--rt-bg)`, and
  custom properties were not implemented at all. Measured on that sheet: 157
  custom properties, 378 uses of `var()`, and one literal colour in 62 KB. So
  a reader without `var()` reads a modern stylesheet and finds no colours.
- **The check that was asking the wrong question.** Page colours were tested
  for readability against **the skin's** surface, which was right while the
  page had no surface of its own. On a page painting its own it gave the wrong
  answer in both directions: a light heading was rejected for being
  unreadable on white, and the near-black body text it fell back to then went
  on near-black paper.
- **Fixed in** v0.4.7. Custom properties are collected from `:root`, `html`
  and `body` in a first pass over each sheet, and `var(--name)` and
  `var(--name, fallback)` resolve against them in a second -- two passes so a
  sheet that uses a name above the `:root` that defines it still works, which
  is not rare once a bundler has concatenated four files. The page's
  background reaches the viewer, which paints the content area with it and
  then checks every colour on the page -- text, links, list markers, rules,
  the quote bar -- against **that** paper. The check was not weakened and has
  no way to be turned off; it was pointed at the right surface.
- **What it still does not do.** Custom properties inherit and may be set on
  any element; this takes the page-wide set only, so a component's override
  reads the page-wide value -- a colour from the same palette, wrong in shade
  and never wrong in contrast. `color-mix()`, gradients and `rgb(var(--x) / a)`
  are not colours this can read, and the properties written that way are left
  unset rather than guessed at.

### BG-163 — An image drew straight over the status bar

- **Found in** v0.4.6. **Found by** loading gaming.recontowers.com, whose
  masthead photograph is the first thing on the page.
- **What it was** the web viewer decided whether an image was *visible* and
  then drew all of it. A picture three hundred pixels tall whose top sits ten
  pixels above the last visible row passed the test and painted the other two
  hundred and ninety over the status bar below the page.
- **Why it was never a problem for text.** A line of text is twenty-four
  pixels tall, so the same test is wrong by less than one line and nobody
  sees it. The check was written for lines and then handed to something
  fifteen times taller without being looked at again -- the same shape of
  fault as BG-161: an answer that was right for one size of thing, reused for
  another.
- **Fixed in** v0.4.6. `recon_draw_image_clipped` takes the band of rows it
  may paint, and `recon_draw_image` is that call with the panel's own height
  as the band -- one implementation, so the clipped path cannot drift from
  the plain one. The viewer passes its viewport.

### BG-162 — A `<br>` ended the heading, not just the line

- **Found in** v0.4.6. **Found by** loading gaming.recontowers.com, whose
  masthead is `<h1>Games built to<br>mean something.</h1>`. The first half
  rendered at heading size and the second at body size, in the middle of one
  sentence.
- **What it was** `<br>` called `break_block`, which ends the open block and
  opens a **paragraph**. But a `<br>` does not end anything except the line:
  it is still the same heading, the same list item, the same quote. So every
  heading, item and blockquote on the web with a line break in it lost its
  kind at the break.
- **How long it had been there.** Since `<br>` was first handled. Not new, and
  not caused by the `display` work in v0.4.5 -- which is what I first wrote
  here, from reading the code instead of the page. The page's masthead uses a
  plain `<br>`; the `display: block` reading was a guess, it was wrong, and
  the fix built on it passed a test while the real page still rendered the
  fault. **Fetching the markup is what found it.** The test had been written
  against the markup I assumed rather than the markup that was there, which
  is a way for a test to pass and prove nothing.
- **Fixed in** v0.4.6. Two helpers, named for what they do: `break_block`
  starts a paragraph, `break_line` reopens the block it just closed with the
  same kind and level. `<br>` takes the second. The test now covers the
  `<br>` the page actually contains, plus `<p>one<br>two</p>` so the ordinary
  case cannot regress into headings.

### BG-161 — A link was underlined once per word, with a hole at every space

- **Found in** v0.4.5. **Found by** loading news.ycombinator.com, where every
  headline is a link of several words.
- **What it was** the rule and the clickable region were drawn by the function
  that draws one *word*, so a link of eight words got eight underlines with
  seven gaps between them -- which reads as damage rather than as a link. It
  also put eight regions in a finite hit table where one would do.
- Invisible until a page with multi-word links: the pages this was built
  against had links of one or two words, where the holes are easy to read as
  letter spacing.
- **Fixed in** v0.4.5. The wrapping loop tracks where a link began on the line
  it is on and draws one rule when it ends -- at the end of the link, at a
  wrap, or at the end of the block. A link that wraps gets one rule per line,
  which is what it should get.
- **And then once more, one level up.** The first fix closed the rule at every
  *run* boundary, and one link is often several runs: `<a>Free software
  <em>can</em> be commercial</a>` is three, so the emphasised word had a hole
  either side of it. The same fault at the next size, which is worth recording
  as the same bug rather than a new one -- "draw this per piece" was wrong
  about words and wrong again about runs, for one reason.

### BG-160 — The window's title was every `<title>` in the page, not the page's

- **Found in** v0.4.5. **Found by** loading wikipedia.org to see whether the web
  viewer reaches the internet. It does; the title bar said **"Wikipedia Close"**.
- **What it was** `<title>` is not only the document's. SVG uses it for the
  accessible name of a drawing, so an icon inside the page can carry one --
  wikipedia.org has several, and the parser turned collection back on at every
  opening tag. The document's title, then the label on a close button inside an
  inline SVG, concatenated.
- **Fixed in** v0.4.5. The first one wins. Which `<title>` is the document's
  cannot be told from the tag, so it is told from the order: a document's title
  is in its head, and the head comes first.

### BG-159 — The web viewer's status line was the bar's colour written in a page's ink

- **Found in** v0.4.5. **Found by** the same load: "396 blocks, 117 KB" was
  drawn on the strip and could not be read.
- **What it was** the strip fills with `bar` and the text was `surface.text-dim`
  -- a dark grey meant for a pale page. Correct for as long as every skin's bar
  was grey. On Beacon's blue or Metallic in garnet the line was there and
  invisible. Exactly BG-151, one window over.
- **Fixed in** v0.4.5. `recon_color_readable_on`, which was written for the
  clock and is the answer to this whole class: keep what the skin asked for
  wherever it can be read, and otherwise take the ink that skin writes on a
  coloured surface. The warning colour goes through the same lens -- a red
  warning on a red bar is the case where being unreadable matters most.

### BG-158 — The search box only showed a caret once somebody had typed

- **Found in** v0.4.0. **Found by** the author: "I can type in it, but I can't
  click into it to know that I'm typing. There's no indicator."
- **What it was** the box drew a plain border, a placeholder, and a caret only
  when the filter was non-empty. That is exactly backwards. The caret is the
  thing that says *type here*, so showing it only after somebody has typed is
  showing it to the one person who no longer needs it -- and clicking the box
  genuinely did nothing, because the click had nothing left to do: the keys
  were already going there.
- The first report was "the search still doesn't do anything when I type",
  corrected a minute later. Worth keeping both: a control that gives no sign
  of being live is first read as broken, and only on a second look as working
  and silent.
- **Fixed in** v0.4.0. It is drawn as the focused field it always is, because
  while the menu is open nothing else in it takes typing. Two pixels of border
  in the selection colour and a caret that is always there -- the border says
  it from across the menu, the caret from close up. The placeholder moves five
  pixels right, so an empty box reads the way a focused empty field reads
  anywhere: the caret is where the next letter lands, the hint is what the
  letters are for.

### BG-157 — The "last run did not finish" card reported itself as `(null)`

- **Found in** v0.4.0. **Found by** running `session` over the control socket
  after a session had been killed rather than stopped: `session: (null)`, while
  `state` correctly reported that the login screen had the input.
- **What it was** the same fault as the one the comment above that table
  already describes, and the fix written for it does not prevent. Designated
  initialisers stop a name landing on the *wrong* stage; they do nothing at all
  about a stage with no name. `STAGE_STOPPED` and `STAGE_LAST_STOP` were added
  to the enum and not to `STAGE_NAMES`, so both read back as a null pointer --
  and printing one through `%s` says `(null)`, which somebody debugging a
  session reads as "the session is broken".
- **Fixed in** v0.4.0. Both named. The table is sized by the last stage rather
  than by what is written in it, so there is always a slot, and a slot nobody
  filled prints `stage N, unnamed` -- which reads as "somebody added a stage
  and not a name", is the truth, and is one line to fix. A hole cannot be
  caught at compile time; it can be made to name itself.
- Confirmed on the stage that was broken: killed a session, started another,
  and it now says "the last run did not finish".

### BG-156 — A picture icon on a coloured title bar was dark on dark

- **Found in** v0.4.0. **Found by** the author: "the icons in the top left
  corner can't be seen, and they should be."
- **What it was** an icon that is a *silhouette* is drawn in the skin's own
  ink, so it reads on whatever the skin puts behind it. An icon that is a
  *picture* keeps the colours it was made with and takes its chances.
  Measured on the Photos window under Metallic in garnet: a blue picture-frame
  icon on a dark red title bar. Classic was worse -- the same blue on navy.
- **Why it survived** every skin whose icons are silhouettes was fine, and the
  ones that are not had grey title bars until the tint arrived.
- **Fixed in** v0.4.0. At chrome size a picture gets a one-pixel outline in the
  ink the title text is written in. That colour is by definition one the skin
  knows reads on that surface, and an outline says where the shape is without
  pretending to know what colour the shape should be. Silhouettes are
  untouched: they already answer this by being recoloured.

### BG-155 — The selection colour was the one thing on screen not told which skin this is

- **Found in** v0.4.0. **Found by** the author: "the right-click context menus
  aren't showing up properly. They're not theming like they're supposed to."
- **What it was** `role_takes_tint` left out the accent, the warning *and* the
  selection, on the rule that a colour carrying a meaning should not change
  with the decor. That rule is right and the selection is not it. The accent
  says "this is the important one" and the warning says "this will lose
  something" -- learned colours. A selection says "this one, the one you are
  pointing at", and it says that by being different from its neighbours, not
  by being blue.
- So a deep garnet desktop opened a grey menu with a slate blue bar across the
  row under the pointer.
- **Fixed in** v0.4.0. The menu highlight, the list selection, the field
  selection and the desktop selection follow the tint. On a solid-tint skin
  they are *shifted* rather than repainted -- they keep their own lightness and
  move only in hue -- because they carry white text and Silver's lightness
  under white text is a row nobody can read.

### BG-154 — Sixteen skins was thirteen shipped ones and a rounding error

- **Found in** v0.4.0. **Found by** the skin tests, the moment Metallic became
  the thirteenth built-in: ten checks failed at once, all of them about
  renaming.
- **What it was** `THEMES_MAX` was 16. Thirteen shipped, and the rename test
  makes a copy and then another -- so the table filled, `recon_theme_copy`
  refused, and every check after it failed. A rename that cannot allocate
  looks exactly like a rename that is broken.
- **Fixed in** v0.4.0. Thirty-two, which is nineteen of somebody's own. The
  number now says what a person might reasonably have rather than what the
  built-ins happen to need.

### BG-153 — A tint chosen on one skin was applied to a skin that never offered it

- **Found in** v0.4.0. **Found by** photographing Metallic and finding it pink.
- **What it was** the account remembers one tint, not one per skin, and
  `recon_tint_current` looked the remembered name up in the whole table rather
  than in the list the current skin offers. A Rose picked on Glass was still
  remembered on switching to Metallic, and Rose is a real tint, so it applied.
  Metallic came out pink -- not one of the metals, and not a choice anybody
  made there.
- It could not happen while one skin was tintable. It arrived with the second.
- **Fixed in** v0.4.0. A remembered tint that the current skin does not offer
  is treated exactly like a name nothing recognises: no tint, and the skin
  shows as itself until somebody picks from its own row.

### BG-152 — The skin list rounded its sample buttons by painting the corners out

- **Found in** v0.4.0. **Found by** the author: "if you're gonna round them
  all, make sure they are actually rounded. This doesn't look right. And it's
  not just this way on Beacon. It's like this on a lot of them."
- **What it was** BG-142, still alive in one place after being fixed
  everywhere else. The Appearance page's sample button filled a square,
  bevelled the square, and then painted the four corners back out in a colour
  it *believed* was behind them. The bevel is square and the paint-out is
  round, so the two disagree about where the corner is: what survives is a
  button rounded on the corners the paint-out reached and ragged on the ones
  the bevel had already lit.
- **Why it lived here** the sample is drawn in *another* skin's numbers, so it
  could not call the framework's button -- that reads the current skin. It had
  its own copy of the drawing and its own copy of the radius cap.
- **Fixed in** v0.4.0. `recon_fill_button_radius` is the same function every
  other button uses with the radius passed in, and `recon_button_radius_of`
  puts the cap in one place instead of two. A copy of a rule is a rule that
  will differ later.

### BG-151 — The clock took its colour from a role that also means "on a button"

- **Found in** v0.4.0. **Found by** the author, on Beacon: "you can barely see
  the time".
- **What it was** `bar.text` is used for the label on a task button *and* for
  the clock, which is the one thing written straight onto the taskbar. Beacon's
  task buttons are pale, so its `bar.text` is a near-black -- correct on a
  button and fifty-five levels from the deep blue bar. The skin was not wrong.
  The role was being asked a question it cannot answer, because it does not
  know which of its two surfaces is being drawn on.
- **Fixed in** v0.4.0. `recon_color_readable_on` keeps what the skin asked for
  wherever it can be read and otherwise takes the ink that skin writes on its
  *title* bar -- which is by definition its ink for a coloured surface, so the
  answer still comes from the palette. The date is the time's ink pulled a
  third of the way back toward the bar, so it stays quieter.
- A test now checks the colours the clock will actually use, for every skin
  that ships, including that the date stays quieter than the time. It checks
  what is drawn rather than what the table holds, because the table is allowed
  to hold a dark `bar.text`.

### BG-150 — A button's outline was a grey that had never looked at the background

- **Found in** v0.4.0. **Found by** the author, on Beacon: "there's that weird
  black line on the outer ring of the buttons".
- **What it was** the outline was the face mixed toward black, and nothing
  else. On Beacon the taskbar's buttons are E0E6F2 on a bar of 2959C4 -- a
  hundred and forty-four levels apart -- and the rule put a 777A81 ring round
  each one. A neutral grey against a saturated blue does not read as an edge;
  it reads as dirt.
- **Why the rule was there at all** BG-141. On Glass the Calculator's keys are
  E8EBF5 on a panel of F0F2F8, eight levels apart, and without an outline the
  button has no edge. Both are true. They are the same rule at two ends of a
  range nobody had noticed was a range.
- **Fixed in** v0.4.0. An outline exists to separate a control from what is
  behind it, so how strong it needs to be is not a property of the control. It
  is now the shaded tone when the two are close and slides toward a shadow of
  the *background* as they separate -- fully so by 128 apart. Glass keeps its
  edge; Beacon gets a deep blue shadow instead of a grey ring.

### BG-149 — The taskbar was see-through, so a window under it washed it out

- **Found in** v0.4.0. **Found by** the author: "when a window opens, no matter
  what it is, for some reason the taskbar greys out and you can't see it. It's
  supposed to stay fully functional and fully visible, like as if it's the only
  thing on screen."
- **What it was** the taskbar took the skin's full chrome opacity, on the
  written reasoning that it is "a strip with a few short labels and survives
  being see-through". Measured, it does not. On Smoked the bar is 1C222C, the
  wallpaper behind it 26374B, and at 200 of 255 the composite lands on 1E --
  sixteen levels from the desktop it is supposed to be in front of.
- And the report is literally true: the same strip measured `1C1D2B` with the
  desktop behind it and `2E2D47` with the Calculator behind it. Seventy-eight
  per cent of a taskbar plus twenty-two per cent of somebody's window.
- **Fixed in** v0.4.0. The taskbar takes no glass, and that is not a number a
  skin can lower -- same reasoning as BG-146 putting it in its own scene layer.
  A skin keeps its bar colour and its gradient, which is what makes an opaque
  strip still look like glass.
- Smoked's bar was also 1C222C against a window frame of 1A1E26 -- the strip
  everything else sits in front of was the *lighter* of the two. It is 0E121A
  now: on a dark skin the taskbar is the floor.

### BG-148 — Switch user was a person standing next to a floating notch

- **Found in** v0.4.0. **Found by** photographing the start menu's five
  session buttons at five times size, after BG-147 was found the same way.
- **What it was** the two figures are drawn back one first, then a gap in the
  button's own colour, then the front one. The gap was a disc and a wide
  rectangle placed over where the *front* figure goes -- which is the wrong
  shape to cut: the rectangle reached across and took the middle out of the
  **back** figure's shoulders, leaving it a head and a disconnected block.
- **Fixed in** v0.4.0. The cut is now the front figure's own outline, two
  pixels fat -- its head disc and its shoulder rectangle, each grown by two.
  What survives is a line that follows the front figure, which is exactly what
  says one person is standing behind another.

### BG-147 — The power symbol was a closed ring with a bar across it

- **Found in** v0.4.0. **Found by** photographing the start menu's five
  session buttons at five times size. At the size they are drawn the mark is
  fourteen pixels and reads as roughly a circle; at five times it reads as a
  *no entry* sign.
- **What it was** `glyph_ring` takes one gap, and a gap that crosses straight
  up cannot be written as one pair of angles -- so the power symbol asked for
  two rings, one missing 340-360 and one missing 0-20. Two rings do not
  intersect, they add: the first drew 0-340, the second drew 20-360, and
  between them they drew all of it. The gap the stem was supposed to pass
  through was never there.
- **Fixed in** v0.4.0. A gap where `gap_to` is less than `gap_from` wraps
  through zero, so it is written the way it is said: from 340 round to 20. One
  ring, one gap.

### BG-146 — Windows covered the taskbar

- **Found in** v0.4.0. **Found by** the author, moving windows about: "the
  task bar should always be on top of every window no matter what because it's
  required to be able to access most things. Yet windows are going over it."
- **What it was** every drawn thing joined the scene's single root tree, so
  the order was decided by whoever raised last. `recon_appwin_focus` raises
  the window it focuses and has no reason to know the taskbar exists, so
  focusing a built-in window put it in front of the taskbar.
- The disguise: the shell re-raised its own panels in the path that handles
  *client* windows and not in the path that handles built-in ones. So it never
  happened to anything a client opened, and always happened to the Control
  Panel and the Calculator -- which looks like a bug in those two programs.
- **Fixed in** v0.4.0. Four trees under the scene root: background, windows,
  chrome, system. A node can be raised to the top of its own tree and no
  further, so where something sits is decided once, at creation, by which tree
  it joined. The taskbar is not in the argument at all -- it is how you reach
  everything else, which makes it a different kind of thing from a window.
- The top tree is what has *taken* the screen: the login screen, the security
  box, a modal dialog and its dimmer. Dimming everything and leaving the
  taskbar bright would say the taskbar still works. The tooltip is up there
  too: it explains whatever is in front, takes no clicks, and leaves on its
  own.

### BG-145 — The outline was diluted by the fill at every corner

- **Found in** v0.4.0. **Found by** the author, going through the skins: "in
  Recon, you can see those weird triangles too."
- **What it was** a control was filled and then outlined -- two blends. At a
  corner the second lands on a pixel the first has already *part*-covered, so
  the outline came out mixed with the **face** where it should have been mixed
  with whatever is behind the control. On a light button over a dark title bar
  that is a pale wedge: measured on a Recon caption button, the corner ran
  `464B5C 707070 9F9F9F C3C3C3 C8C8C8` -- two light greys sitting outside an
  outline of `6A6A6A`.
- Invisible on a light skin, because there the face and the background are
  close enough that mixing with the wrong one of them barely shows. It needed
  a dark title bar under a light button to become a shape anybody could point
  at.
- **Fixed in** v0.4.0. `recon_fill_round_rect_edged` does both in one pass. A
  corner pixel is three things at once -- what is behind it, the outline, and
  the face -- so it is worked out as three, and the three shares add to
  exactly one pixel. Same corner afterwards: `2C3449 656567 6A6A6A 6A6A6A
  C8C8C8`, every blend on the dark side of the outline.

### BG-144 — A white line above the taskbar and above every button, on dark skins

- **Found in** v0.4.0. **Found by** the author switching to a dark skin:
  "there's this weird white line that appears above all the different buttons,
  and I don't know why it's here. It's also appearing above the task bar too."
  Faint in Beacon, a dark line in Protan, absent in Tritan, Contrast, Reading
  and Aqua -- which is the shape of a fault that depends on how light the skin
  is.
- **Two of them, one cause.** The taskbar's top edge was a hardcoded
  `E8E8E8`. Its own comment had already caught half of this -- "a fixed light
  grey line on top of that was a stripe of the wrong colour across a blue
  bar" -- and guarded against skins that *grade* the bar, which left every
  skin that is merely **dark**. Midnight's bar is `24282E`, so the line was
  four times its brightness and ran the full width of the screen.
- The line above each button was the bevel highlight, mixed 110 of 255 toward
  white. A constant fraction of the distance to white is not a constant
  effect: it lifts `E8EBF5` by nine levels and `3A3E46` by **eighty-five**.
  The same arithmetic that reads as a soft sheen on a light button is a pale
  grey bar across a dark one.
- **Fixed in** v0.4.0. `recon_color_highlight` adds a proportion of the
  headroom capped at a step, so on something already light the proportion wins
  and on something dark the cap does -- and both come out looking like the
  same lamp. The taskbar's line is the bar's own colour through it, and the
  guard for graded bars stays. Measured on Midnight: the button highlight went
  from `8E9195` to `545860` against a face of `3A3E46`, and the bar's top line
  from `E8E8E8` to `3E4248` against a bar of `24282E`.
- **The third time this shape of fault has been found in one session.** A
  fixed grey where a derived colour belongs: the bevel (BG-141), this line,
  and this highlight. All three were written when every skin was grey.

### BG-143 — Every selection and hover highlight was square

- **Found in** v0.4.0. **Found by** the author, going round the desktop:
  "when you highlight something, it's square. They should be rounded too...
  If I go to the desktop and I highlight the Recycle Bin, it's the same thing.
  All the highlights are square. They should be matching the shape of
  everything else."
- **What it was** the same shape of fault as BG-138: around thirty plain
  `recon_fill_rect` calls, one wherever a highlight was needed, each written
  out where it was wanted. Nothing owned "what a highlight looks like", so
  when everything else in the system learnt to round, they did not.
- **Fixed in** v0.4.0. `recon_widget_highlight` rounds by the same rule a
  button of that size rounds by, so a highlight and the thing it highlights
  agree without either being told about the other, and a skin asking for
  square gets square here too.
- **Two compositing faults surfaced on the way**, both invisible until
  something anti-aliased was drawn:
  - **Blending onto a transparent pixel produced nothing.** `blend_over`
    keeps the destination's alpha, which is correct where it was used --
    carving a corner replaces one opaque colour with another -- and wrong for
    compositing. The desktop's panel is transparent wherever the wallpaper
    shows through, so the anti-aliased corners of the selection box came out
    at alpha zero. The straight parts used `recon_fill_rect`, which sets the
    pixel outright, so three rectangles appeared and four corners did not:
    a selection box with square bites out of it, which looks exactly like the
    rounding never having been applied.
  - **The colour's own alpha was ignored.** The desktop's selection is
    translucent so the wallpaper reads through it. Composited at full strength
    the corners came out as the *undimmed* colour -- `2F7FD4` against a body
    of `1B487D` -- a bright rim around a darker shape. Coverage says how much
    of the pixel the shape covers and the colour says how solid the shape is;
    the pixel wants both multiplied.

### BG-142 — Every rounded corner was painted out with a guess at what was behind it

- **Found in** v0.4.0. **Found by** the author: "now it has that weird corner
  issue that the windows had... the buttons here in the corner have that same
  thing, where the edges have that weird white triangle."
- **What it was** `recon_round_rect` fills a square rectangle's corners with a
  `behind` colour the caller passes -- a colour the caller *believes* is
  underneath. Every place the belief is wrong shows as a wedge of the wrong
  colour. It is wrong over a gradient always, because a graded surface has a
  different colour on every row and one flat colour cannot be all of them;
  wrong over a wallpaper; and wrong whenever a caller passes the colour of the
  panel it is on rather than the colour of the strip the control is actually
  sitting on.
- The title bar was the worst case and the most visible: several skins grade
  it, so the corners of every close, maximize and minimize button were carved
  back to the bar's single named colour and were therefore wrong on every row
  but one.
- **Fixed in** v0.4.0. `recon_fill_round_rect` and `recon_stroke_round_rect`
  draw the rounded shape *as a shape*, compositing it over what is already
  there. The corner pixels are never overwritten, so whatever is behind them
  stays behind them -- a gradient, a photograph or a flat fill -- without the
  drawing code being told which. Nothing is guessed because nothing is
  painted over.
- **The taskbar was left on the old path in the first attempt, and still had
  the wedge.** It is the one control that cannot be drawn as a shape: it
  fills, draws a window's icon and title into itself, *washes the lot* when
  the window is put away, and only then asks for the edge -- the wash has to
  cover the icon and the title, so it cannot come before them and the fill
  cannot come after. Leaving it carving to `THEME(BAR)` left it wrong on
  exactly the skins that grade the bar, which is where it had been reported.
  It keeps its corner pixels before it starts and puts them back when it has
  finished, which is exact where a colour is a guess. `recon_corners_keep`
  and `recon_corners_restore`; nothing in the system passes a `behind`
  colour any more.

### BG-141 — Half of every button's edge was missing on any skin that was not grey

- **Found in** v0.4.0. **Found by** the author, pointing at the Calculator:
  "there's this weird white space between the buttons. It doesn't even show
  that they're rounded. So it just looks like they're incomplete... they have
  these random edges that look like they've been cut, and then they're white.
  They're not even colour tone like they're supposed to."
- **What it was** two faults with one cause, and the cause was a comment
  sitting in `recon_draw_bevel` that read *"Fixed highlight and shadow for now;
  a skin would supply these."* Nothing ever did. Every button in the system was
  edged in `EEEEEE` and `555555` whatever it was painted in -- so on a skin
  whose buttons are lavender and whose panels are lavender, the edge was the
  one thing on screen belonging to neither. That is the "not even colour tone"
  half, and it is exactly what the comment predicted would go wrong.
- **The other half is worse.** A 95 bevel is a *lighting effect*: light on the
  top and left, dark on the bottom and right. It reads as an edge only while
  the button is lighter than its own shadow and darker than its own highlight
  **relative to whatever is behind it**. Measured on Glass: the Calculator's
  keys are `E8EBF5` and the panel they sit on is `F0F2F8` -- eight levels
  apart -- so the lit half of the bevel landed *lighter than the background*
  and the top and left of every key stopped existing. Two edges out of four,
  which is what "they look incomplete" is a picture of.
- **Fixed in** v0.4.0. A boundary is not a lighting effect, so it is drawn as
  one: a one-pixel outline in the shaded tone on all four sides, following the
  corner, in a colour derived from the button's own. The highlight stays, and
  is free to be subtle now that it is not the only thing saying where the edge
  is. Both tones come from the surface, so a skin made this afternoon gets a
  correct edge without naming one -- the same reasoning that decides hover.
- **Three things went wrong while fixing it**, each found by measuring rather
  than by looking, and each worth keeping:
  - A stroked rectangle plus a corner carve leaves the outline present along
    the top and down the left and **absent across the diagonal** -- rows 397
    to 401 of a key had nothing but fill fading into background. An outline
    has to follow the curve, so the curve has to draw it.
  - Filling the interior to get a clean surface to carve **paints back over
    the corner that was just cleared**, and the inner carve then restores the
    *outline* colour there rather than the background: five rows of solid
    `7B7D82` across the corner.
  - Repainting the interior at all **erases what the caller already drew**.
    The taskbar fills its buttons, draws the window's icon and title into
    them, and asks for the edge afterwards -- so the fix produced two blank
    rounded rectangles where two windows should have been. The outline is
    composited now and fills nothing.

### BG-140 — recon_icon_get wrote through the pointers a caller had said were NULL

- **Found in** v0.4.0, while fixing BG-139 and reading the function around it.
  Not found by it happening.
- **What it was** `recon_icon_get(name, width, height)` filled in both out
  parameters on a cache hit without checking either for NULL.
  `recon_appicon` calls it as `recon_icon_get(shortened, NULL, NULL)` -- it
  wants to know whether an icon exists by that name and nothing else -- and
  that call reaches the write on any cached hit.
- **Why nothing crashed**: the cache held 32 entries and stopped accepting
  new ones (BG-139), and the name recon_appicon asks about is a client's
  application id, which is rarely one of the first 32 icons a running desktop
  loads. The condition that hid one bug hid the other.
- **Fixed in** v0.4.0. Both writes are guarded. Found by reading, which is
  worth noting because most things here were not.

### BG-139 — The icon cache filled up and then answered every further name with nothing

- **Found in** v0.4.0. **Found by** adding sixteen account pictures and seeing
  six of the twenty-four drawn in the picker -- which looks exactly like
  sixteen pictures having failed to be written, and is why it was chased in
  the generator first.
- **What it was** the cache held 32 icons, and once full `recon_icon_get`
  returned NULL for every name not already in it. Not evict, not reload each
  time. Refuse. Nothing said so: `recon_icon_draw` returns false and its
  callers draw their own fallback, which is the same thing they do for an
  icon that genuinely does not exist.
- The desktop, the taskbar and the window frames fill most of 32 between them
  before an application opens, so the fault belonged to whichever screen next
  wanted several icons at once -- and it moved depending on what had been
  opened first, which is the worst property a bug can have.
- **Why it survived**: 32 was chosen when the whole system had fewer icons than
  that, and every screen since had wanted a handful. The account picture
  chooser was the first to want two dozen. At eight pictures it showed all
  eight and the limit was invisible.
- **Fixed in** v0.4.0. Full now means evict the least recently used, and the
  bound is on bytes rather than on count -- 32 by 32 icons were four kilobytes
  each and the generated set is 128 by 128 now, sixteen times that, while an
  icon somebody drops in the folder has no size limit at all. A count alone
  was a bound on the wrong quantity.

### BG-138 — Nothing in the system reacted to being pointed at or pressed

- **Found in** v0.4.0. **Found by** the author, looking at the three buttons in
  the corner of a window: "there's no animation to them. When I click on them,
  it just does the action. The buttons don't move. They don't show a click.
  They don't even highlight. And in most operating systems, when you highlight
  the x, it's supposed to highlight red."
- **What it was** not a bug in any one place. There was no *concept* of a
  control being under the pointer. Every application drew its own buttons --
  a fill, a bevel, a label, a hit region, a tooltip, five or six lines at a
  time, around two hundred times across the repository -- and hover was
  simply not one of the five lines anybody wrote.
- Four surfaces had grown their own private answer to it: the start menu, the
  context menu, the security box and the login screen each kept an integer of
  their own and compared it by hand. Each picked its own hover colour, and
  three of the four picked `BUTTON_ACTIVE` -- the colour a *pressed* button
  is -- which left them saying "pressed" for pointing and with nothing left to
  say for pressing. The taskbar never got a copy written for it at all.
- **The caption buttons had a second fault on top.** They acted on the press
  rather than the release, so even if they had drawn themselves sunk, the
  window was closed or minimized before that frame reached the screen. It also
  meant there was no way to change your mind: pressing Close and sliding off
  it closed the window anyway.
- **Fixed in** v0.4.0, by building the thing that was missing rather than by
  adding hover in two hundred places. `recon_widget` owns what a control looks
  like and how it behaves; the panel keeps a hot id and a held id; every
  application says what its buttons *are* and is told nothing about bevels or
  radii. Close goes warning-coloured under the pointer, buttons sink when held,
  disabled controls stop answering clicks, and the caption buttons act on the
  release over the control they went down on.
- **The related absence**: `metric.button-corner` defaulted to zero, so nine of
  the eleven built-in skins drew square buttons -- not because anything had
  decided they should, but because rounding had been added for the two skins
  that asked for it and never given a default. Same shape of fault as the
  hover: the capability existed and nothing reached it.
- **Why it survived so long**: every individual button looked deliberate. A
  square, inert button is what a plainer system would draw, so nothing about
  any one of them said "unfinished" -- only all of them together did, and only
  to somebody using the desktop rather than reading it.

### BG-137 — Every rounded corner came out square, and every glass surface opaque

- **Found in** v0.4.0. **Found by** the author looking at the screen and saying
  the corners were "cut off" -- a notch of the frame's own colour where the
  curve should have been. Every window had it, under every skin that asks for a
  corner.
- **What it was** ReconOS draws in **straight** alpha: `recon_color_fade`
  scales the alpha channel and leaves the colour alone, and
  `recon_round_top_corners` thins a corner pixel's alpha to zero while its RGB
  still holds whatever the frame painted there. wlroots renders a scene buffer
  with `WLR_RENDER_BLEND_MODE_PREMULTIPLIED`, which is a different agreement:
  it takes the colour as **already scaled by its own alpha** and adds it to
  what is behind.
- So a fully transparent pixel carrying the edge colour was not skipped -- it
  was **added at full strength**. The corner came out as the frame's edge
  colour, which is why it read as a square with a bite taken out of it rather
  than as a curve.
- **The same fault made every glass surface look solid.** At 210 of 255 the
  arithmetic became `behind * 0.18 + colour`, which saturates on any light
  chrome: title bars and the taskbar looked opaque, and the skin that exists to
  be see-through was the one that looked least like itself.
- **Why it survived so long**: it is invisible on anything opaque, which is
  almost everything. Only two features use alpha at all, and both were added
  recently. And the wrongness *looks like a design decision* -- a square corner
  and a solid taskbar are what a plainer skin would draw anyway.
- **Fixed in** v0.4.0. The conversion happens in `recon_panel_commit`, at the
  copy into the buffer handed to the scene, and nowhere else. Everything that
  draws keeps working in straight alpha -- which is what makes `fade` and the
  corner rounding composable, since each can be applied after the other -- and
  the one place that hands pixels to somebody else's compositor converts them.
  One model inside the program, one at the boundary.
- **Found by measurement, after reading failed.** The corner code was read
  three times and is correct; the buffer format is ARGB8888; `dump_panel` is
  read-only. What settled it was printing the corner's alpha at commit --
  `corner-alpha=0`, exactly right -- which moved the question downstream of
  anything in this repository and left only what the compositor does with it.

### BG-136 — Every file dialog put a hole through the window behind it

- **Found in** v0.4.0. **Found by** photographing a file picker newly added to
  the Calculator, and seeing the What's New window through the middle of it.
  The first guess was that the new code had broken the Calculator's drawing.
  Notepad's Open dialog, which has worked for versions, does exactly the same
  thing -- so the bug was older than the feature that revealed it.
- **What it was** `recon_filedlg_draw` dims the window behind it, and the
  `dim` colour is a translucent black: `RGBA(00,00,00,99)` in the default
  skin, about 60% opaque. It laid it down with `recon_fill_rect`, which
  **writes** the colour rather than blending it. So instead of a dimmed form
  the content area got a 60%-opaque black *pixel* -- a hole. The compositor
  then blended the window over the desktop through it, and the wallpaper and
  whatever windows were behind showed through the middle of an opaque
  application.
- **Every caller had it**: Notepad, Mail, and the Calculator's new picker. It
  had never been reported because a dimmed form and a hole to the desktop look
  similar enough in a screenshot of a plain wallpaper, and the harness only
  ever photographed one window at a time.
- **Why `recon_fill_rect` is right to write.** A Glass window frame is drawn
  with an alpha below 255 *on purpose*, so that the compositor blends the
  window over the wallpaper -- that is the whole skin. Making `fill_rect`
  blend would have made the frame opaque and the skin pointless. The two are
  different operations: making a window see-through, and veiling something
  inside it.
- **Fixed in** v0.4.0. A new `recon_blend_rect` does source-over into the
  panel, and the dialog uses it. Full compositing rather than a straight mix,
  because the destination is not always opaque -- a dialog over a Glass window
  has to end up with the two alphas combined rather than the overlay's. Fully
  opaque colours are handed to `recon_fill_rect`, which is both faster and what
  a reader expects.
- **Confirmed by picture, twice**: the Calculator's picker and Notepad's Open
  dialog, before and after. Reasoning about the drawing code produced a wrong
  first answer; the photograph produced the right one immediately.

### BG-135 — A setting with a leading space lost it at the next start

- **Found in** v0.4.0. **Found by** writing a round-trip test for the registry
  after `scripts/coverage.sh` put it at 57%: eighteen awkward values, stored
  and read back, once from memory and once after a restart.
- **What it was** The file is `key = value` lines and the reader trims what
  comes after the `=`. That trim takes the format's own space -- and any the
  value began with. `escape` handled a backslash, a newline and a carriage
  return, and nothing else. So `"  indented"` was stored and came back as
  `"indented"`, every time.
- **The worst shape a bug in this file could have.** The setting works all
  session and is different after a restart, so what somebody blames is the
  setting rather than the store underneath it. Four of the eighteen values
  changed: a lone space, a leading space, both-ends, and a value of separators.
- **Trailing spaces survived**, which is why nothing had ever noticed: the
  half of the problem somebody would think to try is the half that worked.
- **Fixed in** v0.4.0. A space or tab at either end is written as a backslash
  and the character. That needs nothing added to `unescape` -- its last case
  already copies whatever follows a backslash -- so a file written by this
  version reads correctly on an older one and the other way round. Only the
  first and last characters are escaped, so an ordinary value still reads as
  itself when somebody opens the file, which its own header invites.
