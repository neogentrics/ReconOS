# ReconFS — the design

Checkpoint 13. This document exists before the code, because an on-disk format
is the most expensive thing in this project to get wrong: the moment somebody's
data is in it, the format is frozen, and every mistake in it has to be carried
forever or migrated with a tool that must itself be perfect.

It is also the one piece with **no second opinion available**. For partition
tables the reader was checked against disks written by `sgdisk` and `sfdisk` —
tools that share no code with this and no misunderstanding with it. Nobody else
implements ReconFS. Every test would otherwise be this kernel checking its own
arithmetic, which is the failure mode `scripts/make-partition-fixtures.sh`
opens by naming: *a writer and a reader built from the same misunderstanding
agree perfectly.*

That single fact shapes the format more than anything about performance.

---

## What it has to satisfy

Seven constraints, four recorded when checkpoint 13 was deferred and three that
arrived from the desktop side afterwards.

1. **Create with mode.** No path may produce a file whose mode is decided after
   its bytes exist.
2. **Recoverable after power loss.**
3. **It must hold `recon_fs`'s shape** — System, Programs and User volumes with
   a recycle bin each.
4. **Case sensitivity and drive letters** are open, and belong with the
   installer.
5. **Rename must be atomic**, including over an existing target.
6. **Create-with-mode is a latent bug this kernel will arm.** Not one awkward
   TLS key: *every* file the system writes that should not be world-readable is
   written and then `chmod`'d. Nothing can exploit that today only because the
   whole tree runs as one host user. The day account roles are enforced by the
   kernel is the day every one of those windows becomes real, all at once.
7. **Directory listing under concurrent modification** must have a stated
   guarantee, or a stated absence of one. An unstated guarantee is the worst of
   the three options.

   **Answered both ways.** `reconfs_readdir` has none and says so;
   `reconfs_list` reads the whole directory in one call and therefore has one.
   The first version of this had an unstated guarantee that was also false — see
   the end of this document.

### One correction to the record

Constraint 5 arrived described as preserving an existing pattern: that the
registry, theme files and package receipts already write a temporary file and
rename over the target. **They do not.** There is no `.tmp` write anywhere in
the desktop's sources, `recon_fs_write` opens with `"wb"` — which truncates
before it writes — and `recon_fs_rename` refuses outright when the target
exists.

