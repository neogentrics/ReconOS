# Disks, drives, and how big they are allowed to get

Two questions, asked together because they have the same answer: *the kernel
must not carry a limit it inherited from nobody, and must not treat two very
different media as one.*

---

## How big a disk can this hold?

**No ceiling worth stating.** Where the numbers stop:

| Layer | Limit | Why |
|---|---|---|
| ReconFS volume | 2^64 blocks — 64 ZiB at the smallest block size | `total_blocks` and every owner are 64-bit |
| NVMe | none reachable | the command carries a full 64-bit LBA |
| AHCI / SATA | 128 PiB | ATA's own LBA48, not ours |
| GPT | 8 ZiB | the partition table's 64-bit LBAs |
| MBR | 2 TiB | inherent to MBR; a bigger disk needs GPT, which is why hybrid tables are read |

### It was 16 TiB, and that was wrong

The owner in the allocation table was a 32-bit block number, capping a volume at
2^32 blocks — 16 TiB at a 4 KiB block. That was *documented*, which is not the
same as being acceptable: 20 TB and 24 TB drives are on sale now, and a
filesystem that refuses the disk somebody just bought does not have a
limitation, it has a defect.

Widening the owner to 64 bits costs 0.2% of the volume rather than 0.1%, which
is the price of the reverse sweep being an independent derivation of what is
allocated. It is worth it, and the block size below takes most of it back.

### Why the arithmetic is tested at sizes no disk here can reach

The ceiling was found by being asked about, not by being hit — every test ran on
a volume a hundred thousand times smaller than the limit, and every one passed.

So `reconfs_layout_self_test` runs the layout arithmetic at volumes from 1 GiB
to 1 EiB, on every boot, with no device involved. It compares what the format
computes against a derivation written separately from the definition, so the two
can disagree; a test that derives the answer and then checks its own answer
proves only that it agrees with itself, which is what the first version of it
did.

It was checked by reintroducing a depth bug, and it named the sizes it should:
2^32 blocks of 4096 — exactly the old ceiling — and 5,859,375,000 blocks, which
is a 24 TB drive.

---


## What the kernel can and cannot know about a drive

Whether to tell a drive that blocks have stopped being needed depends on whether
it is flash. Today that is answered from the transport:

| Transport | How | Reliable |
|---|---|---|
| NVMe | everything on it is flash | yes, by construction |
| ATA | IDENTIFY word 217, nominal rotation rate | yes |
| **USB mass storage** | **nothing to read** | **no — see KF-127** |

USB mass storage is SCSI in a wrapper: no IDENTIFY word 217, no NVMe identity.
Linux, given a USB flash drive with no moving parts, reports
`/sys/block/*/queue/rotational : 1`, because when nothing says otherwise the
block layer assumes rotating. Measured on real hardware, not inferred.

So the answer has three states and not two — rotating, solid state, and
**unknown** — and the discard path must decline to guess rather than default
either way. Defaulting to "rotating" wears out every external SSD attached to
ReconOS; defaulting to "flash" sends TRIM to devices that will reject it.

For USB specifically there is a real answer to read: the SCSI Block Device
Characteristics VPD page carries a medium rotation rate, where 1 means
non-rotating. Checkpoint 11b should ask that rather than infer from the bus.

## Making the drive's own nature visible

Three facts now travel with every block device, because a filesystem cannot make
one important decision without them and had been guessing:

```
bool seek_is_free;        /* solid state: any block is as good as any other */
bool discard_supported;   /* the device wants to be told about freed space */
u32  transfer_hint;       /* bytes it would rather move at once; 0 = did not say */
```

**`transfer_hint` is declared and nothing sets it yet** — every driver leaves it
zero, which is the documented "did not say". It is listed here because the
allocator will want it, not because it works; a field with no writer is a promise
the system has not earned, and saying so is cheaper than discovering it.

### `seek_is_free` is the one that matters

On solid state a seek costs nothing and scattering a file across the volume is
free. On a spinning disk a seek costs milliseconds — five orders of magnitude
more than the transfer — and a file scattered across the platter reads at a
fraction of the drive's sequential speed.

Copy-on-write scatters by nature. So for ReconFS this is not a tuning detail: it
is the difference between a filesystem that is pleasant on a hard disk and one
that is unusable on it.

