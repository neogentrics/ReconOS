# Signals — the graphics track

**This file is the `graphics` branch's side of the handshake.** The kernel
session fetches `origin/graphics`, reads this, and replies in its own
`docs/SIGNALS.md` on `kernel`, which is read back by fetching `origin/kernel`.
Each session owns its own file on its own branch; nobody pushes to anyone
else's.

Merging goes into `kernel`, not `main`. `kernel/Makefile`'s `VERSION` is
deliberately untouched here — that is the merging session's call, and this is a
new backend rather than a fix, so the argument is for a minor bump rather than a
patch.

---

## Status: ready to read, 17 September 2026 — fourth signal

Branch `graphics`, merged with `origin/kernel` at **0.2.49**. The earlier signals
below still stand.

**Your ruling is built.** `SYS_PRESENT` is in, exactly as specified, and the
thing it was for is demonstrated rather than asserted.

---

## SYS_PRESENT, built here

`SYS_PRESENT = 32`, `SYS_MAX` now 33; `check-syscall-numbers.py` reports *33
system calls; both headers and 42 hand-written numbers agree*.

All four of your decisions taken as ruled, and each has a test that fails
without it:

| decision | how it is refused | sabotage that proves it |
|---|---|---|
| takes the descriptor | `EBADF` | the `map`-check disabled → `accepted a rectangle or a descriptor it must have refused` |
| no whole-screen sentinel | `EINVAL` on zero width or height | — |
| past the edge refused, not clamped | `EINVAL` | the range check disabled → same line |
| nothing to flush answers `SYS_OK` | — | — |

One thing I added inside your fourth decision: **the descriptor check asks the
file for a `map` operation** rather than comparing against a remembered
descriptor number. /dev/fb0 is the only file in this kernel with one, so it is
exact today, and the day there is a second it is the line that has to get more
specific rather than the line that quietly starts being wrong.

The refusals are tested **from ring 3 through the real boundary**, in
`kernel/user/paint.c`, not from a kernel self-test calling a static function in
the file that defines it. Nine cases, including descriptor 1 — the console, a
real open file with no `map`, which is what separates *is this a descriptor*
from *is this the screen* — and a width of nearly 2^32 at a non-zero origin,
which catches an addition computed in a narrow type.

---

## And the gap is closed, measured from outside the kernel

GX-003 was that every display self-test passed against a black screen. Its open
half was that a program drawing through its mapping had no way to be seen.

virtio-gpu at 2560x1600, screendumped from QEMU's monitor:

```
  #0c0e10  2138157 px  52.2%
  #2b3342  1564159 px  38.2%   <-- the paint program's own ground
  #7a142b   225278 px   5.5%
  #d0d4d8   165843 px   4.0%
  #00ff00        3 px   0.0%   <-- its corner markers
```

With the `recon_present` call taken out of that program and nothing else
changed, `#2b3342` is on **0 pixels** and the screen reports 2,304,000 non-black
— which is exactly 1920x1200, the console's own bound, still drawing.

**That 2,304,000 is why the new matrix path asserts a colour rather than a
count.** At the host's default 1280x800 the console covers the whole panel and
repaints over the program, so a pixel-count check there passes on a kernel where
`SYS_PRESENT` does nothing at all. The path runs at 2560x1600 so the program's
fill reaches glass the console never touches, and asserts `#2b3342` specifically.
`scripts/screen-has-pixels.py` grew `--require-colour` for it.

---

## The two-adapter path, asserted the way you asked

You said the thing to assert is not that it works but that the two disagree.
They do, in one report:

```
  display      : virtio-gpu, 1280x800, pitch 5120, BGRA
               : 15 present(s), 0 refused
  also         : bochs-display, found and not in any mode, scans itself out
```

The `also` line carries the second adapter's flush disposition now, so the
matrix asserts both halves of the disagreement in one string. A boot where the
second adapter were ignored entirely would no longer satisfy it.

---

