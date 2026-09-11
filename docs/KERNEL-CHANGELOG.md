# ReconOS kernel change log

What changed in each version of the **kernel**, newest first. The version
number tracks what works, not what is planned.

`docs/CHANGELOG.md` is the **desktop's** and stays that way. This is a separate
file for the same reason `docs/KERNEL.md` is separate from `docs/ROADMAP.md`:
the two tracks ship on their own numbers, and one file holding both would need
every entry to say which half it is about.

## The rule

- **A patch per bug fixed.** One fix, one bump.
- **0.2.0 is not reached until sections 1.1 to 1.9 of the blueprint audit are
  all built.** Not a judgement about what counts as a big change -- a fact about
  the audit that anybody can check.

Eleven rows stand in the way as of 11 September, plus the four things section
1.8 states in prose:

| section | what is not built |
|---|---|
| 1.3 Process, thread, execution | IPC -- signals, sockets, message queues, FIFOs |
| 1.4 Interrupts and timers | the HPET and the timer wheel |
| 1.6 Filesystem and storage | Block I/O (in verification); ext2/4, ramfs, devfs, procfs |
| 1.7 Device drivers and buses | USB host controllers; I2C / SPI |
| 1.8 Network | all of it |
| 1.9 Security and diagnostics | self-relocation, video, embedded filesystems, initrd, UEFI |

1.1, 1.2 and 1.5 are complete.

The rule also lives beside `VERSION` in `kernel/Makefile`, because that is the
line that has to change and a rule kept only in a document is a rule read after
the fact.

### Why this file exists

The version moved on **every commit** from 0.0.2 to 0.0.11 across 4 and 5
September. Then the habit lapsed. 0.0.11 held through checkpoints 12, 13, 14,
15, 16, 11b and 9b -- a filesystem, an installer, a second bootloader and USB
storage -- before 0.1.0 on 10 September, and 0.1.0 then held through twenty-five
more commits and six fixed bugs.

Nothing forbade a bump in either gap. Nothing asked for one either. The kernel
had no change log of its own, so there was no step at which somebody was
required to write down what changed and notice the number had not moved.

That is the same shape as BG-186, one level up: a number that is only correct
while somebody remembers is a number that will eventually be wrong, and will
look exactly the same when it is.

---

## 0.1.7 -- 11 September 2026

**Recovery does not write to the volume, and the volume is what refuses.**
(BG-188) Recovery promised to look and not touch, and nothing enforced it -- the
promise was kept by every caller happening not to write. A self-test that
replaces a file broke that on every boot. The volume is mounted read-only on a
recovery boot now, refused at the one place a change can begin rather than by
each caller remembering.

Cleaning up after the write would not have been enough: ReconFS is
copy-on-write, so a file created and then deleted still moves the root and still
changes the disk. Only not writing keeps a byte-for-byte promise.

**Five self-tests need a volume and no matrix path has one.** (BG-187) Every
boot path attaches a blank disk, so they print "no volume on this machine" and
are counted as having run. A skipped test and a passing test look identical in a
total.

**Requests reach the disk in an order**, and the block layer's transfer counters
are printed. (BG-186) They had been incremented since the block layer was
written and displayed nowhere, so an I/O rewrite that dropped them could not be
noticed -- and one did.

**A file on the volume can be replaced**, which is what made the page cache's
invalidation reachable at all. Replacing is its own flag; `OPEN_CREATE` still
means *it must not already exist*, because a create that silently overwrites is
how running a key-generation routine a second time destroys the key that was
working.

**A file on the volume can be removed.** `reconfs_remove` had existed since the
filesystem was written and nothing above it ever called it.

## 0.1.6 -- the six bugs 0.1.0 should have moved for

Not a release that existed; recorded so the numbering adds up. Between 0.1.0 and
0.1.7 these were found and fixed while the version sat still:

- **BG-186** -- transfer counters incremented and printed nowhere.
- **BG-185** -- the loader's ELF reader trusted a signature check the default
  build does not perform. One byte changed in a file on the EFI partition faults
  the firmware before the kernel starts.
- **BG-184** -- the page cache key named the file but not the filesystem, so
  ramfs slot 10 and volume dossier 10 collided.
- **BG-183** -- the reapers had no callers. Both were written, both correct, and
  each had exactly one caller: a self-test.
- **BG-182** -- tearing down an address space freed the page tables and not the
  pages they pointed at, under a comment saying the memory was somebody else's
  job. There was no somebody else. Twelve pages leaked per program.
- **BG-163** -- two disks of one kind stalled the boot. A legacy interrupt line
  nothing acknowledged, taking 1,770,000 interrupts on an ordinary one-disk boot
  and printed in the summary the whole time.

## 0.1.0 -- 10 September 2026

Raised from 0.0.11 because the kernel had stopped fixing things and started
gaining them: a block cache, input, swap, identity, capabilities, and interrupts
a driver asks for. A patch number that only ever goes up is a version that has
stopped saying anything.

Checkpoints 18 to 21 landed on this number: input on two buses, processes with
an ELF loader and an address space each, identity the kernel enforces, and a
virtual filesystem.

## 0.0.11 -- 5 September 2026

Eight processors, all of them working.

**This number held far longer than it should have** -- through checkpoints 12
(partition tables), 13 (ReconFS), 14 (FAT32 read and written), 15 (the
installer), 16 (its own BIOS bootloader), 11b (USB mass storage) and 9b (every
core on x86_64). Six days, and a kernel that went from reading a disk to
installing itself onto one.

## 0.0.10 -- 5 September 2026

Threads, and a tick that takes execution away.

## 0.0.9 -- 5 September 2026

Two clocks and a tick.

## 0.0.8 -- 5 September 2026

A fault that says what happened, instead of resetting the machine.

## 0.0.7 -- 5 September 2026

A heap, and slabs that go back when they empty.

## 0.0.6 -- 5 September 2026

The kernel stops running on borrowed page tables.

## 0.0.5 -- 4 September 2026

ReconOS boots itself. Its own UEFI bootloader on both architectures; GRUB comes
out of the boot path.

## 0.0.4 -- 4 September 2026

The kernel reads what the processor can actually do -- not what the oldest
processor would have.

## 0.0.3 -- 4 September 2026

The kernel can hand out memory.

## 0.0.2 -- 4 September 2026

The kernel learns what machine it is on: which firmware booted it, and what
memory exists.

## 0.0.1 -- 4 September 2026

The kernel boots, on two architectures, and prints its identity.
