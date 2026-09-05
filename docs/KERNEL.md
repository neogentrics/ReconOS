# The kernel — plan and checkpoints

Phase 2 of the [roadmap](ROADMAP.md), begun early. The code lives in
[kernel/](../kernel/README.md) and builds separately from everything else in
this repository, because it targets a different machine: no libc, no wlroots,
no Linux underneath it.

Starting it now, while the compositor is still the real system, is the point.
The compositor is where the requirements come from — every time it asks Linux
for something, that is a note about what this kernel will one day have to
provide. [KERNEL-WANTS.md](KERNEL-WANTS.md) is that list, written from hitting
the seam rather than from imagining it, and it is the authority on what the
desktop needs. This file is the kernel's side.

## Version

`0.0.11`. The number says what works. What works: it boots four ways across two
architectures, knows which firmware is underneath it, knows what the processor
can do, knows what memory exists, hands pages of it out, runs on page tables it built itself, allocates memory by
the byte, says where and why it faulted instead of resetting, keeps two
clocks, services a hundred interrupts a second, runs threads and takes
execution away from them, and is started by a bootloader we wrote.

## What "finished" means

A machine with no operating system on it, or with Windows or Linux or macOS
already on it, boots from ReconOS media, is told where to install, and comes up
into the ReconOS desktop on its own kernel — on either architecture, under
either firmware, without GRUB, without Linux, and without destroying whatever
was already on the disk unless asked to.

Every checkpoint below is a step on that path. None of them is a refactor.

## The rule about other people's code

ReconOS boots itself. The bootloader is not an implementation detail to be
delegated — it is the first thing that runs, and the project's claim is that it
was built rather than assembled. Open-source components are used where there is
genuinely no alternative, and are named in [THIRD_PARTY.md](../THIRD_PARTY.md)
when they are.

GRUB is gone from the boot path as of checkpoint 4. `reconboot` starts the
kernel on both architectures under real UEFI firmware. The Multiboot2 header
stays in the image — it is forty bytes, it costs nothing, and it means a machine
that already has GRUB can chain-load ReconOS without an install. It is a
courtesy now rather than a dependency.

## Checkpoints

| # | Checkpoint | Status |
|---|---|---|
| 0 | Builds and boots on x86_64 and aarch64, prints its identity over serial | **Done** |
| 1 | Knows what firmware booted it and what memory exists | **Done** |
| 2 | A physical page allocator | **Done** |
| 3 | Reads what this processor can actually do | **Done** |
| 4 | Its own UEFI bootloader, both architectures — GRUB comes out of the tree | **Done** |
| 5 | Its own page tables, a direct map, and large pages where the CPU has them | **Done** |
| 6 | A kernel heap | **Done** |
| 7 | Interrupts and exceptions, and a fault that reports itself instead of resetting the machine | **Done** |
| 8 | A timer, a tick, and time | **Done** |
| 9 | Threads, a scheduler, and preemption | **Done** |
| 9b | Every core in use — waking the other processors | **aarch64 done**, x86_64 open |
| 10 | User mode, the first system call, and the kernel moves to the higher half | **Done** |
| 11 | Block devices — storage the kernel can read and write | |
| 12 | Partition tables — GPT and MBR, and every layout it will meet | |
| 13 | ReconFS — a filesystem of its own | |
| 14 | Reads foreign filesystems well enough to install beside them | |
| 15 | The installer | |
| 16 | Its own BIOS bootloader on x86_64 — a machine with no UEFI at all | |
| 17 | Boots on real hardware: both architectures, both firmwares | |

Eighteen, numbered 0 to 17, is a lot to hold in your head, so they group into four stages: **the
kernel exists** (0–3, done), **the kernel owns the machine** (4–10),
**the kernel owns the disk** (11–15), and **it runs on real iron** (16–17).

### Checkpoints 0 to 3 — done

**0 — two architectures, one portable core.** `arch.h` is six functions long
and `make check-portable` fails the build if machine-specific code appears in
`core/`. That discipline is why a third architecture is four files rather than
a rewrite.

**1 — firmware and memory.** Four boot paths, each actually booted, because
"the code handles UEFI" is a claim and a memory map printed under OVMF is
evidence:

| Path | Firmware reported | Regions |
|---|---|---|
| PVH, QEMU direct | paravirtual (no firmware) | 8 |
| Multiboot2 via GRUB, SeaBIOS | BIOS | 8 |
| Multiboot2 via GRUB, OVMF | UEFI | 18 |
| Image header + device tree, aarch64 | device tree | 5 |

Firmware detection on x86_64 is by *evidence*: no register says "UEFI started
you", but GRUB emits Multiboot2 EFI tags only when EFI is what started it. The
UEFI run reporting eighteen regions with ACPI NVS ranges where BIOS reports
eight is what makes it evidence rather than a constant.

aarch64 needed a real arm64 Image header before any of it worked. Reading `x0`
for a device tree pointer got zero, because a loader handed a bare ELF has no
handoff convention to follow — it clears the registers and tells the kernel
nothing. Diagnosed by printing the pointer rather than theorising about it.

Overlapping regions are resolved, not reported: firmware describes memory in
layers and ownership is subtracted from availability, always in that direction,
because unused free memory is a waste and reused owned memory is corruption.

**2 — a physical page allocator.** A bitmap, one bit per page. A free list
allocates faster but stores its links *inside* the free pages, so a stale
pointer corrupts the allocator itself and surfaces somewhere else entirely.
Based at the lowest usable address rather than at zero — indexing from zero is
invisible on x86 and describes 262,433 pages of nothing on ARM, where RAM
starts at 1GB. Self-test runs at boot on the real machine and passes on all
four paths.

**3 — what the processor can do.** Not which architecture — the firmware
settled that — but what this chip offers within it, because a 2008 Core 2 and a
2024 Zen 5 are both x86_64 and differ in nearly everything that matters for
going fast. The same binary, on two CPUs:

```
-cpu qemu64   sse2 nx                                    1 core   2MB pages
-cpu max      sse2 sse4.2 avx aes-ni rdrand avx2 rdseed  4 cores  1GB pages
              sha-ni smep smap nx 1g-pages
```

Every field is there because something will branch on it. Large pages decide
whether a gigabyte costs one page table entry or two hundred thousand. Hardware
random decides whether the entropy pool has a real source. Physical address
bits bound the page tables. Core count bounds the scheduler.

### Checkpoint 4 — done

**reconboot boots the kernel on both architectures under real UEFI firmware.**
Not compiled — booted, against OVMF and AAVMF, off a FAT image laid out as a
real EFI System Partition.