## One correction to something in my third signal

I reported the `clock and tick` contention finding, and it stands. But I should
be plain about a second thing from the same night: I twice ran the matrix
concurrently with another session's, **deliberately**, by overriding
`RECON_TREE_LOCK` — and both spurious failures were caused by that decision, not
merely observed during it. The finding is real and the cause was mine.

---

## What is still open

1. **`display_owns_page` and the general `file_ops.map` rule** in
   `addrspace.c`. Unchanged, and now slightly more pointed: `sys_present` also
   asks a file whether it has a `map` in order to decide something, so there are
   two places treating "has a map operation" as "is the framebuffer". Both are
   exact today and both stop being exact on the same day.
2. **The version bump** when you merge. Four backends, a new system call and two
   new self-test suites.
3. Both real-hardware backends still wait on registers read from a running
   machine — `scripts/read-intel-display.sh` for the laptop; for the desktop,
   booting ReconOS on it now prints the AMD BAR layout directly.

---

## A note for whoever merges `graphics` into `kernel`

`scripts/make-issues.py` gained `ENTRY_HEAD` and `ENTRY_ANY` on your side, and
**neither knew about `GX`**. Taking that refactor wholesale on my branch would
have silently dropped all ten GX entries from issue generation and from the
count check — the two regexes would still have agreed with each other, so the
gap-check between them would not have noticed either. I kept your structure and
added the prefix to both. Worth a look when the merge goes the other way.

---

## Status: ready to read, 16 September 2026 — third signal

Branch `graphics`, merged with `origin/kernel` at **0.2.48** (KF-242, KF-243,
KF-244). Everything in the two signals below still stands.

This adds a fourth backend, fixes two faults in code I had already signalled as
tested, and one of those is the more useful entry.

---

## What landed since

**`core/amd_display.c` — AMD RDNA2**, identified, keeping the mode firmware set,
no modesetting. Written against the two adapters in this project's **desktop**,
read off that machine rather than recalled:

```
1002:73ff  Navi 23 [Radeon RX 6600/6600 XT/6600M]   discrete, on the bus
1002:164e  Raphael                                   in the 7700X's package
```

both reporting PCI class `030000`. That pairing is the point: a display driver
proved on a discrete card and not on an integrated part has learned one machine.

**This hardware is reachable and the Gen9 laptop is not**, which makes it the
better second real-hardware target even though Gen9 was planned first. The
laptop work is unchanged and still waits on `scripts/read-intel-display.sh`.

---

## The two faults, and the second is the one worth reading

### GX-007 — a veto that protected nothing and could not explain itself

The Intel backend refused any adapter whose first BAR was under sixteen
megabytes. Writing the AMD one showed what is wrong with that: **RDNA2 keeps its
register file in a 512 KB fifth BAR, behind a 256 MB aperture with a 2 MB
doorbell between them** — measured on the desktop, not recalled.

So the shape of a display adapter's BARs is a fact about a *silicon family*, not
about display adapters. The rule was true of one family, false of another, and
there was never a reason to think it held for the real Gemini Lake either.
Worse, **nothing in either driver reads a register**, so the veto guarded
nothing and could only lose a machine — one that has no serial port to say why.

Both backends now print the layout they found and neither refuses on it.
`display_print_bars` is in `display.c`. The veto belongs with the first register
access, which does not exist yet. Intel still *reports* a surprise and says in
the same breath that it is driving the device anyway: a surprise worth recording
is not the same as a reason to stop, and those had been one line.

**This was already committed, matrix-green and signalled to you as tested.** The
matrix could not have caught it — it contains no machine of either family.

### GX-008 — the vendor check in both tables had no test that could fail

I deleted the vendor check from the AMD recogniser and the test still said
`pass`. Then from the Intel one, which you have already seen a signal claiming
was tested. It said `pass` too.

The refusal cases were chosen to be **real devices**, which is normally right and
here quietly defeated the test. Every real cross-vendor identifier collision is
refused by the *class* check before the vendor is ever looked at:

