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

The kernel branch's twelve were renumbered to **BG-114 through BG-125**, in the
same order, and the next fault found took BG-126. `main` keeps its numbers,
being the longer sequence and the one the arrangement says is authoritative.

| Was | Is |
|---|---|
| BG-082 | BG-114 |
| BG-083 | BG-115 |
| BG-084 | BG-116 |
| BG-085 | BG-117 |
| BG-086 | BG-118 |
| BG-087 | BG-119 |
| BG-088 | BG-120 |
| BG-089 | BG-121 |
| BG-090 | BG-122 |
| BG-091 | BG-123 |
| BG-092 | BG-124 |
| BG-093 | BG-125 |

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

### BG-119 — A transaction could overwrite storage the live filesystem still used

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
  after BG-114 and BG-115.

  Testing it directly also turned up a smaller thing worth fixing: running out
  of space marked the whole transaction failed, which made "the volume is full"
  indistinguishable from "the allocator broke". Exhaustion now returns zero and
  leaves the transaction usable, which is what let the test fill a volume on
  purpose.

### BG-120 — Only one of the two superblocks was ever written

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

### BG-121 — Renaming over a file leaked every block it occupied

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

### BG-122 — Every directory rewrite leaked a block, and the checker was built not to notice

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

### BG-123 — A machine with more processors than the kernel holds reported the wrong number, under a comment saying it never would

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
  which is the same shape as BG-120 (a correct sentence above an incorrect line)
  and BG-122 (an exemption written for a category, applied as a membership
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

### BG-133 — The bootloader build ignored its headers, so a security check silently was not there

[#292](https://github.com/neogentrics/ReconOS/issues/292)

- **Found:** 7 September 2026, by the verification rig, on a change whose own
  test had just passed.
- **Cost:** nothing shipped. It is here because of what it would have cost.

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

### BG-132 — A handle used two lines after it was closed, under a comment saying it was open

[#291](https://github.com/neogentrics/ReconOS/issues/291)

- **Found:** 7 September 2026, the first time the loader tried to verify a
  kernel signature.
- **Cost:** most of an hour, all of it spent suspecting the wrong code.

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
lines from it — see BG-118, BG-125, BG-127.

  A comment states what somebody meant. Only the machine states what happens.

The volume is closed after the verification now, which is the only place it can
be.

### BG-131 — The bootloader never passed a command line, for four checkpoints

[#290](https://github.com/neogentrics/ReconOS/issues/290)

- **Found:** 6 September 2026, writing the first test that installs from a real
  medium and then boots the installed disk.
- **Cost:** nothing yet, and it would have cost the entire installer.

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

### BG-130 — Every rewrite renamed the file, including the one firmware looks for

[#288](https://github.com/neogentrics/ReconOS/issues/288)

- **Found:** 6 September 2026, writing the same files into a FAT32 volume twice
  and reading the result back with `mtools`.
- **Cost:** none yet. There is no installer to have run twice.

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

### BG-129 — An unsupported conversion made every later value in the line wrong

[#287](https://github.com/neogentrics/ReconOS/issues/287)

- **Found:** 6 September 2026, listing a real EFI System Partition with
  `%-30s`, and getting a file of 2,148,777,108 bytes that was a pointer.
- **Cost:** minutes, because the wrong number was absurd. It would have cost far
  more if it had been plausible.

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

### BG-128 — One medium could not carry two architectures, because both loaders opened the same filename

[#286](https://github.com/neogentrics/ReconOS/issues/286)

- **Found:** 6 September 2026, building an actual bootable USB stick and trying
  to boot it on both architectures.
- **Cost:** nothing yet — no install medium had ever been built until today.

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

### BG-127 — Whether a drive is flash is a question USB cannot answer, and the storage layer assumes it can

[#285](https://github.com/neogentrics/ReconOS/issues/285)

- **Found:** 6 September 2026, on a physical USB stick, the first real disk this
  kernel has ever been shown.
- **Status:** **Open.** It cannot bite until checkpoint 11b exists, because
  there is no USB driver for it to be wrong in yet. Recorded now because the
  assumption is already written into the storage layer, and the moment a USB
  disk appears it becomes wrong silently.

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
the same shape as BG-119's comment arguing "this case does not arise here" about
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

### BG-126 — A transaction's capacity was written down as one number, and it is three

[#284](https://github.com/neogentrics/ReconOS/issues/284)

- **Found:** by running the ReconFS battery against a 512 MB disk instead of the
  16 MB one the verification rig uses.
- **Cost:** nothing yet. It has never been reached by anything but a test.

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

**The same shape as BG-117.** A number written in a comment, stated as a fact,
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

**And the rig, for the third time this session.** BG-124 hid below nine
processors because the rig stopped at eight. BG-125 hid because the only node
with a child was one the rig never asked about. This hid because the disk was
16 MB. Every one of them is the same sentence: *the fault is real from the first
line; what was missing was a machine big enough to show it.*

### BG-125 — The device tree walk lost any node that had a child

[#283](https://github.com/neogentrics/ReconOS/issues/283)

- **Found:** chasing BG-124's fix, which did not work and had two wrong
  diagnoses before this one.
- **Cost:** an entire debugging session, most of it spent suspecting the pointer
  the walk was given rather than the walk.

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
BG-124's symptom exactly, which is why two rounds of fixing BG-124 changed
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

### BG-124 — The kernel panicked at boot on any ARM machine with more than eight processors

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

### BG-117 — ReconFS could not have held a drive you can buy today

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

### BG-118 — A block-size rule that read like a rule and behaved like a constant

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

  Third in a row of this shape, after BG-114 and BG-115: **something that looks
  like it is working, is not.**
- **Fixed in** kernel 0.0.11. Replaced with an explicit table — 4KiB under 2TiB,
  16KiB under 16TiB, 64KiB above — with the trade-off written down beside it,
  because a bigger block wastes proportionally more on every small file and no
  formula can know what a volume will hold. An explicit size passed by the
  caller always wins, which is what the installer is for.

### BG-114 — The crash harness never cut the power, and reported that it had

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

### BG-115 — The crash harness passed cleanly with its checker missing

[#271](https://github.com/neogentrics/ReconOS/issues/271)

- **Found in** kernel 0.0.11. **Found by** fixing BG-114 and watching the next
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

### BG-116 — The flush instrument counted zero on a driver that was flushing

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