No gnu-efi: `boot/include/efi.h` declares what the loader calls, written from
the specification, for the same reason the kernel has no libc. Built by clang
rather than gcc, because a UEFI application is a PE binary and clang targets
both architectures' PE from one installation.

The x86_64 run against OVMF reports:

```
framebuffer : 1280x800 at 0x0000000080000000, pitch 5120, BGRA
```

which is the thing UEFI gives back that is hardest to obtain any other way, and
most of the argument for writing the loader before the drivers.

**One kernel binary, three ways in.** The kernel is entered at
`reconboot_entry`, not at its ELF entry point — that belongs to the 32-bit
trampoline Multiboot2 and PVH arrive through, and UEFI has already put the
machine in 64-bit mode. The image carries a header naming its 64-bit entry and
the loader scans for it. That is what makes "BIOS, UEFI, or both" a build with
one output rather than three.

**Two bugs, both found by running it, both worth generalising.**

`BOOT_MAX_REGIONS` was 64, with a comment reasoning that "real machines report
well under thirty regions" — true of every machine tested at the time, and
wrong the first time our own loader ran. OVMF reported 86, the kernel silently
dropped 68, and it claimed 487MB usable where the truth was 505MB. It was caught
by a warning added defensively that had never been expected to fire. *Firmware
fragments its memory map as it allocates, so the region count reflects how much
work the firmware did before handing over, not how much memory the machine has.*
A number derived from observing three machines is a guess wearing evidence's
clothes.

The map also printed the same range twice under two names — loader memory and
kernel image, both true, with nothing saying which was operative. The carve-out
only subtracted non-usable from usable. It now has an explicit priority, so the
more specific claim wins: kernel, then bad, then ACPI, then reserved, then
bootloader, then usable.

**A limitation stated rather than discovered.** On aarch64 with virtio-gpu the
firmware offers a Blt-only Graphics Output Protocol: there is no linear
framebuffer and the base address comes back as zero. The loader reports the
layout as not understood and the kernel reports no framebuffer, rather than
handing anything a pointer to address zero. A framebuffer is therefore available
on x86_64 UEFI today; on ARM it needs firmware offering a linear mode, or our
own display driver.

### Why the bootloader was checkpoint 4 and not checkpoint 17

It could reasonably have gone last. It went fourth because of what UEFI gives
back: the memory map, a framebuffer at a known address, and file access on the
boot volume, all before the kernel exists. Each of those is otherwise a driver
the kernel has to write, and two of them cannot be obtained on ARM at all
without firmware help. Writing it early also meant the handoff protocol was
designed by us rather than inherited from GRUB.

### Checkpoint 5 — done

**The kernel stops running on borrowed page tables.** Until now it ran on the
trampoline's identity map, or the firmware's — both map more than they should
and neither can be changed.

Verified on all eight boot paths: PVH, GRUB on BIOS, GRUB on UEFI and reconboot
on UEFI for x86_64; device tree and reconboot on UEFI for aarch64; plus both
architectures with a CPU that has gigabyte pages.

**Large pages, and the evidence they are actually used.** The same binary on
two CPUs, mapping the same 2GB machine:

| CPU | Pages used | Page tables cost |
|---|---|---|
| `-cpu qemu64` (no 1GB pages) | 6,398 × 2MB, 1,036 × 4KB | 23 pages, 92KB |
| `-cpu max` (1GB pages) | **12 × 1GB**, 1,022 × 2MB, 1,036 × 4KB | **12 pages, 48KB** |

Also a direct map of all physical memory at a fixed offset, so allocating a page
and writing to it no longer needs a page table edit; and pages that cannot be
executed, on every CPU that has the bit.

**A span is not a size, again.** The first version mapped one range from zero
to the highest address in the memory map. This machine's map ends with twelve
gigabytes of reserved space at the 1TB mark, so that was 524,288 entries and
4MB of page tables to describe a hole. Region by region it is 29 pages. The same
mistake the page bitmap made at checkpoint 2, wearing different clothes — and
worth noticing that it was made twice, by the same person, three days apart.

**Two bugs, and both are about the moment the map changes.**

*The root table pointer stopped being valid at the instant it was installed.*
Every pointer to a page table was obtained through the map we were handed;
afterwards only the kernel image is identity mapped, and the root table is not
in the kernel image. It presented as a page fault at exactly the root table
address plus the index being looked up — `CR2=0x5800` for a root at `0x5000`
and index 256 — which is a satisfying thing to be able to read off a fault.
Same class as the allocator's bitmap, one level up.

*Supported is not enabled.* Bit 63 of a page table entry is the no-execute bit
only once `EFER.NXE` says so. Until then it is a **reserved** bit, and setting a
reserved bit does not mean "executable" — it means every access through that
entry faults, whatever the entry otherwise says. The two UEFI paths worked and
the two using our own trampoline did not, because firmware enables NXE for
itself and our trampoline never had a reason to. CPUID reports the feature is
*supported*; it does not report that it is *enabled*, and code that reads the
first as the second works only on machines that happened to enable it already.

**The higher half was deferred to checkpoint 10, deliberately, and done
there.** It was in this checkpoint's title and it was not done here. The
paragraphs below are the reasoning at the time, kept because the reasoning was
the point.

The reason to move it is to leave the low half of the address space free for
user processes — and there are no user processes until checkpoint 10, so today
it would buy tidiness and nothing else. The direct map, which is the part that
other code will build on, exists now. Moving the kernel image itself is a linker
script change plus one jump, and doing it beside the work that first needs it
means it can be tested by something that actually cares. Recorded here rather
than quietly dropped, because a checkpoint that shrinks to fit what got done is
not a checkpoint.

### Checkpoint 6 — done

**A kernel heap.** The page allocator hands out 4KB at a time, which is the
right unit for page tables and the wrong one for a forty-byte structure.

Small allocations come from *slabs*: one page, carved into objects of a single
size, with a header at the front of the page and a free list threaded through
the free objects themselves. Two things fall out of that shape:

- **No per-object header.** A sixteen-byte allocation costs sixteen bytes. The
  usual alternative — a size word before every object — costs fifty percent on
  the smallest class, and this project's whole argument is about not spending
  memory it does not have to.
- **Freeing needs no search.** The slab a pointer belongs to is the page it sits
  in, found by masking off the low twelve bits.

Large allocations come straight from the page allocator and are page-aligned as
a result — and *that* is what tells the two apart at `kfree()`, unambiguously
and without reading anything that might not be a header: **a slab object can
never be page-aligned, because the slab header occupies the start of the page.**