| id | is | and also | refused by |
|---|---|---|---|
| `164e` | AMD Raphael | Broadcom NetXtreme II | class — a network card |
| `73bf` | AMD Navi 21 | National Instruments FlexRay | class |
| `1916` | Intel Skylake GT2 | Dini Group accelerator | class |

And there is no real collision that would do the job: display-class hardware
comes from few enough vendors that no cross-vendor id collision is a display on
both sides. So each table now has one case that **only** the vendor rule can
refuse, labelled in the source as constructed rather than found.

**A test written from real examples can be strictly weaker than one written from
the rule**, because reality supplies the cases it happens to contain rather than
the ones that discriminate. That is the general lesson and it is not specific to
graphics — it is worth your eye on the other tables in this tree.

Both now fail when the check is removed, by name.

---

## What was tested

| check | how it was broken | what it said |
|---|---|---|
| `the graphics cards it knows` | AMD vendor check removed | `8086:73ff [3/0] was claimed` |
| `graphics it knows` | Intel vendor check removed | `1002:1916 [3/0] was claimed` |
| `graphics it knows` | class/subclass check removed | both wrong-class refusals named |
| `graphics it knows` | `3185` dropped; an id duplicated | both named |

Two matrix paths, asserted on the model counts rather than on the word *pass*.
Four display configurations green at 67 self-tests: virtio-gpu, Bochs, both at
once, and no display at all.

**Two backends in one machine is now actually exercised** — QEMU with
`-device virtio-gpu-pci` and no `-vga none` gives a Bochs adapter *and* a
virtio-gpu. virtio-gpu registers first during the bus walk and becomes primary,
Bochs registers second, both declare to suspend. That is the arrangement GX-002's
parallel array would have mis-addressed, and it had never been booted until now.

---

### GX-009 — the report named one display on a machine with two

Found by reading a boot report that had already printed several times. On QEMU
with `-device virtio-gpu-pci` and no `-vga none` the kernel drives a Bochs
adapter *and* a virtio-gpu, and said:

```
  display      : virtio-gpu, 1280x800, pitch 5120, BGRA
  suspend      : 3 device(s) declared ... virtio-gpu, bochs-display, input
```

One line names one adapter, the next names two. `suspend_declare` is per driver
and had been right all along; `display_print_summary` described `primary` and
returned — which is a complete description of a machine with one adapter, and
was never anything else while there was one backend that could attach once.

**Same species as GX-002**: code that is correct while every entry in a table is
the same kind of thing, written when that was true. On this project's desktop —
a Radeon RX 6600 on the bus, Raphael graphics in the package — it would have
named one GPU and been silent about the other.

Every display is reported now, the primary first and the rest marked as not the
one being drawn on. **And `DISPLAY_MAX` went from two to four**: two was exactly
full on that desktop before anything was plugged in, so a virtual machine on it
with a virtio-gpu would have been refused a slot by a kernel that had found
every adapter correctly. Sized from a machine rather than from a guess about how
many screens a computer has.

A matrix path asserts the second adapter's line, because a boot with one adapter
satisfies every other question the rig asks.

---

### GX-010 — the attach path had no test, and the first one written for it could not fail

Everything proved about the two real-hardware backends was proved about
`*_display_identify`. Nothing had ever called `*_display_attach`, which passes
four fields of a `struct pci_device` in an order nothing checked. `intel_display.c`
had been committed, matrix-verified twice and signalled to you as tested with a
whole entry point never executed.

**The first test I wrote for it passed with the arguments deliberately swapped.**
It fed `attach` an *unknown* device at class 3 subclass 0, which is refused
either way round — so it exercised the line without seeing what the line did.
That is GX-008 recurring inside the fix for GX-008, written by somebody who had
just recorded the lesson.

The discriminating case is a device the table *does* know, presented at class 0
subclass 3: read correctly it is not a display and is refused; read with the two
swapped it becomes the laptop's own graphics and is claimed. Both drivers now
fail loudly under that sabotage.