So the deliverable is larger than the constraint said. It is not "ReconFS
supports atomic rename"; it is "the registry writer uses it". Filed as
[issue #269](https://github.com/neogentrics/ReconOS/issues/269), because the
truncate-before-write window is a live data-loss bug today, on Linux, that needs
no kernel at all to fix.

---

## What the medium underneath actually promises

Measured and read, not assumed. This is the foundation, and a design resting on
a guarantee that is not there is wrong in a way no care in the format can fix.

**A flush is real, and it is device-wide.** All three drivers issue a genuine
durability command and wait for it: `VIRTIO_BLK_T_FLUSH`, NVMe Flush, ATA FLUSH
CACHE EXT. There is no range-scoped or file-scoped flush and there cannot be one
here — flushing through a partition commits everything pending on the whole
controller. **ReconFS's durability promise must be stated in those terms** and
must never claim one file is safe while another is not.

**A flush is not always real, and that used to be invisible.** virtio-blk
returns success from flush having issued nothing when the device did not offer
the flush feature. The comment that stood there asserted such a device "has
nothing volatile to flush" — an assumption about the device, not something the
kernel checked. Devices now carry `flush_is_durable`, the summary says so in
capital letters, and **ReconFS refuses to promise power-loss recovery on a
device where the answer is no.**

**Between two writes with no flush there is no durability ordering.** There is
strict *issue* ordering today, because every path is synchronous and one request
is in flight per device — and that is an accident of the current shape, not a
guarantee. `block.h` says the synchronous interface will not change when a wait
queue arrives; that is true of the signature and false of the ordering. A
ReconFS that leaned on issue ordering would pass every test today and corrupt
disks the day the block layer gets a wait queue.

> **Therefore: every ordering point in ReconFS is an explicit `block_flush`.
> The list of flush points below is the durability contract. A guarantee whose
> flush cannot be pointed at has not been designed.**

**One block is the largest unit that may be treated as atomic**, and even that
is convention rather than something this kernel verifies. `scripts/crash-test.sh`
cuts the power at swept moments and finds no torn block and no write out of
order — but read the next section before leaning on that, because the first
version of this document leaned on a number that was not measured.

So: **any structure spanning more than one block must be self-validating on
read.** Either a checksum over the whole run, or a commit record whose presence
is the only thing that gives the run meaning.

### The measurement that was not a measurement

The paragraph above originally read that the power had been cut twenty-eight
times across both architectures with no write out of order and no block torn.
That number should not have been in this document, and the two reasons it should
not have been are worth more than the number was.

**The cut never happened.** The harness launched QEMU as
`timeout -s KILL 30 qemu-system-... &` and took `$!` as the victim's pid. `$!` is
the pid of `timeout`. The kill went to the wrapper, SIGKILL cannot be caught so
`timeout` never forwarded it, and QEMU was orphaned and carried on running —
after which the harness read the disk image out from under a live guest and
found, unsurprisingly, an unbroken prefix. Its "wait until the process is
genuinely gone" loop polled the wrapper's pid and returned immediately.

What exposed it was not the result. The result looked exactly like a pass. It
was twelve orphaned emulators still running minutes after the harness had exited
reporting success.

**And the cut, once fixed, does not measure flushing.** Killing QEMU does not
lose the writes QEMU has already made: those bytes are in the *host's* page
cache, and the host writes them out whether or not the guest ever asked for a
flush. The harness now takes a `nocache` argument that tells QEMU to discard
guest flushes entirely, and with flushes discarded it returns the same clean
result. A test that passes identically whether or not flushes are honoured is
not testing flushes.

> **So the cut measures ordering, and only ordering: that writes arrive in the
> order they were issued, and that a block is not observed half-written. The
> flush is measured separately, or it is not measured.**

**Where the flush is measured.** `scripts/flush-reaches-device.sh` runs the same
workload with QEMU tracing the *device model* — `pci_nvme_flush_ns` for NVMe,
ATA opcode `0xea` for AHCI — and counts the durability commands that arrive at
the emulated controller. That is the far side of the boundary the kernel is
responsible for: a flush that was never issued cannot appear there, and one that
was issued cannot fail to. Four thousand and ninety-six markers produce four
thousand and ninety-seven flush commands: on NVMe on both architectures, and on
AHCI, which only the x86 machine has. virtio-blk is not covered — this QEMU
build publishes no trace point for its flush — which is worth saying plainly,
because virtio-blk is the driver that told this exact lie in the first place.

It was checked the only way a checker is worth anything: `nvme_flush` was
altered to return success having issued nothing — the exact lie virtio-blk used
to tell — and the instrument reported *0 flush commands for 4096 markers* while
passing the untouched AHCI driver in the same run.

**What is still not measured** is whether the host, or a real disk, honours a
flush once it has one. That is below this boundary, and nothing runnable on this
machine reaches it. The claim this design is entitled to make is therefore:
*the kernel issues a real durability command for every ordering point, and its
writes arrive in order.* Not that the medium keeps its side of the bargain.

**A partial write is unknowable.** `block_write` returns a status and no
transferred count, so on failure the target range is partially and
*unmeasurably* modified. Any range written as one call must be one whose partial
application is detectable from the medium alone.

---

## The design: copy-on-write, one commit

Nothing a live superblock can reach is ever overwritten. A change is built
entirely out of newly allocated blocks, and becomes real in a single write.

**The ordering rule, which is the whole design in one sentence:**

> **Nothing reachable from a live superblock is ever overwritten, and a change
> becomes real when — and only when — a superblock naming it is written after a
> flush that has already put everything it names on the medium.**

That is checkable against code. Every write in ReconFS is either to a block no
live superblock can reach, or it is a superblock write, and every superblock
write is immediately preceded by a flush and immediately followed by one.

### Why this and not a journal

A journal writes metadata twice and needs recovery to *replay* — which means
recovery is a program with state, and a crash during recovery is a case the
design has to reason about separately. Copy-on-write has no replay: recovery is
"read two superblocks, believe the valid one with the higher generation", which
is a **pure function of the image bytes**. Recovering the same image twice
produces byte-identical results, and a crash during recovery changes nothing
because recovery writes nothing.

That property is worth more here than the write amplification costs, for a
reason specific to this checkpoint: it is the property that makes the crash
suite able to *assert* something. A recovery that is a pure function can be
tested by running it twice and comparing. A replay cannot.

### Why not careful ordering with no journal and no copy-on-write

Because the ordering it needs does not exist. Soft updates discharge every
dependency by ordering two writes — and between two writes with no flush this
kernel guarantees nothing about durability. Buying that ordering means a flush
per dependency, which is more flushes than the copy-on-write commit, not fewer.

### The two allowed outcomes

For every operation, **the set of post-crash outcomes has exactly two members**:
the commit landed, or it did not. There is no third state, no half-applied
change, and nothing for recovery to untangle. That is the assertion the crash
suite makes, and a design where that set has three members gives the suite
nothing to check.

Atomic rename falls out of it rather than being added: a rename is one commit.
So does create-with-mode — **the name becomes reachable in the same write that
carries the mode**, so there is no image, crashed or otherwise, in which a file
is reachable with the wrong mode. Constraint 6 is a durability property here,
not only a race property.

---

## The verification problem, and the one honest answer to it

A single author writing both a reader and a writer gets no independence from
testing one against the other. There is one form of independence available, and
it has to be **built into the format because it cannot be added afterwards**:

> **Every allocated object carries a back-reference to its owner** — a block to
> its file, a file to its directory — so that walking *down* from the root and
> sweeping *up* from every allocated object are two independent derivations of
> the same set.

The forward walk follows pointers. The reverse sweep reads the owner table and
nothing else. If the two disagree, one of them is wrong, and neither was
computed by the other.

**Every allocated block must be reachable from something — with no exceptions,
including the filesystem's own.** The comparison used to exempt blocks owned by
the archive, reasoning that the superblocks and the owner table are owned by
nothing above them and so cannot be reached by a walk from the root. That is
true of those blocks and false of everything else the archive owns — which
includes every stale copy of the *root directory*, because the root has no
parent and archive-ownership is what "no parent" looks like.

So the forward walk claims them explicitly instead: both superblocks, the
reserved run between them, and the owner table's own index blocks and leaves,
walked from the table root. Following the table's *structure* is the forward
walk's business; the sweep still reads only its *contents*, so the two stay
disjoint.

An exemption written for a category — "the superblocks and the owner table",
which is a list of specific blocks — had been applied as a membership test,
"owned by the archive", which is a property those blocks share with something
else. It hid a leaked block on every commit (BG-122,
[#279](https://github.com/neogentrics/ReconOS/issues/279)). That is the closest
thing to a second opinion a single author can build, it costs bytes per object,
and **it must be in the first version or never.**

The whole-image checker must not call any function the reader uses to resolve a
path. If checking requires the reader, the checker is not a second opinion.

### The second implementation, and what it is actually worth

This document opens by saying no second opinion is available. There is one now,
and it is worth being precise about how much: `scripts/reconfs-check.py` reads
a ReconFS image and reports whether it holds together, written **from the
format header** rather than from `kernel/core/reconfs*.c`, in a different
language, structured differently.

It is not an independent *author*. The same person wrote both, and a
misunderstanding about what the format means could sit in both. What it does
remove is every shared *mechanism*: no shared constants, no shared struct
definitions, no shared arithmetic, no shared assumptions about what a field is
called or where it sits.

**And the limit is not theoretical.** Both checkers had the archive-exemption
hole described above — written separately, in different languages, and wrong in
the same place, because the mistake was in the *reasoning* rather than in any
mechanism. That is a fair measure of what a second implementation by one author
buys and what it does not.

**That was enough to find a bug the kernel could not have found.** The commit
was writing its superblock to block 1 — a constant meaning "the second
superblock" from before the two copies moved to fixed byte offsets. The real
second copy still held the epoch the format left it, so a crash during a
superblock write would have rolled the volume back to freshly formatted.

Nothing inside the kernel could see it. Mounting reads the second copy from the
right place and finds a valid, older superblock, which is what a healthy volume
looks like. Every self-test passed. It took a reader that did not share the
constant. (BG-120, [#277](https://github.com/neogentrics/ReconOS/issues/277).)

The Python reader is also what judges a power cut, because the kernel cannot
check an image it was killed in the middle of writing.

### The two-outcome rule, tested

`scripts/rename-crash-test.sh` runs the workload the registry needs — create a
temporary name, then rename it over the real one, as **two separate commits**,
because that is what the caller does and a crash landing between them is the
interval the guarantee is about. It cuts the power at swept moments and reads
what survived.

Three things must be true of every surviving image:

- it mounts — a superblock validates
- `settings` resolves to exactly one object, and that object reads
- the tree and the owner table agree, block for block

A leftover `settings.tmp` is allowed, and is not a fault: the workload creates
it in one commit and consumes it in the next.

**The file has real contents, and that is not a detail.** Each version is 9,001
bytes — deliberately not a whole number of blocks, so the partial tail is always
exercised — carrying a magic number, the round it belongs to at *three*
separate places, and a checksum over everything else. A file assembled from the
head of one version and the tail of another shows two different round numbers,
which is visible without knowing which version was supposed to be there. After a
power cut, nobody does.

Before that, the workload wrote empty files. **An empty file is always
complete**, so the test could only assert that a name resolved — and it hid
BG-121, in which renaming over a file leaked every block its contents occupied.
The self-test renamed one file over another and ran the whole checker, which is
the right shape; both files were empty, so there was nothing to leak, and the
checker correctly reported a volume with nothing wrong.

**And the harness proves its own checker before it believes a single round**,
by breaking an image three ways and requiring all three to be caught: a byte
flipped inside the live root inode, a reachable block marked unclaimed, and a
*torn* payload — the head of one version with the tail of another.

That third one has to be built by hand, because QEMU does not tear a block. It
is the failure this whole design is arranged against and the one thing here that
cannot be produced naturally, so it is constructed and the checker is required
to notice. The first version
of that control was itself useless — it reached into superblock A's fields
while B was the live one, damaged a block nothing pointed at, and got back a
correct report of a healthy volume. A negative control that misses its target
reports exactly what a working checker reports.

### How crash consistency is tested

`scripts/crash-test.sh` already exists and already cuts the power at swept
moments — it was built for the durability measurement above. ReconFS plugs into
it: run a workload, cut, then **mount the image and assert**. The assertions are
the two-outcome rule made concrete:

- the image mounts
- the forward walk and the reverse sweep agree
- every file is either entirely its old contents or entirely its new contents
- a renamed name resolves to exactly one of two things, never to nothing
- recovery run twice produces byte-identical images

### What that plan cannot cover

It cannot cover a device that lies about its flush — not because QEMU does not
lie, which was the original claim here and is wrong, but because QEMU lies on
request and the cut cannot tell. `crash-test.sh ... nocache` is that lie, and it
passes. What catches a lying *driver* is `flush-reaches-device.sh`; nothing here
catches a lying *device*.

It cannot cover a torn block, because QEMU does not tear one — so the format's
resilience to tearing is *designed* rather than *tested*, and that gap is real
and is written down here rather than discovered later.

Both gaps have the same closer, and it is worth naming so it is not reinvented:
a write-recording layer between the guest and the image — Linux's
`dm-log-writes` is the one built for this — records every write and every flush
and can replay the device to any point, which is what makes "what would a crash
here have left" answerable rather than sampled. It needs privileges this build
environment does not have. Filed as
[issue #273](https://github.com/neogentrics/ReconOS/issues/273) rather than
pretended away.

---

## The two open questions

**Case sensitivity: decided, and the answer is case-preserving and
case-insensitive for lookup.** This is not deferred, because the format has to
know: an index built for one cannot answer the other. The reason is the
constraint that ReconFS must hold `recon_fs`'s shape, and the desktop it holds
is one where `Documents` and `documents` being different folders would be a bug
report from every person who ever used it. Preserving what the user typed and
comparing without case is what a person means by a name.

The cost, stated: comparison is not `memcmp`, and case folding beyond ASCII is a
table this kernel does not have. **ASCII-only folding in the first version, and
a name outside ASCII compares exactly** — which is a limitation that shows up
as "two files that look different both exist", not as data loss.

**Drive letters: still with the installer, and here is the reason.** This one
genuinely cannot be decided here, because it is not a property of the
filesystem. It is a property of how volumes are *presented*, and the format is
identical either way — a volume has a UUID and a label, and whether the shell
shows `C:` or `/System` is a decision above the kernel that costs nothing to
change later. Deferring it does not cost a format change; deferring case
sensitivity would.

---

## What to write tests for first

1. **The two-outcome rule, for rename.** Cut the power inside a rename and
   assert the target is old-complete or new-complete. Anything else — absent,
   mixed, or both names present — breaks the pattern the registry is about to
   depend on.

   **Done**, and it earned its place immediately: the first run of it found
   BG-120, a bug that made the second superblock useless and that nothing inside
   the kernel could have detected.
2. **The reverse sweep, before the forward walk is trusted.** The sweep is the
   independent derivation, and a sweep that quietly uses the reader's path
   resolution is not independent. Test the checker against a deliberately
   corrupted image first, and confirm it *fails*.

   **Done** — five faults, five caught, and one of them was written
   specifically because the sweep had a hole: it treated an owner chain
   arriving at zero as having climbed home, when only the root may have no
   parent. The hole was found by reading the loop, and the fault was added so
   the fix could be *watched* working rather than argued for.
3. **A structure that spans two blocks, and a crash between them.** Since a
   partial multi-block write is unknowable, every such structure must be
   detectable as incomplete from the medium alone. The test writes one, cuts
   between the blocks, and asserts recovery sees an incomplete structure rather
   than a plausible one.

### The rule these three harness bugs bought

Every one of them — the pid that was not the workload, the checker that never
ran, the trace pattern that matched nothing — presented as a clean pass. None
presented as an error. Two of them were in the *same script*, and the second was
found only because the first was being fixed.

> **A test that has never been seen to fail is not a test yet.** Every checker
> here is run once against a deliberately broken subject and required to
> report the breakage before its passing result is allowed to mean anything.

That is why the reverse sweep is second on the list above, and why it says to
corrupt an image and confirm the checker *fails* before trusting it on a good
one. It is not a nicety. It is the only thing that separates these three bugs
from a filesystem whose crash suite is green for the same reason.

---

## What exists

The format is `kernel/include/recon/kernel/reconfs.h`, and it is the part that
freezes. Two things were caught there before it did, both of which would have
been permanent:

**The owner table could not have been a contiguous run.** Under copy-on-write,
changing one entry in a contiguous table means copying the whole table, and on
a 16TiB volume the table is 16GiB. One commit per file write, each rewriting
16GiB, is not slow — it is a layout that cannot be used at all. It is a radix
tree instead, and its depth is *stored* in the superblock rather than derived
from the volume size, so a reader never has to agree with a writer about a
calculation.

**A file would have been capped at two megabytes.** `direct` plus a single
`indirect` is enough for everything the desktop writes today, and "no file may
exceed two megabytes" is not a limitation anyone would accept discovering after
their data was in it. Three levels are in the inode now. The deeper two are not
written yet, and a file that would need one is *refused* rather than silently
truncated — refusing rather than truncating being this project's standing rule.

### The decisions that ended up as bytes

**Every block carries a checksum over itself.** A device block may be 512 bytes,
so a 4096-byte filesystem block is up to eight of them, which makes *every*
filesystem block a multi-block structure by this document's own definition. One
mechanism rather than two: a block whose checksum does not match did not happen.
That covers tearing, a partial write whose transferred count `block_write` does
not report, and a device returning stale bytes.

**The owner table is the free-space map and the back-reference at once.** A
block is free exactly when its owner is zero, and otherwise the owner *is* the
back-reference the reverse sweep needs. One structure, four bytes per block, and
no second free-space structure that can disagree with the first — and no header
inside data blocks, which would have cost file data its block alignment for the
benefit of a checker.

**An inode is a whole block, and its number is that block's number.** Wasteful,
and deliberate: allocating an inode is allocating a block, so create-with-mode
is not a special case — the mode is in the same block as everything else about
the file, and that block becomes reachable in the single write that commits it.
There is no image, crashed or otherwise, in which a file is reachable with a
mode written afterwards. Small files live inside their inode and get most of the
waste back.

The consequence, which is stated because every filesystem trains people to
assume the opposite: **copy-on-write moves an inode on every write, so its block
number is not a stable name.** Anything needing a name that survives a write
uses `object_id`, allocated once from a counter and never reused.

### The checker, and the only result worth reporting about it

`reconfs_check` runs the two derivations and compares them. They read disjoint
fields — the forward walk never reads `parent` or the owner table; the reverse
sweep never follows a directory entry or resolves a name — so their agreement is
evidence rather than construction.

It is not run only on good images. `reconfs=<device>` formats a volume, checks
it, and then breaks it on purpose four ways:

| what is broken | what the checker said |
|---|---|
| a block allocated to nobody | allocated but not reachable from the root |
| a reachable block marked free | reachable from the root but not allocated |
| an inode whose parent is wrong | an inode's parent is not the directory that names it |
| a superblock that does not add up | refused it and believed the other one |
| an owner chain going nowhere | an owner chain ends at an inode with no parent |

**Five of five, on both architectures.** That number is the point, not the fresh
volume passing. A checker that has only ever been run on good images has never
been observed to do anything at all.

### That it formats is why it is gated

`reconfs=<device>` is required, it names the device explicitly, and it refuses
a device carrying a partition table. A self-test that formats the first disk it
finds is the most destructive thing this kernel could contain, and this kernel
is meant to install beside somebody's existing operating system without harming
it. There is no flag to override the refusal.

### The ceiling that should never have been there

The owner in the allocation table was a 32-bit block number, which capped a
volume at 2^32 blocks — **16 TiB** at a 4 KiB block. It was written down as a
documented limit, and being written down is not the same as being acceptable:
20 TB and 24 TB drives are on sale now, and a filesystem that refuses the disk
somebody just bought does not have a limitation, it has a defect.

It was found by being asked about, not by being hit. Every test ran on a volume
a hundred thousand times smaller than the limit, and every one passed — which is
the same failure shape as everything else in this document: the result looked
exactly like success.

**The owner is 64 bits now**, and there is no ceiling left worth stating. What
stops first is elsewhere and is somebody else's: ATA's LBA48 caps SATA at
128 PiB, and MBR caps a partition table at 2 TiB. NVMe carries a full 64-bit
LBA and stops nowhere.

The cost is the owner table doubling from 0.1% of the volume to 0.2%, which is
what the reverse sweep costs. The block size below takes most of it back.

### Block size is chosen when the volume is made

The owner table is eight bytes per block whatever the block is, so its share of
the volume is exactly `8 / block_size` — 0.195% at 4 KiB, 0.012% at 64 KiB.
Larger blocks also mean longer transfers and fewer of them, which both media
prefer.

The cost is the tail of every file: a 1 KB file costs 4 KB at the smallest block
and 64 KB at the largest, on every small file on the volume. So the default
scales gently — 4 KiB under 2 TiB, 16 KiB under 16 TiB, 64 KiB above — and an
explicit size always wins, because the installer is where somebody who knows
what the volume will hold gets to say so.

The size lives in the superblock and nothing in the code may assume the default.
Which forces one thing worth stating: **both superblocks sit at fixed byte
offsets** — zero and 65536 — because if the second one's position depended on
the block size, finding it would require reading the first, and a volume whose
first superblock is damaged would be unrecoverable exactly when the second copy
exists to save it.

`docs/STORAGE.md` has the whole of this, along with what the drivers now report
about the medium underneath.

### The commit path exists

`reconfs_txn_begin` / `reconfs_txn_alloc` / `reconfs_txn_commit`. Every write in
it is one of exactly two kinds, and a reviewer can check that claim line by
line:

1. a write to a block the live superblock's owner table calls unclaimed, which
   by construction no live superblock can reach;
2. the superblock write itself, which is preceded by a flush and followed by
   one.

There is no third kind. If a change ever adds one, the ordering rule is broken
and the filesystem stops being crash-safe — that is the one thing to look for
when reading `reconfs_txn.c`.

Abandoning a transaction needs no undo. Nothing it wrote was ever reachable, so
abandoning is forgetting.

The chicken-and-egg — the owner table records what is allocated, and copying a
table leaf on write requires allocating a block, which changes a table leaf — is
resolved by doing all allocation in memory, against images of the touched
leaves, and writing nothing until every block the transaction will need has been
decided. Leaves are written before the index blocks that name them, because an
index naming a block that does not exist yet is a structure a crash could make
permanent.

**Tested by:** an empty commit (the epoch moves, the superblock alternates, the
volume still checks clean), a commit that takes a block (the owner table is
copied on write), and a remount afterwards — because a commit that is only
correct in the memory of the process that made it is not a commit. All three, at
all three block sizes.

**And one case where a commit refuses.** The allocation table's index blocks are
planned around where the leaves are at that moment. Planning them allocates
blocks, and if one of those allocations happens to dirty a leaf nothing had
touched, that leaf now needs to move — to a place the already-planned index does
not point at. The new copy would be unreachable and the old copy, already
released, would still be named.

That is silent corruption wearing a successful commit's clothes, so the commit
returns `RECONFS_ERR_RETRY` instead. Nothing it wrote was ever reachable, so
retrying is free. It needs the index's own allocation to cross a leaf boundary,
which is rare — and rare is exactly the property that would have made it
impossible to find afterwards.

### Not written yet

Nothing named in the constraints. What is left is the layer above: how a volume
gets found, mounted and presented, which is the installer's question rather than
the format's.

### Constraint 7, answered both ways

*A listing must have a stated guarantee about concurrent modification, or a
stated absence of one, because an unstated guarantee is the worst of the three.*

`reconfs_readdir` takes an index and returns one entry, and it has **no
guarantee at all** — which is now what it says. Between two calls the directory
can change, and worse, the block the caller is holding can be *reused*:
copy-on-write frees the old copy when the change commits, and a later
transaction may allocate it for something else. A caller iterating across such a
change can see an entry twice, miss one, or read a block that is no longer a
directory.

An earlier version of that comment claimed the opposite — "a listing is a
snapshot of one epoch" — which is precisely the failure the constraint was
written against, made worse by being stated confidently. It is withdrawn, and
recorded here rather than quietly corrected.

`reconfs_list` reads the whole directory in one call, and that one *does* have a
guarantee for the simplest possible reason: **a listing with no part-way cannot
observe a change part-way through.** The cost is that the caller supplies a
buffer for the whole directory and is told when it is not big enough, rather
than handed a prefix — a caller given the first two of three names with a
success status has no way to know.

**Moving between directories works.** It is four directory rewrites and two
chains copied to the root, done in sequence against a tree that is consistent at
every step: take the name out of the source and rebuild that chain, then walk to
the destination *in the tree that now exists*, rewrite the moved object with its
new parent, put the name in and rebuild that chain. All four rewrites are one
transaction, so the superblock write at the end makes the whole move real at
once — there is no image in which the name exists in both places or in neither.

Walking the destination from the *new* root rather than the committed one is the
part that is easy to get wrong: after the first rebuild the committed root is
still the old one, and walking from it would find the destination's previous
block and produce a chain that undoes the work.

And this is where the dossier decision pays: the moved object's back-reference
is its parent's dossier, which is stable, so a move rewrites the object and
nothing below it. Had the back-reference been the parent's *block*, moving a
directory would have rewritten every descendant.

**Nested directories work.** Copy-on-write makes them the awkward case: changing
anything in `/System/Recycled` writes a new inode for that directory, which
changes its block, so `/System` must be rewritten to name the new block, which
changes *its* block, so the root must be rewritten too. The chain is copied from
the change up to the root, and one superblock write makes every new block real
at the same instant — a crash anywhere in the middle leaves the entire old tree,
including the directory being changed.

That is split in two on purpose. `reconfs_walk_path` finds the chain down;
`reconfs_rebuild_path` copies it back up once the deepest directory has moved;
in between, the caller uses the single-directory operations unchanged. So there
is one implementation of "create a name" rather than one per depth, and the path
machinery can be read on its own.

The shape the checkpoint was given — System, Programs and User, each with a
recycle bin — is built and checked: three volumes' worth of directories, a file
three levels down written and read back byte for byte, and a path through a file
(`/User/Recycled/note.txt/x`) refused rather than resolved to something nearby.

Removing a name works: `reconfs_remove`, one commit, releasing the object's
contents and its inode. Released and not erased — the blocks keep whatever they
held until something else is written into them, which is worth saying plainly
because "delete" is a word people reasonably expect to mean the other thing. A
directory that still has entries is refused rather than recursed, since a
recursive delete is a decision for the layer that knows whether the user meant
it.

Its test does not check that the name is gone — that is the easy half. It writes
a file large enough to need blocks of its own, records the volume's block count,
removes it, and requires the count to return to where it started. That is what
found BG-122.

Contents work: `reconfs_write_named` and `reconfs_read_named`, whole-file only —
which is what the callers above actually do, since a registry or a theme file is
built in memory and written out entire, and under copy-on-write a partial write
is barely cheaper anyway. Bytes live inside the inode while they fit, then in
the twelve direct pointers, then in one indirect block: about two megabytes at a
4KiB block and half a gigabyte at 64KiB. Beyond that a write is refused, not
truncated.

Tested at every size where the layout changes shape — nothing, one byte, exactly
the inline maximum, one past it, several blocks with a partial tail, and one
block past the direct pointers — read back byte for byte against a
position-dependent pattern, because a file of zeroes has the right length too.

Names and rename exist: `reconfs_create`, `reconfs_lookup`, `reconfs_readdir`
and `reconfs_rename`, with lookup case-insensitive and storage case-preserving.
A directory holds its entries inline while they fit and in its direct blocks
after that, which is about fifteen hundred names at a 4KiB block; more needs the
indirect block, which the format has and the code refuses rather than
truncating.

Renaming between two directories is refused with a status of its own. It needs
the moved object's parent rewritten, both directories rewritten, and the path
from each to the root copied — and that path-copy helper is the next piece of
work, because it is also what nested directories need.

---

## What this refuses to do

No fragmentation story, no extents, no compression, no snapshots exposed to
anyone, and no attempt to be fast. Copy-on-write makes snapshots nearly free and
they are still not offered, because there is no caller and an interface invented
before its first caller gets its shape wrong.

**It will not promise durability on a device whose flush is not real**, and
there is no flag to override that — which is this project's standing rule about
safety checks.