**Empty slabs go back.** A slab whose last object is freed is returned to the
page allocator rather than kept for next time. Without that, a burst of
allocations that is then freed holds every page it touched forever — which is
precisely the shape of "the machine gets slower the longer it runs" that this
project exists not to have. The self-test checks for it: it counts slabs before
and after, not just bytes.

**The portability check earned its keep again, and was right this time.** It
flagged `0x52534C41` in `core/heap.c` — a magic number, not a hardware address,
so a false positive. But long hex literals in portable code are exactly what a
hardware address looks like, and weakening the check to admit a magic number
would also admit the next real address. The magic is spelled out from its
characters instead, which reads better anyway. *When a guard fires wrongly, the
question is whether the guard or the code is easier to make right.*

**Not built, deliberately:** no `realloc`, because nothing wants one and a
realloc written before its first caller gets the semantics wrong. No locking:
one CPU runs kernel code until checkpoint 9, and a lock invented before the
concurrency it guards is a lock in the wrong place.

### Checkpoint 7 — done

**A fault now says what happened.** Before this, a mistake in kernel code
triple-faulted and the machine reset. The only reason the previous three bugs
were findable is that they happened inside an emulator that keeps its own log of
every exception; real hardware keeps no such log.

A genuine bug, injected into a throwaway build to see what it produces:

```
=== ReconOS kernel fault ===
  exception    : 14, page fault
  error code   : 2
  at           : 0x0000000000101ca0
  touched      : 0x00000deadbeef000
  what happened: page not present, on a write, in kernel mode
  rax 0x00000deadbeef000  rbx 0x00000000001042b1
  ...
```

and on aarch64, where the hardware says more and the decoding is worth the
space:

```
  class        : data abort
  touched      : 0x00000deadbeef000
  what happened: translation fault, level 0, on a write
```

*Level 0* means the walk failed at the very first table — nothing is mapped
anywhere near it. Level 3 would mean the tables exist all the way down and only
the last entry is missing. That distinction is the difference between "wild
pointer" and "off-by-one in the mapping code", and it is free.

**The GDT had to become ours too, and that is not obvious.** On the boot paths
through our own trampoline it is already in the kernel image. On the UEFI path
it is the *firmware's* — sitting in memory the firmware marked as boot-services
data, which we correctly treat as free, and which the allocator will hand out
and something will write over. Nothing breaks immediately. It breaks at the
next interrupt, when the CPU reloads a segment descriptor from memory that now
belongs to somebody else.

**Three bugs, and the third is the interesting one.**

*The aarch64 exception frame was one slot short.* ESR landed on top of SPSR, so
the handler restored the processor state from the fault syndrome and `ERET`
returned into nonsense. It presented as a *second* fault after the first was
handled correctly — a long way from the cause.

*A syndrome's "valid" bit covered less than assumed.* Bit 24 of a data abort's
syndrome says whether the *other* syndrome fields are valid; the direction bit
is valid on its own. Gating the direction on bit 24 made every report say the
direction was unknown while the register sat there holding it.

*And the one worth generalising: you cannot resume execution somewhere else in
C.* The self-test was written portably, taking a label's address with `&&label`
and having the handler set the program counter to it. The compiler deleted the
label's block as unreachable — taking a label's address does not make its block
reachable — and left the recovery address pointing at the function prologue. So
every recovery re-ran the setup, re-armed the handler, and faulted again,
forever, at full speed, printing nothing.

The disassembly is what settled it: `resumed:` was not merely in the wrong
place, it was *absent*, and `trap_recovery` held the address of the instruction
that loads a variable in the prologue. Reading the generated code took two
minutes after an hour of reasoning about what the optimiser might have done.

The fix is that provoking a fault and resuming from it lives in `arch/`, with
the resume label **inside the same assembly block as the faulting
instruction** — where nothing can move it, delete it, or come between them.
Which is the honest conclusion: "continue at this other address" is not
something C can express, and code that pretends otherwise depends on the
optimiser's mood.

A runaway is now a failed test rather than a hang: the handler counts its
catches, and more than one per expected fault is reported. An infinite loop at
full speed with no output is the single least informative failure a kernel can
have.

### Checkpoint 8 — done

**Two clocks and a tick**, on both architectures, on all eight boot paths.

The date is read from real hardware and is correct: the x86 run reports
`1788588634 seconds since 1970`, which is today. That number came out of a CMOS
chip through two 1981-era I/O ports, in binary-coded decimal, with a
two-digit year.

| | x86_64 | aarch64 |
|---|---|---|
| Counter | Time stamp counter, calibrated against the PIT | Generic timer, frequency read from `CNTFRQ_EL0` |
| Tick | 8259 controller remapped, 8254 timer at 100Hz | GICv2 configured, EL1 physical timer |
| Date | CMOS real-time clock | PL031 |

**Two clocks, not one, and they are two functions with two names.** Monotonic
never goes backwards and has no relationship to the calendar; wall clock is what
a person reads and it jumps. Anything that measures an interval with the second
one will eventually measure a negative one. The desktop already needs both —
its clock shows wall time and its network probe times out on a duration.

**ARM is kinder in one respect and harsher in another**, and the contrast is the
useful part. Kinder: the generic timer is architectural, runs at a fixed
frequency the CPU will tell you, and needs no calibration — x86's entire
calibration dance is one register read. Harsher: an interrupt does not arrive
because a device raised it, it arrives because the *interrupt controller* was
configured to route it, so the tick needs a GIC driver before it needs a timer.

**The first thing the fault reporter paid for.** The GIC was configured before
it was mapped, and the kernel took a translation fault. Checkpoint 7 printed:

```
  class        : data abort
  touched      : 0x0000000008010004
  what happened: translation fault, level 2, on a write
```

`0x08010004` is the GIC's priority mask register. Two minutes, no theorising,
no emulator log. That is exactly what checkpoint 7 was for, and it is the first
time it has been used in anger rather than tested.

**Overflow, written down because it is the classic one.** Converting a counter
to nanoseconds as `elapsed * 1000000000 / hz` overflows after about eighteen
seconds at 62.5MHz — which is long enough for everything to look fine during
testing and to fail on a machine that has been up a minute. Whole units first,
then the remainder scaled, so no intermediate can overflow however long the
machine has run.

**The tick is honest and not yet efficient.** A hundred wakeups a second on an
idle machine is a hundred wakeups a second doing nothing, and that is how a
laptop runs warm with nothing running. Stopping the tick when there is nothing
to wake for is a real thing to build and it belongs with the scheduler, because
"nothing to wake for" is a question only a scheduler can answer. The scheduler
now exists and the tick is still unconditional -- so this has moved from "not
yet possible" to "not yet done", which is a different and more accountable kind
of outstanding.