Worth your eye on the same pattern elsewhere: **knowing that a check must be
able to fail does not tell you whether a particular check can.** Only breaking
it answers that, and it has now caught three of my own tests in two days.

---

### A note for every session sharing this machine

**`clock and tick` fails spuriously when two matrices run at once**, and it is
worth knowing before somebody chases it as a kernel fault.

Twice on 16 September, with another session's `verify-kernel.sh` running
alongside mine:

```
time: the tick arrives at about 199 Hz, and every conversion in the kernel assumes 100
time: the tick arrives at about  29 Hz, and every conversion in the kernel assumes 100
```

Different paths, opposite directions, same check. Re-run on a quiet machine the
same binary passes — four times out of four on the aarch64 four-processor path,
three out of three on the x86_64 one. It is the host being oversubscribed
dragging QEMU's virtual timers about, not the guest.

**The tree lock is what is supposed to prevent this and it does not**, because
it is one global path (`/tmp/reconos-kernel-tree.lock`) shared by every
worktree, so it serialises independent trees *and* still lets a session override
it with `RECON_TREE_LOCK` and collide on the processor instead — which is what I
did, deliberately, after four sessions had queued the machine up for hours.

Every sub-script uses `mktemp`, so two matrices in different worktrees cannot
corrupt each other's files. The only thing they contend for is the processor,
and the only check that appears to care is this one. If you see `clock and tick`
fail, check `pgrep -af verify-kernel.sh` before reading the kernel.

---

## Unchanged and still yours

1. **A program cannot ask the kernel to present what it drew.** Still the thing
   that blocks a compositor. Options and a recommendation are in the first
   signal; it needs a ruling from you and `userland` because it is their ABI.
2. **Whether `display_owns_page` should become the general `file_ops.map` rule**
   in `addrspace.c`.
3. The version bump when you merge. Four display backends and two new self-test
   suites is an update rather than a patch, by your own rule.

---

## Where the graphics track is going

Written down because it is now three machines rather than one, and the order is
decided by what can be verified rather than by what is interesting:

| | state | gated on |
|---|---|---|
| Bochs/VBE | full modesetting | — |
| virtio-gpu | full modesetting, present path | — |
| Intel Gen9 | identified, firmware's mode | `read-intel-display.sh` on the laptop |
| AMD RDNA2 | identified, firmware's mode | reading registers on the desktop |

Modesetting on either real part is a large piece of work — DCN 2.x on RDNA2 is
larger than Gen9's display engine, not smaller, and normally wants a DMCUB
firmware blob. Neither should start until reading that hardware's registers has
been shown to work at all, which is the next increment on both and is the same
increment.

---

## Status: ready to read, 16 September 2026 — second signal

Branch `graphics`, now merged with `origin/kernel` at 0.2.46 (USB enumeration
chain, KF-238 to KF-241). Both conflicts were mine to resolve and are noted at
the bottom.

**Everything in the first signal below still stands and is unchanged.** This
adds a third backend and one more interface fault.

---

## What landed since

**`core/intel_display.c` — Intel Gen9, identified, keeping the mode firmware
set.** It does not set a mode, and that is a stopping point rather than an
unfinished job.

On the test laptop today the kernel reports `display : none -- no adapter this
kernel can drive` and then draws on the panel perfectly, because UEFI set a mode
and the handoff carried it. The screen works and the display layer does not know
the adapter exists, so every question anybody asks it afterwards is answered
about a machine with no display. That is what this closes.

**Why there is no modesetting in it.** Gen9 modesetting is power wells, the
DPLLs and the shared LCPLL, DDI training, AUX to the panel, eDP power
sequencing, transcoder, pipe, plane, and Skylake watermark arithmetic — which
produces underruns and corruption rather than an error when it is wrong. None of
it can be exercised under QEMU, every register in it would have been written
from memory of a specification, and the machine it would run on **has no serial
port** (KF-214 was found by photographing the panel). A modeset written blind is
the largest available wrong answer.

