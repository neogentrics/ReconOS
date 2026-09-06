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
cut the power at swept moments twenty-eight times across both architectures and
saw no torn block and no write out of order — but QEMU is not a disk. That
result means the kernel and the drivers order correctly. It is not a promise
about a cheap SSD that lies about its cache.

So: **any structure spanning more than one block must be self-validating on
read.** Either a checksum over the whole run, or a commit record whose presence
is the only thing that gives the run meaning.

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

The forward walk follows pointers. The reverse sweep visits every block marked
allocated, reads whose it is, and walks up to the root. If the two disagree, one
of them is wrong, and neither was computed by the other. That is the closest
thing to a second opinion a single author can build, it costs bytes per object,
and **it must be in the first version or never.**

The whole-image checker must not call any function the reader uses to resolve a
path. If checking requires the reader, the checker is not a second opinion.

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

It cannot cover a device that lies about its flush, because QEMU does not lie.
It cannot cover a torn block, because QEMU does not tear one — so the format's
resilience to tearing is *designed* rather than *tested*, and that gap is real
and is written down here rather than discovered later.

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
2. **The reverse sweep, before the forward walk is trusted.** The sweep is the
   independent derivation, and a sweep that quietly uses the reader's path
   resolution is not independent. Test the checker against a deliberately
   corrupted image first, and confirm it *fails*.
3. **A structure that spans two blocks, and a crash between them.** Since a
   partial multi-block write is unknowable, every such structure must be
   detectable as incomplete from the medium alone. The test writes one, cuts
   between the blocks, and asserts recovery sees an incomplete structure rather
   than a plausible one.

---

## What this refuses to do

No fragmentation story, no extents, no compression, no snapshots exposed to
anyone, and no attempt to be fast. Copy-on-write makes snapshots nearly free and
they are still not offered, because there is no caller and an interface invented
before its first caller gets its shape wrong.

**It will not promise durability on a device whose flush is not real**, and
there is no flag to override that — which is this project's standing rule about
safety checks.
