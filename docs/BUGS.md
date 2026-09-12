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

### BG-140 — The block self-test would write to a stranger's partition, and only ever on a stranger's machine

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
whose *only* reachable case is the case nobody tests — after BG-119's comment
arguing that a case "does not arise here" about a case that then arose. The
shape to look for is not a wrong branch. It is a fallback whose precondition is
"the situation the rig never builds".

### BG-141 — A processor count that saturates, and states the shortfall as a fact

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
device tree, which the parser can now do — it gained the ability at BG-125, when
the walker learned to report a node that has children. Until then the number is
a floor rather than a total, and the wording says so.

**Worth generalising.** The bug is not in either layer. It is that a lower layer
was allowed to return a *saturated* value through an interface whose caller
treats it as a *total*, with no way to ask which it got. The same shape as a
read that returns fewer bytes than asked for and is treated as a failure — see
the transfer-event residue in `xhci.c`, where the distinction was built in
deliberately for exactly this reason.

### BG-142 — The scheduler's own test hung one boot in three, on a counter that `volatile` did not protect

[#381](https://github.com/neogentrics/ReconOS/issues/381)

- **Found:** 8 September 2026, sweeping processor counts on x86_64 after
  checkpoint 9b started the secondaries.
- **Cost:** nothing shipped, and it had been latent on aarch64 since 9b landed
  there — see BG-143 for why nothing saw it.

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

### BG-143 — The rig reported a boot that stopped half way as a boot that passed

[#382](https://github.com/neogentrics/ReconOS/issues/382)

- **Found:** 8 September 2026, immediately after BG-142, by asking why a hang
  that happened on one boot in three had never turned the matrix red.
- **Cost:** nothing yet. It is the reason BG-142 could have shipped.

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

### BG-144 — Every UEFI test booted whatever kernel happened to be in the ESP first

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

**Worth generalising, and it is the third time in this shape.** BG-133: a
generated header absent from the dependency file, so creating it rebuilt
nothing. BG-138: stage 2 outgrew stage 1's read, and the fix was attached to a
rule `make` alone does not build. Now this. Each time, **a build artefact that
does not depend on what it contains**, and each time the symptom was a change
that appeared to have no effect.

The class is worth naming: *if an image embeds a copy of something, the rule
that builds the image must depend on the thing it copied.* And the diagnostic is
worth remembering — when a fix appears to do nothing, check that the artefact
under test contains the fix, before looking at the fix again.

### BG-139 — Two tests generated a signing key into the source tree and left it there

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
  that header is a tracked dependency, which it was not until BG-133.

- **The shape.** The instrument changing the thing it measures, which is BG-137
  four hours earlier wearing a different coat: there, the test observed a signal
  the loader was not producing; here, the test *produced* a condition the next
  test then observed. Both are the harness being part of the experiment rather
  than outside it.

  Worth naming the direction that did **not** happen and could have: a leftover
  key makes a test that expects *"signature: not checked"* see a real check
  instead. That way round, a test passes for the wrong reason rather than
  failing for one — and nobody investigates a pass.

### BG-138 — Stage 2 outgrew the number of sectors stage 1 reads, and the magic check passed anyway

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
  is a build step that is sometimes absent** — BG-133 wearing different clothes,
  two days later. The patch now belongs to `stage1.bin`'s own rule, so the only
  stage 1 that can exist is a patched one.

  The harness also asks the question directly now, before booting anything: it
  reads the count out of `stage1.bin` at the symbol's offset and compares it
  with stage 2's size on disk. That is the question the magic number cannot
  answer, asked where the answer is cheap.

- **The shape.** A constant that was true when written, consumed later by code
  that had no way to know it had stopped being true — the same family as
  BG-126 (a transaction's capacity written down as one number when it is three)
  and BG-119 (a value correct when computed, used after it had gone stale).
  What is new here is the *check that gave cover*: the magic test made the hop
  look verified, so the one visible symptom had an explanation that was already
  known to be handled.

### BG-137 — The BIOS harness read a mirror of the screen and called it serial output

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
  belongs with BG-133 and BG-134: three in two days where the thing under test
  was not the thing being observed. The instrument is part of the experiment,
  and *this* one also manufactured a bug rather than hiding one, which is the
  more expensive direction to be wrong in.

### BG-134 — Eight test harnesses looked for a kernel instead of building one

[#291](https://github.com/neogentrics/ReconOS/issues/291)

- **Found:** 7 September 2026, hours after BG-133, by the recovery test failing
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

**This is BG-133 again, one night later, through a different door.** There a
generated header was not a dependency; here the binary was not a target. Both
end at the same place: *a test that ran a binary other than the one it was
written to prove, and reported the result as though it had.* BG-133's stale
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
  thing and the machine did another (BG-120, BG-122, BG-123, BG-132, this) —
  and the second where the disagreeing party was a *test*, which is the worst
  place for it, because a test is the thing everything else is believed on.


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

### BG-132 — A handle used two lines after it was closed, under a comment saying it was open

[#293](https://github.com/neogentrics/ReconOS/issues/293)

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

### BG-125 — The device tree walk lost any node that had a child, which is why the GICv3 fix could not be reached

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


### BG-145 — Whether the kernel could write through a read-only page depended on which firmware booted it

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

### BG-146 — Sixteen bytes past the end of the vector save area, into the next field of the same thread

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



### BG-147 — A new process inherited the last one's permission to touch addresses

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



### BG-148 — A thread was runnable before it belonged to its process, and address spaces made that fatal

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
  unexpected fault -- a different mode, recorded as BG-150 rather than folded in
  here.

**What let it hide, and is worth more than the bug.** Every "N self-tests, all
pass" row in `scripts/verify-kernel.sh` is a *single-processor* run.
`check_cpus`, which is the only thing that boots with `-smp`, asserts processor
counts, idle ticks and shootdowns — and never that the self-tests passed. So a
self-test that fails only on more than one processor is invisible to the matrix,
which is the same shape as BG-143 and the reason this was found by hand.



### BG-149 — The page allocator and the kernel heap have no locking, on a kernel verified at thirty-two processors

[#388](https://github.com/neogentrics/ReconOS/issues/388)

- **Found:** 10 September 2026, while looking for the cause of BG-148. It is not
  that cause, and it is worse than that cause.
- **Cost:** none observed, and that is not reassurance. A lost page is invisible
  until something writes through a mapping it no longer owns.
- **Status:** fixed, 10 September 2026, as its own change with its own
  measurements — which is why it was recorded rather than fixed inside BG-148.

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



### BG-150 — About one boot in sixty, a user program does not finish, and nothing says why

[#389](https://github.com/neogentrics/ReconOS/issues/389)

- **Found:** 10 September 2026, as the part of BG-148 that fixing BG-148 did not
  account for.
- **Cost:** none yet. It is recorded because the alternative is rediscovering it.
- **Status:** open, and probably BG-156 — see below.

The interleaved measurement that confirmed BG-148 also measured the kernel with
BG-148 fixed, and it is not zero:

    A (BG-147 and BG-148 fixed) : 1 of 60 failed, 0 entry-point faults
    B (BG-148 restored)         : 4 of 60 failed, 3 entry-point faults

The three entry-point faults are BG-148 and they are gone. The remaining one is
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



**10 September, later the same day: this is very likely BG-156.** A system call
entered on one processor and returned on another read the saved user stack
pointer out of the wrong processor's block, or out of a GS base of zero. The
symptom is a program that does not finish, at a rate that depends on how often a
thread happens to be preempted inside a system call — which is exactly the shape
of "about one in sixty, and it moves with host load".

It is not being closed on that reasoning. After BG-156 was fixed, **sixty
consecutive boots at `-smp 4` passed with no failure and no panic**, which is
consistent with it and does not prove it: the previous rate would predict about
one failure in sixty. This stays open until a longer run says otherwise, because
closing it on a run that would have been just as likely to be clean by chance is
how BG-148 got the credit for a fault it had not fixed.

### BG-151 — Eight processors, on a machine that may have five hundred

[#390](https://github.com/neogentrics/ReconOS/issues/390)

- **Found:** 10 September 2026, asked directly: would this run on a two-socket
  server board?
- **Cost:** none yet, because no such machine has run it. On one that did, it
  would use eight cores of however many are there and say so.

`MAX_CPUS` was 8. It is not a bug in the sense of something behaving wrongly --
9b's BG-141 already made the shortfall a reported number rather than a silent
saturation, so a 64-core machine says how many more it found than it can hold.
It is a bug in the sense that the number was chosen when the largest machine
this kernel had ever met was a QEMU guest, and modern server parts are two
orders of magnitude past it: EPYC reaches 128 cores per socket (192 on Turin),
Xeon 128 P-cores or 288 E-cores, and a dual-socket board is routinely 256 to 576
*logical* processors.

Raised to **256**, and that number has a reason rather than being the next round
one up: **255 is the largest processor an 8-bit APIC identifier can name.**
Going past it is not a bigger array, it is implementing x2APIC -- see BG-152.

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

### BG-152 — Processors above 255 are found and cannot be started

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

### BG-153 — On a multi-cluster ARM machine, two processors would believe they are the same processor

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
BG-152 produces a machine that runs on fewer processors than it has and reports
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

### BG-154 — The page allocator scans, and a terabyte is a billion pages

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


### BG-155 — The boot thread was processor 0's idle thread, so nothing on the boot path could ever wait

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
  meaningful and wrong value. Exactly the shape of BG-147.
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

### BG-156 — A system call entered on one processor and returned on another, and the kernel stack did not travel with it

[#395](https://github.com/neogentrics/ReconOS/issues/395)

- **Found:** 10 September 2026, immediately after BG-155 — a double fault at the
  first instruction of the system-call entry stub, with `%gs` based at zero.
- **Cost:** a kernel fault on the way out of a system call that had worked. **It
  is the best candidate so far for BG-150**, the one-boot-in-sixty that had no
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

### BG-157 — The clock ran at 201 Hz against a constant that said 100, and every test still passed

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


### BG-158 — An idle thread took its turn in the round robin, and the machine ran at half speed with every test green

[#397](https://github.com/neogentrics/ReconOS/issues/397)

- **Found:** 10 September 2026, by the verification matrix — **seven paths failed and
  every one of them writes to a disk.** They wrote correct data and ran out of
  time doing it.
- **Cost:** roughly half the machine, for the length of one matrix run.
- **Status:** fixed.

Fixing BG-155 meant giving processor 0 an idle thread of its own, because the boot
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
  BG-148 were: waking the other processors, or in this case giving one an idle
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

Same family as BG-157, found the same day: a fault whose only symptom is time.


### BG-159 — A thread was available to every other processor while the one it was leaving was still standing on its stack

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

**This is also a better candidate for BG-150 than BG-156 was**, and neither is
being credited with it. Both are real, both were fixed the same day, and the
honest position is that the one-in-sixty has not been seen since without a run
long enough to say so.

### BG-160 — The power-cut harnesses timed their cut from launch, so a slower boot meant they cut before anything had been written

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


### BG-161 — A header promised that either pointer could be null, and one of them could not

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
  BG-149's expired comment and BG-156's predicted one. A header is a claim about
  behaviour, and a claim nothing checks is a claim that drifts — this one had been
  wrong since it was written and would have stayed wrong until something believed
  it. What made it visible immediately was that the believer was in the kernel and
  crashed; a user program would have got a corrupted answer.

### BG-162 — Power-off declared the machine had refused, while the machine was in the middle of obeying

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
  BG-157 and BG-158 cost duration with the whole suite green, and BG-160 was the
  same harness cutting power before the guest had spoken. Each was invisible to
  a test that asks only whether the right things happened. The matrix runs
  several guests at once, which is why it is the thing that found this and the
  quick check is not.

### BG-163 — Two virtio-blk disks on one machine, and every request to them times out

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

### BG-179 — The interrupt summary counted devices before the bus had been walked

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
- **Family:** BG-157, BG-158, BG-160, BG-162 and now this -- five faults this
  month whose only symptom was *when* something happened rather than what.

### BG-180 — "Can signal by memory write" was implemented as "has MSI"

- **Found:** 10 September 2026, immediately after BG-179 made the number
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
- **Lesson, which is the same one twice:** both this and BG-179 are a *reported
  number that nobody had ever had a reason to check*. It went unnoticed for as
  long as nothing depended on it, and was wrong in two independent ways the
  moment something did.

### BG-181 — The vector self-test assumed it was the only thing holding a vector

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
  because the kernel got *better* -- after BG-165, where a race was cured and
  the test that had been watching it went red.

### BG-182 — Tearing down an address space freed its page tables and not its pages

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

### BG-183 — A process that ends is never reaped, so it keeps its slot and its memory

- **Found:** 11 September 2026, immediately after BG-182 and by the same
  measurement — which is the interesting part, because the measurement is what
  said the first fix had not worked.
- **Cost:** a process table that fills, and every ended program's memory held
  for the life of the machine. Measured at **8 of 32 slots used on an ordinary
  boot**, all of them `ended` with no threads.
- **Status:** fixed.

**The instrument corrected the diagnosis, and would not have if it counted one
number instead of two.** Having fixed BG-182, the same probe still showed
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

- **Was:** believed to be the same fault as BG-182. It is not, and BG-182's fix
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

`thread_exit` said so, in the same shape as BG-182 one file over:

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
  shape as `off_cpu` in BG-159, one level up.
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

### BG-184 — The page cache asked which file and not which filesystem, so two of them were the same file

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

### BG-193 -- virt_to_phys answered for addresses it cannot answer for, so a driver wrote to memory that does not exist and reported success

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
as BG-190 (a map that could not distinguish empty from occupied), BG-188
(recovery's promise enforced by nobody), and BG-186 (counters nothing printed).

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

### BG-192 -- The kernel boots from a disk over BIOS and then cannot see it

- **Found:** 12 September 2026, by the self-test assertion added for BG-187.
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

#### What this means for BG-187

BG-187 predicted the second boot would go red from tests that leave files
behind. It did not, and not because the tests are idempotent: **there is no
volume on that boot to write to.** The only second-boot-on-one-disk in the whole
matrix cannot mount the disk. So that half remains unexercised and is recorded
as such rather than assumed closed.

### BG-191 -- The BIOS loader handed the kernel dirty registers, breaking its own stated invariant

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

That is the same mechanism as BG-186 the day before (counters found because an
unrelated cache test failed) and BG-099 on the desktop (found because two things
that should have agreed did not, neither being watched on purpose).

### BG-190 -- The processor identity check could not tell an empty map from one processor

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
BG-187 restated.

### BG-189 -- Changing VERSION rebuilt nothing, so the kernel printed the old number

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
every build anybody actually does. That is the same shape as BG-134 on the
desktop: **the one configuration that ships was the one configuration nothing
ran**, inverted -- here the one configuration nothing ran is the one everybody
uses.

Fixed by making every object depend on the Makefile. The cost is that editing it
rebuilds everything, which is the correct price: almost anything changed in that
file changes how the code is compiled.

### BG-188 -- Recovery wrote to the volume it was inspecting, and the read-only promise was never enforced

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
[BG-187](#bg-187).

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

### BG-187 -- Five self-tests need a volume, every matrix disk is blank, and the boot reports green either way

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

**A skipped test and a passing test look identical in a total.** That is BG-186
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

### BG-186 -- The block layer counted every transfer and printed the number nowhere, so an I/O rewrite lost them silently

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

### BG-185 — The loader's ELF reader trusted a signature check that the default build does not perform

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

### BG-164 — The reaper assertion asked one processor to have already done what another one owed it

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
on has finished with it. **That is BG-159's fix**, and it is what stops a stack
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
- **Family:** the fourth this month whose only symptom was timing -- BG-157 (a
  clock at twice its stated rate with every tick-counting test green), BG-158
  (an idle thread halving the machine with twenty-six self-tests passing),
  BG-160 and BG-162 (a harness and a kernel each mistaking *slow* for *did not
  happen*). Every one was invisible to a suite that asks only whether the right
  things happened.

### BG-165 — The process test raced the very threads it was pretending to end

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
  says so under BG-148: *"putting a thread in the ring first and setting the
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
- **Family:** the second in a day, after BG-164, where the matrix was right to
  go red and wrong about why -- and both were instruments rather than machines.
  The difference from BG-157 and BG-158 is worth keeping: those were real
  faults with timing as their only symptom, and these two were correct kernels
  with tests that could not tell *not yet* from *not ever*.

### BG-166 — The signature tests rewrite the tree every other test builds from

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