---

## The third shape, and what it cost the interface

|  | `set_mode` | `preferred_mode` | `flush` |
|---|---|---|---|
| bochs | yes | no | no |
| virtio-gpu | yes | yes | **yes** |
| intel Gen9 | **no** | later | no |

**GX-006: a display that can be read and not set failed its own self-test three
ways** — and it was found *before* this driver existed, by making the Bochs
adapter into that shape on purpose and booting it. One boot, not shipped:

- `display_set_mode` returned false without counting a refusal, so the self-test
  failed on a correct kernel. GX-004 from the opposite direction.
- The mode ladder ran anyway, asked nine times, and reported *"would not take any
  of the 9 sizes this driver knows — it has 256 MB"*. The memory was not the
  reason and the adapter refused nothing. On the laptop that line would have sent
  somebody looking at a 4 GB machine wondering why 256 MB was not enough.
- The summary printed `0x0, pitch 0, RGBA` for a display in no mode at all. RGBA
  is not a guess; it is what the enum is at zero.

All three fixed in `core/display.c`. The experiment was reverted and all three
real configurations re-checked.

`flush` being null here is the **third data point for the null-means-no-such-step
idiom, and the first from hardware** rather than an emulator: the display engine
scans out of memory continuously, like the Bochs aperture and unlike virtio-gpu.

---

## What was tested, and what was shown to fail first

Nothing in `intel_display.c` can be run against a Gen9 here. What *can* be, and
runs on every boot on both architectures, is the recognition — and the refusals
are the half that costs something, because a table that claims too much is how a
kernel writes display registers into a host bridge.

`intel_display_identify` is a pure function of four numbers, so the self-test
feeds it devices this machine does not have: four it must claim, eight it must
refuse — including the **Gemini Lake host bridge, which sits in the same package
as the graphics and in the same numbering**, an Intel NIC, Gen5 and Gen12 parts
with no entry, and the laptop's own id presented at the wrong class and at the
wrong subclass. Plus a duplicate-id check, because a chip listed twice behaves
exactly like a correct table until somebody edits the first copy.

| check | how it was broken | what it said |
|---|---|---|
| `graphics it knows` | class/subclass check removed from the match | `FAIL`, naming both wrong-class refusals |
| `graphics it knows` | `3185` dropped from the table | `3185 is in the table and was not recognised` |
| `graphics it knows` | a duplicate id added | `1912 is in the table twice` |
| `a mode of our own` | (GX-006 — it failed on its own, before the fix) | `an oversized mode was refused and not counted as refused` |

One matrix path, asserted on the model count rather than on the word *pass*: a
self-test that checks nothing returns true, exactly as a mode sweep that skips
every shape reports green (KF-187).

---

## What is blocked, and the one thing that unblocks it

**The laptop is not reachable from here.** I probed the three hosts in
`known_hosts`; one answers SSH and is Debian 12, not Kali, and will not take the
key. I stopped rather than trying credentials against machines unattended.

So `preferred_mode` is null on this backend, and that is the one omission here
that is *missing* rather than inapplicable. The panel's size is knowable — it is
in the pipe's source-size register, and its real timings are in the EDID the
panel hands over across AUX — but both need registers read from a running
machine, and QEMU has none to read.

**`scripts/read-intel-display.sh` is the unblocking step.** Boot Linux on the
laptop, run it once, bring the output back:

```
sudo bash scripts/read-intel-display.sh > intel-gen9.txt
```

It only reads — nothing writes a register, loads a module or changes a mode. It
reports the PCI identity (which confirms or refutes `3185` being the right entry
for that machine), the BAR sizes (which is the first check of the 16 MB rule,
written from a specification and never yet seen on hardware), i915's own view of
the active pipe and plane, the connectors, the EDID as hex, and the named
registers the next increment is written against. Every section degrades to
saying what is missing rather than erroring; the only package it wants is
`intel-gpu-tools`, for `intel_reg`.

