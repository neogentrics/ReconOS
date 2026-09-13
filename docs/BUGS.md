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

### The one exception, 6 September 2026

That rule was broken once, deliberately, and this is the record of it.

ReconOS is built by two sessions sharing one repository — a desktop half on
`main` and a kernel half on a branch. The arrangement was that the kernel
session reports a fault and the desktop session writes the entry, so that one
register exists. That stopped happening, and both sides began writing into their
own copy of this file, each taking the next number from the copy in front of it.

They agree up to and including BG-081. **BG-082 through BG-093 then named twelve
faults on `main` and twelve entirely different faults on the kernel branch**, and
`main` ran on to BG-113. Two GitHub issues were titled `BG-085`, for unrelated
bugs.

The kernel branch's twelve were renumbered to **KF-114 through KF-125**, in the
same order, and the next fault found took KF-126. `main` keeps its numbers,
being the longer sequence and the one the arrangement says is authoritative.

| Was | Is |
|---|---|
| BG-082 | KF-114 |
| BG-083 | KF-115 |
| BG-084 | KF-116 |
| BG-085 | KF-117 |
| BG-086 | KF-118 |
| BG-087 | KF-119 |
| BG-088 | KF-120 |
| BG-089 | KF-121 |
| BG-090 | KF-122 |
| BG-091 | KF-123 |
| BG-092 | KF-124 |
| BG-093 | KF-125 |