### Checkpoint 9b — aarch64 done

**Eight processors, all running, all being preempted.** Verified at two, four
and eight, on the device-tree path and through our own bootloader under UEFI:

```
Processors
  found        : 4, 4 online
Scheduler
  switches     : 103, of which 18 were preemptions
  thread 0     : boot,   running, 2 ticks
  thread 1     : idle-1, running, 19 ticks
  thread 2     : idle-2, running, 19 ticks
  thread 3     : idle-3, running, 20 ticks
```

**x86_64 is honestly not done** and reports one processor rather than pretending.
It needs three things ARM did not: reading the processor list out of ACPI, a
real-mode trampoline placed below one megabyte (because the startup message
carries a page number in a single byte), and the hand-timed INIT/SIPI sequence.
None of it is hard so much as long. PSCI, by contrast, is one call into
firmware.

**Three bugs, and each is the same shape wearing different clothes: a thing that
looked global because there had only ever been one processor to have one.**

*The stack was a direct-map address.* A processor started by PSCI begins with
its MMU off, so the address the allocator hands back means nothing to it —
setting the stack pointer to one and pushing faults instantly. It gets the
physical address instead. But that stops meaning anything the moment the
processor turns its own MMU on, because the kernel identity-maps only its image,
so the stack is identity mapped as well and the same pointer is right on both
sides of the switch.

*One shared word for the stack pointer was a race* — the boot processor writes
the next one's while the previous may not have read its own. One slot each.

*And the one that took longest: `VBAR_EL1` is per-processor.* Three secondaries
came online, reported healthy, enabled interrupts and armed their timers — and
took not one tick between them, because their exception vector base was still
whatever the firmware left. Every interrupt vanished into firmware code with no
idea what this kernel is. **Nothing failed. They simply never came back.**

**And the console had to be serialised, which four processors demonstrated
immediately:**

```
[[cpu 3:cp online, iu d says 3,2: ir oqs online, n]
```

Three messages plaited together. Funny once, and then it is the console you have
to read a fault report on.

Serialising it produced a *second* bug of its own, and a better one.
`kprintf` took the lock and then called `kputs` to print the `0x` before a
pointer — and `kputs` took the lock again. **A spinlock taken twice by the same
processor is a processor waiting for itself**, and it presented as the kernel
stopping mid-word on the exact line it was printing.

Panic and the fault reporter now use a path that never locks at all: they run
when something has already gone wrong, possibly while that lock is held by the
code that went wrong, and taking it would turn a report into a hang — which is
the one outcome worse than the fault.

*That sentence was written before it was true.* Panic used the lock-free path;
the fault reporter still called `kprintf`, so a fault taken while the console
lock was held would have hung inside the very code meant to explain it. The fix
splits the formatter into a locked wrapper and an unlocked core — which is
exactly the condition the file's own comment had named for doing it: "when a
second caller wants a `vkprintf`, that is the moment to split it out".

### The locking, which came first

Written and passing before there was a second processor to need it. That order
was deliberate: **a run queue two
processors can edit is a run queue that will eventually contain a cycle, and the
moment to make that safe is before there is a second processor to prove it.**
Retrofitting locks onto a working single-processor kernel means finding every
place that was safe only by accident.

Spinlocks, with two forms rather than a flag: the interrupt-safe one masks
interrupts while held, because a lock taken by ordinary code and also by an
interrupt handler on the same processor deadlocks — the handler spins for a lock
the code it interrupted is holding, and that code cannot continue until the
handler returns.

**The restore is by saved state, not by re-enabling.** `arch_irq_save()` returns
how the interrupt flags were and `arch_irq_restore()` puts exactly that back, so
an inner lock releasing cannot enable interrupts an outer one deliberately
masked. The self-test takes a nested lock specifically to check it, because that
is the case an unconditional enable gets wrong and it only shows up under
nesting.

**A deadlock reports rather than hangs.** Ten million spins is many
milliseconds, which no correct caller reaches, so passing it means something is
wrong — and a kernel that stops with no message is the least informative failure
there is.

**One finding that will bite anything freestanding.** GCC on aarch64 does not
emit atomic instructions directly by default. It emits a *call* to a libgcc
helper that checks at run time whether the CPU has the large-system-extension
atomics and picks a path. That is a good default for a program and useless for a
kernel: the link fails on `__aarch64_swp1_acq` with nothing to say where it came
from. `-mno-outline-atomics` — and checkpoint 3 already detects that CPU feature
for ourselves.

### Pages are cleared on the way out of the allocator, not on the way in

Decided here rather than at checkpoint 10, because deciding it later means
deciding it about a system that has already leaked.

The desktop found a password `memset` that survived only by accident of build
flags (BG-090). Its fix protects the *previous owner's* copy — and that is a
different obligation from the kernel's. **A kernel that hands a freed page to
another process without clearing it has leaked the secret however carefully the
previous owner scrubbed it.**

Clearing on *free* trusts every caller to have called free. Clearing on
*allocate* is unconditional and cannot be forgotten by anybody, including code
that has not been written yet. It costs a page-zeroing on every allocation,
which is real and is the right thing to pay.

### Checkpoint 9 — done, except the other cores

**The kernel stops being one thing doing one thing.** Threads, a round-robin
scheduler, and preemption driven by the tick. On both architectures, on all
eight boot paths:

```
Scheduler
  slice        : 5 ticks (50 ms)
  switches     : 15, of which 8 were preemptions
  thread 0     : boot, running, 3 ticks
```

The context switch is the one function in the kernel that genuinely cannot be
written in C, and it is worth being exact about why: **it is called on one stack
and returns on another.** C's whole model is that a function returns where it
was called from with the stack it was called with. What makes it work is that
the calling convention already names the registers a function must preserve — so
saving exactly those, swapping the stack pointer, and restoring exactly those
from the new stack leaves the CPU in a state the *other* thread's code believes
is its own.

**A self-test that passed without testing anything.** The first version ran
three threads for a fixed count of four million increments each and asserted
that the number of context switches had risen. Every thread finished inside its
own slice, so none was ever preempted — each ran to completion and switched
*voluntarily* on exit. Four switches for three threads: exactly what pure
cooperation produces, reported as a pass for preemption that had not happened
once.