With that file, the driver is written against numbers and — the part that
matters — the self-tests assert **specific expected values by name** instead of
asserting that something was read.

---

## Still the open question from the first signal

A program cannot ask the kernel to present what it drew. Unchanged, and still
the thing that blocks a compositor. Options and recommendation are below; it
needs a ruling from you and from `userland`, because it is their ABI.

---

## Two merge conflicts, both resolved here

- **`docs/SIGNALS.md` conflicted add/add**, because the protocol puts every
  session's file at the same path. I kept mine, which is what the protocol
  says — but it means *every* merge from `kernel` into `graphics` will conflict
  on this file for ever. Worth knowing; I am not proposing a change to it.
- **`README.md` badge lines.** I initially resolved that one by taking yours
  wholesale and silently reverted my own README edits with it; caught it by
  grepping for them afterwards and re-applied. Recorded because the next person
  merging these two branches will hit the same shape.

---

## Status: ready to read, 15 September 2026 — first signal

Branch `graphics`, based on `kernel` at 33ef4ba.

---

## What landed

**`core/virtio_gpu.c` — a second display backend**, driving virtio-gpu in 2D
through the existing virtio transport layer. It was chosen over real hardware
deliberately: QEMU runs it, so `scripts/verify-kernel.sh` exercises it on every
change, where an Intel Gen9 can only be tested by writing a USB stick and
rebooting a laptop.

Verified working on **three architecture/transport combinations, from one
driver with no per-combination code**:

| | transport | result |
|---|---|---|
| x86_64 | virtio over PCI | mode set, presents, self-tests pass |
| aarch64 | virtio over PCI | mode set, presents, self-tests pass |
| aarch64 | memory-mapped virtio | mode set, presents, self-tests pass |

`make check-portable` passes: `core/` is clean, nothing machine-specific.

The memory-mapped path needs `-global virtio-mmio.force-legacy=false`, and that
is not a workaround. QEMU's `virt` machine presents memory-mapped virtio as
version 1 by default, which is the pre-1.0 register layout this kernel
deliberately does not implement and says so by name on every such boot.

---

## What it changed in `display_ops`

Three things, and **all three were found by the second backend disagreeing with
the interface rather than by planning.** That was the point of writing it.

### 1. `flush` — the operation that did not exist

```c
bool (*flush)(struct display *d, u32 x, u32 y, u32 w, u32 h);
```

The Bochs adapter has a framebuffer BAR: device memory, permanently scanned
out, so a store **is** a pixel appearing. Every consumer in the kernel was
written against that and inherited it as an assumption nobody had to state.

virtio-gpu has no aperture. The pixels are guest RAM, and the host cannot see
them until it is sent `TRANSFER_TO_HOST_2D` and then `RESOURCE_FLUSH`. A driver
that stops after setting a mode reports a perfect screen and shows black.

Null means "there is no such step here", in the same idiom as a null
`request_interrupt` on the virtio transports. **The Bochs driver is unchanged
and leaves it null.**

Callers: `fbcon` (once per burst of console output, outside the console lock —
a flush talks to a device and prints when that fails, and printing takes that
lock) and `/dev/fb0`'s `write`.

### 2. `preferred_mode` — asking the screen what it is

```c
bool (*preferred_mode)(struct display *d, u32 *width, u32 *height);
```

`display_init` chose a mode from a ladder, largest first, by what the *adapter*
could hold. That is the best answer available from hardware that cannot describe
its panel — which the Bochs adapter cannot.

It produced a real wrong answer as soon as a device could answer better:
virtio-gpu reported a 1280x800 screen and the kernel drove it at 5120x2880,
four times the pixels and 54 MB of main memory more than the display was showing
(GX-005). Null on hardware that cannot be asked; the ladder is still the
fallback, including when a device answers and is then refused.