**It defaults to false**, which is the safe direction. Treating an SSD as a disk
costs a little allocator effort and nothing else. Treating a disk as an SSD
fragments it in a way that cannot be undone without rewriting the volume.

### Where each driver gets its answer

- **AHCI asks the drive.** IDENTIFY word 217 is the nominal media rotation
  rate: `1` means non-rotating, `0x0401`–`0xFFFE` is the RPM, anything else
  means it did not say. Asked rather than inferred from the bus, because SATA
  has carried both kinds for fifteen years. The driver prints what it found —
  `solid state`, `7200 rpm`, or that the drive would not say and it is assuming
  the worst.
- **NVMe knows from the bus.** It is flash on a PCIe link; no NVMe device has a
  platter.
- **virtio-blk cannot know.** Whatever the host put underneath is invisible, so
  it says false and means it.

### Discard, and why an unsupported one is not success

A solid-state drive cannot overwrite in place: it erases a much larger region
and rewrites it, preserving everything else in that region — including blocks
nobody will ever read again, because nothing told it so. A drive never told
about freed space gradually behaves as though it is full even when the
filesystem says it is half empty.

Copy-on-write makes this sharper than for most filesystems: **every write frees
the block it replaced**, so a ReconFS volume produces freed space continuously,
not only when files are deleted.

`block_discard` exists now, and NVMe implements it as Dataset Management with
the Deallocate bit — a real command, asked for only after Identify Controller's
ONCS field says the controller has it.

A device with no discard returns `BLOCK_ERR_UNSUPPORTED`, **not** `BLOCK_OK`.
Returning success would tell a caller the drive knows about the freed space when
it does not — the same shape as the virtio-blk flush that returned success
having issued nothing, which this kernel already had once. Once is a mistake.
Twice would be a pattern nobody looked for.

`discard_supported` is set only when the drive says yes **and** the driver can
act on it. AHCI currently detects TRIM and does not issue it, so it reports
false — the drive's answer alone would be a claim the block layer could not
honour.

---

## Block size, chosen when the volume is made

The owner table is eight bytes per block whatever the block is, so its share of
the volume is exactly `8 / block_size`:

| Block | Table overhead | On a 24 TB drive |
|---|---|---|
| 4 KiB | 0.195% | 47 GB |
| 16 KiB | 0.049% | 12 GB |
| 64 KiB | 0.012% | 3 GB |

Bigger blocks also mean longer transfers and fewer of them, which both media
prefer and which an SSD controller especially prefers.

The cost is the tail of every file. A 1 KB file costs 4 KB at the smallest block
and 64 KB at the largest — **sixteen times the waste**, on every small file on
the volume, of which a system disk has hundreds of thousands.

So the default scales gently, and an explicit choice always wins:

| Volume | Block | Because |
|---|---|---|
| under 2 TiB | 4 KiB | a system disk; small files dominate |
| under 16 TiB | 16 KiB | large enough that metadata starts to matter |
| 16 TiB and up | 64 KiB | an archive or media volume; large files dominate |

The installer is where somebody who actually knows what the volume will hold
gets to say so.

### The first version of this rule never fired

It grew the block size while the owner table exceeded a fiftieth of a percent of
the volume. The table's share is 8/block_size, which is 0.195% at the smallest
block — already under the threshold. The condition was true on the first test at
every size from 64 MB to 24 TB, and the function returned 4096 always.

It read like a rule and behaved like a constant, and nothing in its output said
so. Recorded because it is the same failure as the harness bugs earlier in this
work: **something that looks like it is working, is not, and produces a
plausible number every time.**

---

## Still to do

- **Allocation policy that uses `seek_is_free`.** The facts are recorded and
  nothing reads them yet, because the allocator is a first-fit scan and there is
  no file data to place. On a spinning disk it will need to keep a file's blocks
  near each other and near its inode.
- **Batching discard.** It is issued now, one command per freed block, after the
  commit is durable. One command per block is the naive shape; NVMe's Dataset
  Management carries up to 256 ranges per command and should be used that way
  once there is a workload that frees many at once. There is not yet, and a
  batching path with no caller gets its shape wrong.
- **AHCI TRIM.** Detected, not issued.
- **A read cache.** Nothing caches anything yet; every read reaches the device.
  This matters far more on a spinning disk than on an SSD, which is the same
  distinction as above and should use the same recorded fact.