Two separate things were wrong, and fixing either alone would have left a test
that still lied. The threads now run against the clock rather than a count, so
they cannot finish inside a slice however fast the machine is. And preemptions
are counted *apart* from voluntary switches, so the assertion is about the thing
being claimed rather than a number both would move. It now reports 8 preemptions
out of 15 switches, and would fail if that first number were zero.

*The general form: an assertion that both the working and the broken case
satisfy is not an assertion.*

**A reaper that reaped exactly one.** Finished threads' stacks are freed by a
scan, which restarted after each removal using `continue` inside a `do`-`while`
— and `continue` in a do-while jumps to the *condition*, which the restart had
just made false. One thread freed, the rest leaked. Caught only because the
summary printed the ring and two finished threads were still in it. **The
instrument again: nothing failed, a number was just wrong on screen.**

**The ordering hazard, on both architectures.** The interrupt controller is
acknowledged *before* the context switch, never after. A switch does not return
to the interrupt handler — it returns on another thread's stack — so an
acknowledgement placed after it would never happen, the controller would send
nothing further, and the machine would freeze on the first preemption with every
individual part of it looking correct.

**Other cores are not done, and this is checkpoint 9b rather than a footnote.**
Secondary processors are still parked in `boot.S` on both architectures. Waking
them needs a way to start a CPU — a mailbox and PSCI on ARM, an interrupt
sequence on x86 — and locking on everything the scheduler touches, because a run
queue that two processors can edit at once is a run queue that will eventually
contain a cycle. That is real work, it is where BG-088's lesson about ownership
checks will first bite this kernel, and it is listed rather than assumed.

Also absent by choice: priorities (a scheme invented before there is a workload
to shape it around is fitted to nothing) and blocking (waiting for a key or a
disk needs the key or the disk to exist).


### Not a checkpoint, but on the record: virtualization

Asked for, and deliberately not scheduled yet. Recorded here rather than
remembered, which is the whole reason this file exists.

**It is a kernel thing.** Both architectures put it in the processor, and
checkpoint 3 already reports whether this machine has it. On x86_64 it is VMX or
SVM: the kernel enters root mode and a guest runs in non-root mode, exiting into
the kernel on anything it may not do itself. On aarch64 it is EL2, and there is
a catch worth knowing early — a kernel *entered* at EL2 can drop to EL1 and keep
EL2 for itself; a kernel entered at EL1 cannot climb back. ReconOS runs at EL1
and does not currently record which it was entered at. That is free at boot and
unrecoverable afterwards, so it is the one piece worth doing before the rest.

**Why it comes after the disk work rather than before.** A hypervisor needs
nested page tables (an extension of checkpoint 5, and part of why the page table
code is a thing rather than a fixed layout), memory the physical allocator is
not also handing out, virtual devices — which needs a device model, which needs
checkpoint 11's block layer — and somewhere to keep a disk image, which needs a
filesystem. Every one of those is already on the line for its own reasons.
Reaching for virtualization first would mean building all of them badly, in a
hurry, for one caller.

**It does not replace the compatibility layers, and they do not replace it.**
A compatibility layer runs *one program* from another system by translating its
system calls: cheap, fast, and exactly as complete as the translation. A
hypervisor runs *the whole other system*: complete by construction, and costs a
machine's worth of resources to do it. The personality pointer added at
checkpoint 10 is the first half of the first answer. Both are wanted, for the
same reason every desktop that has both wants both.