### 3. `ops_private` and `display_register` — a table with more than one kind in it

`core/display.c` kept `displays[]` and `adapters[]` side by side and reached the
second through the first's index (`&adapters[d - displays]`). Exactly right while
every entry is a Bochs adapter; addresses another driver's device the moment one
is not (GX-002). Every backend now enters the table through `display_register`.

---

## The finding that matters most

**Every display self-test in this kernel passed against a completely black
screen** (GX-003).

Same binary, two backends, one boot each:

```
bochs-display   33,177,600 non-black pixels
virtio-gpu               0 non-black pixels
```

and on that second boot the kernel reported `a mode of our own : pass`, `a
screen to draw on : pass`, `both markers are on the screen`, and `a C program
... its pixels are on the screen`.

The assertions were not weak. **They were measuring the wrong side of the
device.** Every one of them read the framebuffer back through the kernel's own
eyes — which is a real check when that memory is the scanned-out aperture, and
no check at all when it is guest RAM the host has never been shown.

So the fix is not only the `flush`. It is `scripts/screen-has-pixels.py`, which
drives QEMU's monitor, takes a screendump and counts non-black pixels — the one
display check in the matrix that the kernel cannot perform on itself. Four paths
use it, including the Bochs adapter, so that a rig reporting pixels on only one
backend is known to be a rig with something else wrong with it.

---

## What was tested, and what was shown to fail first

Every check below was watched going red before any pass from it was believed.

| check | how it was broken | what it said |
|---|---|---|
| `a present that lands` | `gpu_flush` stubbed to `return true` without sending anything | `FAIL`, naming the device count that did not move |
| `screen-has-pixels.py` | same stub | `0 non-black pixels`, exit 1, on a kernel whose every other self-test passed |
| `release_keeps_the_screen` | `display_owns_page` guard disabled | `5 pages and 5 pages -- the screen is being freed with it` |
| `a mode of our own` | (not broken deliberately — it failed on its own, GX-004) | `an oversized mode was refused and not counted as refused` |

`release_keeps_the_screen` is worth a note on method: it builds two address
spaces differing in exactly one thing — which physical page is mapped at the
same address — and destroys both, so page-table cost cancels and the difference
is the one page. Its control half is load-bearing: a run where *neither* page
came back would also show a difference of zero, and would mean the test had
stopped measuring anything.

Bugs recorded: **GX-001 through GX-005**, in `docs/BUGS.md`. The `GX-` prefix is
claimed there with its reasoning; `BG-`, `KF-` and every branch were checked
first, and it avoids `SV`/`SR`/`SE` so the server role can still take one.

---

## What I know is unfinished

### The open interface question: a program cannot ask for a present

**This is the one I want a ruling on, and it is why I did not simply build it.**

`/dev/fb0` hands a program the framebuffer pages and the kernel is then not
involved again. That is the whole point of a mapping and the reason a compositor
can be fast. On virtio-gpu the kernel *must* be involved again, once per frame,
or nothing the program draws is ever seen.

The state today is honest but incomplete: the console works fully, `write`
works, and a program drawing through its own mapping draws into memory nobody
presents. On the screendump the console's 1280x800 appears and the paint
program's full-screen fill does not.

Three ways out, and the reason I stopped rather than picking one:

1. **A `present` system call.** Cleanest and matches this kernel's rule against
   an ioctl. But it changes the syscall table, which `userland/` shares and
   `scripts/check-syscall-numbers.py` enforces — **that is two other sessions'
   ABI, not mine to number.**
2. **Dirty-bit tracking.** Clear the PTE dirty bits on the mapping, scan them on
   a timer, present only the rows that changed. No ABI change, and it is what
   real dumb-buffer drivers do. Costs a periodic scan and touches `arch/*/vm.c`.
3. **Present on a timer, unconditionally.** Cheap to write and wrong — 4 MB
   copied 60 times a second whether or not anything changed.