Their GitHub issues (#270–#283) were retitled and carry a line saying what they
were. **Commit messages were not rewritten and cannot be** — commits on the
kernel branch dated before 6 September 2026 that cite `BG-082` through `BG-093`
mean the numbers in the right-hand column above.

The rule that made this necessary is not "do not renumber". It is: *take the
next number from the register, not from your copy of it.*

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
titled with its `BG-` or `KF-` number and labelled `bug` plus its
area. The issue is
where discussion happens; this file is the durable record. If they disagree,
this file is wrong and should be corrected — the issues carry the timestamps.

Features, patches and releases are tracked the same way: see
[Labels](#labels) at the foot of this file.

---

## Two prefixes, and why

`BG-` is the desktop's. **`KF-` is the kernel's** -- a kernel *fault*, and not
`KB-`, because this kernel prints KB for kilobytes in the very summaries these
numbers appear in.

They were one sequence until 12 September 2026, on the rule that a track record
of the system should be a track record of all of it. The rule was right and
nothing enforced it: one sequence needs one allocator, and there were two
branches counting up from BG-081 with no way to see each other's claims. **51
numbers ended up naming two unrelated bugs** -- 145 was a diluted outline on the
desktop and a read-only page fault in the kernel.

It had happened once before and been patched by renumbering the kernel's entries
from 082 upward to 114; `scripts/make-issues.py` still carried the comment. That
deferred it rather than fixing it, which is what a renumber does.

**The digits did not change.** BG-145 became KF-145. Anything already written --
a commit message, a comment, a row on the audit -- needs its prefix substituted
and nothing else. Kernel commits before 12 September 2026 citing `BG-114` and
upward mean `KF-`.

Kernel faults from here take the next free `KF-` number; desktop faults the next
free `BG-`. Neither track can take the other's, so neither has to look at the
other's file first.

---

## Open

8, and each entry says why. They are listed because a register that only
shows what is currently broken says nothing about the work -- and one that
claims nothing is broken while entries say otherwise is worse than either.
Checked against the entries by `python scripts/make-issues.py --check`.

- **BG-105** — A register that says nothing until the port is already running
- **BG-104** — An unimplemented region reporting exactly four gigabytes
- **BG-103** — A 64-bit base address restored only in its low half
- **KF-127** — Whether a drive is flash is a question USB cannot answer, and the storage layer assumes it can
- **KF-150** — About one boot in sixty, a user program does not finish, and nothing says why
- **KF-154** — The page allocator scans, and a terabyte is a billion pages
- **KF-192** — The kernel boots from a disk over BIOS and then cannot see it
- **KF-187** — Five self-tests need a volume, every matrix disk is blank, and the boot reports green either way

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
### KF-119 — A transaction could overwrite storage the live filesystem still used

[#276](https://github.com/neogentrics/ReconOS/issues/276)

- **Found in** kernel 0.0.11. **Found by** reading the allocator back while
  writing the discard path, not by any test — there was no test that could have
  produced it, which is recorded below.
- **Was** ReconFS's whole safety argument is that every write in a transaction
  goes to a block the *live* superblock's owner table calls unclaimed, so no
  write can damage the filesystem that currently exists.

  `reconfs_txn_free` broke it. Releasing a block marked it unclaimed in the
  transaction's in-memory owner table immediately — which is right, because the
  *next* transaction must be able to use it. But the live superblock still
  reaches that block, and keeps reaching it until the new superblock is durable.

  So a transaction that freed a block and then allocated it back would write into
  storage the current filesystem still points at. A crash before the commit would
  leave the old superblock — still the truth at that moment — naming a block that
  had already been overwritten. Silent corruption, in the exact place the design
  exists to prevent it.

  It was reachable, not theoretical: the allocator scans forward from a hint and
  wraps at the end of the volume, and after wrapping it walks straight back over
  everything the transaction had just released. Every commit frees the old copy
  of each owner-table leaf it touches, so there is always something on that list.

  The shape worth keeping: **the invariant was stated correctly in a comment
  directly above the function that violated it.** The comment said "a block the
  live table calls free", and the code asked the *transaction's* table, which is
  not the same table. A correct sentence and an incorrect line, adjacent, and the
  sentence made the line look right.
- **Fixed in** kernel 0.0.11. A transaction keeps the list of blocks it released
  and the allocator refuses them; a freed block becomes available to the next
  transaction, never to this one. Overflowing that list fails the transaction
  rather than dropping the exclusion — an allocator that cannot prove a block is
  safe to hand out must not hand it out.

  Testing it took three attempts, and the two failures are worth more than the
  fix:

  1. **The obvious test could not reach the bug.** Allocate, free, allocate
     again, and check the two differ — which passes whether or not the exclusion
     exists, because the allocator scans *forward* from a hint and has already
     moved past the released block. Run with the bug deliberately reintroduced,
     it passed three times out of three. The allocator only revisits a released
     block after wrapping at the end of the volume, so the test now fills the
     volume first, which makes the wrap unavoidable.
  2. **The negative control silently did not apply.** Removing the call to the
     exclusion left the function it called unused, `-Werror=unused-function`
     failed the build, the build error was being discarded, and the *previous*
     binary ran and passed. The control was rewritten to keep the function and
     neuter its body instead, and to check that the build actually succeeded.

  With a control that genuinely applies, the allocator hands back block 4095 —
  the exact block released moments earlier — and the test fails. That is the
  third instance tonight of a check that reported success while doing nothing,
  after KF-114 and KF-115.

  Testing it directly also turned up a smaller thing worth fixing: running out
  of space marked the whole transaction failed, which made "the volume is full"
  indistinguishable from "the allocator broke". Exhaustion now returns zero and
  leaves the transaction usable, which is what let the test fill a volume on
  purpose.

### KF-120 — Only one of the two superblocks was ever written

[#277](https://github.com/neogentrics/ReconOS/issues/277)

- **Found in** kernel 0.0.11. **Found by** a second implementation of the format
  — `scripts/reconfs-check.py`, written in Python from the header rather than
  from the kernel's reader — reporting an epoch one behind what the kernel said
  it had committed.
- **Was** ReconFS keeps two superblocks and writes whichever is not live, so a
  crash during that write leaves the other intact and current. That is the only
  reason there are two.

  The commit chose its target with `RECONFS_SUPER_B`, a constant meaning *block
  1* from when the two copies were adjacent blocks. They had since moved to
  fixed *byte* offsets — zero and 65536 — so the second copy's block number
  depends on the block size, and at 4KiB it is block 16.

  The constant kept compiling and kept meaning block 1. Every commit wrote its
  superblock into the reserved run between the two copies, where nothing ever
  reads it, and **the real second superblock stayed at the epoch the format
  left it — an empty volume.** A crash during a superblock write would have
  rolled the filesystem back to freshly formatted.

  **Nothing inside the kernel could have found this.** Mounting reads the second
  copy from the right place and finds a valid, older superblock, which is
  exactly what a healthy volume looks like; the checker walks the live tree,
  which was correct; every self-test passed. It took reading the image with
  something that did not share the constant.

  The shape worth keeping: **a constant survived a change in what it named.**
  `RECONFS_SUPER_A` was still right, because block zero is byte zero at every
  block size, and its neighbour being right made the pair look right.
- **Fixed in** kernel 0.0.11. The commit asks `reconfs_super_b(block_size)`, and
  the constant is deleted rather than corrected — a constant whose meaning has
  moved is worse than no constant.

  Tested by `scripts/rename-crash-test.sh`, which cuts the power inside a rename
  and judges the surviving image with the Python reader. It proves that reader
  on every run first, by breaking an image two ways and requiring both to be
  caught — and the first version of *that* control was itself useless, reaching
  into superblock A's fields while B was live, damaging a block nothing pointed
  at, and getting a correct report of a healthy volume.

### KF-121 — Renaming over a file leaked every block it occupied

[#278](https://github.com/neogentrics/ReconOS/issues/278)

- **Found in** kernel 0.0.11. **Found by** the Python reader, the moment the
  crash workload started writing real contents — thirty-six blocks reported as
  allocated to an object nothing could reach, after twelve replacements.
- **Was** `reconfs_rename` releases the object the new name used to refer to, so
  its blocks go back to the unclaimed state. It released the inode and nothing
  else. Every block of the replaced file's *contents* stayed marked as owned,
  reachable from nothing, forever.

  On a volume being written the way the desktop's registry writer writes — a
  temporary file renamed over the real one, every time settings change — the
  free space falls by the size of the file on every save, and never comes back.

  **It had a test, and the test could not see it.** The self-test renames one
  file over another and then runs the whole checker, which is exactly the right
  shape — but both files were empty, so there were no content blocks to leak,
  and the checker correctly reported a volume with nothing wrong.

  The shape worth keeping: **a test whose subject has no instance of the thing
  that can go wrong**. Renaming empty files exercises every line of the rename
  and none of the consequences.
- **Fixed in** kernel 0.0.11. The replaced object's contents are released with
  its inode, after the new directory is built and before the commit — so the
  live superblock still reaches the old contents until the change is real.

  What made it findable was giving the crash workload a payload: 9,001 bytes,
  self-describing, with a round number at three places and a checksum over the
  whole thing. An empty file is always complete, so a crash test on empty files
  can only assert that a *name* resolved.

### KF-122 — Every directory rewrite leaked a block, and the checker was built not to notice

[#279](https://github.com/neogentrics/ReconOS/issues/279)

- **Found in** kernel 0.0.11. **Found by** a test written for something else:
  the new remove operation records how many blocks the volume is using, deletes
  a file, and requires the count to return to where it started. It came back
  three higher — one for each commit that had rewritten the directory.
- **Was** two faults, and the second is the one that matters.

  1. **`write_dir` never released the directory it replaced.** Every operation
     that changes a directory writes a new inode for it, because that is what
     copy-on-write means. The old copy stayed marked as owned, forever. One
     leaked block per create, per rename, per write, per remove — on a
     filesystem whose entire pattern of use is rewriting directories.

  2. **The checker was built to skip exactly those blocks.** Its comparison
     exempted every block owned by `RECONFS_OWNER_ARCHIVE` from having to be
     reachable, reasoning that the superblocks and the owner table are owned by
     nothing above them and so cannot be reached by a walk from the root.

     True of those blocks. Not true of everything owned by the archive — which
     includes every stale copy of the *root directory*, because the root has no
     parent and archive-ownership is what "no parent" looks like in the table.
     So the one check that would have named the leak was the one place it was
     invisible.

  The shape worth keeping: **an exemption written for a category, applied to a
  membership test.** "The superblocks and the owner table" is a list of specific
  blocks; "owned by the archive" is a property those blocks happen to share with
  something else entirely.
- **Fixed in** kernel 0.0.11. `write_dir` releases the inode it replaces, and
  the blocks its entries were in. The checker now *claims* the archive's blocks
  explicitly — both superblocks, the reserved run between them, and the owner
  table's own index blocks and leaves, walked from the table root — and the
  exemption is gone entirely. **Every allocated block on the volume must now be
  reachable from something.**

  Removing the exemption made an existing test fail immediately and correctly:
  it had been allocating a block owned by the archive and reachable from
  nothing, which is a leak, and which had passed for the same reason.

  The same hole existed in `scripts/reconfs-check.py`, written independently and
  independently wrong in the same place — which is a fair measure of how much
  independence a second implementation by the same author buys, and worth
  recording as such.

### KF-123 — A machine with more processors than the kernel holds reported the wrong number, under a comment saying it never would

[#280](https://github.com/neogentrics/ReconOS/issues/280)

- **Found in** kernel 0.0.11. **Found by** running the new x86_64 processor
  discovery at 1, 2, 4, 8 and 16 and noticing that sixteen reported eight.
- **Was** `smp_init` calls `arch_smp_discover(cpu_ids, MAX_CPUS)` and then:

  ```c
  if (found > MAX_CPUS) {
          /* Reported, never silently truncated. ... */
          dropped = found - MAX_CPUS;
          found = MAX_CPUS;
  }
  ```

  `arch_smp_discover` returned the number it had *stored*, which is bounded by
  the `max` it was given. So `found > MAX_CPUS` was unreachable, `dropped` was
  never set, and the warning at the bottom of the summary — which exists, and is
  correct — never printed.

  A machine with sixteen processors said "found: 8" and nothing else.

  The comment above it says *"Reported, never silently truncated"*, and cites
  the memory map's region cap as the lesson that taught it. The lesson was
  learned, written down, and then implemented as code that could not do it —
  which is the same shape as KF-120 (a correct sentence above an incorrect line)
  and KF-122 (an exemption written for a category, applied as a membership
  test). Three of these now.
- **Fixed in** kernel 0.0.11. `arch_smp_discover` returns how many processors
  *exist*, which may exceed `max`; only `max` are written to the array. Both
  architectures count past what they can store — aarch64 probes to twice the
  array rather than stopping at it.

  Sixteen processors now says `found: 8` and `WARNING: 8 more than this kernel
  can hold`; thirty-two says twenty-four more.

  Also corrected here: a failure to start a secondary printed *"the firmware
  refused"*, which on x86_64 names a participant that is not in the path at all.
  A message that blames the wrong thing sends whoever reads it somewhere else
  entirely.

### KF-140 — The block self-test would write to a stranger's partition, and only ever on a stranger's machine

[#379](https://github.com/neogentrics/ReconOS/issues/379)

- **Found:** 8 September 2026, booting the real install medium off a physical
  USB stick in preparation for checkpoint 17 — the first boot where the kernel
  could see a partitioned disk it did not create.
- **Cost:** nothing, because the run that found it was deliberately read-only.
  It was one boot away from writing to the test machine's system disk.

`block_self_test` picks a device to write a pattern to, read back, and restore.
Its preference order was: an unpartitioned disk, **failing that a single
partition**, failing that a read-only test on anything.

The middle option was written as the careful choice — *"bounded, and exercises
the slice arithmetic"* — and is the opposite of one, because of **when it
fires**. It is reached only when no disk is blank, which is to say only on a
machine whose disks are all already partitioned. That is not a description of
the test rig. It is a description of somebody's computer.

So the branch that wrote to a partition it did not own was reachable *only* on a
machine where every partition belongs to somebody else. And it never ran in the
rig at all: every disk the rig builds is blank, so the first preference always
won, and this branch sat unexercised from checkpoint 12 to checkpoint 17.

**What made it visible was the thing that made it dangerous.** Until checkpoint
11b the kernel could not see a USB disk, so a machine booted from the install
medium reported no storage and the picker had nothing to choose. With the USB
driver working, the kernel finally saw the stick it had booted from — which is
partitioned — and the self-test chose the BIOS boot partition and wrote to it:

    block: could not write to usb0p1: the hardware reported a failure

That failure is QEMU refusing the write because the physical stick was attached
`readonly=on`, on purpose, for exactly this class of surprise. On a writable
stick it would have succeeded. On the machine this was being prepared for, the
first partition of a 1 TB disk with Windows on it would have been chosen
instead.

It restores what it borrows and it would very probably have left no trace. That
is not a defence — it is the reason it could have run for years unnoticed.

**The fix is a removal.** A partition belongs to whoever's data is in it, and
there is no version of a self-test worth a write to that. The picker now writes
only to a whole device with no table and no slices, and everything else takes
the read-only path, which is still a real test and already existed. Nothing lost
any coverage: the rig always took the first branch anyway.

- **Fixed in:** `kernel/core/block.c`, `pick_test_device`. The rationale is
  written where the old one stood, because the old one read as careful.

**Worth generalising.** This is the second time this project has found a check
whose *only* reachable case is the case nobody tests — after KF-119's comment
arguing that a case "does not arise here" about a case that then arose. The
shape to look for is not a wrong branch. It is a fallback whose precondition is
"the situation the rig never builds".

### KF-141 — A processor count that saturates, and states the shortfall as a fact

[#380](https://github.com/neogentrics/ReconOS/issues/380)

- **Found:** 8 September 2026, sweeping processor counts on aarch64 while
  finishing checkpoint 9b on the other architecture.
- **Cost:** nothing. No machine in the rig has more than sixteen processors, and
  the number is only printed.

aarch64 has no device-tree walk for `/cpus` yet, so it discovers processors by
*asking*: PSCI `AFFINITY_INFO` for each candidate identifier, where a machine
that has no such processor gives a definite refusal. The probe is bounded at
twice the array, and the comment says why — *"twice the array is enough to
notice; probing without a bound would ask forever."*

Enough to **notice** is not enough to **report**, and the code then reports:

| machine | says | true answer |
| --- | --- | --- |
| 9 processors | at least 1 more than this kernel can hold | 1 |
| 16 processors | at least 8 more | 8 |
| **32 processors** | **8 more** | **24** |

The count stops at sixteen, so a 32-processor machine is indistinguishable from
a 16-processor one and the shortfall is understated by two thirds — stated in a
sentence with a number in it, which is the form people believe.

**This is the exact failure `smp_init` has a comment about.** Fifteen lines
above, on the cap this number comes from: *"Reported, never silently truncated.
The memory map's region cap taught this: a limit that quietly drops what it
cannot hold produces a wrong number rather than an error."* The cap does report.
The **probe** truncates, one layer down, and the reporting layer cannot tell.

- **Fixed in:** `kernel/core/smp.c` — the warning now says *at least* N. That is
  true whether the count was exact or bounded, and a message that is never wrong
  is worth more than one that is precise and sometimes false.

**Still open:** the count itself. The real answer is to read `/cpus` from the
device tree, which the parser can now do — it gained the ability at KF-125, when
the walker learned to report a node that has children. Until then the number is
a floor rather than a total, and the wording says so.

**Worth generalising.** The bug is not in either layer. It is that a lower layer
was allowed to return a *saturated* value through an interface whose caller
treats it as a *total*, with no way to ask which it got. The same shape as a
read that returns fewer bytes than asked for and is treated as a failure — see
the transfer-event residue in `xhci.c`, where the distinction was built in
deliberately for exactly this reason.

### KF-142 — The scheduler's own test hung one boot in three, on a counter that `volatile` did not protect

[#381](https://github.com/neogentrics/ReconOS/issues/381)

- **Found:** 8 September 2026, sweeping processor counts on x86_64 after
  checkpoint 9b started the secondaries.
- **Cost:** nothing shipped, and it had been latent on aarch64 since 9b landed
  there — see KF-143 for why nothing saw it.

The scheduler self-test runs three threads against a deadline, then waits:

    static volatile unsigned finished;

    while (time_monotonic_ns() < test_deadline_ns)
            counter[which]++;
    finished++;                       /* each of the three threads */

    while (finished < 3)              /* the waiter */
            sched_yield();

`finished++` is a read, an add and a write. **`volatile` stops the compiler
keeping the value in a register and does nothing whatever about another
processor landing between those three steps.** Two threads increment, one
increment is lost, the count reaches two, and the waiter yields for ever.

**The deadline makes the collision the expected case rather than a rare one.**
All three threads stop at the *same* `test_deadline_ns`, so on a machine with
three free processors they arrive at that increment together by construction.
Measured: three hangs in six boots at `-smp 4`, and six clean boots in six after
the fix.

It was written when there was one processor, where it was correct. The general
shape is the one this project keeps meeting and has now met on both
architectures: **a property that held because there was only ever one processor
to break it** — the same family as the shared TSS descriptor found in the same
checkpoint, and as `VBAR_EL1` at 9b on aarch64.

- **Fixed in:** `kernel/core/sched.c`. The increment is
  `__atomic_add_fetch(..., __ATOMIC_RELEASE)` and the reads are
  `__atomic_load_n(..., __ATOMIC_ACQUIRE)`.

**And the wait is now bounded**, which is a separate fix for a separate fault.
An unbounded wait for a count that never arrives is a machine that stops with
nothing printed — the least informative failure a kernel can have. This project
already converted one of those into a failed test, at checkpoint 7, when a
recovery loop ran forever at full speed printing nothing. The same conversion
applies here: after five seconds it reports how many of the three actually
reported in, and fails.

### KF-143 — The rig reported a boot that stopped half way as a boot that passed

[#382](https://github.com/neogentrics/ReconOS/issues/382)

- **Found:** 8 September 2026, immediately after KF-142, by asking why a hang
  that happened on one boot in three had never turned the matrix red.
- **Cost:** nothing yet. It is the reason KF-142 could have shipped.

`check()` in `scripts/verify-kernel.sh` decides a boot passed by four tests, and
a boot that hangs part-way through satisfies **all four**:

| it asks | a hung boot |
| --- | --- |
| did any self-test run? | yes — five of them |
| did any report FAIL? | no |
| did it panic? | no, it is still sitting there |
| did it name the disk it was given? | yes |

So it printed `5 self-tests, all pass` and added five to the total. There is no
count to compare against, because the number of self-tests legitimately differs
between paths — a diskless run has fewer than one with a disk.

**The storage summary is what makes this quiet.** It prints *before* the
self-tests, so the string every `check_for` waits on is already in the log by
the time anything can hang. The aarch64 processor sweep — the check whose whole
purpose is to exercise several processors — was therefore structurally incapable
of noticing a hang in the scheduler, which is the thing several processors break.

- **Fixed in:** `scripts/verify-kernel.sh`. `check()` now also requires the log
  to contain `Idling.`, which `main.c` prints as its last line. Nothing else in
  the boot says "and it got to the end", and `medium-boot-test.sh` had already
  worked this out for itself — it waits for `Idling` rather than for the banner,
  after an earlier version waited for the banner and asserted on evidence that
  had not been printed yet.

**Worth generalising, and it is the third time.** A check that asks *"did
anything go wrong?"* passes a run in which nothing went wrong because nothing
happened. The two before it: a crash harness that reported success on rounds
where the checker had not run at all, and a recovery test that passed because the
damage tool was given its arguments the wrong way round and damaged nothing. The
answer each time is the same — **assert that the thing you are measuring
actually took place**, not merely that no complaint was printed.

### KF-144 — Every UEFI test booted whatever kernel happened to be in the ESP first

[#383](https://github.com/neogentrics/ReconOS/issues/383)

- **Found:** 9 September 2026, while fixing a page fault that would not go away
  no matter how many times the kernel was rebuilt.
- **Cost:** three debugging runs, and it could have cost far more: the rig would
  have gone on reporting five boot paths green against a kernel that no longer
  existed.

`boot/Makefile` built the ESP image from the loader and the kernel:

    $(ESP_IMG): $(EFI) $(KERNEL_ELF)

and got the kernel with a rule that had **no prerequisites at all**:

    $(KERNEL_ELF):
            $(MAKE) -C ../kernel ARCH=$(ARCH)

To make, a file that exists and has no prerequisites is up to date for ever. So
the sub-make ran exactly once, when the file did not yet exist, and never again.
`make -C boot esp` after a kernel change printed *"Nothing to be done for
'esp'"* and left the image carrying the kernel from whenever it was first built.

**Every UEFI path in the verification rig boots from that image** — reconboot
under OVMF, GRUB under UEFI, install-then-boot, the boot menu, the install
medium. All five test a kernel that arrives inside `esp.img`.

**How it presented.** A page fault in new code was found, fixed, and rebuilt --
and faulted again, identically, three times. What settled it was disassembling
the reported address: `0xffffffff801253bd` fell in the *middle* of an
instruction in the binary on disk. An address that is not an instruction
boundary is not an address in the binary you are looking at, and that is the
tell -- the machine was running a different kernel.

- **Fixed in:** `boot/Makefile`. The kernel rule now depends on a phony target,
  so the sub-make is always *asked*; the kernel's own makefile still decides
  whether anything needs building.

**Worth generalising, and it is the third time in this shape.** KF-133: a
generated header absent from the dependency file, so creating it rebuilt
nothing. KF-138: stage 2 outgrew stage 1's read, and the fix was attached to a
rule `make` alone does not build. Now this. Each time, **a build artefact that
does not depend on what it contains**, and each time the symptom was a change
that appeared to have no effect.

The class is worth naming: *if an image embeds a copy of something, the rule
that builds the image must depend on the thing it copied.* And the diagnostic is
worth remembering — when a fix appears to do nothing, check that the artefact
under test contains the fix, before looking at the fix again.

### KF-139 — Two tests generated a signing key into the source tree and left it there

[#296](https://github.com/neogentrics/ReconOS/issues/296)

- **Found:** 7 September 2026, by the verification matrix failing five paths
  that had nothing wrong with them.
- **Cost:** nothing shipped. Twenty minutes, and it would have cost far more the
  first time somebody else ran the suite.

`signed-kernel-test.sh` and `bios-signed-test.sh` both generate a signing key,
and the public modulus is **compiled into the loader** — so it is written to
`boot/src/signing_key.h`, in the source tree, because there is nowhere else for
it to go. Both scripts cleaned up their temporary directory. Neither put the
tree back.

So running either one leaves the repository in a state where **the loader
verifies signatures**, and every other harness boots an unsigned kernel. The
loader refuses them, correctly, and the matrix reports:

    5 path(s) failed:
        reconboot, UEFI
        reconboot, UEFI
      install then boot
      boot menu
      bios signature

Five failures, none of them a fault in what they were testing.

**It is only order that hid this.** Inside the matrix the signing tests run
last, so nothing came after them to be affected. It surfaced when one was run on
its own beforehand — which is the ordinary way to work on a single harness.

- **Fixed in** kernel 0.0.11. Both scripts save `signing_key.h` if it exists,
  and restore it — or remove it — from a trap, so the tree is as it was however
  the script ends. The loader is rebuilt afterwards by whoever needs it, because
  that header is a tracked dependency, which it was not until KF-133.

- **The shape.** The instrument changing the thing it measures, which is KF-137
  four hours earlier wearing a different coat: there, the test observed a signal
  the loader was not producing; here, the test *produced* a condition the next
  test then observed. Both are the harness being part of the experiment rather
  than outside it.

  Worth naming the direction that did **not** happen and could have: a leftover
  key makes a test that expects *"signature: not checked"* see a real check
  instead. That way round, a test passes for the wrong reason rather than
  failing for one — and nobody investigates a pass.

### KF-138 — Stage 2 outgrew the number of sectors stage 1 reads, and the magic check passed anyway

[#295](https://github.com/neogentrics/ReconOS/issues/295)

- **Found:** 7 September 2026, by the BIOS harness, on the commit that added a
  FAT32 reader.
- **Cost:** nothing shipped. It is here for the shape of it.

`stage1.S` carried the number of sectors to read as a literal: four, which was
2048 bytes, which was true when it was written. Adding the FAT32 reader took
stage 2 to **2797 bytes**, and stage 1 went on reading 2048 of them.

**The check that should have caught this is the one that passed.** Stage 2
begins with a magic number precisely so that stage 1 never jumps into a sector
it has not identified — and the magic is in the *first four bytes*, which had
loaded perfectly. So stage 1 confirmed the image was ours and jumped into a copy
whose second half was whatever the disk had at those blocks beforehand.

Nothing was reported. The machine printed `ReconOS` and stopped.

A magic number answers *"is this the thing I meant to load?"*. It cannot answer
*"did all of it arrive?"*, and the two questions look like one until a file
grows.

- **Fixed in** kernel 0.0.11. `boot/bios/patch-stage1.sh` computes the sector
  count from stage 2's actual size and writes it into stage 1 at build time,
  finding the offset from the **symbol table** rather than by counting bytes in
  a hex dump — an offset worked out by hand goes wrong silently the first time
  the source is edited. It reads the value back afterwards, because a patch that
  quietly did nothing would leave exactly the stale constant this exists to
  prevent, and it refuses more than 127 sectors, which is as much as a single
  BIOS extended read can be relied on to fetch.

  **The failure mode is removed rather than detected.** A build-time check that
  the literal still matched would have been an improvement; computing it means
  there is no literal to be wrong.

- **And the fix was wrong once, in the same shape as the bug.** The patch was
  first attached to the `mbr.img` rule. `make` alone builds `stage1.bin` and
  `stage2.bin` and *not* `mbr.img`, and the harness assembles its own disks out
  of `stage1.bin` — so the harness used an unpatched stage 1 and failed exactly
  as before. It passed locally, because a `make disk` run by hand had patched
  the file in place, and it failed in the verification rig, which starts from a
  clean copy. **A build step that runs only when some other target is asked for
  is a build step that is sometimes absent** — KF-133 wearing different clothes,
  two days later. The patch now belongs to `stage1.bin`'s own rule, so the only
  stage 1 that can exist is a patched one.

  The harness also asks the question directly now, before booting anything: it
  reads the count out of `stage1.bin` at the symbol's offset and compares it
  with stage 2's size on disk. That is the question the magic number cannot
  answer, asked where the answer is cheap.

- **The shape.** A constant that was true when written, consumed later by code
  that had no way to know it had stopped being true — the same family as
  KF-126 (a transaction's capacity written down as one number when it is three)
  and KF-119 (a value correct when computed, used after it had gone stale).
  What is new here is the *check that gave cover*: the magic test made the hop
  look verified, so the one visible symptom had an explanation that was already
  known to be handled.

### KF-137 — The BIOS harness read a mirror of the screen and called it serial output

[#294](https://github.com/neogentrics/ReconOS/issues/294)

- **Found:** 7 September 2026, while bringing up checkpoint 16, by turning the
  VGA mirror off to check something unrelated.
- **Cost:** an evening chasing a register-clobbering bug that did not exist.

`qemu -nographic` mirrors the VGA text console to stdout. The BIOS loader
printed with `INT 10h`, which writes to VGA, so its output appeared on the
harness's stdin and everything looked connected. **It had never written a byte
to a serial port.**

Two things followed from that, and the second is the expensive one:

1. **On a headless machine there would have been no output at all** — which is
   every machine this loader is actually for. A bootloader whose only console is
   a screen is a bootloader you cannot debug on the hardware it fails on.

2. **The mirror drops characters.** `E:RD` arrived as `E:R`, and
   `drive 0x80` as `drive 0x8`. A missing final character reads as truncation,
   and truncation of a *hex byte* reads as the low nibble being lost — so the
   search went straight to `print_hex_byte`, and found a plausible culprit:
   the byte was held in `%cl` across an `INT 10h` call, and AH=0Eh promises to
   preserve `AX` and nothing else. That reasoning is correct and the bug was not
   there. It was rewritten to keep the byte in memory, which changed nothing,
   because nothing had been wrong with it.

**What settled it was one command, not more reading:** running with
`-display none -serial stdio` instead of `-nographic`. The same loader produced
**nothing whatsoever**, which is the true state of it, and every earlier
observation was explained at once.

- **Fixed in** kernel 0.0.11. Both stages write to COM1 themselves — stage 1
  initialises the port — *and* keep the `INT 10h` call, because they are for two
  different readers: the screen is where a person standing in front of a machine
  that will not start is looking, and the serial port is where the harness and a
  headless machine are. `scripts/bios-boot-test.sh` uses
  `-display none -serial stdio` so that nothing but what the loader wrote can
  reach it.

- **The shape.** Every previous entry of this kind was a claim disagreeing with
  an implementation. This one is a **harness measuring the wrong signal** — the
  output was real, it simply was not coming from where the test believed. It
  belongs with KF-133 and KF-134: three in two days where the thing under test
  was not the thing being observed. The instrument is part of the experiment,
  and *this* one also manufactured a bug rather than hiding one, which is the
  more expensive direction to be wrong in.

### KF-134 — Eight test harnesses looked for a kernel instead of building one

[#291](https://github.com/neogentrics/ReconOS/issues/291)

- **Found:** 7 September 2026, hours after KF-133, by the recovery test failing
  three of five assertions against a kernel that did not contain the code the
  assertions were about.
- **Cost:** nothing shipped. Two hours of reading `recovery.c` for a fault that
  was not in it.

Every harness that boots a kernel began the same way:

    [ -f "$KERNEL" ] || { echo "build the kernel first: ..." >&2; exit 1; }

*Exists* is not *current*. `scripts/recovery-test.sh` ran in the verification
rig, whose tree is rsynced from the repository with `--exclude=build` — so the
rig's `build/` was whatever the last run had left there, `recovery.c` had never
been compiled into it, and the harness happily booted a kernel from before the
feature existed and reported that the feature did not work.

**This is KF-133 again, one night later, through a different door.** There a
generated header was not a dependency; here the binary was not a target. Both
end at the same place: *a test that ran a binary other than the one it was
written to prove, and reported the result as though it had.* KF-133's stale
loader hid the absence of a safety check; this one invented a bug that was not
there. The register now has both directions of the same mistake.

- **Fixed in** kernel 0.0.11. All eight harnesses build before they check:

      make -C kernel ARCH="$ARCH" >/dev/null 2>&1 || true
      [ -f "$KERNEL" ] || { echo "the kernel did not build" >&2; exit 1; }

  The `|| true` is deliberate and the second line is the real test: a build
  failure and a missing binary should be reported as *the kernel did not build*,
  not as make's last line of output, which on a link error is about a symbol
  nobody running a filesystem test is looking for.

- **The shape.** A harness is a claim about what was tested. `[ -f ]` states
  that a file with that name exists, which is not the claim anybody wanted.
  This is the fifth entry in this register where a comment or a check said one
  thing and the machine did another (KF-120, KF-122, KF-123, KF-132, this) —
  and the second where the disagreeing party was a *test*, which is the worst
  place for it, because a test is the thing everything else is believed on.


### KF-133 — The bootloader build ignored its headers, so a security check silently was not there

[#292](https://github.com/neogentrics/ReconOS/issues/292)

- **Found:** 7 September 2026, by the verification rig, on a change whose own
  test had just passed.
- **Cost:** nothing shipped. It is here because of what it would have cost.
- **Status:** fixed twice, on purpose. The bootloader build emits dependency
  files so a changed header rebuilds what includes it, and the signed-boot
  harness now asks first whether the loader can refuse at all, failing with
  that as the reason when the answer is "not checked".

`boot/Makefile` compiled each object against its `.c` file and nothing else --
no `-MMD`, no dependency files. Changing a header rebuilt nothing.

That was survivable while every header was checked in and changed by hand. It
stopped being survivable the moment one became **generated**:
`scripts/make-signing-key.sh` writes `src/signing_key.h`, and `make` then
compared `main.o` against `main.c`, found it unchanged, and reused a loader
compiled *before the key existed*.

**The direction of the failure is the whole point.** The stale loader announced
itself plainly and did the wrong thing anyway:

    signature    : not checked (this loader was built without a key)
    ReconOS kernel 0.0.11

An unsigned kernel ran, on a machine that had just been given a signing key, and
the test that generated the key was satisfied — **because the loader it tested
was not the loader it had asked for.** A security feature that is silently
absent, with a green test beside it.

**Why the test could not catch it and the rig could.** In the tree the test was
written in, the generated header happened to exist *before* the build, so the
loader was correct by accident. The rig starts from a clean copy of the
repository, where the header does not exist because it is deliberately not
committed — so the rig produced the stale binary and the isolated run never
could. This is the argument for running everything on every change rather than
the test just written.

Fixed twice, on purpose:

  - `-MMD -MP` in `boot/Makefile`, so headers are dependencies as they are
    everywhere else in the project;
  - and the test deletes the loader before rebuilding, because its entire
    subject is a loader that trusts a specific key. A test should not depend on
    the dependency tracking being right in order to prove the thing it tests.

**The shape.** This is the hazard already written into this project's own notes
about injecting faults — *the edit orphans a function, the build fails, and the
previous binary runs and passes*. Same mechanism, different trigger, and a worse
consequence: there the stale binary hid a fix, here it hid the absence of a
safety check.

**Seen a third time, 7 September 2026, and header dependencies do not fix this
one.** The BIOS loader reaches the same key through
`#if __has_include("signing_key.h")`. On a build made *before* the key existed
the answer was no, so **the header never entered the `.d` file** — and creating
it afterwards triggered no rebuild. `-MMD` records what a compile *did* include;
a generated header that does not exist yet cannot be recorded as a dependency of
the compile that did not find it.

The verification rig keeps its build directory between runs, so it ran a stage 2
compiled with no key: it announced *"not checked"*, ran everything it was given,
and all four refusal cases failed for a reason that was not the reason. Local
runs passed, because there the object had been built after the key.

Two fixes, and the second is the one that generalises:

  - `bios-signed-test.sh` deletes stage 2's objects before building, as
    `signed-kernel-test.sh` already did for the UEFI loader;
  - and it now **asks first whether the loader can refuse at all**, failing
    immediately with that as the reason if the answer is "not checked". A
    harness that cannot tell *"the check said no"* from *"there was no check"*
    is reporting on the wrong thing, and reports four confusing failures instead
    of one clear one.

### KF-132 — A handle used two lines after it was closed, under a comment saying it was open

[#293](https://github.com/neogentrics/ReconOS/issues/293)

- **Found:** 7 September 2026, the first time the loader tried to verify a
  kernel signature.
- **Cost:** most of an hour, all of it spent suspecting the wrong code.
- **Status:** fixed. The volume is closed after the verification rather than
  before it, which is the only place it can be closed.

The verification step needs the volume the kernel came from, because the
signature is a second file on it. The volume was closed two lines above the
call:

    file->Close(file);
    root->Close(root);          <-- here

    *size_out = size;

    /* Verified here, where the volume it came from is still open ... */
    if (!verify_kernel(root, buf, size)) {

Calling through the closed handle landed in freed pool memory, which EDK II
fills with `0xAF`, so the machine stopped with:

    !!!! X64 Exception Type - 0D(#GP - General Protection) !!!!
    RIP  - AFAFAFAFAFAFAFAF

**Why that misled for so long.** `RIP` full of one repeated byte looks exactly
like a smashed return address, and the code added in the same commit was
hand-written 2048-bit arithmetic with several 512-byte structures on the stack.
Every hypothesis followed from there. It was the wrong shape entirely: `0xAF` is
not stack corruption, it is *freed pool*, and it was naming the actual fault the
whole time.

**What settled it, in one run.** Compiling the verifier out -- the loader has a
path for a build with no key -- and booting again. It crashed identically, which
proved the crypto innocent immediately and reduced the remaining surface to a
few lines. Reading them was then enough.

**The part worth keeping.** The comment claiming the volume was still open was
written in the same minute as the bug, by the same person, and it is what made
the handle look innocent. This is the fourth time this month that a *comment
describing an intention* has sent the investigation away from code sitting two
lines from it — see KF-118, KF-125, KF-127.

  A comment states what somebody meant. Only the machine states what happens.

The volume is closed after the verification now, which is the only place it can
be.

### KF-131 — The bootloader never passed a command line, for four checkpoints

[#290](https://github.com/neogentrics/ReconOS/issues/290)

- **Found:** 6 September 2026, writing the first test that installs from a real
  medium and then boots the installed disk.
- **Cost:** nothing yet, and it would have cost the entire installer.
- **Status:** fixed. The loader reads an optional command-line file from the
  medium, so it can be changed on a stick without rebuilding; absent is
  normal, and a trailing CR is stripped because a file edited on Windows ends
  CR LF.

The handoff structure has carried a `cmdline` field since checkpoint 4, and the
kernel has always honoured it — `reconboot.c` copies it and hands it to
`boot_info()`. **The loader never filled it in.** Every firmware boot handed the
kernel an empty command line.

**Why four checkpoints of testing did not notice.** Every option the kernel
takes was being passed with QEMU's `-append`, and `-append` only exists on the
`-kernel` path — which loads the kernel directly and *skips this bootloader
entirely*. So the rig was exercising two things and neither was the real one:
boots with a command line that had no loader in them, and boots with a loader
that had no command line. The combination a real machine performs was never run.

The cost only appears when something needs it. An installed system, or an
install medium, had no way to tell the kernel anything at all: the installer
could not be told which disk to write to, and a recovery mode could not be asked
for. **The protocol supported it and the implementation quietly did not**, which
is the same shape as several faults this month — a comment and its code
disagreeing with nothing arranged to notice.

The loader now reads an optional `\reconos\cmdline` from the medium. A file
rather than something built in, because the point is that it can be changed on a
stick without rebuilding. Absent is normal. CR and LF are stripped, because a
file edited on Windows ends CR LF and a command line with a carriage return in
it matches nothing — which presents as an option being ignored for no visible
reason.

**And the second mistake, on the way to fixing the first.** The copy into the
handoff was placed *above* the call that reads the file, so it copied a buffer
that was still empty. Nothing failed: the kernel received no command line and
behaved exactly as it does when there is none, which is indistinguishable from
working.

What settled it was the *order of the loader's own output* — it prints
`framebuffer` and `acpi rsdp` before `kernel: ... bytes read`, so the handoff was
being built before the kernel was read, and the copy sat in the handoff. Source
reads top to bottom; a machine does not. Not given its own number because it
never existed outside an afternoon, but recorded because the shape is the
recurring one: **a value that was correct when computed, consumed at a moment
when it was not yet.**

### KF-130 — Every rewrite renamed the file, including the one firmware looks for

[#288](https://github.com/neogentrics/ReconOS/issues/288)

- **Found:** 6 September 2026, writing the same files into a FAT32 volume twice
  and reading the result back with `mtools`.
- **Cost:** none yet. There is no installer to have run twice.
- **Status:** fixed. The collision test ignores the entry being replaced,
  matched on the name being written rather than on a cluster number -- an
  empty file's first cluster is zero, and every empty file would otherwise
  look like the same one.

Writing a file that already exists looks up the name, finds the entry, and then
picks an 8.3 alias. The entry it is about to replace is still in the directory
at that moment — so the file **collides with itself**, and gets a fresh alias
every time it is written:

    install 1:  BOOTX64  EFI
    install 2:  BOOTX6~1 EFI   BOOTX64.EFI
    install 1:  BIG      BIN
    install 2:  BIG~1    BIN   big.bin

**Why this one matters more than it looks.** `\EFI\BOOT\BOOTX64.EFI` is the
*removable-media path*: the one filename UEFI firmware will run with no boot
entry registered, and therefore the whole reason a USB stick boots a machine
that has never seen it. Every reinstall or kernel update pushes that name one
step further from what firmware is looking for. Firmware that reads long names
still finds it; firmware that does not, does not.

**It cannot be fixed by deleting the old entry first.** The old entry survives
deliberately until the new data is on the medium — that ordering is what makes a
crash mid-write leave one whole file rather than neither. So the collision test
learns to ignore the entry being replaced instead, matched on the *name being
written* rather than on a cluster number: an empty file's first cluster is zero,
and every empty file would look like the same one.

**And how nearly it was fixed against the wrong model.** The first attempt to
confirm it ran three installs and reported `BOOTX64 EFI`, unchanged — which
said the drift was not real. It was: that run had looked at the wrong output.
Two installs with the trace visible showed the drift plainly. **A fix built on
the first result would have been a fix for nothing, with a test that passed
because the bug was not being reached.** That is the same shape as the GICv3
work earlier the same day, where two correct fixes changed nothing because the
thing making them unreachable was three files away.

### KF-129 — An unsupported conversion made every later value in the line wrong

[#287](https://github.com/neogentrics/ReconOS/issues/287)

- **Found:** 6 September 2026, listing a real EFI System Partition with
  `%-30s`, and getting a file of 2,148,777,108 bytes that was a pointer.
- **Cost:** minutes, because the wrong number was absurd. It would have cost far
  more if it had been plausible.
- **Status:** fixed, as a refusal. The width of an unsupported conversion is
  exactly what is not known, so the argument cannot be skipped; the printer
  names the conversion that defeated it and stops the line.

`kprintf` handled an unrecognised conversion by printing the two characters and
carrying on, under a comment saying *"print it visibly instead of silently
dropping it"*. The intent is right and the consequence is the opposite of it:
**the argument was never consumed**, so every conversion after it read the
previous caller's argument — a pointer printed as a length, a length printed as
an address.

The output stays perfectly well formed and every value in it is wrong. That is
the worst way for a printer to fail, because a printer is the instrument you
reach for when something else is wrong.

    EFI                            <dir>
    NvVars                         2148777108 bytes     <- a pointer

**Why the compiler did not catch it.** `kprintf` is annotated so GCC checks its
format strings, and it does: `%q` is rejected at build time. The reachable fault
is the *other* one — a conversion that is **valid printf and unimplemented
here**. `%-30s` is ordinary C. So is `%o`. Both compile; neither existed in the
switch.

**The fix, and why it is a refusal.** There is no way to skip the argument,
because its width is exactly what is not understood. So the printer says which
conversion defeated it and stops the line:

    %<unsupported conversion 'o'; the rest of this line is not printed>

Missing output is a bug somebody fixes. Wrong output is a bug somebody believes.

Field widths are implemented too — `%-30s` and `%8u` — because the reason
anybody reached for one is that every table this kernel prints is columns, and
the alternative was padding them by hand inside the format string.

**Shown a fault before being believed.** `%o` injected on purpose, watched to
produce the marker, and watched *not* to print the `%u` after it.

### KF-128 — One medium could not carry two architectures, because both loaders opened the same filename

[#286](https://github.com/neogentrics/ReconOS/issues/286)

- **Found:** 6 September 2026, building an actual bootable USB stick and trying
  to boot it on both architectures.
- **Cost:** nothing yet — no install medium had ever been built until today.
- **Status:** fixed. The kernel on the medium carries its architecture in its
  name -- `kernel-x86_64.elf`, `kernel-aarch64.elf` -- and the old path is
  still tried as a fallback, so media written before the rule still boot.

UEFI's removable-media path is *already* per-architecture: `BOOTX64.EFI` and
`BOOTAA64.EFI` sit side by side in `\EFI\BOOT`, and a machine runs the one it
can without any boot entry registered in firmware. That is what makes a stick
bootable on a machine that has never seen it, and it means **one stick can carry
loaders for both architectures and boot on either**.

The kernel could not follow. Both loaders opened `\reconos\kernel.elf` — one
filename — so the medium could hold exactly one kernel, and the other
architecture would read a binary built for a machine it is not.

**What that actually looks like**, which is the part worth keeping:

    reconboot -- the ReconOS bootloader
      kernel       : 549024 bytes read
    reconboot: the kernel wants to live at 0x100000 and the firmware will not
               give it up.
    reconboot: claiming the kernel's memory failed, status 0x800000000000000e

Every word of that is true and it points at the wrong thing. `0x100000` is an
x86_64 load address; the ARM loader had read the x86_64 kernel and dutifully
tried to honour it. It reads as a firmware refusing an allocation, and it is a
file with the wrong name. **A correct error message about the wrong subject is
worse than a vague one**, because it is convincing.

The name carries the architecture now — `kernel-x86_64.elf`,
`kernel-aarch64.elf` — and the old path is still tried as a fallback, so media
that predates the rule still boots. The ESP recipe writes the new name.

**Why it went unfound for four checkpoints.** The loader has been booted against
OVMF and AAVMF since checkpoint 4, on every run of the verification rig — but
always from an ESP image built for *one* architecture, by a rule that put that
architecture's kernel in it. The rig never built a medium holding both, because
until somebody wanted a real bootable stick there was no reason to. The fault is
real from the first line; what was missing was a medium that could show it.

### KF-127 — Whether a drive is flash is a question USB cannot answer, and the storage layer assumes it can

[#285](https://github.com/neogentrics/ReconOS/issues/285)

- **Found:** 6 September 2026, on a physical USB stick, the first real disk this
  kernel has ever been shown.
- **Status:** **Half fixed, 8 September 2026, and the open half is the one that
  bites.** Checkpoint 11b's driver now *asks*: it sends INQUIRY for vital
  product data page B1 and sets `seek_is_free` from the medium rotation rate,
  rather than inferring anything from the bus. A device that does not offer the
  page leaves the field at its default, which is now a measured default rather
  than a guess -- the question was put and the device declined to answer.
  `scripts/usb-storage-test.sh` asserts the question is asked, from QEMU's SCSI
  trace, so it cannot quietly stop being asked.

  **What is still open is the part this bug is actually about.** The hazard was
  never the allocator hint; it was that a USB SSD would never be told a block
  had been freed. `usb_storage.c` sets `discard = NULL`, so it still is not
  told -- correctly reported as `BLOCK_ERR_UNSUPPORTED` rather than as success,
  but not told. SCSI's answer is UNMAP, gated on the Block Limits VPD page, and
  nothing has asked for it yet. And `seek_is_free` remains a bool, so the three
  states this entry argues for -- rotating, solid state, **unknown** -- are
  still two. The driver knows which of the three it is and has nowhere to say
  so.

The storage layer decides whether a drive is solid state in order to decide
whether to tell it about blocks that stop being needed — Dataset Management on
NVMe, TRIM on ATA. It answers that question two ways, and both are properties of
a *transport*:

- NVMe: everything on it is flash, so the answer is yes by construction.
- ATA: IDENTIFY word 217, the nominal media rotation rate.

**USB mass storage has neither.** It is SCSI in a wrapper; there is no IDENTIFY
word 217 to read and no NVMe identity page. Linux, asked about the stick this
was found on, says:

    /sys/block/sde/queue/rotational : 1

on a device with no moving parts, because when nothing tells the block layer
otherwise it assumes rotating. A USB SSD — an ordinary external drive — would
therefore be classified by ReconOS as a hard disk and would never be told that a
block had been freed. Not a crash and not corruption: it wears the drive out
faster and slows it down over months, invisibly, with every individual line of
code correct.

**What makes it worth a number rather than a note.** The two answers were not
written as "these are the transports we can ask". They were written as *the*
answer to "is this flash", and a question with two answers that each happen to
work for one transport looks finished until a third transport arrives. That is
the same shape as KF-119's comment arguing "this case does not arise here" about
a case that then arose.

The honest form is three states — rotating, solid state, **unknown** — and a
discard path that declines to guess. SCSI has a real answer available for the
third case (the Block Limits and Block Device Characteristics VPD pages, where
a medium rotation rate of 1 means non-rotating), and that is where 11b should
read it from rather than inferring from the bus.

**And what actually found it:** not reading the code. Plugging in a drive and
asking the operating system that already had it what it thought the drive was.
No emulator would ever have said this, because QEMU is always truthful about
what it is pretending to be.

### KF-126 — A transaction's capacity was written down as one number, and it is three

[#284](https://github.com/neogentrics/ReconOS/issues/284)

- **Found:** by running the ReconFS battery against a 512 MB disk instead of the
  16 MB one the verification rig uses.
- **Cost:** nothing yet. It has never been reached by anything but a test.
- **Status:** fixed. Capacity is computed from the block size rather than
  written down as one number, the check says which of the two cases it hit,
  and `reconfs_txn_capacity` exists so a caller can ask instead of finding
  out.

A transaction may touch a fixed number of owner-table leaves, and a leaf holds
one owner per eight bytes of *itself*. So its capacity scales with the block
size:

    capacity = TXN_LEAVES * (block_size / 8)

      4 KiB blocks ->   8,192 blocks =  32 MiB of allocation change
     16 KiB blocks ->  32,768 blocks = 512 MiB
     64 KiB blocks -> 131,072 blocks =   8 GiB

The comment above the constant said "sixteen leaves is 16,384 blocks", flat,
with no block size attached. That figure is right at 8 KiB and at no other size
the format allows — and wrong by a factor of two at 4 KiB, which is the size
every volume under two terabytes gets.

**The same shape as KF-117.** A number written in a comment, stated as a fact,
wrong, and unreachable from any test. That one was a 16 TiB ceiling and was
found only because somebody asked whether the limit was real. This one was found
only because a disk was made bigger than the rig's.

**What it is not.** Not a file-size ceiling: `reconfs_write_named` already
refuses a file needing more than one indirect block, which is 512 blocks at
4 KiB — far inside the bound. Nothing in the filesystem can reach this today.

**What went wrong in the test.** The freed-block-exclusion test fills the volume
to force the allocator to wrap, which is the only way to reach the case it
exists for. A volume with more blocks than one transaction can track cannot be
filled by one transaction — so on a large enough disk the test reported a
filesystem fault when what had actually happened is that it had outgrown its own
method. Bracketed exactly:

    32 MB disk,  8,192 blocks of 4 KiB : pass (8,157 taken)
    64 MB disk, 16,384 blocks of 4 KiB : FAIL (8,141 taken, txn failed)

Still a failure either way — a check that cannot reach its case is not a check,
and passing quietly is how it would stop being one — but it now says which of
the two happened, and `reconfs_txn_capacity` exists so a caller can ask rather
than find out.

**And the rig, for the third time this session.** KF-124 hid below nine
processors because the rig stopped at eight. KF-125 hid because the only node
with a child was one the rig never asked about. This hid because the disk was
16 MB. Every one of them is the same sentence: *the fault is real from the first
line; what was missing was a machine big enough to show it.*

### KF-125 — The device tree walk lost any node that had a child, which is why the GICv3 fix could not be reached

[#283](https://github.com/neogentrics/ReconOS/issues/283)

- **Found:** chasing KF-124's fix, which did not work and had two wrong
  diagnoses before this one.
- **Cost:** an entire debugging session, most of it spent suspecting the pointer
  the walk was given rather than the walk.
- **Status:** fixed. Both device-tree walkers report a node when its first
  child begins as well as at its end, and the kernel prints which interrupt
  controller generation it picked rather than deciding in silence.

`fdt_each_compatible` collects a node's `compatible` and `reg` as the properties
go by, and reports the node when it sees `FDT_END_NODE`. A node's children sit
between its properties and its own end, and `FDT_BEGIN_NODE` clears both fields
to start the child fresh. So a node with a child had its match erased by that
child, and its `FDT_END_NODE` found nothing to report.

Both walkers in the file had it. Neither had ever been asked about a node with
children:

    pci-host-ecam-generic   pcie@10000000         children: 0
    virtio,mmio             virtio_mmio@a000000   children: 0
    arm,gic-v3              intc@8000000          children: 1   <-- the ITS

So PCI worked, storage worked, and the *first* question ever asked of this walk
about a node with a child was "does this machine have a GICv3" — answered "no"
by a machine holding one. The kernel fell back to GICv2, wrote to a CPU
interface that does not exist on such a machine, and panicked at boot. That is
KF-124's symptom exactly, which is why two rounds of fixing KF-124 changed
nothing: the fix was correct and could not be reached.

A node is fully described the moment a child begins, because properties always
precede children. Both walkers now report there as well as at the end.

**What actually found it.** Not reasoning — dumping the machine's own device
tree with `-M virt,dumpdtb=` and reading it with a script that had nothing to do
with the kernel. Two rounds of reading the C and reasoning about what it must be
doing produced two confident wrong answers, and the second one was written into
the source as a comment explaining a mechanism that does not exist. The blob
took one command to obtain.

**And the instrument that should have existed.** The kernel decided its
interrupt controller generation in silence. Being wrong about it does not
produce a message; it produces `external abort, on a write, 0xffff800008010004`,
an address that means nothing unless you already know which generation the
kernel picked. It prints the generation now — one line, and the difference
between a five-minute diagnosis and a session-long one.

### KF-124 — The kernel panicked at boot on any ARM machine with more than eight processors

[#281](https://github.com/neogentrics/ReconOS/issues/281)

- **Found in** kernel 0.0.11. **Found by** running the processor discovery past
  the sizes the verification rig uses — the rig stops at eight, and eight is
  exactly where this starts.
- **Was** the interrupt controller code spoke GICv2, at a hardcoded address.
  GICv2 supports at most eight processors; above that a machine has a GICv3,
  whose distributor looks similar and whose **CPU interface is not memory at
  all** but a set of system registers. The address the kernel wrote to is not a
  CPU interface on such a machine, the write took an external abort, and the
  kernel panicked before finishing boot.

  QEMU's `virt` board switches at exactly that boundary, so `-smp 8` booted and
  `-smp 9` did not. Every run in the rig was at eight or fewer.

  This is the shape recorded in [[project-reconos-instrument-over-theory]] as
  *a whole class of bug invisible below some particular machine size* — the same
  reason the rig boots at 2, 4 and 8 processors rather than once. The rig was
  built on that principle and then had its own ceiling, one processor below the
  first machine that would have shown this.

  The requirement it violated is explicit: *support all architectures, and use
  any CPU to its fullest including multithreading and multicore.* A twelve-core
  ARM machine would not have started.
- **Fixed in** kernel 0.0.11. The generation is read from the distributor's
  peripheral identification register, which both generations place at the same
  offset — which is the only reason asking is possible before knowing which is
  there. On v3:

  - the distributor gets affinity routing enabled, without which it behaves as
    though it were a v2 and the redistributors are never consulted;
  - each processor finds **its own** redistributor frame by matching its MPIDR
    affinity against `GICR_TYPER`, not by index — nothing guarantees the frames
    are in processor order, and using the wrong one arms the timer on somebody
    else's processor, which looks like one processor that never ticks and
    another that ticks twice;
  - the redistributor is woken, because it powers up asleep and an asleep
    redistributor forwards nothing;
  - the timer's interrupt is enabled in the redistributor rather than the
    distributor. **This is the one that catches people:** on v3 the distributor
    does not own the per-processor interrupts, so a timer configured there is
    configured perfectly and never fires;
  - the CPU interface is reached through system registers, with `ICC_SRE_EL1`
    set first because until it is the others do not exist.

  The rig now boots aarch64 at sixteen processors as well, so the ceiling it
  had is gone rather than moved.

  **The first fix broke every machine it was not for.** Detecting the
  generation by reading the distributor's peripheral identification register
  looks obvious and is wrong: GICv3 puts that register at offset `0xFFE8` and
  GICv2 puts it at `0xFE8`. So the kernel asking "which generation are you"
  read an offset that does not exist on half the machines it was asking, took an
  external abort, and panicked — on every machine with *eight or fewer*
  processors, while nine and above worked perfectly. The fix and the fault had
  swapped places.

  It is detected from the device tree now: `arm,gic-v3` on the interrupt
  controller node is the machine describing itself, it cannot fault, and it is
  the same source the memory map and the PCI window already come from. A machine
  with no device tree falls back to v2, which is written down as an assumption
  rather than a discovery.

  Two things this is worth keeping for. **A register offset is not a place to
  ask a question whose answer decides where the register is** — the query needs
  a source that is valid before the answer is known. And the range that caught
  it is the same range that caught the original: booting one machine size proves
  something about one machine size.

### KF-117 — ReconFS could not have held a drive you can buy today

[#274](https://github.com/neogentrics/ReconOS/issues/274)

- **Found in** kernel 0.0.11. **Found by** the author asking whether the system
  would support drives larger than 16TB, having seen other systems stop at 4 or
  8. It would not have.
- **Was** the owner recorded in ReconFS's allocation table was a 32-bit block
  number, which caps a volume at 2^32 blocks — **16TiB** at the smallest block
  size. It was written down in the format header as a documented limit, with a
  note that a larger volume must be refused at format time rather than silently
  wrapped.

  Documenting a limit is not the same as the limit being acceptable. 20TB and
  24TB drives are on sale now, and a filesystem that refuses the disk somebody
  just bought does not have a limitation, it has a defect.

  The shape worth keeping is how it survived: **every test ran on a volume a
  hundred thousand times smaller than the limit, so every test passed.** A
  capacity ceiling is invisible to a test suite that cannot reach it, and the
  ceiling was found by being asked about rather than by being hit.

  Nothing else in the stack had a ceiling. NVMe carries a full 64-bit LBA, AHCI
  stops at ATA's own LBA48 (128PiB), and GPT at 8ZiB. The only limit in the
  system was the one written here.
- **Fixed in** kernel 0.0.11. The owner is 64 bits, which costs the allocation
  table 0.2% of the volume instead of 0.1% — the price of the reverse sweep
  being an independent derivation — and the block size is now chosen at format
  time, which takes most of that back on a large volume (0.012% at 64KiB).

  And the arithmetic is now tested where a disk cannot reach:
  `reconfs_layout_self_test` runs the layout maths at volumes from 1GiB to 1EiB
  on every boot, comparing what the format computes against a derivation written
  separately from the definition. Verified by reintroducing a depth bug, which
  it caught at exactly the sizes that matter — 2^32 blocks of 4096, the old
  ceiling, and 5,859,375,000 blocks, which is a 24TB drive.

### KF-118 — A block-size rule that read like a rule and behaved like a constant

[#275](https://github.com/neogentrics/ReconOS/issues/275)

- **Found in** kernel 0.0.11. **Found by** running the function over a table of
  volume sizes from 64MB to 24TB and noticing every answer was identical.
- **Was** `choose_block_size` grew the block size while the allocation table
  exceeded a fiftieth of a percent of the volume. The table is eight bytes per
  block, so its share is exactly `8 / block_size` — 0.195% at the minimum block
  size, which is *already under* the threshold. The loop condition was satisfied
  on its first test at every volume size, and the function returned 4096 always.

  It compiled, it ran, it returned a plausible number, and nothing about its
  output said it was not doing anything. A heuristic whose threshold sits on the
  wrong side of its own starting point is indistinguishable from a working one
  unless somebody tabulates it.

  Third in a row of this shape, after KF-114 and KF-115: **something that looks
  like it is working, is not.**
- **Fixed in** kernel 0.0.11. Replaced with an explicit table — 4KiB under 2TiB,
  16KiB under 16TiB, 64KiB above — with the trade-off written down beside it,
  because a bigger block wastes proportionally more on every small file and no
  formula can know what a volume will hold. An explicit size passed by the
  caller always wins, which is what the installer is for.

### KF-114 — The crash harness never cut the power, and reported that it had

[#270](https://github.com/neogentrics/ReconOS/issues/270)

- **Found in** kernel 0.0.11. **Found by** twelve orphaned QEMU processes still
  running minutes after the harness had exited reporting success. Not by the
  result, which looked exactly like a pass.
- **Was** the harness launched the emulator as
  `timeout -s KILL 30 qemu-system-... &` and took `$!` as the pid to kill. `$!`
  is the pid of `timeout`, not of QEMU. SIGKILL cannot be caught, so `timeout`
  died without forwarding anything and QEMU was orphaned and carried on
  running. The harness then read the disk image out from under a live guest and
  found an unbroken prefix of markers, which is what a healthy result looks
  like. Its "wait until the process is genuinely gone" loop polled the dead
  wrapper's pid and returned immediately.

  So twenty-eight reported power cuts, across two architectures, cut nothing.
  The durability measurement that `docs/RECONFS.md` was written on top of
  measured nothing at all.

  The shape is worth keeping, and it is not "a pid bug". It is that **a test
  whose subject is missing looks identical to a test whose subject is
  healthy** — silence is the pass condition for both. This is the second time
  this exact script has had a pid that was not the process it meant; the first
  is recorded in its own comments, and having been burned once did not prevent
  the second.
- **Fixed in** kernel 0.0.11. QEMU is launched directly, so `$!` is the emulator;
  an `EXIT`/`INT`/`TERM` trap kills it if the run is interrupted, so orphans
  cannot accumulate silently again.

### KF-115 — The crash harness passed cleanly with its checker missing

[#271](https://github.com/neogentrics/ReconOS/issues/271)

- **Found in** kernel 0.0.11. **Found by** fixing KF-114 and watching the next
  run print `0 out of order, 0 torn` while every single round had printed
  `can't open file 'scripts/check-markers.py'`.
- **Was** the status switch ended in `*) echo ...`, which printed the round and
  incremented nothing. A round whose check did not run therefore contributed
  zero gaps and zero tears — indistinguishable, in the totals and in the exit
  code, from a round that was checked and was clean. Fourteen consecutive
  failures to check produced a green run and exit 0.

  (The checker was missing because the kernel work was being built inside
  another session's working tree, as untracked files, and something there
  removed them. That is fixed separately by building from a dedicated checkout
  of the `kernel` branch — but the harness must not depend on it.)
- **Fixed in** kernel 0.0.11. Rounds that produce a recognised status are
  counted, and a run where that count is not equal to the number of rounds
  fails with "this is not a result". Verified by hiding `check-markers.py` and
  confirming the harness exits 1.

### KF-116 — The flush instrument counted zero on a driver that was flushing

[#272](https://github.com/neogentrics/ReconOS/issues/272)

- **Found in** kernel 0.0.11. **Found by** the new instrument reporting `0` for
  AHCI in the same run where NVMe reported 4097 — a disagreement between two
  drivers that had no reason to differ.
- **Was** two faults in the new `scripts/flush-reaches-device.sh`, both of which
  made it *lenient*:
  1. It matched QEMU's ATA trace as `cmd=0xea`. QEMU writes `cmd 0xea`. The
     driver was issuing 4097 FLUSH CACHE EXT commands and the instrument saw
     none of them.
  2. `flushes=$(grep -c ... || echo 0)` produced `"0\n0"` when grep matched
     nothing, because `grep -c` already prints `0` before exiting non-zero. The
     `[ -lt ]` comparison then failed with "integer expression expected", and
     the failure fell through to the success message — a shell error counted as
     a pass, again.
- **Fixed in** kernel 0.0.11. The opcode is matched as written on the wire, and
  both counts must parse as numbers or the driver is failed. Verified the only
  way that means anything: `nvme_flush` was altered to return success having
  issued nothing, and the instrument reported *0 flush commands for 4096
  markers* while passing the untouched AHCI driver in the same run.

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


### KF-145 — Whether the kernel could write through a read-only page depended on which firmware booted it

[#384](https://github.com/neogentrics/ReconOS/issues/384)

- **Found:** 9 September 2026, by the address-space self-test, on its first run.
- **Cost:** none yet, and it would have been unbounded: copy-on-write is
  enforced by mapping a shared page read-only, and this made that enforcement
  optional on some boot paths and not others.

`CR0.WP` is the bit that makes a read-only page read-only to ring 0 as well.
Without it, kernel code may write through any mapping regardless of its
read-only bit, and the processor raises nothing.

Nothing in this kernel ever set it. `arch/x86_64/boot.S` *clears* it for four
instructions to write one page-table entry and then restores whatever was
there — so its value was whatever the firmware left behind. Set under OVMF,
which is the only reason boot.S had to clear it at all. Clear on the paths that
boot with no firmware, because that is its state after reset.

So two boot paths of the same kernel differed in whether the read-only bit
meant anything to the kernel, and no test could see the difference, because
nothing in the kernel had yet relied on it.

Copy-on-write is what made it matter. A page shared between programs is kept
shared by being mapped read-only; the first write traps and the writer is given
a copy. With `WP` clear, the kernel's own write does not trap — it goes into the
page every other program is still reading. No fault, no message, and every page
table correct.

- **Was:** a control bit that was only ever cleared and restored, never set, so
  its value was inherited from firmware rather than decided.
- **Fixed in** `arch_vector_enable`, which already runs on every processor and
  already sets the other control bits that make a promise to the hardware.
- **Found by** the address-space self-test reading the shared zero page back
  after a write and requiring it still to be zero. That assertion exists
  because the failure it catches produces no other symptom.

### KF-146 — Sixteen bytes past the end of the vector save area, into the next field of the same thread

[#385](https://github.com/neogentrics/ReconOS/issues/385)

- **Found:** 9 September 2026, when a user program on aarch64 took an
  instruction abort on its own code immediately after being preempted.
- **Cost:** about an hour, and it had been present since the vector unit was
  enabled in blueprint section 1.1.

`struct thread` held the vector state in `u8 vector_state[512]`, and the comment
above it said 512 covered *"aarch64's thirty-two 128-bit V registers with their
two status words"*. Thirty-two registers of sixteen bytes is exactly 512. There
was nothing left for the status words, and `arch_vector_save` wrote them anyway:

    ((u64 *)area)[64] = fpsr;   /* byte 512 */
    ((u64 *)area)[65] = fpcr;   /* byte 520 */

Sixteen bytes into whatever field of `struct thread` came next, on every context
switch. What came next was `wait_next` and then `process`.

x86_64 never showed it. `FXSAVE` writes exactly 512 bytes and not one more, so
one architecture overran its buffer on every switch and the other never did.

It was silent for three checkpoints because nothing after the array mattered
yet. It stopped being silent when processes got address spaces: a user program's
`process` became zero the first time it was preempted, so the scheduler could no
longer find its process, switched it to the kernel's address space, and the
program took an instruction abort on its own code — a fault whose address, whose
class, and whose symptom all pointed at the memory manager rather than at the
context switch.

- **Was:** a comment asserting the arithmetic the code got wrong, and a buffer
  sized to the registers rather than to what was written into it.
- **Fixed in** `sched.h`: `VECTOR_STATE_MAX` is 576 — the registers, the control
  words, and a multiple of 64 so the alignment the store instructions require is
  kept.
- **And a guard**, because the first version of this bug was invisible and the
  next one would be too. Every thread carries a known value after the array, and
  the context switch checks it immediately after saving. Watched to fail: with
  the array put back to 512, the scheduler's self-test reports 45 switches that
  wrote past the area, on the same boot where every other test passes.



### KF-147 — A new process inherited the last one's permission to touch addresses

[#386](https://github.com/neogentrics/ReconOS/issues/386)

- **Found:** 10 September 2026, by reading `addrspace_create` while looking for
  something else, and noticing it was the odd one out in a family of three.
- **Cost:** none observed. It is recorded as a fault rather than a tidy-up
  because of what it would have cost later.

The address-space table is a fixed array of slots reused as spaces are created
and destroyed, and `addrspace_create` set only three of a slot's fields:

    if (as) {
            as->root = root;
            as->refs = 1;
            as->mapped_bytes = 0;
    }

`regions` and `region_count` were left holding whatever the previous occupant of
that slot had put there.

A region is not data. **A region is permission to touch an address**: it is what
`vm_fault_user` consults to decide whether a fault on an unmapped page is demand
paging or a wild pointer. So a process handed a reused slot inherited the
previous process's permissions — and a pointer into that range, in the new
program, was answered with a fresh zero page instead of ending the program.

The failure mode is the quiet one. Nothing crashes; a program that reads memory
it never asked for gets zeroes and carries on, and whether it does depends on
which slot it was given, which depends on how many processes have ended.

Two further consequences, both of them the same fault seen from another side:

- `region_count` was inherited too, so each reuse of a slot by a program that
  reserves a stack pushed the count one higher. After eight, `addrspace_reserve`
  refuses and a program cannot be given a stack at all — a machine that stops
  being able to run programs after it has run enough of them.
- A slot that had held a space with regions could not be told apart from a fresh
  one, so nothing downstream could detect it.

- **Was:** a reused table slot cleared field by field rather than wholesale, in
  the one place out of three that does it that way. `thread_create` uses
  `kzalloc`; `process_create` calls `kmemset` over the whole structure;
  `addrspace_create` named the fields it knew about. Adding a field to the
  structure was enough to make it wrong, and adding `regions` did.
- **Fixed in** `core/addrspace.c`: the slot is cleared whole before anything is
  written into it, the way the other two do it.
- **And asserted**, because reading the fix is not evidence: the self-test now
  reserves a region in a space, releases it, creates another, and requires the
  new one to have no regions and to *refuse* the address the old one was allowed
  to touch. Watched to fail with the clearing removed.



### KF-148 — A thread was runnable before it belonged to its process, and address spaces made that fatal

[#387](https://github.com/neogentrics/ReconOS/issues/387)

- **Found:** 10 September 2026, while building the ELF loader. A user program
  began failing about one boot in eight, on two processors, and the failure
  arrived as *"the program never reached its exit call"* — which is what the
  kernel says when a program is killed by a fault, because `exits` is only
  incremented by `sys_exit`.
- **Cost:** four measurements and two refuted theories. It could have cost far
  more: it is a program starting in somebody else's address space.

`thread_create` ended by putting the new thread in the run ring, which makes it
runnable immediately — on this processor and on every other one. Both user-program
paths then did this:

    t = thread_create(name, user_thread_start, entry);
    t->personality = &personality_recon;
    process_attach(p, t);          /* <- and the thread is already running */

A processor that picked the thread up between those two lines ran it with
`t->process == 0`. The scheduler reads exactly that to decide which address
space to switch to, found none, and left the kernel's loaded — so the program
began executing in an address space where its own code is not mapped, and took
an instruction fault on its first instruction:

    user program fault: page fault at 0x0000000000400000
      it touched 0x0000000000400000, which is not its to touch

**The ordering was always wrong and had never mattered.** Until 10 September
every user program lived in the one address space there was, so a thread running
with no process still had its code mapped and nothing went wrong. Per-process
address spaces did not introduce this fault; they removed what was hiding it.
The commit that made it reachable changed neither line.

- **Was:** a thread made schedulable before the caller had finished building it,
  in an API that offered no way to do otherwise.
- **Fixed in** `core/sched.c`: `thread_create_stopped` and `thread_start` are
  separate, and a thread is handed to the scheduler only once it has a process.
  `thread_create` keeps its name and meaning for the callers that have nothing
  to set — which is every kernel thread.
- **Measured, and the first three measurements were worth nothing.** The rate is
  about one boot in twenty and it moves with how busy the *host* is, so
  "build A, run forty, build B, run forty" compares two binaries and two
  afternoons. It produced 5/40, then 1/40, then 0/60, then 0/40 with the bug put
  back -- four numbers that between them establish nothing.

  What settled it was alternating the two kernels **inside one loop**, sixty
  rounds each, so both arms met the same conditions:

      A (fixed)        : 1 of 60 failed, 0 entry-point faults
      B (bug restored) : 4 of 60 failed, 3 entry-point faults

  The count of failures is noisy. The *signature* is not: the fault at
  `0x400000` is what this mechanism predicts and it appears only in the arm that
  has the bug.

- **Two other theories were tested against the same reproducer and refuted.**
  That the deadline was a wall clock outrunning a loaded guest -- a program
  cannot fault at its own entry point because a clock ran fast. And that
  `addrspace_activate` confused two spaces sharing a recycled table pointer --
  changing the comparison moved 35/40 to 36/40, which is noise.

- **It is not all of it.** One boot in sixty still fails with the fix in, with no
  unexpected fault -- a different mode, recorded as KF-150 rather than folded in
  here.

**What let it hide, and is worth more than the bug.** Every "N self-tests, all
pass" row in `scripts/verify-kernel.sh` is a *single-processor* run.
`check_cpus`, which is the only thing that boots with `-smp`, asserts processor
counts, idle ticks and shootdowns — and never that the self-tests passed. So a
self-test that fails only on more than one processor is invisible to the matrix,
which is the same shape as KF-143 and the reason this was found by hand.



### KF-149 — The page allocator and the kernel heap have no locking, on a kernel verified at thirty-two processors

[#388](https://github.com/neogentrics/ReconOS/issues/388)

- **Found:** 10 September 2026, while looking for the cause of KF-148. It is not
  that cause, and it is worse than that cause.
- **Cost:** none observed, and that is not reassurance. A lost page is invisible
  until something writes through a mapping it no longer owns.
- **Status:** fixed, 10 September 2026, as its own change with its own
  measurements — which is why it was recorded rather than fixed inside KF-148.

`core/pmm.c` and `core/heap.c` contain no spinlock, no atomic, and no interrupt
mask between them. `pmm_alloc_pages` scans a shared bitmap, sets bits in it, and
advances a shared `search_hint`; `pmm_free_pages` clears bits in the same
bitmap; the heap keeps slab free-lists the same way one level up. Two processors
in `pmm_alloc_pages` at once can both find the same clear bit and both claim it
— **the same page handed to two owners**, which is not a crash, it is two
subsystems writing over each other indefinitely.

`heap.h` explains itself, and the explanation is the interesting part:

> No locking: there is one CPU running kernel code until checkpoint 9, and a
> lock invented before the concurrency it guards is a lock in the wrong place.

That was a good decision when it was written and it expired without anybody
noticing. Checkpoint 9 brought threads; **9b woke every processor**; nothing
went back to the sentence. It is not a wrong comment — it is a promise with a
date on it, and nothing in the build or the rig was watching the date.

**What made it reachable is recent.** Not two threads calling `kmalloc`, but two
places that do not look like allocation:

- Demand paging (9 September) means a user program touching its stack calls
  `pmm_alloc_page()` **from inside a page-fault handler**, on whichever
  processor it is running on.
- The scheduler's reaper frees a finished thread's stack with
  `pmm_free_pages()` from a *different* thread on a different processor.

- **Was:** a deliberate absence whose stated condition stopped holding three
  checkpoints ago.
- **When it is fixed**, three things have to be true and are worth writing down
  now: the lock must be taken with interrupts off, because it is taken inside a
  fault handler and a timer interrupt there can preempt into another allocation
  on the *same* processor; the page must be cleared after `mark_used` and
  outside the lock, or every allocation in the machine serialises behind a
  four-kilobyte `memset`; and the order is always heap-then-pages, because
  `kmalloc` calls the page allocator and nothing in `pmm.c` calls the heap.
- **Fixed in** `core/pmm.c` and `core/heap.c`, to the design above: one lock
  each, `spin_lock_irq` everywhere, the page cleared after `mark_used` and
  outside the lock, and `alloc_large` deliberately left outside the heap lock so
  the two are never held together.

**And the test was wrong first, which is the part worth keeping.** The
assertion written for this was four threads allocating a page each, marking it,
yielding, and requiring the mark to survive — ownership, which is the fault this
lock prevents. With the lock removed it passed four times out of four. Two
processors landing on the *same bit* in the same instant is rare enough that
the test almost never saw it.

What catches an unlocked allocator is not ownership, it is **arithmetic**.
`bit_set` and `bit_clear` are read-modify-write on a shared byte and
`free_pages` is a shared counter, so two processors anywhere within the same
eight pages lose one another's updates. Every racer frees exactly what it took,
so the free count must come back to where it started, and a drift either way is
a page leaked or a page handed out twice. The racers were also given a batch of
eight pages per round rather than one, to widen the window.

That version reported a drift of **exactly twenty pages on every run, locked or
unlocked** — which was four thread stacks, allocated by `thread_create` and
freed by the reaper long afterwards, and not a bug at all. The window is now
opened by a barrier once every racer is running and closed by a second barrier
before any of them may leave, so only the racing allocations fall inside it.

Measured, rather than claimed: with the lock removed, **five of six boots at
`-smp 4` fail**; with it in place, eight of eight pass. One unlocked boot in six
still passes, so it is a detector rather than a proof — and the first version
passed with the bug present, which is not a test at all.



### KF-150 — About one boot in sixty, a user program does not finish, and nothing says why

[#389](https://github.com/neogentrics/ReconOS/issues/389)

- **Found:** 10 September 2026, as the part of KF-148 that fixing KF-148 did not
  account for.
- **Cost:** none yet. It is recorded because the alternative is rediscovering it.
- **Status:** open, and probably KF-156 — see below.

The interleaved measurement that confirmed KF-148 also measured the kernel with
KF-148 fixed, and it is not zero:

    A (KF-147 and KF-148 fixed) : 1 of 60 failed, 0 entry-point faults
    B (KF-148 restored)         : 4 of 60 failed, 3 entry-point faults

The three entry-point faults are KF-148 and they are gone. The remaining one is
a different shape: the program does not reach its exit call, and **no unexpected
fault is reported** -- so it is not a program being killed, which is what the
entry race looked like.

Two candidates, neither measured:

- The deadline is two seconds of `time_monotonic_ns()`, which on x86_64 comes
  from the timestamp counter. Under host load QEMU's counter advances with the
  *host* while the guest executes far fewer instructions, so two guest-measured
  seconds can pass with very little guest progress. Every user-mode test in this
  kernel shares that deadline.
- Or the thread genuinely is not scheduled, which would be a scheduler fault and
  a much more serious one.

**The two are indistinguishable from the message the test prints**, which is why
nothing more is claimed here. What has changed is that the next occurrence will
say which: a stalled program now reports its thread's state, the processor it is
on, how many ticks it has had, how many system calls were served while it should
have been running, and how many programs were ended for faulting. A run that
shows *running, 40 ticks, 0 calls served* is a scheduler fault; one that shows
*ready, 0 ticks* is a machine that never got to it.

- **Was:** unknown, and said so rather than folded into the bug next to it.



**10 September, later the same day: this is very likely KF-156.** A system call
entered on one processor and returned on another read the saved user stack
pointer out of the wrong processor's block, or out of a GS base of zero. The
symptom is a program that does not finish, at a rate that depends on how often a
thread happens to be preempted inside a system call — which is exactly the shape
of "about one in sixty, and it moves with host load".

It is not being closed on that reasoning. After KF-156 was fixed, **sixty
consecutive boots at `-smp 4` passed with no failure and no panic**, which is
consistent with it and does not prove it: the previous rate would predict about
one failure in sixty. This stays open until a longer run says otherwise, because
closing it on a run that would have been just as likely to be clean by chance is
how KF-148 got the credit for a fault it had not fixed.

### KF-151 — Eight processors, on a machine that may have five hundred

[#390](https://github.com/neogentrics/ReconOS/issues/390)

- **Found:** 10 September 2026, asked directly: would this run on a two-socket
  server board?
- **Cost:** none yet, because no such machine has run it. On one that did, it
  would use eight cores of however many are there and say so.

`MAX_CPUS` was 8. It is not a bug in the sense of something behaving wrongly --
9b's KF-141 already made the shortfall a reported number rather than a silent
saturation, so a 64-core machine says how many more it found than it can hold.
It is a bug in the sense that the number was chosen when the largest machine
this kernel had ever met was a QEMU guest, and modern server parts are two
orders of magnitude past it: EPYC reaches 128 cores per socket (192 on Turin),
Xeon 128 P-cores or 288 E-cores, and a dual-socket board is routinely 256 to 576
*logical* processors.

Raised to **256**, and that number has a reason rather than being the next round
one up: **255 is the largest processor an 8-bit APIC identifier can name.**
Going past it is not a bigger array, it is implementing x2APIC -- see KF-152.

The cost of the raise is static memory, and it is worth writing down because it
is the reason not to simply pick a huge number: `struct thread boot_threads[]`
is the expensive one at roughly a kilobyte each, so 256 costs about a quarter of
a megabyte of BSS, with the per-processor blocks, task-state segments and
identifier arrays adding tens of kilobytes more.

- **Was:** a limit sized to the test rig rather than to the machines the
  kernel is meant for.
- **Fixed in** `smp.h`, with the ceiling now stated as what the addressing can
  express rather than as a number somebody picked.
- **And the lookup had to become O(1) with it.** `x86_cpu_index()` searched
  `apic_id_for_cpu` from the front, which is free at eight entries and a
  256-iteration scan per lock acquisition at 256 — it sits on the path of every
  `this_cpu()`. There is now a reverse map indexed by APIC identifier, so
  raising the ceiling did not make a bigger machine slower at the thing it does
  most.
- **Untested above 32 processors**, and the header says so. No machine with
  more has run this kernel.

### KF-152 — Processors above 255 are found and cannot be started

[#391](https://github.com/neogentrics/ReconOS/issues/391)

- **Found:** 10 September 2026, reading the interrupt controller while answering
  the same question.
- **Cost:** none yet. It is the wall a real two-socket server hits.
- **Status:** fixed 10 September 2026, and **the fix has never executed.** See
  below, because that sentence is the whole of what is and is not true here.

The MADT walk already reads **x2APIC entries** (type 9), whose processor
identifiers are 32 bits, as well as the older 8-bit type 0. So on a machine with
more than 255 logical processors this kernel *enumerates them correctly*.

It then cannot address them. `x86_apic_id()` is:

    return apic_read(APIC_ID) >> 24;

which is the 8-bit identifier out of the xAPIC register, and INIT/SIPI go
through the xAPIC interrupt command register, whose destination field is equally
8 bits. Anything numbered above 255 can be seen and not spoken to.

The failure is at least honest: 9b made *found* and *online* separate numbers
precisely so that a processor which does not start is a visible discrepancy
rather than a processor nobody counted. A 240-core machine would report finding
240 and starting some smaller number.

- **Was:** half of an interface. The table parser learned about x2APIC and the
  interrupt controller did not.
- **Fixed in** `arch/x86_64/apic.c`: the register block becomes MSRs from
  0x800 upward, the identifier comes from `IA32_X2APIC_APICID` at its full 32
  bits rather than the top eight of a memory-mapped word, and the interrupt
  command is one 64-bit MSR write with the whole destination in the high half.
  It is enabled by a bit in `IA32_APIC_BASE`, once, at boot, from what CPUID
  reports — and because it cannot be turned off again without resetting the
  processor, every processor takes the same decision rather than deciding
  separately. In xAPIC mode a destination above 255 is now **refused with a
  message** rather than truncated into somebody else's identifier.

**And it is untested, which is stated in the code and not only here.** QEMU 8.2
does not implement x2APIC under TCG: asking for it produces

```
TCG doesn't support requested feature: CPUID.01H:ECX.x2apic [bit 21]
```

and even `-cpu max` reports the bit clear, which was measured rather than
assumed. KVM would provide it and this machine cannot reach `/dev/kvm`. So every
line that runs only in x2APIC mode has never been executed.

What *is* tested is the arithmetic those lines depend on, in
`arch_identity_self_test`: which register offset becomes which MSR, and that the
destination lands in the high half of the command word and survives being a
number above 255. Both were watched to fail. That does not make x2APIC tested —
it makes the untested part smaller, and the untested part is now "does the mode
switch take, and does a real processor answer afterwards".

### KF-153 — On a multi-cluster ARM machine, two processors would believe they are the same processor

[#392](https://github.com/neogentrics/ReconOS/issues/392)

- **Found:** 10 September 2026, in the same reading. It is the quiet one of the
  three.
- **Cost:** none yet, and it would not announce itself.
- **Status:** fixed 10 September 2026 — and the search for it found a second,
  worse instance in the boot assembly.

`arch_cpu_id_real()` on aarch64 is:

    return (unsigned)(mpidr & 0xFF);

which is affinity level 0 -- *the processor within its cluster*. The comment
above it says exactly that, and says a many-cluster machine needs the higher
fields folded in.

Every machine this kernel has run on has one cluster, so level 0 is unique. **A
two-socket ARM server does not have one cluster, and neither does anything
big.LITTLE.** Processor 0 of cluster 0 and processor 0 of cluster 1 both return
0, so both index the same entry of every per-processor array in the kernel: the
same task-state, the same current thread, the same active address space, the
same idle thread.

That is worse than the x86 limit above, and the difference is worth naming.
KF-152 produces a machine that runs on fewer processors than it has and reports
the discrepancy. This produces a machine where **two processors share the state
that exists to keep them apart**, with nothing failing until they touch it at
the same moment.

- **Was:** an identity taken from one field of four, correct on every machine
  tested and wrong on the first machine with two clusters.
- **Fixed in** `arch/aarch64/arch.c`, and **not** by folding the higher
  affinity fields in. Folding them would have made the value unique without
  making it an index: MPIDR values are sparse and a second socket may begin at a
  large affinity number. Instead the identity is the kernel's own dense index,
  assigned by discovery and carried by each processor in **TPIDR_EL1**, written
  by `boot.S` before that processor runs any C. It is also one register read,
  which matters because `arch_cpu_id()` is on the path of every lock.
  `arch_cpu_affinity()` still packs all four affinity levels — Aff3 lives at
  bits 39:32 — and is used for PSCI and for reporting, never as a subscript.

- **AND THE SAME MISTAKE WAS IN `boot.S`, where it was worse.** The test that
  decides which processor is the boot processor was also `mpidr & 0xff`. On a
  two-socket or big.LITTLE machine, **core 0 of every cluster passes that
  test**: several processors would each decide they were the boot processor and
  run `kmain` at once, on one stack, before a single line of this kernel's own
  SMP code had executed. It now requires all four affinity levels to be zero. A
  machine whose boot processor is not affinity zero would park every processor
  and hang — visibly, at the first instruction, rather than corrupting a stack
  and appearing to work.

- **The test, and what it does not prove.** QEMU's `virt` board numbers its
  processors 0,1,2,3 whatever topology it is asked for: `-smp 8,sockets=2,cores=4`
  and `-smp 8,clusters=2,cores=4` both produce flat affinities, measured rather
  than assumed. So the machine this bug is about cannot be booted here. What is
  tested is the arithmetic, against MPIDR values written down from the
  specification — with a control requiring the *old* rule to alias on that table,
  so the test fails if the table stops describing a multi-cluster machine. It
  found a real error on its first run: the Aff3 fixtures were shifted a byte too
  far.
- **And each processor now says who it thinks it is.** `smp_secondary_main`
  records `arch_cpu_id()` about itself, and the boot processor checks afterwards
  that every answer matches the slot it was started into and that no two match
  each other. The summary prints the machine's identifier beside the kernel's
  index, because a summary showing only the dense index looks identical on a
  machine whose identities alias and one whose do not.

### KF-154 — The page allocator scans, and a terabyte is a billion pages

[#393](https://github.com/neogentrics/ReconOS/issues/393)

- **Found:** 10 September 2026, working out whether a four-terabyte machine
  would work.
- **Cost:** none observed. It is a scaling property rather than a fault.
- **Status:** open.

Two things about the physical allocator stop being reasonable at server sizes,
and neither is wrong today:

**The bitmap is one bit per four-kilobyte page.** Four terabytes is a billion
pages, so the bitmap is 128 MB -- and it is placed as a single contiguous
allocation out of the firmware's map before anything else exists. Large, and on
a fragmented map possibly unplaceable.

**`pmm_alloc_pages` is a linear scan.** It starts from a hint and wraps once, so
the common case is short, but a fragmented billion-page bitmap has a worst case
of sweeping a billion bits for one page. Bounded, and bounded is not the same as
fast.

And a third that is neither of those: **there is no NUMA awareness at all.** On
a two-socket board every allocation is as likely to land on the far socket as
the near one, and nothing in the allocator, the scheduler or the address space
code knows there is a difference. That costs latency rather than correctness,
and it is the kind of thing that is far cheaper to design in than to retrofit.

- **Was:** a design correct for a 512MB guest and asked to describe a machine
  eight thousand times larger.
- **Note:** the direct map is *not* a limit here, which is worth recording
  because it looks like one. It begins at PML4 slot 256 and grows into
  successive slots as the map requires -- 127 TB of reach -- because it was
  deliberately placed at the bottom of the kernel half with room above it.


---


### KF-155 — The boot thread was processor 0's idle thread, so nothing on the boot path could ever wait

[#394](https://github.com/neogentrics/ReconOS/issues/394)

- **Found:** 10 September 2026, by the first piece of kernel code that tried to
  sleep. `timer_sleep_ns` reported that a thread could not sleep, and it was
  right.
- **Cost:** invisible for as long as nothing on the boot path waited for
  anything, which is exactly how long it lasted.
- **Status:** fixed.

`sched_init` clears the boot thread with `kmemset` and fills in the fields it
knows about. `idle_for` is a processor number where **-1 means "nobody's idle
thread"**, so zeroing it left the boot thread claiming to be processor 0's.

Two things followed from that, and both had been true since the field was added:

- it could only ever be scheduled on processor 0, because an idle thread must
  not be taken by another processor; and
- **it could never block.** `wait_sleep` refuses an idle thread, since a blocked
  idle thread is a processor that has stopped.

Nothing noticed because nothing had tried. The self-tests that use wait queues
all create threads of their own; the boot path ran straight through. The first
caller to attempt it was the timer wheel.

- **Was:** a structure cleared wholesale, with one field whose zero is a
  meaningful and wrong value. Exactly the shape of KF-147.
- **Fixed in** `core/sched.c` and `core/smp.c`: processor 0 is given a real idle
  thread like every other processor, and the boot thread becomes an ordinary
  thread that can sleep.
- **And the pinning was kept, deliberately.** Removing `idle_for` also removed
  the accidental pinning, and the boot thread promptly migrated to another
  processor mid-boot — which broke the identity self-test, and would have been a
  far worse problem than the one being fixed, since the boot sequence assumes
  throughout that it is processor 0 doing the work. `struct thread` now has a
  `pinned_to` field that says so on purpose, separate from `idle_for` because
  the two are pinned for different reasons.

### KF-156 — A system call entered on one processor and returned on another, and the kernel stack did not travel with it

[#395](https://github.com/neogentrics/ReconOS/issues/395)

- **Found:** 10 September 2026, immediately after KF-155 — a double fault at the
  first instruction of the system-call entry stub, with `%gs` based at zero.
- **Cost:** a kernel fault on the way out of a system call that had worked. **It
  is the best candidate so far for KF-150**, the one-boot-in-sixty that had no
  explanation.
- **Status:** fixed.

Interrupts are enabled across `syscall_dispatch`, deliberately: a system call
that cannot be interrupted is a system call a user program can use to stop the
machine. So a thread can enter one on processor A and be rescheduled onto
processor B. Two things then break, and **neither of them is per-thread state
being lost — both are per-*processor* state being used as if it were
per-thread**:

- `GS_BASE` is a register, one per processor, and it does not travel with the
  thread. The stub held the kernel's GS across the whole call and read the saved
  user stack pointer out of `%gs:8` at the end. A processor that had been running
  kernel threads and never come from user mode has `GS_BASE` at zero, so that
  read takes a page fault on address 8 — in ring 0, on a double-fault stack.
- Even with GS right, `%gs:8` is the *other* processor's slot. The user stack
  pointer was written into processor A's block and read out of processor B's:
  the program returns standing on some other thread's stack.

And underneath both, a third: `percpu[cpu].kernel_rsp` and `tss[cpu].rsp[0]` —
where a trap from user mode lands — were recorded **once**, by whichever
processor ran `arch_enter_user`. A comment in that function said, correctly:

> One thread's stack, recorded once. That is correct for exactly as long as one
> thread at a time runs in user mode; when processes arrive, this has to move
> into the context switch.

Processes arrived at checkpoint 19. The comment was right and nothing went back
to it.

- **Was:** three pieces of per-processor state standing in for something that
  belongs to a thread, on a path that was made preemptible before there was a
  second processor to be preempted onto.
- **Fixed in** `arch/x86_64/user_entry.S`, `arch/x86_64/user.c` and
  `core/sched.c`. The stub now swaps GS *twice at the top* — per-processor
  storage is used only for the two instructions where the thread has no stack
  yet, and the user stack pointer is then carried on the kernel stack, which is
  the thread's own and travels with it. There is no swap on the way out and no
  window in which a migration matters. `KERNEL_GS_BASE` is set once per processor
  in `arch_user_init`, so a processor that never entered user mode has it too.
  The kernel stack a trap lands on is recorded on the thread and told to the
  processor by a new `arch_thread_switched_in`, called from the context switch —
  which is where the comment said it would have to go.
- **aarch64 does not have this**, and that is worth recording as a difference
  rather than luck: an exception from EL0 switches to SP_EL1, and SP_EL1 *is* the
  kernel stack pointer the context switch has just set. There is no second place
  holding the answer, so there is nothing to get out of step.

### KF-157 — The clock ran at 201 Hz against a constant that said 100, and every test still passed

[#396](https://github.com/neogentrics/ReconOS/issues/396)

- **Found:** 10 September 2026, by a 50 ms sleep that took 30 ms — the first
  assertion in this kernel that had ever compared a tick count to a wall clock.
- **Cost:** every timer, sleep and scheduling slice wrong by a factor of two,
  from the moment the interrupt lines moved onto the I/O APIC.
- **Status:** fixed.

The 8254 was programmed with command `0x36` — **mode 3, a square wave**. A square
wave holds its output high for half the period and low for the other half, so
there are *two* transitions per tick. The 8259 as emulated counts one of them.
The I/O APIC counts both.

So the routing change was correct and the timer was wrong, and the two together
doubled the tick rate. Mode 2, a rate generator, pulses the output low for a
single input cycle and leaves it high for the rest: one transition, one
interrupt, whichever controller is listening. It is what every other kernel uses
the chip in.

- **Was:** a timer mode that happened to work with the only controller that had
  ever listened to it.
- **Fixed in** `arch/x86_64/time.c`, one command byte.
- **And the reason it needed finding at all is the interesting part.** Every
  tick-counting assertion still passed: the ordering of timers held, the cascade
  fired at the right *tick*, the scheduler preempted. `TIME_TICK_HZ` is not a
  measurement, it is a promise that every nanosecond-to-tick conversion in the
  kernel relies on, and nothing had ever checked it against a clock that does not
  come from the tick. `time_self_test` now measures the rate against the
  monotonic counter and fails if it is out by more than half. Watched to fail by
  putting `0x36` back: 203 Hz against 100.


### KF-158 — An idle thread took its turn in the round robin, and the machine ran at half speed with every test green

[#397](https://github.com/neogentrics/ReconOS/issues/397)

- **Found:** 10 September 2026, by the verification matrix — **seven paths failed and
  every one of them writes to a disk.** They wrote correct data and ran out of
  time doing it.
- **Cost:** roughly half the machine, for the length of one matrix run.
- **Status:** fixed.

Fixing KF-155 meant giving processor 0 an idle thread of its own, because the boot
thread had been serving as one and therefore could never block. That put an idle
thread in the run ring **alongside a working thread on the same processor for the
first time**, and `pick_next` returned the first eligible thread it found.

So the boot thread and `idle-000` were handed alternate slices. And the slice
handed to the idle thread was not a short one: `idle_loop` calls
`arch_wait_for_interrupt`, so it holds the processor until the next tick, doing
nothing. A machine with one processor and real work to do spent about half its
time stopped.

- **Was:** a scheduler that had never had to decide between work and idleness,
  because the ring had never contained both for the same processor. The boot
  processor had no idle thread and a secondary had nothing else. **The line was
  wrong before it was ever executed**, in the same way the TLB shootdown and
  KF-148 were: waking the other processors, or in this case giving one an idle
  thread, made existing code wrong without changing it.
- **Fixed in** `core/sched.c`: an idle thread is now a *last resort* rather than a
  turn in the round. It is remembered as a fallback and returned only when nothing
  else on that processor wants to run.

**And the reason it needed a matrix to find it is the point.** Twenty-six
self-tests passed. Nothing was incorrect — every thread ran, every timer fired in
order, every assertion held. The only symptom was *how long things took*, and the
entire suite is blind to that.

`sched_self_test` now counts, and fails on, an idle thread being scheduled while
another thread on that processor is READY. It is an invariant with a number
attached rather than a statistic: an idle thread exists so a processor has
something to do when nothing else will have it, and running one with work waiting
is the processor doing nothing on purpose. Watched to fail by restoring the old
line: four violations in one boot.

Same family as KF-157, found the same day: a fault whose only symptom is time.


### KF-159 — A thread was available to every other processor while the one it was leaving was still standing on its stack

[#398](https://github.com/neogentrics/ReconOS/issues/398)

- **Found:** 10 September 2026, by the verification matrix, as a kernel panic on
  aarch64 at four processors — about one boot in three. The link register read
  back as `0xacce5501`, which is the page allocator's own concurrency-test marker.
- **Cost:** two processors on one stack, and a freed stack written through. It
  had been reachable since checkpoint 9b woke the second processor.
- **Status:** fixed.

`sched_switch` marked the outgoing thread READY **before** calling
`arch_context_switch`:

```c
	if (prev->state == THREAD_RUNNING) {
		prev->state = THREAD_READY;
		prev->cpu = -1;
	}
	...
	arch_context_switch(&prev->stack_pointer, next->stack_pointer);
```

`arch_context_switch` is what saves `prev`'s callee-saved registers onto its
stack and writes `prev->stack_pointer`. So between those two points the thread is
advertised as runnable and **its saved stack pointer has not been written yet**.
Another processor picking it up in that window resumes it from a stale pointer,
with two processors executing on one stack.

The comment beside the lock release argued the opposite, and was wrong in a
precise way: *"prev is READY and owned by nobody, next is RUNNING and owned by
this processor"*. `prev` being owned by nobody is exactly the problem — it was
still being used by this one.

**Three separate paths share the fault, which is why the fix is an invariant
rather than three fixes:**

- `thread_exit` marks a thread FINISHED and then switches away. The reaper's
  guard was `t != this_cpu()->current`, which does not cover a thread that is
  current on a *different* processor — so a live stack was freed, handed to
  `pmm_concurrent_test`, and had a marker written over a return address. That is
  the panic above.
- `wait_sleep` marks a thread BLOCKED and then switches away; a waker on another
  processor can make it READY inside that gap.
- The ordinary preemption case above.

- **Was:** "READY" used to mean two things — *this thread wants to run* and *no
  processor is using it* — and they stopped being the same thing when there was a
  second processor.
- **Fixed in** `core/sched.c`, `sched.h` and `smp.h`, as one rule: a thread
  carries `off_cpu`, and `pick_next` and the reaper both require it. It is cleared
  when a thread is chosen and set by **whoever runs next on that processor**,
  which is the first instant at which the outgoing thread has genuinely stopped.
  `struct cpu_local` carries the thread waiting to be released; `sched_switch`
  releases it immediately after `arch_context_switch` returns, recomputing
  `this_cpu()` rather than reusing the pre-switch value, because a thread resumes
  on whichever processor picked it and not necessarily the one it left.
- **A thread running for the first time has no such return point**, so every
  thread now starts in a small C wrapper that releases its predecessor and then
  calls the entry point. In C rather than in each architecture's assembly
  trampoline: an agreement between two assembly files is the kind that drifts.
- **Measured:** the failing configuration went from panicking about one boot in
  three to 10 of 10 clean, and x86_64 at four processors 12 of 12.

**This is also a better candidate for KF-150 than KF-156 was**, and neither is
being credited with it. Both are real, both were fixed the same day, and the
honest position is that the one-in-sixty has not been seen since without a run
long enough to say so.

### KF-160 — The power-cut harnesses timed their cut from launch, so a slower boot meant they cut before anything had been written

[#399](https://github.com/neogentrics/ReconOS/issues/399)

- **Found:** 10 September 2026, by the harness itself, which said exactly what
  had happened: *"every round wrote nothing — the cut is landing before the disk
  is found, so this measured nothing. Raise the delay."*
- **Cost:** none, because it failed loudly rather than passing. That is the whole
  point of the entry.
- **Status:** fixed.

`crash-test.sh` swept its cut across `900 + (round * 137) % 2200` milliseconds
**after QEMU was launched**, and `rename-crash-test.sh` across `1500 + (round *
211) % 2600`. Both lower bounds were chosen against the boot time of the day they
were written.

Checkpoint 20 added about a second of self-tests to every boot — the timer
wheel's cascade case waits seventy ticks on purpose, because that is what it
takes for a timer filed on the second wheel to be walked down to the first — and
the early rounds started cutting a guest that had not reached the disk.

- **Was:** a delay measured from the wrong event. What these tests sweep is time
  spent *writing*; how long the kernel took to get there is not part of the
  question, and building it into the constant tied the harness to a boot time
  nobody was watching.
- **Fixed in** both scripts: each now waits for the line the guest prints when it
  begins writing, then sweeps from there. The wait is bounded, and a round whose
  guest never arrives is not counted as checked — which the existing *"only N of
  M rounds were checked at all"* assertion turns into a failed run rather than a
  quiet zero.
- **Re-tuning the constant would have worked**, until the next change to how long
  a boot takes. This is the third fault recorded in these two files where a
  harness measured something other than what it claimed; the other two are
  written up beside the code that caused them.


### KF-161 — A header promised that either pointer could be null, and one of them could not

[#400](https://github.com/neogentrics/ReconOS/issues/400)

- **Found:** 10 September 2026, by the first caller that took the header at its
  word — the new VFS, reading a file to check it did not exist yet.
- **Cost:** a write to address zero in kernel mode, and a panic. Nothing had ever
  passed null before, so the promise had been untrue and unexercised since it was
  written.
- **Status:** fixed.

`rootfs_read_file` says, in its header:

> Reads a whole file, and tells the caller the mode it was created with. **Either
> pointer may be null.**

`mode` honoured that — it is guarded by `if (mode)`. `got` did not: it was passed
straight through to `reconfs_read_named`, whose first act is `*got = 0`.

- **Was:** a contract stated in one file and kept in another, where only half of
  it was ever exercised. The half that worked was the half something used.
- **Fixed in** `core/rootfs.c`: a local stands in when the caller does not want
  the count, so the promise is kept where it was made rather than pushed down to
  a function whose own header never made it.
- **Why it is worth an entry at all**, being three lines: it is the same shape as
  KF-149's expired comment and KF-156's predicted one. A header is a claim about
  behaviour, and a claim nothing checks is a claim that drifts — this one had been
  wrong since it was written and would have stayed wrong until something believed
  it. What made it visible immediately was that the believer was in the kernel and
  crashed; a user program would have got a corrupted answer.

### KF-162 — Power-off declared the machine had refused, while the machine was in the middle of obeying

[#401](https://github.com/neogentrics/ReconOS/issues/401)

- **Found:** 10 September 2026, by the verification matrix — one path, "power
  off", on a run where four guests were booting at once. The same path had
  passed every previous matrix and passes eight times out of eight when it is
  the only thing running.
- **Cost:** a red matrix on a green tree, and a kernel that reports a false fact
  about the machine it is on.
- **Status:** fixed.

The guest printed `power: the machine was told to turn off and did not`, and
then powered off anyway. The harness reads that line as a failure, correctly:
the kernel had said the shutdown was refused.

`power.c` wrote the shutdown control register and then returned `POWER_REFUSED`,
under a comment asserting that the write does not return on a machine that
obeys. That is true of the **instruction** and not of the **machine**. The store
retires as soon as the write is accepted; the shutdown it triggers is carried
out by the platform, asynchronously, and the processor keeps executing until it
is stopped. On an idle host that gap is too short to measure. With four guests
competing for the host's processors it is long enough to run the next dozen
instructions, one of which announced a refusal.

- **Was:** a comment that described the semantics of a store rather than the
  behaviour of the device behind it, and code that trusted the comment. The
  kernel treated "I am still running" as proof of "it did not happen", when the
  only thing it proves is that the answer has not arrived yet.
- **Fixed in** `core/power.c`: after the write, a bounded wait of 200 ms against
  `arch_monotonic_ns()` before `POWER_REFUSED` is returned. Bounded rather than
  infinite, because a machine that genuinely will not turn off must still be
  reported and not hung on — the fact is worth having, it just has to be true.
- **Measured, not assumed.** Removing the wait and running thirty-two guests
  eight at a time reproduced the refusal three times; restoring it and running
  thirty-two more reproduced it none. A fix for a nine-percent flake that is
  only ever watched to pass is not a fix that has been tested: eight clean runs
  happen by chance nearly half the time with the fault fully present.
- **Family:** the third fault this month whose only symptom was *timing* —
  KF-157 and KF-158 cost duration with the whole suite green, and KF-160 was the
  same harness cutting power before the guest had spoken. Each was invisible to
  a test that asks only whether the right things happened. The matrix runs
  several guests at once, which is why it is the thing that found this and the
  quick check is not.

### KF-163 — Two virtio-blk disks on one machine, and every request to them times out

[#402](https://github.com/neogentrics/ReconOS/issues/402)

- **Found:** 10 September 2026, by attaching a second disk while testing the
  new block cache. Nothing had ever attached two before.
- **Cost:** a machine that looks hung. It is not hung -- it is making progress
  at two seconds a request, because that is the driver's timeout and every
  request is reaching it.
- **Status:** fixed.

Measured, on x86_64:

| devices | reaches the self-tests |
|---|---|
| one virtio-blk | yes |
| one NVMe | yes |
| two NVMe | yes |
| NVMe + virtio-blk | yes |
| **two virtio-blk** | **no** |

So it is not "two disks" and not "the second device". It is two instances of
*this driver*. On aarch64 the same pair over virtio-mmio is fine, which points
at the PCI transport rather than at virtio_blk itself.

The symptom is not a hang and calling it one would send somebody to the wrong
place. `virtio_blk`'s `run()` waits two seconds for a request and then returns
`BLOCK_ERR_TIMEOUT`, deliberately leaking the three descriptors because the
device may still write into them. So the boot continues, one request every two
seconds, until the queue runs out of descriptors -- at which point requests
start failing quickly instead. A 60-second run reached the partition tables; a
240-second run got no further than the storage probe, which is the variance you
would expect from something that is timing out rather than stopping.

- **Was:** not yet known. The candidates are the ones this shape usually comes
  from -- a memory-mapped window that the second device's mapping lands on top
  of, a notify address computed from the wrong device, or an interrupt both
  devices are told to use. `struct virtio_blk` and `struct virtio_pci` are both
  per-device arrays, so it is not the obvious kind of shared state.
- **Why it was never seen:** the verification matrix attaches exactly one disk
  on every one of its paths. Eighteen boot paths, six storage configurations,
  and not one of them has two devices of the same kind. That is the finding
  behind the finding, and the matrix should gain such a path -- but adding one
  now would put a red line on a green board for a fault that is already
  written down here, so it goes in with the fix.
- **Not a regression.** Reproduced on the tree as it stood before the block
  cache was written, which was checked first precisely because the cache was
  the thing that had just changed.

**What has been ruled out**, by instrumenting a copy of the tree rather than by
reading it. Each of these was a candidate and each is now a fact:

- *shared transport state.* Both devices get their own `slots[]` entry and
  their own mapped registers: `common` at 0xfe000000 and 0xfe004000, notify and
  device-config at distinct offsets in each, notify multiplier 4 for both.
- *shared driver state.* Both reach `virtio_blk_attach` and both complete it,
  with distinct `struct virtio_blk` and distinct `regs` pointers.
- *overlapping queue memory.* The two virtqueues and their scratch pages are
  disjoint and consecutive -- 0x2f000+3 pages and 0x32000 for the first,
  0x33000+3 pages and 0x36000 for the second. The page allocator is not the
  problem.
- *bus mastering.* Set per device, in `pci.c`'s `examine`.
- *a leaked busy flag.* `device_acquire` was instrumented to report on its very
  first failed attempt. It never reported: the flag is clear and the acquire
  succeeds.
- *the request never being issued.* Both devices complete a read of sector 0
  under instrumentation.

**What is implicated.** Replacing the `sched_yield()` in `virtio_blk`'s poll
loop with `arch_cpu_relax()` -- busy-waiting instead of yielding -- moves the
boot past the point where it had been stopping, through the partition scan of
the second device and on into the ACPI section. So the fault involves *yielding
while waiting for this device*, not merely the device being slow.

That also explains the one observation that made no sense on its own: **the
stall point moves when unrelated code is added.** Adding a `kprintf` to a path
that is not involved changes where it stops. Something scheduling-shaped, not
something device-shaped, which is why every device-side hypothesis above came
back clean.

**It was not the yield.** The answer, found on 10 September while building MSI-X
for the same driver, is the *legacy interrupt line*, and the evidence is one
kernel with one line changed between runs:

| INTx | completions | two virtio-blk disks |
|---|---|---|
| asserted | polled | **stalls in `block_init`** |
| disabled | polled | boots |
| disabled | by MSI-X | boots |

The middle row is the one that settles it. No message-signalled interrupt is
involved, the polling loop and its `sched_yield` are untouched, and the machine
boots.

**The device was behaving correctly and so was the kernel.** On completion a
virtio-pci device sets its interrupt status bit and asserts its line, and the
line stays asserted until a driver reads that byte to acknowledge it. No driver
here ever reads it -- this kernel polls the used ring, which tells it everything
it needs and leaves the acknowledgement undone. A level-triggered line that is
asserted and never acknowledged does not fire once. It is stuck on.

**And the machine had been saying so on every single boot.** The interrupt
summary of a perfectly ordinary one-disk run:

```
  11 (nobody)     1770000 taken, 1770000 with nobody to take them
```

One million seven hundred and seventy thousand interrupts on a line no driver
claims, printed in the boot summary since the day that summary was written, on
a path the matrix runs eighteen times. Nobody read the number. The same line
after the fix is absent entirely, and the lock counter on the same boot goes
from 703,505 acquisitions to 1,801,182 -- the machine had been spending well
over half of itself in an interrupt nobody wanted.

Why one disk survived it and two did not is the ordinary arithmetic of a
storm: with one device the processor still got enough time between interrupts
to make progress, and with two it did not. That is also why *the stall point
moved when unrelated code was added*, which was the observation that made no
sense on its own -- timing-sensitive, because it was a race against a storm.

- **Was:** `virtio_pci_probe` left the device's legacy interrupt line enabled,
  on a kernel with no INTx handler at all.
- **Now:** `PCI_COMMAND_INTX_DISABLE` is set as the device is claimed. Not a
  workaround for the unacknowledged line -- a device whose interrupt nothing
  services should not be asserting one, and the command register is where that
  is said. The day a driver here wants INTx it turns it back on and reads the
  status byte.
- **Deliberately in probe, not in the MSI-X path**, where it was first written.
  A machine with no MSI-X would otherwise still stall, and that is most of the
  reason to put a fix at the layer the fault is in rather than at the layer it
  was noticed from.
- **Six candidates were eliminated before this one**, each by instrumenting a
  copy of the tree, and every one of them came back clean -- because every one
  of them was about the *device*, and the fault was about the *line*. The
  seventh candidate, `sched_yield`, was implicated by a real experiment that
  pointed in a real direction and still named the wrong thing: busy-waiting
  helped because it kept the processor in the loop between interrupts, not
  because yielding was broken.
- **The matrix still attaches one disk on every path.** Two-of-a-kind goes in
  with this, which is the finding behind the finding and the only reason it
  took until September to see.

### KF-179 — The interrupt summary counted devices before the bus had been walked

[#406](https://github.com/neogentrics/ReconOS/issues/406)

- **Found:** 10 September 2026, while deciding whether to wire a driver to MSI.
  The boot summary said `MSI : 0 device(s) can signal by memory write`, which
  would have meant there was nothing to wire it to.
- **Cost:** nearly the wrong decision. The plan was to skip message-signalled
  interrupts on the grounds that no device here has them.
- **Status:** fixed.

`arch_irq_print_summary()` is called from `main()` at line 166. `block_init()`,
which walks the PCI bus, is called at line 184. The count was of
`pci_device_count()`, which is zero until the latter has run.

So the sentence had been printing `0` on every boot this kernel has ever made,
and it was not a fact about the hardware. It was a fact about *when it was
printed*. Called eighteen lines later, the same function on the same machine
says `4 device(s) ... 3 of them by MSI-X`.

- **How it was caught:** by not believing it. The summary said zero and QEMU's
  `info pci` showed the virtio disk with a 4KB BAR1, which is where a
  transitional virtio device keeps its MSI-X table. Two instruments disagreeing
  is a result, and the disagreement was the bug.
- **Was:** a device fact printed beside the processors, before there were any
  devices.
- **Now:** `arch_irq_print_device_summary()`, called after `block_init`.
  Separate from the routing summary because the two are true at different
  moments -- which is the whole of the fault, so it is the whole of the fix.
- **Family:** KF-157, KF-158, KF-160, KF-162 and now this -- five faults this
  month whose only symptom was *when* something happened rather than what.

### KF-180 — "Can signal by memory write" was implemented as "has MSI"

[#407](https://github.com/neogentrics/ReconOS/issues/407)

- **Found:** 10 September 2026, immediately after KF-179 made the number
  visible for the first time.
- **Cost:** none yet, because nothing had ever read the number.
- **Status:** fixed.

`x86_msi_capable_devices` counted capability `0x05`. The sentence it fed said
"can signal by memory write" -- which is also true of every device with
capability `0x11`, MSI-X, and no MSI.

On this machine that is not an edge case. It is both disks: the only two
devices that actually do it. The count said two (an ethernet controller and an
AHCI controller, neither of which this kernel drives by interrupt) and omitted
the two that matter.

- **Was:** a count of one mechanism under a heading naming the category.
- **Now:** either capability counts, with the MSI-X subtotal printed beside it,
  so the sentence and the number say the same thing.
- **Lesson, which is the same one twice:** both this and KF-179 are a *reported
  number that nobody had ever had a reason to check*. It went unnoticed for as
  long as nothing depended on it, and was wrong in two independent ways the
  moment something did.

### KF-181 — The vector self-test assumed it was the only thing holding a vector

[#408](https://github.com/neogentrics/ReconOS/issues/408)

- **Found:** 10 September 2026, the first time a real driver claimed a vector.
- **Cost:** one red line on an otherwise green boot, and thirty seconds of
  suspecting the driver.
- **Status:** fixed.

The allocator's self-test claimed vectors in a loop and asserted that all
sixteen were free. That was true on the day it was written, when nothing in the
kernel used one. `virtio-blk` then asked for one -- correctly, which is what the
allocator is *for* -- and the test failed:

```
  irq: only 15 of 16 vectors could be claimed
```

- **Was:** an assertion about a quantity that no longer belonged to the test.
- **Now:** it claims until refused, and asserts what was actually worth
  asserting: that they run out, that running out is reported rather than
  papered over, and that giving them back makes them available again. None of
  those depends on who else is holding one.
- **The pattern:** a test that passes only while it is the sole user of a shared
  resource is a test with an expiry date, and the date is the day the thing it
  tests gets its first real caller. It is the second test this week to fail
  because the kernel got *better* -- after KF-165, where a race was cured and
  the test that had been watching it went red.

### KF-182 — Tearing down an address space freed its page tables and not its pages

[#409](https://github.com/neogentrics/ReconOS/issues/409)

- **Found:** 11 September 2026, while working out whether a page cache could
  safely put shared pages into a program's map. The question was "who frees a
  mapped page", and the answer turned out to be nobody.
- **Cost:** every page a program ever touched, for the life of the machine.
- **Status:** fixed.

`free_lower_tables` walks a dying space and frees the tables. Its own comment
said what it did not do:

> The pages a program's mappings *pointed at* are not freed here either. This
> frees page tables; whoever allocated the memory frees the memory, and the two
> are not the same list.

**There was no other list.** Nothing anywhere freed a program's code page, its
stack, or any page it faulted in. The same sentence, in the same shape, sat in
the aarch64 copy.

- **Was:** a comment describing a division of responsibility with only one
  side.
- **Now:** the leaves are freed with the tables. Only the user half is walked
  — the top-level loop stops at 256 entries — so the kernel's own mappings are
  not reachable from here, which is what makes freeing leaves safe at all. A
  page shared with other spaces is left alone, which today means the shared
  page of zeroes: freeing that would hand the one page every program reads to
  whoever allocated next. The question is asked through
  `addrspace_page_is_shared` rather than compared inline, because it is about
  to have a second answer.
- **The test is the whole round trip**: free pages counted before the space
  exists and after it is gone, with the pages arriving by *fault* the way a
  real program's do, and *written* rather than read so each one is its own
  rather than eight references to the shared zeroes. Watched failing: the
  control reports `8 page(s) did not come back`, which is exactly the number
  faulted in.

### KF-183 — A process that ends is never reaped, so it keeps its slot and its memory

[#410](https://github.com/neogentrics/ReconOS/issues/410)

- **Found:** 11 September 2026, immediately after KF-182 and by the same
  measurement — which is the interesting part, because the measurement is what
  said the first fix had not worked.
- **Cost:** a process table that fills, and every ended program's memory held
  for the life of the machine. Measured at **8 of 32 slots used on an ordinary
  boot**, all of them `ended` with no threads.
- **Status:** fixed.

**The instrument corrected the diagnosis, and would not have if it counted one
number instead of two.** Having fixed KF-182, the same probe still showed
exactly twelve pages lost per program run — and a counter printed beside it
showed the new code had freed eleven leaves in the entire boot. The fix was
running and had almost nothing to do.

The reason is that `addrspace_release` is reached from `process_reap`, and
nothing reaps. A process ends, records its exit status for somebody to collect,
and waits for a collector that never comes:

```
Processes
  table        : 8 of 32 in use
  facts        : id 7, parent 0, uid 65534, 0 threads, ended
  facts        : id 8, parent 0, uid 65534, 0 threads, ended
  facts        : id 9, parent 0, uid 65534, 0 threads, ended
```

Every one of those holds an address space, and every space holds its pages.

- **Was:** believed to be the same fault as KF-182. It is not, and KF-182's fix
  is necessary without being sufficient — a space that is never released cannot
  be released correctly.
- **Why it is not simply "reap on exit":** the exit status is the thing being
  kept, and something has to be allowed to read it. The kernel's own tests read
  a program's exit code after its thread has ended, so an immediate reap would
  destroy the answer before the question. A process whose parent is the kernel
  has no one who will ever call wait, and that — not "ended" — is the condition
  that makes reaping safe. Writing that down rather than guessing at it is why
  this is open.
- **Not a regression.** Nothing has ever reaped; the table is large enough that
  a boot which runs a handful of programs has never exhausted it, which is
  exactly why it went unseen. The number was printed on every boot.

**The cause was simpler and worse than the entry above guessed.** It is not
that reaping needed a policy. `reap()` — which frees finished threads' stacks,
and which is correct — had **exactly one caller in the whole kernel, and it was
a self-test**. `process_reap` had two, both in another self-test. Both reapers
were written, both work, and nothing ever ran them.

`thread_exit` said so, in the same shape as KF-182 one file over:

> The stack cannot be freed here: this code is standing on it. It is left for
> **whoever notices** the thread is finished.

Nobody noticed. That is twice in two days that a comment delegated to a
collaborator who was never created.

- **Now:** `thread_exit` schedules deferred work that runs the reaper. Deferred
  because here is the one place it cannot be done, and the worker refuses a
  second queueing while the first is pending — so one pending reap collects
  however many threads have died by the time it runs.
- **`reap` now takes the ring lock, which it never did.** That was safe for
  exactly as long as it only ran from a test on a quiet machine. Running it
  from the worker makes it concurrent with every scheduling decision on every
  processor, and an unlocked walk of a list somebody else is splicing is a
  pointer into freed memory. Victims are taken under the lock and freed
  outside it, because freeing one can release an address space and walking
  page tables with the scheduler's lock held stops the machine.
- **A process is reaped when the last thread that ran it has itself been
  reaped**, which is not the same as when its last thread *ended*. The thread
  count drops to zero inside `thread_exit`, while that thread is still standing
  on a kernel stack reached through the very page tables being freed. The same
  shape as `off_cpu` in KF-159, one level up.
- **Keeping a status is opt-in.** `process_expect_status` says somebody will
  collect; the default is that nobody will, which is the honest default because
  nothing in this kernel reads a process's exit status except the test that
  asks. Not inferred from the parent: every process here is made with a parent
  of zero, so "has a live parent" would reap the one process a test is about to
  ask about.

**And one measurement corrected the fix twice.** After the first version,
pages-per-run went from twelve to seven and the table still showed eight slots
in use. A counter said `threads=23 processes=0`: the thread half worked and the
process half never fired once. A second counter said `calls=23 noproc=23` —
every reaped thread had no process by the time the reaper saw it, because
`process_thread_ended` clears `t->process`, correctly, when the thread stops
running as anybody.

- **Was:** the reaper looked up the process through `t->process`, which means
  *the process this thread is running as* and is nothing by then.
- **Now:** `t->counted_by`, set at attach and never cleared, which means *the
  process that cannot be reaped until this thread has been*. Two different
  facts that had been sharing a field.
- **Measured, same program three times:** twelve pages lost per run before,
  **zero** after, and the process table goes from eight slots in use to one.
  Watched failing: with the reaper never asked to run, the test reports "a
  process nobody will collect was still holding a slot two seconds after it
  ended".

### KF-184 — The page cache asked which file and not which filesystem, so two of them were the same file

[#411](https://github.com/neogentrics/ReconOS/issues/411)

- **Found:** 11 September 2026, by the eviction test, on its first run that
  managed to fill the table.
- **Cost:** a file on the volume read back holding a file from `/tmp`. Caught
  the same hour it was written and never pushed.
- **Status:** fixed.

The cache is keyed on what a filesystem calls a file. Each one numbers its own
files from its own space and has every right to: **ramfs answers with a slot**,
so small integers from one, and **the volume answers with a dossier**, which
also starts small.

Keyed on that number alone, ramfs slot 10 and volume dossier 10 are the same
entry.

**It needed the table to be full to appear at all.** Until eviction existed,
nothing ever created enough entries to reach a colliding number -- so the first
test that flooded the cache is the thing that found it, and it found it
immediately:

```
  one copy, from the volume : pass
  pagecache: the cached page is not what was written
  a file can be forgotten : FAIL
```

The volume file had been reading correctly all boot. It started reading a
`/tmp` file the moment the flood reached slot 10.

- **Was:** `find(id, offset)`, and `pagecache_forget(id)` with it -- which would
  also have dropped another filesystem's pages that happened to share a number.
- **Now:** the key is `(filesystem, id, offset)`, where the filesystem is the
  `file_ops` pointer: one static table per filesystem, for the life of the
  machine.
- **The file warns about this in its own header**, using the block cache's
  partition-and-disk as the example -- *a partition and its disk are the same
  sectors under two names* -- and the warning was written before the mistake was
  made. Knowing the shape of a fault is not the same as noticing you have just
  built one.
- **Why the earlier tests could not see it:** every one of them used a single
  filesystem. Two mappings of one file, a rewrite, a shared write -- all of them
  correct, all of them blind to a key that is only wrong when two filesystems
  are in the table at once. The test that found it was not written to look for
  it.

### KF-194 -- virtio-net used a descriptor index as if it were a ring slot, in two different ways

[#412](https://github.com/neogentrics/ReconOS/issues/412)

- **Found:** 12 September 2026, by reading the driver back before trusting it.
- **Cost:** a corrupted descriptor chain on receive, and a double free on
  transmit. Neither had fired yet, because both need the ring under load.
- **Status:** fixed before the driver was ever run.

Two sites, one mistake: **borrowing a field that already has an owner.**

```c
n->rx.desc[head].next = (u16)slot;      /* remember which slot this is */
```

`next` is what chains the head descriptor to the second one. A virtio-net
buffer is submitted as a chain of two -- the twelve-byte virtio header, then the
frame -- so `next` is *in use*, and it is the device that reads it. Writing a
slot number there points the device at whatever descriptor happens to have that
index.

The transmit side had the same fault with different arithmetic:

```c
n->tx_bufs[head % TX_RING] = b;
```

The queue holds up to 64 descriptors and the shadow table 32, so descriptors 0
and 32 name one entry. Two frames in flight would share it: the second
completion would free a buffer that had already been freed, and the first
buffer would leak.

**Fixed by keeping a map that belongs to this driver** -- `rx_head_slot[]` and
`tx_head_hdr[]`, both sized by the queue rather than by the ring, because a
descriptor index is not a slot index and the two only coincide while the ring is
empty.

### KF-195 -- a broadcast could not leave a card with no address, which makes DHCP impossible

[#413](https://github.com/neogentrics/ReconOS/issues/413)

- **Found:** 12 September 2026, the first time the stack was pointed at a real
  network rather than at its own tests.
- **Cost:** no machine could ever obtain an address. The stack was complete and
  unusable.
- **Status:** fixed.

```c
for (i = 0; i < device_count; i++) {
        struct net_device *d = &devices[i];

        if (!d->up || !d->ip)
                continue;
```

Right for ordinary traffic: a card with no address cannot be a packet's source.
**Wrong for the one protocol whose entire job is to run before there is an
address to be the source of.** DHCP sends from 0.0.0.0 to 255.255.255.255 by
necessity, and this refused to route it.

It looked like this:

```
  dhcp         : 1 sent, 0 answers, 0 not for us
  udp          : 1 sent, 1 failed, 1 received
    tx         : 0 packets, 0 bytes, 0 dropped, 0 errors
```

The card transmitted nothing at all. A broadcast is now routed out of the first
card that is up, addressed or not, and that is the only case where an
unaddressed card may send.

### Why no self-test could have caught it

**Every test in the stack configures its device by hand before using it**, which
is the only way to test routing without a network -- and it is exactly the
condition under which this bug cannot occur. The test sets `dev->ip`, so the
device is never in the state that fails.

That is the same shape as KF-193 (a guard whose condition was unreachable) and
KF-187 (a test that passed by being unable to do the thing it tested): not a
wrong answer, but a correct one to a question the real path never asks.

### KF-197 -- the block self-test writes to any disk it thinks is blank, and a disk whose table it could not read looks blank

[#414](https://github.com/neogentrics/ReconOS/issues/414)

- **Found:** 12 September 2026, by working out what would happen if a real USB
  stick were attached at boot. Nothing failed; the fault was reasoned to before
  it could cost anybody a disk.
- **Cost:** 256 KB written to the end of somebody's disk, silently, during the
  boot self-tests. Whether it happens depends on whether this kernel can read
  that disk's partition table.
- **Status:** fixed.

`block_self_test` writes to whatever `pick_test_device` reports as writable, and
that function's test is:

```c
if (!d->parent && !d->slice_count && !whole_blank)
        whole_blank = d;
```

"No parent and no partitions" is meant to mean *a blank disk*. **A disk whose
partition table this kernel could not read also has no partitions**, because
none could be worked out -- so it is indistinguishable from a blank one, and the
difference is somebody's data.

### The kernel already knew the difference

`BLOCK_SCHEME_UNREADABLE` is a distinct value from `BLOCK_SCHEME_NONE`, and it
is set deliberately: a disk carrying a protective MBR whose GPT will not parse
is *unreadable*, not an MBR disk, and `partition.c` says so in a comment. The
installer consults it and refuses:

```c
if (disk->scheme == BLOCK_SCHEME_UNREADABLE) {
        out->verdict = INSTALL_NO_TABLE_READABLE;
        return out->verdict;
}
```

**One caller made the distinction and the other did not.** That is this
project's most repeated shape, and the third instance this week after KF-193
(a guard whose condition was unreachable) and KF-195 (routing that was right for
every packet except the one that has to work before there is an address).

The fix is one condition in `pick_test_device`, refusing UNREADABLE the way the
installer already does.

### What it does not fix

A genuinely blank disk is still written to by the self-test, on purpose. That is
the test doing what it exists to do. The rule this restores is narrower and is
the one that matters: **a disk this kernel cannot read is not a disk it may
write to.**

### KF-199 -- USB hot-plug notices the first arrival and then stops for ever

[#415](https://github.com/neogentrics/ReconOS/issues/415)

- **Found:** 12 September 2026, on the real 16 GB stick, by testing the
  departure half after the arrival half had been proved.
- **Cost:** a device pulled out is never noticed, and neither is any later
  arrival. The block device stays registered, pointing at hardware that is no
  longer there. **Every other deferred work item on the machine stops too.**
- **Status:** **fixed.** And it was two things, only one of which was a bug.

Arrival works and is proven on hardware. A machine booted with an empty xHCI
controller, then given the real stick through QEMU's monitor:

```
xhci : 16 slots, 8 ports, 0 connected, 0 addressed
usb0 : 13fe:6400 on port 1, class 8.6 protocol 80, 512-byte packets
usb0 : bulk in 81 (1024 byte), bulk out 2 (1024 byte)
usb0 :  USB DISK 3.0, 30277632 blocks of 512 bytes
```

30,277,632 blocks is exactly what Linux reports for the same stick.

**Removal is never noticed.** `device_del` succeeds -- QEMU's own `info usb`
shows the bus empty afterwards -- and no departure line is ever printed.

### What it is not

Not the port failing to report the disconnect, which was the first guess. The
poll never gets far enough to read the port again.

Not `ud->port` being unset, which was the second guess. It is set in
`address_device`, at `core/xhci.c:770`.

### What it is

The poll is a timer that schedules a work item. Instrumented, the timer keeps
firing and the schedule is refused every time, for ever:

```
DIAG poll:  port 1 was 0 now 1, portsc 135683     <- arrival, detected
DIAG port 1: portsc 4611                          <- one more census runs
DIAG timer: work_schedule refused at poll 10      <- and then this, endlessly
```

`work_schedule` refuses when `w->queued` is already true, and `queued` is
cleared at **dequeue**, before the item runs (`core/work.c:103`). So a permanent
refusal means the item is still on the queue and **the worker thread never came
back for it**.

The last thing it completed was a port census, one poll after the claim. So the
worker wedges shortly after a real device is claimed, and since there is one
worker thread, it takes every other deferred work item with it.

### Why the emulated test did not find it

An emulated `usb-storage` device hot-plugs and unplugs correctly, departure
included -- that is how this feature was first tested and it is why it was
believed to work. The real stick is a **SuperSpeed device at 5000 Mb/s** and
takes a different path through claiming.

That is the whole lesson of this entry, and it is the same one KF-127 taught on
this same stick: **a fixture behaves the way the person who wrote the fixture
expected, and real hardware does not have to.** Two tests, one emulated and one
real, disagreed -- and the disagreement is the result.

### What it actually was

**Three theories were wrong before the right one, and each was killed by an
instrument rather than by argument.**

Not the port failing to report the disconnect. Not `ud->port` being unset. Not
the worker thread wedging -- the work function printed a clean exit as its last
line, so it returned.

The measurement that settled it: the timer fired, the work ran, `timer_start`
reported success **every time**, and the chain stopped anyway after 24 polls and
17 schedule refusals. Those 17 refusals are the claim holding the worker for
about eight seconds, and each one had another timer armed behind it.

**Nothing else in this kernel re-arms a timer from inside its own callback.**
The three other users of `timer_start` are one-shots -- a wait deadline, a
signaller, a work timeout. This was the first periodic timer here, and the first
thing to depend on that pattern holding.

So it stops depending on it. The timer callback now only hands off; **the work
arms the next timer when it has finished.** Three things follow and all are
improvements rather than a workaround:

- The timer is only ever armed from thread context, never from inside its own
  callback.
- **At most one timer is outstanding at any moment.** Before, a poll that took
  eight seconds had sixteen firings behind it.
- The interval is measured from the **end** of one poll to the start of the
  next. Half a second *between polls* is what was wanted; half a second between
  their *starts* is what was written, and those differ exactly when a poll is
  slow -- which is exactly when a device has arrived and there is work to do.

Measured after the fix: **three plug and unplug cycles in one boot**, all four
devices arriving and all three departing.

```
block: usb0 is gone
block: usb1 is gone
block: usb2 is gone
```

Before it, hot-plug worked exactly once per boot.

### And the half that was never a bug

Departure still cannot be observed with the **real** stick, and that is not the
kernel's fault. The port register is measured, after the removal, with the poll
now running:

```
p1 = 0x1203    CCS set, PED set      <- still reports a device connected
p2..p8 = 0x02A0                       <- nothing connected
```

QEMU's own `info usb` shows the bus empty. **The xHCI port never reports the
disconnect for a `usb-host` passthrough device**, so there is nothing for the
kernel to notice. The values are fresh rather than stale -- the register changed
from `0x21203` to `0x1203` when the poll acknowledged the change bit -- and the
same build departs an *emulated* device correctly.

**Whether a physical unplug on real hardware clears the bit is untested**, and
this is the honest limit of what this stick can prove: it is attached through
`usbipd` and QEMU, and neither of them is a person pulling a stick out of a
socket.

### KF-198 -- retiring a device let seventeen callers hand out one that is gone

[#416](https://github.com/neogentrics/ReconOS/issues/416)

- **Found:** 12 September 2026, by matrix 27 failing `partitions` on the first
  boot path.
- **Cost:** any code walking the device table could be handed a retired device
  and would use it as though it were there. The self-test caught it; a
  filesystem would have read from a disk nobody could reach.
- **Status:** fixed.

`block_unregister` introduced a state the table had never held: **a slot that is
occupied and absent at once.** Twenty places walk the table with
`block_device_at()`. Three checked `present`. Seventeen did not, and were
correct not to -- until that morning no device could ever be absent.

It surfaced immediately:

```
  PVH, direct kernel load      1 of 54 FAILED
        partitions         : FAIL
```

The retirement self-test registers a device and a partition of its own, retires
them, and leaves them in the table. `partition_self_test` then walks the table
looking for anything with a parent, finds the retired slice first, and asks it
to refuse a read past its end. A retired device refuses *every* read, with a
different error, so the assertion failed.

### The fix is not seventeen checks

Adding `if (!d->present) continue;` to seventeen loops is seventeen chances to
forget the eighteenth, and the eighteenth is whatever gets written next month.

**Enumeration stops being able to produce one instead.** `block_device_at(i)` is
the i-th *present* device and `block_device_count()` is how many of those there
are, so a loop written before retirement existed goes on being correct without
knowing retirement exists.

Absent entries keep their slots, which matters twice: `block_device_by_id` can
go on refusing a retired identity, and **no pointer moves**. Compacting the array
would have been simpler and would have invalidated every `struct block_device *`
held outside `block.c` -- `usb_storage`'s among them -- which is a
use-after-free rather than a bug.

### KF-196 -- "long ago" was written as zero, on a machine whose clock starts at zero

[#417](https://github.com/neogentrics/ReconOS/issues/417)

- **Found:** 12 September 2026, by the ARP test failing on a kernel whose ARP
  code was correct.
- **Cost:** none shipped -- the fault was in the test. Recorded anyway, because
  a test that cannot fail is a test that is not there.
- **Status:** fixed.

The test made a cache entry stale by stamping it with time zero:

```c
e->learned_ns = 0;      /* long ago */
```

An entry is stale when `now - learned_ns` exceeds sixty seconds. **A machine
four seconds into its uptime is four seconds from zero.** The entry was not old,
it was as new as the machine. The lookup correctly handed it back, and the test
correctly reported a failure that was its own.

Fixed by subtracting from *now* rather than reaching for an absolute past:

```c
e->learned_ns = time_monotonic_ns() - ARP_TTL_NS - 1;
```

Correct even when the subtraction underflows, because the comparison is a
difference in unsigned arithmetic and comes out as `TTL + 1` either way. That is
the same property TCP relies on to compare sequence numbers across the wrap at
2^32, used here for the same reason: **there is no "before the beginning" on a
monotonic clock, and arithmetic that wraps consistently does not need one.**

### KF-193 -- virt_to_phys answered for addresses it cannot answer for, so a driver wrote to memory that does not exist and reported success

[#418](https://github.com/neogentrics/ReconOS/issues/418)

- **Found:** 12 September 2026, by ext2 reading a 512-byte superblock into a
  stack array.
- **Cost:** any buffer handed to a device driver from outside the direct map was
  written to a physical address computed from nonsense. On this machine the
  writes vanished; on a machine that decodes that part of the address space they
  would have landed on something. Every layer reported success.
- **Status:** fixed, on both architectures.

```c
paddr_t virt_to_phys(const void *virt)
{
	u64 v = (u64)(uintptr_t)virt;

	if (direct_map_live && v >= DIRECT_MAP_BASE)
		return (paddr_t)(v - DIRECT_MAP_BASE);
	return (paddr_t)v;
}
```

**The test is one-sided.** Physical memory is mapped from `DIRECT_MAP_BASE`
(`0xFFFF800000000000`) upward, and the kernel image runs at `KERNEL_VMA`
(`0xFFFFFFFF80000000`) -- which is *above* it. So a stack address, a pointer
into the kernel image, a device mapping, anything in the higher half at all,
passed the test and was subtracted.

For a stack buffer at `0xFFFFFFFF801DB7F0` that produces `0x7FFF801DB7F0` --
about a hundred and forty terabytes in, which is not memory on any machine this
runs on.

### The guard against this already existed and could never fire

`virtio_blk` has carried the correct comment since it was written:

> The buffer the caller handed us has to be somewhere the *device* can reach,
> which means a physical address, which means it has to be in the direct map.
> **A stack address or anything else would translate to a physical page that has
> nothing to do with the buffer.**

and the check under it:

```c
paddr_t p = virt_to_phys(buf);

if (!p)
	return false;
```

It tests for zero. `virt_to_phys` never returned zero. **The guard was correct,
the comment was correct, and the function they both depended on made the
condition unreachable.**

That is this project's most repeated shape, in its purest form yet: not a wrong
answer, but a check resting on a premise nobody had verified -- the same family
as KF-190 (a map that could not distinguish empty from occupied), KF-188
(recovery's promise enforced by nobody), and KF-186 (counters nothing printed).

### Why it stayed invisible

**Every other caller in the kernel hands drivers pages from the page
allocator**, and a page from the allocator is a direct-map address by
construction. The block self-test, ReconFS, the partition reader, the installer,
USB storage -- all of them. There was no call in the kernel that could expose
this until a filesystem read a 1024-byte superblock into a local array.

What it looked like from above:

```
blk entry : lba 0 count 1 range=0 buf=1     the request is well formed
blk diag  : queued lba 0, serving=0, head=1 it reaches the queue
blk diag  : issued lba 0 count 1 -> 0       the driver reports success
ext2 probe: lba0 st=0 b0=aa b1=aa           the buffer still holds its fill byte
```

### Measured, one variable at a time

The same device, the same LBA, the same count, two buffers:

```
stack buf=ffffffff801db7f0 st=0 b0=aa b1=aa                  nothing transferred
page  buf=ffff8000009f2000 st=0 b0=0 b1=10 b56=53 b57=ef     correct
```

`b1=0x10` is `inodes_count = 4096`; `b56 b57 = 53 ef` is the ext2 magic. The
difference is not alignment -- both are 16-byte aligned -- it is **which region
the pointer is in**.

### Fixed by refusing rather than guessing

The direct map is now bounded at both ends. An address below `DIRECT_MAP_BASE`
is identity-mapped from before the switch and is still its own physical address,
which is the path early boot takes. An address in the higher half that is not in
the direct map has no physical address to give, and zero is returned -- which is
what every caller was already checking for.

**aarch64 had the identical fault with the identical constants**, and was fixed
in the same change rather than left for whatever found it there. It has the same
layout and the same drivers above it.

### And ext2 was wrong too, separately

The kernel's rule is that a buffer handed to a driver comes from the page
allocator. `ext2_mount` used a local array. Both were fixed: the one that let it
happen, and the one that did it.

### KF-192 -- The kernel boots from a disk over BIOS and then cannot see it

[#419](https://github.com/neogentrics/ReconOS/issues/419)

- **Found:** 12 September 2026, by the self-test assertion added for KF-187.
- **Cost:** on a machine with no UEFI, ReconOS starts and has no storage. It
  cannot mount its own volume, read its own programs, or write anything down.
- **Status:** **open.** This is a missing driver, not a fault in existing code.

The BIOS path of `install-then-boot-test.sh` attaches the installed disk as
**IDE**:

```
-drive "file=$W/target.img,format=raw,if=ide"
```

SeaBIOS reads that disk through INT 13h, which is how stage 1 and stage 2 load
and how the kernel gets into memory. Then the kernel looks for storage and finds
none:

```
Storage
  pci          : 6 devices on bus 0
  devices      : none found
```

There are three block drivers -- **virtio**, **NVMe** and **AHCI** -- and no
driver for a legacy IDE/ATA controller. PCI enumeration works; nothing claims
the device.

### Why this matters more than it looks

Checkpoint 16 exists because *a machine with no UEFI at all* is a real machine
somebody owns, and checkpoint 17 is booting on one. **The machines that have no
UEFI are the same machines likely to present their disk as IDE** -- or as SATA
in a legacy/compatibility mode that looks like it. So the configuration this
kernel is least able to read is the one the BIOS bootloader exists to serve.

The boot succeeds, which is what makes it quiet: every assertion about the BIOS
path has been about *reaching the kernel*, and reaching it is not the same as
being able to use the machine afterwards.

### What it is not

Not a regression, and not the harness being unfair. `if=ide` is a reasonable
thing for that test to do -- it is testing a machine with no UEFI, and IDE is
what such a machine has. The harness was right and nothing was reading its
output.

#### What this means for KF-187

KF-187 predicted the second boot would go red from tests that leave files
behind. It did not, and not because the tests are idempotent: **there is no
volume on that boot to write to.** The only second-boot-on-one-disk in the whole
matrix cannot mount the disk. So that half remains unexercised and is recorded
as such rather than assumed closed.

### Where the driver has to live, which is not where the other three do

Measured before writing any of it, because getting this wrong means writing the
driver twice.

`core/` may contain no inline assembly. That is the rule `make check-portable`
enforces, and it is what keeps a third architecture four files rather than a
rewrite. **virtio, NVMe and AHCI all live in `core/` and none of them break it**,
because all three are reached through memory: the controller's registers are
behind a PCI BAR, the kernel maps that BAR, and reads and writes to it are
ordinary loads and stores that compile on any machine.

Legacy IDE is not reached that way. A controller in **compatibility mode** does
not use its BARs at all -- its registers are at fixed I/O ports (`0x1F0`-`0x1F7`
and `0x3F6` for the primary channel, `0x170`-`0x177` and `0x376` for the
secondary), and I/O ports are an x86 instruction, `in` and `out`, with no
equivalent anywhere else. Checked rather than assumed: there is **no port I/O
helper reachable from `core/` at all**, and no file under `core/` calls one.
The two places that do are `arch/x86_64/pci.c` and `arch/x86_64/ps2.c`, both on
the correct side of the line.

So `ide.c` belongs in **`arch/x86_64/`**, and that is the right answer rather
than a concession:

- Compatibility-mode IDE ports are a fact about x86, in the same way that
  reaching PCI configuration space through two I/O ports is.
- The machines this bug is about -- no UEFI, disk presented as IDE -- are
  x86 machines. An aarch64 board does not have an IDE controller to find.
- Putting it in `core/` and reaching for an arch-provided port helper would put
  a *portable* file's correctness at the mercy of something only one
  architecture can answer, which is the shape the rule exists to prevent.

The hook is already there and costs one line: `arch_storage_probe` in
`arch/x86_64/storage.c` walks the bus and offers each device to `xhci_attach`,
`nvme_attach` and `ahci_attach` in turn. `ide_attach` joins that chain and
answers for class `0x01` subclass `0x01`.

> The comment at the top of `arch/x86_64/storage.c` still says *"Only virtio is
> attached so far. NVMe and AHCI are what matter on real hardware and are the
> obvious next drivers"*. Both have been attached for some time and the lines
> calling them are eleven lines below the sentence saying they are not. Noted
> here rather than fixed in passing, because it is the same shape as KF-193: a
> correct comment that quietly stopped being true, sitting directly above the
> code that disproves it.

### KF-191 -- The BIOS loader handed the kernel dirty registers, breaking its own stated invariant

[#420](https://github.com/neogentrics/ReconOS/issues/420)

- **Found:** 12 September 2026. `handoff : rbx arrived holding something`.
- **Cost:** the two boot paths were distinguishable to the kernel, which is
  precisely what the loader's own comment says must not be true.
- **Status:** fixed.

`reconboot` clears every register it does not need before jumping to the kernel,
under a long comment explaining why: a kernel that accidentally reads one works
on the firmware it was written against and fails on the next, and that failure
arrives as a machine that will not boot with no console to say why.

The **BIOS** loader did not. Its final handoff set `RDI`, computed `RAX`, used
`RCX` as scratch, and jumped:

```asm
/* RDI is the first argument in the ABI the kernel was compiled for.
 * The kernel must not be able to tell which loader started it. */
	movl	handoff_addr, %edi
	...
	jmp	*%rax
```

**The comment states the invariant the code does not keep.** `RBX`, `RDX`,
`RSI`, `RBP` and `R8`-`R15` arrived holding whatever stage 2 left in them -- so
a kernel reading one got firmware leftovers under UEFI and loader leftovers
under BIOS, different rubbish from the two paths that are supposed to be
indistinguishable.

Two registers still survive, for the reasons `reconboot` already gives: `RDI`
carries the handoff, and `RAX` holds the address being jumped to, because naming
a target without a register to hold it is not something the instruction offers.
`RSP` is left alone deliberately -- it was set above to a stack clear of stage 2
and the page tables.

### The four of these share one cause

None was found by reading code. All four came out of **adding an assertion to a
boot nothing had been reading** -- the BIOS boot's self-tests, which
`install-then-boot-test.sh` captured in full and grepped only for the kernel
banner and a partition count. Two of them had been failing on every BIOS boot
since the checks were written.

That is the same mechanism as KF-186 the day before (counters found because an
unrelated cache test failed) and BG-099 on the desktop (found because two things
that should have agreed did not, neither being watched on purpose).

### KF-190 -- The processor identity check could not tell an empty map from one processor

[#421](https://github.com/neogentrics/ReconOS/issues/421)

- **Found:** 12 September 2026, on the BIOS boot, by the same assertion.
- **Cost:** `telling them apart : FAIL` on every boot of a machine with no MADT.
- **Status:** fixed.

```
smp: APIC 0x0 belongs to processor 0 but the fast map says 0
```

The two maps are asymmetric, and the asymmetry is documented in the code that
created it. The **reverse** map stores the kernel index *plus one*, so zero is
free to mean "not a processor we know about" and the array needs no
initialisation pass. The **forward** map stores the raw identifier -- and APIC 0
is both a legitimate processor and what an untouched array already contains.

A machine that boots without a MADT never registers anybody. Both arrays stay
zero, and the check read slot 0 as a real processor holding APIC 0, looked it up,
and found nothing pointing back. **It reported the map as broken on a machine
that had no map.**

Fixed with a count of how many have ever been registered. When it is zero the
check says so out loud rather than passing quietly -- a check that prints nothing
when it did not run looks exactly like one that ran and was happy, which is
KF-187 restated.

### KF-189 -- Changing VERSION rebuilt nothing, so the kernel printed the old number

[#422](https://github.com/neogentrics/ReconOS/issues/422)

- **Found:** 12 September 2026, from a boot that reported `ReconOS kernel 0.1.0`
  out of a tree whose Makefile said `0.1.7`.
- **Cost:** the version a running kernel reports can silently disagree with the
  version in the tree. A whole verification run -- 951 self-tests, no failures --
  had already passed against a mislabelled kernel.
- **Status:** fixed.

`VERSION` is handed to the compiler with `-D`, and the object rules were:

```make
$(BUILD)/%.o: %.c
```

**The Makefile was not a prerequisite.** So raising the version marked nothing
dirty, `make` rebuilt nothing, and `main.c` -- which is the file that embeds and
prints it -- kept the number it was last compiled with. The Makefile said one
thing and the binary said another, and neither was obviously wrong.

A *clean* build was always correct. Only incremental builds were wrong, which is
every build anybody actually does. That is the same shape as KF-134 on the
desktop: **the one configuration that ships was the one configuration nothing
ran**, inverted -- here the one configuration nothing ran is the one everybody
uses.

Fixed by making every object depend on the Makefile. The cost is that editing it
rebuilds everything, which is the correct price: almost anything changed in that
file changes how the code is compiled.

### KF-188 -- Recovery wrote to the volume it was inspecting, and the read-only promise was never enforced

[#423](https://github.com/neogentrics/ReconOS/issues/423)

- **Found:** 11 September 2026, by matrix 22. The only failing path in a
  nineteen-path run.
- **Cost:** a recovery boot modified the disk it was asked to examine. On a
  machine that will not start -- the one situation recovery exists for -- that
  is a write to a filesystem somebody is hoping to get their data back from.
- **Status:** fixed.

`scripts/recovery-test.sh` installs onto a disk, boots recovery, damages a
volume, **hashes the disk**, boots recovery again, and requires the hash to be
identical:

```
recovery wrote nothing    CHANGED -- recovery is supposed to look, not touch
1 of 5 failed
```

### What wrote

`pagecache_run()` is called from `main()` **before** `recovery_run()`. The
page-cache invalidation test creates `/rewritten`, reopens it with
`OPEN_REPLACE`, and writes `"second"` -- unconditionally, on every boot,
including a recovery boot.

The test could not do that until the same day. Before `OPEN_REPLACE` existed it
called `rootfs_create_file`, which answered `ERR_EXISTS` on the second boot and
wrote nothing. **The old test passed by being unable to do the thing it claimed
to test.** Making it work made it write.

### The part worth keeping

The write is the symptom. The fault is that **recovery's promise was never
enforced** -- `recovery.c` opens by stating that a recovery environment must not
depend on the thing it repairs, and nothing anywhere stopped a caller changing
the volume. It held for weeks because every caller happened not to write, which
is not the same as being unable to.

That is this project's recurring shape: correct behaviour resting on a premise
nobody was checking. A comment promising one processor until checkpoint 9; a
signature check compiled out; reapers with no callers; counters printed nowhere.

### Why cleaning up after the test would not have fixed it

The obvious repair is to have the test delete what it wrote. It does not work,
and the reason is worth writing down: **ReconFS is copy-on-write.** A file
created and then removed still allocates blocks and still moves the root, so the
disk differs even where the directory tree does not. Nothing but *not writing*
keeps a byte-for-byte promise.

### Fixed at the boundary, not in the callers

`rootfs_init` asks once whether this is a recovery boot, and
`rootfs_create_file`, `rootfs_replace_file` and `rootfs_remove_file` answer
`RECONFS_ERR_READ_ONLY` when it is. One place, because a rule every caller has
to remember is a rule the next caller forgets.

The tests that write ask `rootfs_is_read_only()` and say they are not running,
rather than meeting the refusal and reporting a failure -- the refusal is the
feature. Said out loud rather than skipped in silence, which is
[KF-187](#bg-187).

### It also removed a third copy of a parser

`main.c` and `recovery.c` each carried their own word-matcher for the kernel
command line, under a comment reading *two call sites is not yet a reason to
share one*. That was a fair call at two. The volume needing to know whether this
is a recovery boot made three, so `boot_cmdline_has()` now lives in `boot.c` and
all three use it.

### Measured

```
reports a healthy machine as healthy        both ReconOS volumes sound
the damage tool actually damaged something  block 17: reachable but not allocated
recovery names the damaged volume           DAMAGED -- reachable from the root but not all
and still calls the other volume sound      discriminating, not alarming
recovery wrote nothing                      the disk is byte for byte

5 of 5: found the damage, kept its hands off the disk
```

### KF-187 -- Five self-tests need a volume, every matrix disk is blank, and the boot reports green either way

[#424](https://github.com/neogentrics/ReconOS/issues/424)

- **Found:** 11 September 2026, from a stale disk image left behind by an
  earlier run of `try-disk.sh`.
- **Cost:** none yet, and that is the problem. The page-cache invalidation test
  -- written the same day, and the entire point of the commit before it -- has
  never run in a verification matrix.
- **Status:** open. The assertion is written and syntax-checked; it lands with
  the fix.

`scripts/verify-kernel.sh` gives every boot path sixty-four megabytes of zeroes,
deliberately and with the reason written down:

> A fresh one per run. The block self-test restores every byte it borrows, so
> reusing an image would work -- and a test whose correctness depends on the
> previous run having tidied up is a test that hides the first failure to do so.

That reasoning is right about the block layer and wrong about everything above
it. **A disk of zeroes has no ReconFS volume on it**, so five tests that need one
never run:

| test | what it reports with no volume |
|---|---|
| `files carry a mode` | `no volume on this machine` |
| `files by descriptor` | `vfs: no volume on this machine to open a file on` |
| `a program from a volume` | skipped |
| `a file, mapped` | skipped |
| `a rewrite is noticed` | skipped |

None of them fails. The boot reads green, and the run's total counts them.

**A skipped test and a passing test look identical in a total.** That is KF-186
one level up: there, four counters were incremented and displayed nowhere; here
five tests are displayed and never run. Both are measurements that exist without
being observed, and the second kind is worse, because it looks like evidence.

### Where it does run, and where it is thrown away

`install-then-boot-test.sh` installs onto a blank disk and then boots the result
**twice** -- once under OVMF, once under SeaBIOS -- to prove the installer wrote
both paths. Those are the only two boots anywhere with a ReconFS volume mounted,
and the second is the only place in the entire matrix where **a volume that has
already been written to is mounted again**.

Both boots are captured in full. `$boot` is grepped for `ReconOS kernel` and
`nvme0n1p3`; `$bios_boot` for `ReconOS kernel` and `firmware : BIOS`. Neither is
ever asked whether its self-tests passed. The evidence was being generated and
discarded in the same script.

### Measured

The stale image that surfaced it produced all five at once:

```
  rootfs: could not create the test file (17)
  files carry a mode : FAIL
  vfs: the file existed before it was closed, so the close is not what commits it
  files by descriptor : FAIL
  user: committing the program failed (-6)
  a program from a volume : FAIL
  a file, mapped      : FAIL
  pagecache: the cached page is not what was written
  a rewrite is noticed : FAIL
```

Status 17 is `RECONFS_ERR_EXISTS`. The first boot created those files, the second
found them already there, and nothing put them back. The **same source file**
that catches this for the block layer states the rule:

> Put it back, and only then report. A test that leaves the disk modified is a
> test that can only be run once.

`block_self_test` borrows blocks and restores them. The tests above it do not,
and until now nothing was in a position to notice.

### Two faults, and the fix is only the first

1. **The two volume boots are not asked about their self-tests.** One helper,
   called twice, failing loudly and naming what failed. It also refuses a boot
   that printed *no* self-tests at all -- a kernel that stopped early greps the
   same as a kernel with nothing wrong, which is the failure this whole helper
   exists to make visible.
2. **The volume tests are not idempotent.** They will go red the moment the
   first fault is fixed, which is the point: the assertion has to be watched
   failing before it is worth anything. Making each of them put back what it
   wrote is its own change and does not belong in the same commit as the
   assertion that proves it is needed.

### KF-186 -- The block layer counted every transfer and printed the number nowhere, so an I/O rewrite lost them silently

[#425](https://github.com/neogentrics/ReconOS/issues/425)

- **Found:** 11 September 2026, while restoring a cache invalidation that the
  same rewrite had dropped.
- **Cost:** none to a running machine. The cost was to every future diagnosis:
  the one number that says how much work reached the disk had been unreadable
  since the block layer was written.
- **Status:** fixed.

`reads`, `writes`, `blocks_read` and `blocks_written` were incremented on every
transfer, reset by `block_init`, and **read by nothing**. Not printed in the
storage summary, not exposed through a call, not asserted by a test. Four
file-scope counters, written and never observed.

`-Werror` had no complaint to make, and it was right not to: a static that is
assigned is used as far as the compiler is concerned. The variable is live. The
*statistic* is not.

### How it surfaced

The I/O scheduler replaced the bodies of `block_read` and `block_write`
wholesale rather than editing them, and the new bodies did not carry the
increments across. Three things went with them in the same edit:

| dropped | noticed by |
|---|---|
| `bcache_invalidate(dev, lba, count)` | `blocks kept nearby : FAIL`, same boot |
| `dev->slice_count && !dev->claimed_raw` refusal | nothing |
| the four counters | nothing |

One of three. The cache invalidation had a test watching it and failed loudly
within a minute. The refusal that stops a write to a partitioned disk from
landing in the middle of somebody's filesystem had no test, and neither did the
counters, and both would have stayed gone.

**The counters are the smaller loss and the more instructive one.** A missing
safety check can at least be found by reading the code, and this one was --
because the cache failure sent somebody to `git show HEAD:kernel/core/block.c`
to compare. Nothing sends anybody to compare a number that is never displayed.

### Fixed by printing it

`block_print_traffic()` runs beside the cache summary, late, after the machine
has done its work. It reports transfers and blocks in each direction, and per
device the queue depth the new scheduler saw:

```
Block traffic
  transfers    : 12 read, 4 written, 1 flushed
  blocks       : 137 read, 80 written
  virtio0      : 16 queued, 0 put in order
```

Twelve reads plus four writes is sixteen transfers, and sixteen is what the
queue saw. That cross-check is now visible on every boot, and it is exactly the
line that would have gone to zero.

`0 put in order` is not a fault. On a boot with one caller the queue never holds
two requests -- the first arrival finds the device idle and services itself --
and the scheduler saying so is more useful than a number that implies it did
work it did not do.

### KF-185 — The loader's ELF reader trusted a signature check that the default build does not perform

[#426](https://github.com/neogentrics/ReconOS/issues/426)

- **Found:** 11 September 2026, reading reconboot while the verification run
  held the tree.
- **Cost:** a one-byte change to a file on the EFI system partition faults the
  firmware before the kernel starts. Nothing had ever pointed it at a file that
  was not ours.
- **Status:** fixed.

`load_kernel` checked the image size **once**, against the ELF header, and then
trusted every number in it: the program-header offset, count and entry size; the
file offset and length of each segment; and the difference between a segment's
size in the file and its size in memory.

**The argument for that was written down and was reasonable.** The kernel's own
ELF loader says it plainly, by way of contrast with this one:

> Deliberately not the bootloader's reader, and that is the whole design. The
> bootloader loads our own kernel, whose signature it has already checked, so a
> field it dislikes means the file is corrupt and the machine should stop.

**The premise is what failed.** `verify_kernel` returns `TRUE` outright when
compiled without a key — which is the default, and how every machine in the
verification run boots: *"signature : not checked (this loader was built without
a key)"*. In those builds nothing has been checked when the parser runs, and
"our kernel" means whatever is on the partition.

The caveat about a keyless build *was* recorded. It said such a loader will not
refuse an unsigned kernel. It did not say the parser would trust every field in
one.

### Measured, on the same corrupt file

Setting one segment's `filesz` to `memsz + 1` makes `memsz - filesz` underflow,
and the loader zeroes from that segment onward until it runs out of mapped
memory:

```
!!!! X64 Exception Type - 0E(#PF - Page-Fault)  CPU Apic ID - 00000000 !!!!
!!!! Can't find image information. !!!!
```

With the checks in place, the same file is refused before anything is written:

```
reconboot: reading a kernel segment failed
```

- **Was:** one bounds check, on the header, and none after it.
- **Now:** the program-header table must lie inside the image; `phentsize` must
  be at least a header; each segment's file range must lie inside the image;
  `filesz` may not exceed `memsz`; and a segment's placement may not wrap the
  address space. **Every one is a subtraction**, because `offset + length` wraps
  and then compares as comfortably inside — the same rule `elf.c` states and
  follows.
- **A second corruption was tried first and was not fatal**, which is worth
  recording: pointing `phoff` past the end made the loader read unmapped-adjacent
  memory, find nothing shaped like a `PT_LOAD`, and stop with "finding anything
  to load in the kernel". An out-of-bounds read that happened to be survivable.
  The fault was real either way; only the second input showed it.
- **Family:** a safety property resting on a build option nobody re-checked.
  The same shape as the comment promising one processor "until checkpoint 9",
  which stayed true only until it was not.

### KF-164 — The reaper assertion asked one processor to have already done what another one owed it

[#403](https://github.com/neogentrics/ReconOS/issues/403)

- **Found:** 10 September 2026, by the verification matrix going red on the
  eight-processor path with one self-test failed — on a kernel where nothing
  was wrong.
- **Cost:** a red board on a green tree, and the worse cost behind it: a matrix
  that fails at random is a matrix people stop believing.
- **Status:** fixed.

The scheduler's self-test called `reap()` once and then asserted that no thread
in the ring was still `THREAD_FINISHED`.

`reap` will not free a thread until `off_cpu` says the processor it was running
on has finished with it. **That is KF-159's fix**, and it is what stops a stack
being handed to the page allocator while another processor is still standing on
it -- the fault whose panic reported a link register of `0xacce5501`. The flag
is set by *that* processor, on its way out of the context switch, afterwards.

So a thread can be finished and not yet reapable, and the assertion was asking
one processor to have already done something another one owed it.

On an idle machine the gap is too short to see. Measured at eight processors
with six guests at once: **two runs in twelve failed before the change, three in
twelve on the commit before that, and none in twelve after it.** The rate does
not depend on what the kernel is doing; it depends on how loaded the host is,
which is why it appeared on the day the host was busiest.

- **Was:** a test that could not tell *not yet* from *not ever*. The thing being
  waited for is another processor reaching the end of a context switch, and
  nothing in the assertion gave it the chance.
- **Fixed in** `core/sched.c`: the check reaps and re-checks against a bounded
  deadline of half a second, yielding between passes -- yielding rather than
  spinning, because what it waits for is another processor making progress, and
  spinning is this one refusing to let it.
- **The assertion still means what it meant.** A stack that genuinely leaks is
  never reaped, so it is still there when the deadline passes. Watched to fail:
  with `reap` neutered it reports *a finished thread was still not reaped half a
  second later*.
- **Family:** the fourth this month whose only symptom was timing -- KF-157 (a
  clock at twice its stated rate with every tick-counting test green), KF-158
  (an idle thread halving the machine with twenty-six self-tests passing),
  KF-160 and KF-162 (a harness and a kernel each mistaking *slow* for *did not
  happen*). Every one was invisible to a suite that asks only whether the right
  things happened.

### KF-165 — The process test raced the very threads it was pretending to end

[#404](https://github.com/neogentrics/ReconOS/issues/404)

- **Found:** 10 September 2026, by the verification matrix on the aarch64
  sixteen-processor path. Every smaller machine passed.
- **Cost:** a red board, and a *misleading* one: the failure said the kernel
  ended a process when its first thread finished and reported the wrong exit
  status. The kernel does neither. Neutering the real bookkeeping produces
  **the same two messages**, so the run was accusing the kernel of exactly the
  fault it does not have.
- **Status:** fixed.

`process_self_test` drives the bookkeeping by hand: it calls
`process_thread_ended` itself, once per thread, and checks that the process
ends on the *second* call and not the first, with the status it passed.

The threads it drove were made with `thread_create`, **which makes a thread
runnable the instant it exists**. `quiet_thread` returns immediately, and
`thread_exit` then calls `process_thread_ended` for it with a status of zero.

So the threads were ending themselves, in a race with a test pretending to end
them. At one to eight processors the test won. At sixteen there are fifteen
idle processors waiting to pick up anything runnable, and they did:

```
  process: it ended when its first thread did, not its last
  process: it exited with 7 and reported 0
```

Both are true statements about what the test observed. The process really had
already ended -- because both of its threads really had finished -- and the
status really was the zero `thread_exit` passes rather than the seven the test
passes.

- **Was:** a test that created live threads and then treated them as inert
  props. `thread_create_stopped` exists for exactly this shape and its header
  says so under KF-148: *"putting a thread in the ring first and setting the
  field second is a race against every other processor"*. The field here was
  "whether the test has finished looking at it".
- **Fixed in** `core/process.c`: the threads are created stopped, so they
  cannot run until the bookkeeping has been checked, and **started afterwards**
  so that nothing leaks -- their exits then find a process that has already
  ended and reaped, which `process_thread_ended` correctly ignores.
- **And a counter that proved nothing now proves something.** `test_started`
  was incremented by the threads and read by nobody. The test now waits on it,
  with a bound, which is what says the two threads really ran and really
  finished rather than sitting stopped for ever as a stack nothing will free.
- **Watched to fail**: with the thread count zeroed on the first exit, the test
  reports both original messages again. Six runs at sixteen processors pass
  with the fix and the failing path failed the run without it.
- **Family:** the second in a day, after KF-164, where the matrix was right to
  go red and wrong about why -- and both were instruments rather than machines.
  The difference from KF-157 and KF-158 is worth keeping: those were real
  faults with timing as their only symptom, and these two were correct kernels
  with tests that could not tell *not yet* from *not ever*.

### KF-166 — The signature tests rewrite the tree every other test builds from

[#405](https://github.com/neogentrics/ReconOS/issues/405)

- **Found:** 10 September 2026. The verification run came back with **seven**
  failures across the install, boot-menu and USB-medium sections, and not one
  of them mentioned a signature.
- **Cost:** an hour, and a diagnosis that started in entirely the wrong place.
  The kernel was correct throughout.
- **Status:** fixed.

Both signing tests write a public key into `boot/src/signing_key.h` and rebuild
the bootloader around it. That is not incidental: the key is **compiled into**
the loader on purpose, because a key read from the same volume as the thing it
verifies is not a check at all.

It makes them the only tests here that change what every other test builds
from. The loader they leave behind refuses any kernel that is not signed, and
the install and menu tests do not sign theirs -- so run beside them, those
tests boot a medium whose loader declines to start the kernel on it.

**The symptom is nothing like the cause.** The install fails, the target is
left empty, and the *next* check reports:

```
init :: non DOS media
Cannot initialize '::'
```

which is mtools being asked to read a filesystem nobody ever wrote. Three
tests fail, and none of them says the word signature. The script even carries a
comment about this exact message meaning something else -- a stale constant --
written the last time it appeared for a different reason.

- **Was:** `sub_launch sig` in the same concurrent batch as `e2e`, `menu` and
  `rec`, under a comment reading *"nothing between the two depends on them, and
  the kernel they all use is already built"*. The kernel was indeed already
  built. The **loader** was not, and three of those tests rebuild it.
- **Why it was rare, and then permanent.** As a race it usually resolves the
  other way. But the key is removed by an EXIT trap, and **a SIGKILL does not
  run traps** -- so a run killed rather than stopped leaves the key behind, and
  every run afterwards fails the same three sections until somebody deletes the
  file. That is how it was found: a run was killed with `-9`, and the next one
  failed seven checks.
- **Fixed in** `scripts/verify-kernel.sh`: both signing tests run **first and
  serially**, before the concurrent batch starts. Afterwards the key is removed
  and the loader rebuilt without one -- unconditionally, because a test that
  failed did not run its trap either. Everything launched after that point gets
  the loader it expects.
- **Confirmed by removing the orphaned key and rebuilding the loader**: the
  install test went from `0 of 5` to `5 of 5` with no change to any kernel
  source.
- **Worth generalising.** Every other sub-test writes only into its own
  `mktemp` directory. These two write into the source tree, and the rig had no
  rule that said they must not -- so the parallelism that makes the run fast
  was silently unsafe for exactly two of its thirteen jobs.

### KF-200 — The register's own filer reads the register with literal patterns, and the register has more than one convention

[#427](https://github.com/neogentrics/ReconOS/issues/427)

- **Found:** 12 September 2026, reconciling the kernel's entries against GitHub
  before moving them to the `KF-` prefix. Found by counting both sides, not by
  reading the script.
- **Cost:** twenty-one entries that could never be filed, six fixed bugs
  standing open, and a run that reported **139 entries against a register of
  153** while printing nothing that looked wrong.
- **Status:** fixed.

`scripts/make-issues.py` finds entries by splitting on a literal:

    blocks = re.split(r'\n### (BG-\d+ — )', text)

An em dash. Fourteen entries were typed with `--` instead. They were not
skipped with a warning and they were not reported as unparsed -- they were
**not seen**, and the summary line counted what the splitter had found.

It decides an issue should be closed the same way:

    closed = '**Fixed in**' in e['body']

The register settled on four forms for that field over a hundred and fifty
entries: `**Fixed in**`, `**Fixed in:**`, `**Fixed by**`, and a `**Status:**`
line that says *fixed* or *open* in prose. Only the first was recognised.
**Thirty of the kernel's seventy-two entries had their state invisible to the
tool that publishes it.**

### Why it looked healthy

Both failures are of the same kind and it is the kind this project keeps
finding: **the number was produced by the thing being checked.** The script
counted the entries it had parsed, so its count could not disagree with its
parser. The only way to see it was to count the register a second way -- `grep
-c '^### BG-'` says 153 -- and compare. That is the same lesson as KF-187,
where a boot that skipped five self-tests and a boot that passed them printed
identical totals, and the same as KF-143, where a hang satisfied every one of
the four tests for a pass.

### What was done

- The splitter takes either prefix and any of `—`, `–` or `--`, and
  canonicalises the separator when it builds the title, so one form reaches
  GitHub however the entry was typed.
- Whether an entry is fixed is read by a function rather than a substring:
  a `**Status:**` line wins if present, otherwise `**Fixed in**` or
  `**Fixed by**`, with or without the colon. *Half fixed* reads as open,
  because it is -- KF-127 says exactly that and means it.
- The `AREA` table gained the twenty-two numbers it was missing. An entry with
  no line there is filed with no area label at all, which the table's own
  comment has warned about since the last time it happened.

**Not fixed by normalising the file.** Every entry could have been rewritten to
one convention, and the next one typed by hand would have broken it again. A
document written by a person is the input; the parser is the thing that has to
be tolerant.

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
3. Add its area to the `AREA` table in `scripts/make-issues.py`. A number
   missing from that table gets no area label and says nothing about it --
   `AREA.get` returns `None` quietly -- which is how twenty entries came to be
   filed with only `bug` on them.
4. Run the script, which opens the issue with the same title and body:

   ```
   python scripts/make-issues.py --dry-run
   python scripts/make-issues.py
   ```

   Entries that already have an issue are left alone, so running it again is
   safe. One with a **Fixed in** line is created and then closed.
5. The script writes the issue link under the heading itself. **Do not write
   one by hand.** Two links were once put in as predictions of the number
   GitHub would assign; neither issue existed, and because creation skips any
   entry whose title it already sees, nothing would ever have looked at them
   again. Reference the number in the commit that fixes it.
6. Check the register against GitHub:

   ```
   python scripts/make-issues.py --check
   ```

   Every link must resolve to an issue whose title is that entry's. This is the
   only step that can catch a link that is simply wrong.

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

### BG-178 — A Max-Age longer than a long could hold was added to the clock

- **Found in** v0.4.21. **Found by** the undefined-behaviour sanitizer, on a
  header the fuzzer made: `Max-Age=99999999999999999999`.
- **What it was** `atoll` saturates at `LLONG_MAX` rather than failing, and
  `now + LLONG_MAX` is signed overflow -- undefined, and in practice a time in
  the past, so a cookie a server asked to keep for ever would have been dropped
  immediately.
- **Fixed in** v0.4.21, commit `c90c7cd`. Clamped to four hundred days, which is
  not a number chosen here: RFC 6265bis says a user agent must clamp to it and
  browsers do. A bound that had to exist for the arithmetic turning out to be
  the standard's is the happy version of that. `Expires` is clamped the same
  way, so a server cannot reach past it by writing the other attribute.

### BG-177 — A cookie name longer than the table holds would have been stored under a different name

- **Found in** v0.4.21. **Found by** `-Wformat-truncation` on the optimised
  build, which is the third fault that warning has found in this project.
- **What it was** the name and the value were copied with `snprintf` into
  fixed buffers, so a name of 200 characters became a different 127-character
  name -- one no server ever set, which would be sent back under that name and
  would never replace the cookie it was meant to be.
- **Fixed in** v0.4.21, commit `c90c7cd`. Both lengths are checked before either
  is copied and an oversized one is refused with a sentence, which is this
  project's rule everywhere else. The copy is a `memcpy` of the measured
  length, so the compiler can see what the check established.

### BG-176 — Epoch zero was both a date and "could not read the date"

- **Found in** v0.4.21, before any of it had run against a server. **Found by**
  the suite, on the first run.
- **What it was** the expiry parser returned 0 for a date it could not read,
  and 0 is a real date: `Thu, 01 Jan 1970 00:00:00 GMT` is epoch zero, and it
  is **the commonest deletion header on the web** -- it is what a server sends
  to sign somebody out. So the one header that matters most was read as "no
  expiry", the cookie was kept, and signing out would have left somebody
  signed in.
- **And a second fault in the same parser, found by the same test.** The
  tokeniser treated `-` as part of a token, so `09-Jun-21` came out as one
  piece that is not a day, not a month and not a year -- and the entire second
  of the three date formats servers send was read as no date at all. RFC 6265
  lists `-` among the characters that *separate* pieces.
- **Fixed in** v0.4.21, commit `c90c7cd`. The parser says whether it read a date
  rather than returning one, and the token is letters, digits and colons.
- **Why this is the entry worth reading.** Neither fault has a symptom anybody
  would report. A cookie that outlives a sign-out looks exactly like being
  signed in, and a date format silently ignored looks exactly like a server
  that did not set an expiry. Both were caught by writing the tests against
  headers real servers send, before the code had spoken to one.

### BG-175 — The Help application's change log stopped fourteen versions ago

- **Found in** v0.4.20. **Found by** `git status` after running
  `scripts/make-help.sh`, which produced fourteen new pages -- so the pages in
  the tree were what the change log had said at v0.4.5.
- **What it was** nothing in the code. `assets/help` is generated from
  `docs/CHANGELOG.md` by a script, and the rule was "run it in the same
  commit". A rule of that shape holds until somebody is busy, and it stopped
  holding at `30c1ad4`.

  So the Help application, opened inside a running v0.4.19, described a system
  that ended at v0.4.5. Everything after -- stylesheets, pictures, tabs,
  bookmarks, find, tables, package signing -- was absent, and nothing said so.
- **Why this one is worth a number rather than a commit.** It is the same
  fault the parser's own header cites as the reason a truncated page has to
  announce itself: *"a page cut off at four thousand blocks looks exactly like
  a page that ended"*. A change log that stops looks exactly like a change log
  that has caught up. The system was telling somebody something that had
  stopped being true, in the one place they would go to check.
- **Fixed in** v0.4.20, commit `11f54f7`. Regenerated, and `scripts/check.sh`
  gained a fourth pass: it regenerates into a copy, compares, and **puts the
  tree back exactly as it found it**. The first version of that pass left the
  regenerated files in place as a favour, which would have made it pass on the
  second run -- a check that repairs what it is checking is one a build can
  defeat by running it twice. Confirmed by drifting the change log on purpose
  and watching it fail, and fail again.

### BG-174 — A three-line box was measured against a line it had made three lines tall

- **Found in** v0.4.20, while forms were being built. **Found by** the first
  photograph of a form. The `<textarea>` was drawn straight through the row of
  buttons under it, while the page's own height said the block had ended long
  before.
- **What it was** a line carrying a control is made as tall as the control, so
  the block works out the tallest control first and grows the line to fit. The
  code that then *placed* each control passed it the grown line height instead
  of the height of the text. A textarea asks for three text-lines plus padding,
  so it was measured against a line that already held a textarea: three lines
  of sixty-five pixels rather than three of nineteen, and it came out about
  three times too tall.
- **Why nothing said so.** The height the block reported was correct -- the
  look-ahead had used the right number. Only the drawing was wrong, so the
  page laid out to exactly the right length with a box hanging out of one of
  its blocks. There is no count that moves when this happens.
- **Fixed in** v0.4.20, commit `11f54f7`. The text's line height is kept in its own
  `const` variable and that is what every measurement uses. A control is
  measured against the text, never against the line it is about to change.

### BG-173 — Working a control with the keyboard let go of it

- **Found in** v0.4.20. **Found by** asking the window where its focus was
  after each key -- `ui app` reports it -- rather than by reading the picture.
  The picture showed a ticked box and looked entirely correct.
- **What it was** pressing a control goes through one function whether a
  pointer or the space bar did it, and that function begins by taking the
  caret out of whatever had it. Taking the caret out clears the focus, which
  is right when the caret is being put down somewhere else and wrong when the
  thing being pressed is the thing that has the focus.

  So Space ticked the box and then dropped it, and the next Tab started again
  from the first control on the page. On a form with a checkbox in the middle
  of it, tabbing through went round in a loop.
- **Why the picture could not show it.** A ticked checkbox looks the same
  whether or not it still has the focus ring, at the size a checkbox is drawn.
  The ring is two pixels outside a fourteen-pixel box.
- **Fixed in** v0.4.20, commit `11f54f7`. Working a control focuses it, in one
  line after the toggle. Which is also what a click should do, and now does.

### BG-172 — A tooltip outlived the button it belonged to

- **Found in** v0.4.20. **Found by** a screen capture of a form being sent:
  the browser's confirmation strip closes the moment *Send* is pressed, and
  its tooltip was still sitting there explaining a button that had sent the
  form and gone.
- **What it was** [BG-089](#bg-089) was this fault for whole windows -- one
  opening under a stationary pointer left the tip of the window now behind it
  drawn over the top -- and the fix was `tip_recheck`, called at the four
  moments a *window* appears or disappears. A control disappearing when its
  own window redraws is the same fault one level down, and none of those four
  moments covers it.
- **Fixed in** v0.4.20, commit `11f54f7`. `recon_appwin_refresh` clears and
  rebuilds its hit regions and then tells the shell, through
  `recon_shell_contents_changed`. There rather than at the five places that
  make a control vanish, because "which redraws can strand a tip" is not a
  question anybody will keep answering correctly -- and the check is cheap: it
  compares the tip under the pointer with the one showing and returns when
  they match, which is almost always.

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
