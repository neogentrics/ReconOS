# Naming in the kernel

Most operating systems inherited their names from whichever system got there
first, and the names carry that system's accidents with them. `/dev/sda` is
named for the order the kernel happened to find it in, which changes when a
cable moves. `C:` is a letter that once meant the third floppy drive. `/mnt`
is short for a word that describes an implementation detail of how the
filesystem was joined on.

None of those are wrong exactly. They are just *someone else's history*, and
this system does not have to inherit it.

That is the opportunity. It is also the trap, because a name invented to be
different is worse than a borrowed one that is understood. So there is a rule.

---

## The rule

> **A name earns its place by saying something the obvious name does not. If the
> plain word is already clear, use the plain word.**

`block`, `inode`, `directory`, `superblock` all stay. They are understood by
everybody who will ever read this code, and renaming them costs every reader a
translation in exchange for nothing.

What gets a new name is what the plain word gets *wrong* — where the ordinary
term is ambiguous, misleading, or collides with something else in this kernel.

---

## Where the names come from

The Crazy Story Universe already contains a system for keeping an enormous,
branching, self-contradicting history straight without letting it drift. That is
the same problem a filesystem has. The correspondence is not decoration; in two
cases below the fiction states a rule this code needed anyway, in clearer words
than the technical literature uses.

Names are taken from there when they fit that well, and not otherwise.

---

## Adopted, and in the code now

### `epoch` — the commit counter

Was `generation`. Two reasons it changed, and only the first is about the story.

`struct block_device` already has a `generation`, meaning how many times that
device slot has been reused. A filesystem's `generation` next to a device's
`generation`, meaning unrelated things, is the kind of collision that is
obvious in a header and invisible at a call site.

And the archive numbers its history in epochs. So does this: an epoch is one
committed state of the whole volume, and the higher valid epoch is the truth.

### `dossier` — the stable identity of a file

Was `object_id`. This one is borrowed whole, because the fiction had already
written the rule down and written it better:

> *Never renumber an existing dossier once it's Canon Locked — that's how a
> retcon sneaks in by accident.*

That is exactly the invariant. A dossier number is allocated once, never
reused, and never renumbered — because copy-on-write moves an inode's *block*
on every write, so the block number cannot be the identity. Reusing a dossier
number would make two files that were never the same file share an identity,
and every reference taken before the reuse would silently point at the wrong
one. A retcon, sneaking in by accident.

### `RECONFS_OWNER_VOID` — space that belongs to nobody

Was `RECONFS_OWNER_FREE`. "Free" reads as an adjective describing a block; the
owner table holds *who owns this block*, and the answer for unallocated space is
"nobody". The Void is the region of the universe that is not claimed by anyone,
which is the same statement.

It also removes a real ambiguity: `free` in a kernel means the verb far more
often than the adjective, and `owner == FREE` reads like a mistake.

### `RECONFS_OWNER_ARCHIVE` — the structure the filesystem stands on

Was `RECONFS_OWNER_FS`. The superblocks and the owner table are owned by nothing
above them and must still not read as unclaimed. `FS` said "this belongs to the
filesystem", which is true of every block on the volume.

---

## Proposed, not yet applied

These are worth doing and are not done, so they are written down rather than
half-applied.

### `the Archivist` — the whole-image checker

Record Beary is *the Archivist*, who records history and keeps continuity
airtight, and who belongs to the group the fiction calls **Observers**: they
"aid and push events along but don't interfere directly."

That is precisely what `reconfs_check` is and is not. It reads the volume by two
independent routes and reports where they disagree. **It writes nothing** — that
is a hard property, it is what makes recovery testable by running it twice, and
"Observer" states it in one word where the code currently needs a paragraph.

### `the causality rule` — the ordering guarantee

Raidus Arbitus, King of Paradox, *enforces causality across the multiverse*.

The ordering rule this whole design rests on is a causality rule and nothing
else: an effect may not be visible before its cause reached the medium. Calling
it that is shorter than "nothing reachable from a live superblock is ever
overwritten, and a change becomes real only when a superblock naming it is
written after a flush that has already put everything it names on the medium" —
which stays as the precise statement, with the short name for referring to it.

---

## Considered and rejected

**Renaming `inode` to `dossier` throughout.** The dossier *number* is a genuine
improvement because it names a rule. The structure is an inode: every reader
knows what an inode is, and nothing is gained by making them learn a synonym.

**Naming the two superblocks after the High Tribunal.** A tribunal of two is not
a tribunal, and "superblock" is understood everywhere.

**Naming the boot stages after the Recon Triad.** The Triad is three polarities;
the boot path is a sequence. Forcing the mapping would mean inventing a third
stage to fit the name, which is the tail wagging the dog.

---

## Devices and volumes: the part still to decide

This is where the borrowed names are worst, and where changing them is a
user-visible decision rather than an internal one — so it is written down here
and **not** decided.

Today the kernel names disks `nvme0n1`, `sata0`, `virtio0`. Those are Linux's
names, and they carry Linux's problem: the number is *discovery order*. Move a
cable and `sata0` becomes `sata1`, and anything that remembered the old name is
now pointing at a different disk.

The block layer already has the fix and does not use it for naming: every device
carries an `id` and a `generation` that together are a stable identity, and every
volume has a UUID and a label.

Three things a scheme has to serve, which pull against each other:

1. **A person has to be able to say which disk they mean**, out loud, to
   somebody on the phone, while looking at a machine.
2. **A stored reference has to survive the hardware moving.** A path written
   into a configuration file last year must still mean the same volume.
3. **It has to be true on a machine with one disk and on a machine with forty.**

The shape that satisfies all three is probably: volumes are referred to by
*label* (`System`, `Programs`, `User` — which is the shape `recon_fs` already
has), devices are named by *where they attach* rather than when they were found,
and neither drive letters nor mount points appear anywhere.

But this belongs with the installer, which is the one place that knows how many
disks there are and what the person wants to call them. Recorded here so it is
decided deliberately rather than by whatever the first caller happens to do.