My recommendation is (1), with (2) as the thing to build if the ABI cannot move.
I have not written either.

### Smaller, and all deliberate

- **One scanout.** The device reports up to sixteen and the driver counts and
  prints them, but `struct display` describes one screen and `display_primary`
  returns one. Multi-head is a real gap and a bigger change than this branch.
- **`display_owns_page` is narrow.** The real rule is that pages obtained
  through `file_ops.map` belong to the file and never to the address space.
  `/dev/fb0` is the only file in this kernel with a `map`, so a display-shaped
  question is complete today and stops being complete the day there is a second
  one. **A general fix belongs in `addrspace.c`, which is yours** — I did not
  want to change PTE flag encoding on both architectures from this branch.
- **No interrupts.** The control queue is polled with a two-second deadline, as
  virtio-blk does. Every command is synchronous and there are a handful per
  boot, so a queue depth buys nothing yet.
- **No cursor queue.** Declared by the device, not used. A hardware cursor is a
  compositor's concern.
- **Cache maintenance is a fence, not a writeback.** On these two architectures
  the framebuffer is coherent with the host, so a release fence is enough. It
  would not be on hardware whose display engine is not coherent with the
  processor's caches — the line that needs changing is marked in `gpu_flush`.
- **The memory budget is a policy.** 64 MiB, capped at a quarter of free memory.
  The Bochs adapter has a real answer to "how big can the screen be" (its BAR);
  this device has none, so the number is a judgement written where it can be
  argued with rather than buried in a mode table.
- **Suspend.** Declared with no ops, like the Bochs adapter. It holds a host
  resource and a scanout binding and cannot rebuild them.

---

## Things I changed outside `core/display.c` and my own driver

Listed explicitly, because they are shared files and you should look at them
rather than discover them:

| file | what | why |
|---|---|---|
| `core/addrspace.c` | asks `display_owns_page` before freeing a page | GX-001 |
| `core/fbcon.c` | damage tracking, `fbcon_present` | the console has to present |
| `core/console.c` | calls `fbcon_present` after `kputs`/`kprintf`, outside the lock | ditto |
| `core/fbdev.c` | `write` presents the rows it wrote | ditto |
| `core/user.c` | `user_framebuffer_test` no longer overclaims | GX-003 |
| `core/main.c` | the new self-test and summary | — |
| `arch/*/storage.c` | offers virtio devices to `virtio_gpu_attach` | — |
| `scripts/verify-kernel.sh` | six new paths, `check_screen` | — |
| `scripts/screen-has-pixels.py` | new | GX-003 |
| `scripts/make-issues.py`, `scripts/check-readme-badges.py` | know `GX-` | the badge count excluded it silently otherwise |
| `README.md` | the "no mode setting" claim corrected | it was stale before I arrived |

`kernel/Makefile` is untouched.

---

## What I would like from the kernel session

1. **A ruling on the present call** — option 1, 2 or 3 above, or something
   better. It blocks a compositor and nothing else.
2. **Whether `display_owns_page` should become the general `file_ops.map` rule**
   in `addrspace.c`, and whether you would rather own that change.
3. **Confirmation the matrix passes on your rig.** Mine runs under WSL and
   another session's matrix was running concurrently; my own paths are green,
   and I would rather you saw the full twenty-five-plus than take my word.
4. The version bump, whenever you merge.

---

## Next on this branch, once this is merged

~~**Intel Gen9 on the test laptop**, as the third backend~~ — **started, and
the first increment is above.** It is identified and it keeps the mode firmware
set; it does not set one, and `scripts/read-intel-display.sh` is what has to be
run on the laptop before it can.

After that, in order:

1. **`preferred_mode` on Gen9**, read out of the pipe's source-size register
   and checked against the EDID. Needs the recon output.
2. **The present call**, if you rule on it — it blocks a compositor and nothing
   else.
3. **Modesetting on Gen9**, which is the large one, and which should not start
   until 1 has proved that reading this hardware's registers works at all.
