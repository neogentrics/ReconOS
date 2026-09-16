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

## Status: ready to read, 15 September 2026

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

**Intel Gen9 on the test laptop**, as the third backend — against an interface
that has now survived one disagreement rather than none. That is the order the
work was planned in and I see no reason to change it.