Tracked as [issue #265](https://github.com/neogentrics/ReconOS/issues/265).

### Checkpoint 10 — user mode, the first system call, and the higher half

**This is the checkpoint the desktop's accounts have been waiting for.**

Everything before it ran at one privilege level. A bug anywhere could write
anywhere, and "this account may not do that" was a decision code made about
itself — which meant any program that simply did not ask was not bound by it.
ReconOS declines to install software for a standard account because its Control
Panel declines to. After this checkpoint the processor declines.

**Entering.** The two architectures are barely comparable here.

On aarch64 a privilege level is a *number in the saved processor state*, and the
same `eret` that returns from an exception is what enters user mode: set SPSR to
say EL0, set ELR to the entry point, set SP_EL0, and return from an exception
that never happened. `arch/aarch64/user.c` is fifty lines and most of them are
comment.

On x86_64 it is descriptors. The GDT grew user code and user data entries, and
their *order* is not free: SYSRET rebuilds both user selectors by adding 8 and
16 to a single base held in one MSR, so user data must follow kernel data and
user code must follow that. A GDT laid out the way a person would find natural —
code, code, data, data — does not work, and fails by loading a plausible wrong
segment rather than by refusing. Entry itself is `iretq` against a hand-built
interrupt frame, because `iretq` can set RFLAGS to anything and there are no
saved flags to restore the first time.

**Returning.** `svc` on ARM lands in vector slot 8, one of the sixteen the
exception table has filled since checkpoint 7 and nothing had used. `syscall` on
x86 needs an entry point of its own, and the interesting thing about it is what
it does *not* do: it does not switch stacks. The processor arrives in the kernel
still standing on the user program's stack, at whatever address the user program
chose. Three instructions — `swapgs`, save, switch — get off it, and the window
before them is why SFMASK clears the interrupt flag on entry.

**The register conventions are Linux's, deliberately.** x8 and x0–x5 on ARM;
RAX and RDI/RSI/RDX/R10/R8/R9 on x86. Matching them costs nothing today and
means a compatibility layer for Linux binaries has one less thing to translate,
which is the whole argument for the next paragraph.

**Personality: the one thing the kernel owes the compatibility layers.** A
system call arrives as a number. What that number *means* is a property of the
program making it — 1 is `write` to a Linux binary and something else entirely
to a Windows one. So the syscall table is a pointer on the process rather than a
global, and a process is created with a personality that selects it. Nothing
needs it yet; there is one table and everything uses it. It is built this way
now because it costs one pointer now and a rewrite later. Linux calls this
`binfmt`; Windows NT called them subsystems.

**Two defences, and both are tested by a program written to attack them.**

The first is code: `user_range_ok` checks every pointer a system call is handed,
for overflow first — a pointer near the top of the address space with a huge
length otherwise produces an end address that wrapped below the start, and every
subsequent range test passes.

The second is the processor: kernel mappings do not carry `VM_USER`, and its
absence is what makes the kernel unreachable. Not a check in any code — a bit
consulted on every access.

So the second test program tries both. It asks the kernel to `write` from the
direct map's base, which is every byte of physical memory laid out conveniently;
that is refused. Then, having been refused, it reads the address itself; that
faults. It is killed, and the kernel is not — which is the first time in this
kernel's life that something can go wrong without the machine stopping.

The test treats the program *exiting normally* as the failure, because exiting
normally is what it does when the first attempt succeeds.

**Two bugs, both older than this checkpoint, both found by the second program.**

`vm_map` never invalidated the TLB when it replaced a live mapping. It could not
have been noticed before: every mapping the kernel had ever made was made once,
at an address nothing had touched, and there was nothing stale to find. The
second user program, mapped at the same virtual address as the first, read and
ran the first program's page — correct page tables, wrong memory. Only
replacements are invalidated now; invalidating unconditionally would mean five
hundred invalidations while building the direct map for no reason.

The aarch64 exception vectors destroyed `x0` before saving it — each of the
sixteen slots began `mov x0, #index`. That was harmless for as long as every
exception was a fault, because nobody wanted the register, only the report. It
stopped being harmless the moment a system call needed its first argument:
`write(1, buf, len)` would have been `write(8, buf, len)`. The frame gained a
slot for the vector number, and `x0` is now stored before anything can clobber
it.

**Verification.** `scripts/verify-kernel.sh` boots the kernel eleven ways and
reads the self-test counts rather than the absence of a crash:

| Path | Self-tests |
|---|---|
| x86_64 PVH, direct kernel load | 10 |
| x86_64 PVH, `-cpu max` | 10 |
| x86_64 Multiboot2 via GRUB, BIOS | 10 |
| x86_64 Multiboot2 via GRUB, UEFI | 10 |
| x86_64 reconboot, UEFI | 10 |
| aarch64 device tree, cortex-a72 | 10 |
| aarch64 device tree, `-cpu max` | 10 |
| aarch64 device tree, 2 / 4 / 8 processors | 10 each |
| aarch64 reconboot, UEFI | 10 |

110 self-tests, no failures. The script itself cost one confusing run to get
right: its patterns were anchored with `$`, and QEMU's serial console ends every
line with a carriage return as well as a newline, so every path reported no
output at all — which reads exactly like a kernel that never booted.

---

#### The higher half

Deferred from checkpoint 5 with a reason, and the reason arrived here: the point
of moving the kernel image out of the low half is to leave that half to user
processes, and there were no user processes until this checkpoint. Now there
are, and a kernel identity-mapped at its load address would put a hole in the
middle of every program's address space, in the same place, that no program may
touch.

**The kernel is loaded low and linked high.** 1MB and 0x40080000 are where the
two architectures are *loaded*, because that is where every loader puts them.
0xFFFFFFFF80000000 is where both now *run*.

The address is the same on both architectures and that is a choice, not a
coincidence. aarch64 does not care -- its code is PC-relative and links
anywhere. x86_64 does: `-mcmodel=kernel` generates references that assume the
top two gigabytes, and it is the only model that produces sensible code for a
high kernel. One number in two linker scripts is easier to hold than two. It
also means the direct map at 0xFFFF800000000000 and the kernel image are two
separate windows rather than one, which costs a mapping and saves an argument.

**Getting there is the whole of the boot code, and it differs completely.**

x86_64 keeps a low region. The trampoline that gets from 32-bit protected mode
to long mode runs before paging exists, so it must be linked where it is loaded,
and so must the descriptor table it loads and the page tables it builds. Those
live in `.boot`; everything after is linked high and loaded low. The map it
builds contains the kernel twice, through two chains to one page directory,
because paging takes effect on the *next instruction fetch* and that fetch has
to still find the trampoline. Then `movabsq` and an indirect jump, which is the
one instruction that moves the kernel to the higher half.

aarch64 keeps nothing low. It starts in the mode the kernel wants, so its boot
code only has to avoid *absolute* addresses until the MMU is on -- and `adrp`
gives the address a symbol has right now rather than the one the linker assigned
it. So the whole image is linked high and `AT()` places every byte of it low.
The rule this creates is worth stating because the failure is silent: `ldr xN,
=symbol` loads a linked address out of a literal pool and may not be used until
the MMU is on. It is the natural thing to write and it is wrong there.

**Three surprises, one per boot path that has firmware.**

*OVMF's page tables are read-only.* The ReconBoot path on x86_64 does not build
an address space; it adds one entry to the one the firmware is already using --
entry 511, which no firmware uses and which is exactly where 0xFFFFFFFF80000000
looks. That keeps the firmware's map of everything else, including wherever it
put the handoff structure. The write faulted with error code 3: present, write.
`CR0.WP` is what makes a read-only page read-only *to ring 0 as well*, and it
has to come off for the length of one store. The error code reads like a bug in
the address, and the address was perfectly correct.

*A page table that is not page-aligned is not a page table.* Moving three
scratch words into the boot region pushed the tables twenty-four bytes along.
The processor ignores the low twelve bits of every pointer to a table, so the
entries were written in one place and read from another, and the kernel simply
never printed anything.

*Every secondary processor was zeroing the tables it was about to share.* The
aarch64 secondaries need the MMU on before they can reach their own stacks,
which are linked addresses. The first version had them run the same routine the
boot processor ran -- build, then install -- and the comment said rebuilding
identical tables was harmless. It is not: between the zeroing and the rebuild
there is a window in which the other processors' translations do not exist, and
one that takes an instruction fetch in that window is gone. It failed about a
quarter of the time on four processors and never on two, which is the
characteristic shape of a race a smaller machine hides. Building is now the boot
processor's job and happens once; installing touches no memory and may be run by
anyone.

**One consequence worth its own paragraph.** On aarch64 the fixed devices --
UART, interrupt controller, real-time clock -- were identity-mapped, and the
low half is no longer there to hold them. They live in the direct map now, and
every driver reaches them through a single variable that is zero while the
kernel runs on the map `boot.S` built and `DIRECT_MAP_BASE` afterwards. It is
assigned in the statement immediately after the tables change, because between
those two statements a single `kprintf` would fault on a device that no longer
exists where the driver believes it is.

**The test that says it worked.** Two lookups rather than one:

```
vm_lookup(KERNEL_VMA + phys) == phys     the kernel is where it is linked
vm_lookup(phys)              == 0        and nowhere else
```

The second is the one that matters. Without it the move would be half done --
the kernel reachable from the top and still sitting in the middle of every
program's address space -- and nothing would say so.


## Using the machine you are on

The project's central claim is that a 4GB machine should not need 16GB to feel
responsive. That is a design commitment, and these are the specific things it
means at kernel level. They are listed here rather than left implicit because
each one is a decision some checkpoint has to actually take.

### Memory

**The kernel never sees DDR generations, and this is worth stating plainly
because it is easy to expect otherwise.** Working out DIMM timings — the thing
that genuinely differs between DDR1 and DDR5 — is memory *training*, and it
happens in firmware before a single kernel instruction runs. By the time the
kernel exists, RAM is a memory map. There is nothing to support and nothing to
get wrong, on any generation.

What does make memory use efficient, on every generation equally:

- **Large pages where the CPU has them** (checkpoint 5). Mapping a gigabyte
  with 4KB pages costs 262,144 page table entries and a TLB miss every time the
  working set moves. With 1GB pages it costs one entry. This is the single
  largest lever, it is free once the CPU has been asked, and checkpoint 3
  already asks.
- **Compressing cold pages when memory is scarce and the CPU is not**
  (after checkpoint 9). This is exactly the trade of spending cycles to avoid
  needing RAM — on a machine with a decent processor and little memory it is
  strictly better than swapping to disk, and on a machine with plenty of RAM it
  never triggers. It is adaptive by construction rather than by a setting.
- **Not keeping what nothing asked for.** The anti-bloat principle applies to
  the kernel first: no caches that exist because caches are traditional, no
  daemons, no background work without a wakeup that justifies it.
- **Reclaiming the bootloader.** Everything marked `MEM_BOOTLOADER` in the
  memory map is memory somebody else was using, and after checkpoint 5 nobody
  is. Already recorded so it can be handed back.

### Processor

**Use what the chip has, not what the oldest chip would have had.** Checkpoint
3 reads the capabilities; the checkpoints that follow spend them:

- **Every core, and every thread on it** (checkpoint 9). A scheduler that
  leaves cores parked is wasting the most expensive thing the machine has.
  Secondary cores are currently parked in `boot.S` on both architectures,
  waiting for an SMP story to wake them into.

  Three distinctions the scheduler has to know about, because treating them as
  interchangeable is how a machine gets slower the more work you give it:

  | | What it means | Why it matters |
  | --- | --- | --- |
  | **Cores** | Independent execution units | Real parallelism |
  | **Threads** (Intel Hyper-Threading, AMD SMT) | Two threads sharing one core's execution resources | Two threads on one core are *not* two cores. Filling both before using an idle core is slower than not |
  | **Core types** (Intel P/E-cores, Apple Silicon performance/efficiency) | Cores of different speed and power in one machine | Putting latency-sensitive work on an efficiency core makes an idle machine feel slow |

  Apple Silicon is aarch64 with heterogeneous cores, so it is the same problem
  as Intel's hybrid parts wearing different clothes; both are read from the
  topology rather than from a table of chip names, because a table of names is
  wrong for every chip released after it was written.
- **Crypto in silicon.** AES-NI and ARM's AES extension turn encryption from a
  loop into an instruction. Disk encryption without it is a reason not to use
  disk encryption.
- **Hardware entropy** for the randomness service, where RDSEED or RNDR exists.
- **The right idle instruction.** `hlt` and `wfi` are already what the idle
  loop uses, which is why idle costs nothing rather than nearly nothing.
- **Balancing, not maximising.** The goal is the least total resource for the
  work, not the most of any one. A subsystem that can run at a lower duty cycle
  and produce the same output should.

## Firmware: BIOS, UEFI, and machines with both

All three are supported, and the third is not a special case.

| Machine | What it offers | What ReconOS boots with |
|---|---|---|
| Modern, UEFI only | UEFI, GPT, Secure Boot | The UEFI loader (checkpoint 4) |
| Old, BIOS only | 16-bit real mode, MBR | The BIOS loader (checkpoint 16) |
| Transitional, both | UEFI with a Compatibility Support Module | The UEFI loader, because it is better |

A machine with both does not require a third loader. It requires that the
install medium carry both, that each be findable by the firmware that looks for
it — an `EFI/BOOT/BOOTX64.EFI` for UEFI and a boot sector for BIOS, on the same
disk — and that the installer write whichever the machine will actually use.
That is a media-layout problem, solved once, at checkpoint 15.

UEFI is built first because one design covers both architectures and it hands
back a memory map, a framebuffer at a known address, and file access on the
boot volume before the kernel exists. A BIOS loader is 16-bit real-mode x86 and
covers one architecture, so it is worth doing second rather than not at all.

## Storage, partitions and filesystems

The goal that shapes all of this: **install beside whatever is already there.**
A disk with Windows on it is a disk ReconOS has to understand well enough not
to damage.

That splits into four separate problems, and conflating them is how the work
looks impossible:

1. **Reading and writing sectors** (checkpoint 11). Drivers: NVMe, AHCI, USB
   mass storage, virtio. This is the kernel's job and nobody else's.
2. **Understanding the partition layout** (checkpoint 12). Three schemes, not
   two:

   | Scheme | Where it is found |
   | --- | --- |
   | **GPT** | Every modern machine. Windows, Linux, and Intel and Apple Silicon Macs |
   | **MBR** | Old PCs, and removable media formatted by anything old |
   | **APM** (Apple Partition Map) | PowerPC Macs, and some old Mac external drives |

   **This is a data format, not a kernel service**, so it is parsed above the
   block layer — the same line [THIRD_PARTY.md](../THIRD_PARTY.md) already
   draws for PNG and TLS. The desktop owns the parser; the kernel owes it
   sectors, a sector size, a sector count, and whether the device is removable.

   *Worth separating carefully, because the two are easy to conflate:* a
   **partition table** says where the partitions are, and a **filesystem** says
   what is inside one. macOS uses GPT for the first and APFS (or HFS+ on older
   systems) for the second. Reading a Mac's partition table needs GPT, which we
   need anyway. Reading a Mac's *files* needs APFS, which is a separate and much
   larger job — and, importantly, is not required in order to install beside
   macOS without damaging it.
3. **A filesystem of our own** (checkpoint 13). ReconFS. Not designed yet, and
   deliberately not designed yet: it should be designed when there is a block
   layer to build it on and real use to shape it, not before. What is already
   known about it is recorded below.
4. **Reading foreign filesystems** (checkpoint 14). NTFS, ext4, APFS, HFS+,
   FAT32. Much bigger than it sounds, and **mostly not needed for the goal.**
   Installing beside Windows requires reading the *partition table* and
   possibly resizing a partition; it does not require reading a single NTFS
   file. FAT32 is the exception and is required, because the UEFI System
   Partition is FAT32 and that is where our own bootloader has to be written.
   That holds on a Mac too: Apple uses a standard FAT32 EFI System Partition,
   so installing beside macOS needs GPT and FAT32 and nothing Apple-specific.
   APFS, NTFS, ext4 and HFS+ come when somebody actually wants to open a file
   on them.

### What is already known about ReconFS

Not a design, just the constraints it has to satisfy, collected as they turned
up:

- **Create-with-mode.** The desktop currently writes a TLS private key and
  *then* tightens it with `chmod`, leaving a window where a private key is
  world-readable. Every secret ReconOS writes has that window. The first
  filesystem calls this kernel defines take a mode at creation — decided now,
  because now is when it is free.
- **Recoverable after power loss.** Which means write ordering that survives an
  unexpected reset, and a `flush` that returns when the data is on the medium
  rather than when it was accepted.
- **It has to hold `recon_fs`'s shape.** The desktop already has System,
  Programs and User volumes with a recycle bin each, deliberately written so
  they can sit on real volumes later.
- **Case sensitivity, and whether drives have letters,** are open questions.
  Windows assigns letters to partitions; Linux mounts them into one tree. The
  decision belongs with the filesystem and the installer together, and is not
  taken yet.

### The installer

Checkpoint 15, and the thing that turns all of the above into an operating
system somebody can have. It needs: the boot medium to carry both loaders and
both architectures; a partitioner that can read an existing layout and shrink a
partition without losing what is in it; somewhere to write the bootloader that
the machine's firmware will actually look at; and a first-run flow, which the
desktop already has.

## Architecture support

| Architecture | Status | Notes |
|---|---|---|
| x86_64 | Boots, BIOS and UEFI both | Multiboot2 for now; own loader at 4 and 16 |
| aarch64 | Boots | arm64 Image header, device tree parsed |
| riscv64 | Not started | Four files and a Makefile stanza when wanted |
| i686 (32-bit x86) | Not planned | See below |

**32-bit x86 is not planned, and this is a decision rather than an oversight.**
A different pointer size means every structure holding an address, every page
table format, and every assumption about how much can be mapped has to work two
ways. It roughly doubles the cost of most checkpoints ahead, for machines that
have not been sold in over a decade.

This is a separate question from *running* 32-bit programs. A 64-bit x86 kernel
can execute 32-bit user code, which is what a Windows 95 application needs.
That capability is unaffected by this decision.

## Running old and foreign applications

Phase 3 and later, recorded now because it constrains a decision taken much
earlier. An application binary needs three things: an instruction set it can
execute (an emulation problem, userspace, independent of the kernel); a loader
for its file format (PE, ELF, Mach-O); and the system calls and libraries it
expects — which is nearly all of the work, and why the plan is to contribute to
Wine rather than reimplement it.

What the kernel owes all three is one small thing: a process created with a
*personality* — a loader and a system-call table chosen per binary rather than
fixed for the system. Linux calls this `binfmt`; Windows NT called them
subsystems. Free to leave room for at checkpoint 10; expensive to retrofit.

## The interface the desktop compiles against

`kernel/include/recon_kernel.h` and `kernel/api/stubs.c`, built by `make api`.
Every call returns "not implemented", and the version does not move for it,
because the version says what works and stubs work at nothing.

Four rules hold across all of it, each paid for by something:

- **Nothing allocates.** The caller owns every buffer. A kernel that allocates
  for a caller must decide what to do when it cannot, halfway through an
  operation the caller has already begun.
- **Every call returns a status, never a bool.** "Failed" is not a sentence a
  settings page can show. Above the boundary the desktop keeps its own
  `bool` + `_last_error()` convention and converts, which is one conversion per
  subsystem and is what a boundary is for.
- **Enumeration is by identity, not position**, with a generation counter.
  Hardware appears and disappears while software is looking at it.
- **No callbacks into the kernel.** Asynchronous things leave a result to be
  collected, the way `recon_net` does with reachability.

Processes are deliberately *not* stubbed. Applications are `dlopen`'d into the
compositor's address space, so there is no boundary to write in advance — the
boundary is an address space and there is not one. `recon_appwin_impl` is the
real surface: a struct of callbacks holding pointers into the compositor's own
memory, which is the part that cannot survive a process boundary unchanged.

**That sentence used to say the module ABI would be settled at checkpoint 5,
"where address spaces arrive."** The numbering has moved twice since it was
written and address spaces arrived at checkpoint 10, which has now landed — so
the deadline is here rather than ahead, and the paragraph is corrected rather
than quietly left pointing at a checkpoint that means something else now.

What checkpoint 10 actually settled is smaller than the sentence promised. The
processor now enforces a privilege boundary, and a process has a personality —
its own idea of what a system call number means. What it does *not* yet have is
its own address space: there is one set of page tables, and every thread shares
it. A process in the sense `recon_appwin_impl` would have to cross needs that,
plus shared memory between two of them, plus a way to pass a handle. None of
those is hard after checkpoint 11's device model; none of them exists today.

**The observation that matters for the decision, and it is the desktop
session's to make.** A function pointer means nothing across an address space,
so the interface cannot simply be recompiled — it has to invert. The shape that
survives is the one where the application owns its buffer, draws into shared
memory, and *submits* a frame, rather than the compositor calling into it. That
is Wayland, which the compositor already implements for external clients. The
out-of-process path therefore already exists in the tree; what does not is
in-tree applications using it.

So the likely answer is not a new ABI but a migration: `recon_appwin_impl`
stays as a convenience for code that genuinely lives inside the compositor's
process, and everything a person installs becomes a client. The kernel's part
is shared memory and handles, and it is on the line for checkpoints 11 through
13 anyway.

The decision does not need making now. It needs making before checkpoint 15,
because the installer is what fixes the meaning of "an application", and by then
the answer has to be true.

The kernel claims error-code area letter `N` in `recon_errors.def`.

## Where this is tracked, at a glance

A status board carrying the checkpoint list, what works today, and the evidence
behind each completed one:
<https://claude.ai/code/artifact/781c0e1d-e3ed-4d35-848e-9e808e8625dc>

It is regenerated when a checkpoint lands, so it never disagrees with this file
for long. This file is the source; the board is the view.

## How this is tracked

The same four places as everything else — this file for the plan,
[CHANGELOG.md](CHANGELOG.md) for what shipped, [BUGS.md](BUGS.md) for faults,
GitHub Issues for what is open. Kernel bugs take `BG-` numbers from the same
sequence as the rest of the system, because a bug number is a place in the
project's history and the kernel is part of the same project. The kernel
session sends the account; the desktop session writes the entry, so that one
register does not become two across a branch.

Developed in a git worktree on the `kernel` branch, merged to `main` when a
checkpoint lands.
