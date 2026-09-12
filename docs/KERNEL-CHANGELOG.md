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

**One row stands in the way.**

| section | what is not built |
|---|---|
| 1.7 Device drivers and buses | USB host controllers -- **EHCI and hot-plug**. xHCI is built and so are hubs; ports are still read once at boot |

**Every other row in 1.1 through 1.9 is Built.**

### What closed, and when

Four rows went over together on 12 September, in matrix 24 -- one verification
run, because they were written in one stretch and there is no sense in claiming
them one at a time:

| section | what closed |
|---|---|
| 1.3 Process, thread, execution | **signals**, on both architectures. Sockets and message queues wait on 1.8 |
| 1.4 Interrupts and timers | **the HPET**, found through ACPI and checked against the TSC across a measured wait |
| 1.6 Filesystem and storage | **ext2 and procfs**. With ramfs and devfs already built, the row is complete |
| 1.7 Device drivers and buses | **I2C / SPI** -- the PIIX4 SMBus this machine has; SPI is declared with no driver and says so every boot |

And 1.8 closed on the same day, in matrix 26: **1065 self-tests across every
path, none skipped**, on a tree frozen at the commit under test.

> **Matrix 25 was killed and discarded rather than read.** The kernel was
> rebuilt four minutes after that run started, so some paths would have tested
> one binary and some another. A run like that does not fail -- it finishes and
> reports a number describing no kernel that ever existed. The rule it broke was
> already written down; the fix was to commit everything first, clear the build
> directory, and then start.

> **ext4 moved to section 2.3 on 12 September**, by decision rather than by
> being dropped, which is why 1.6 closed rather than staying open on it. Reading
> an ext4 volume is an *installing beside a Linux that is already there*
> problem, and that is the same question NTFS, APFS and HFS+ already sit under
> in 2.3. `core/ext2.c` refuses EXTENTS by name, so ext2 being read is not ext4
> nearly read -- it is a separate implementation, and leaving it against 1.6
> would have kept a kernel row open on a bootloader problem.
>
> The row was titled "ext2/4" and is now titled "ext2". Said here rather than
> done quietly: a row renamed to let a gate pass is how a checklist stops being
> worth reading.

> **Corrected 12 September.** This table previously said eleven rows and listed
> self-relocation, video, embedded filesystems, initrd and UEFI. Those are
> **section 2, the bootloader**, and the gate is 1.1 to 1.9. The script that
> produced the list matched only headings beginning `1.`, so five bootloader
> rows were filed under 1.9 and counted against a gate they are not part of.
> The bootloader has five open rows of its own and they are tracked separately.

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

## 0.1.14 -- 12 September 2026

**The network stack, and the three faults it cost.** Section 1.8 was 0 of 4
built and the largest single thing left on the audit.

**Something at the other end answered.** Measured on both architectures against
QEMU's own network: a full DHCP exchange -- DISCOVER, OFFER, REQUEST, ACK --
giving `10.0.2.15` and a gateway, and then an ICMP echo answered in 447
microseconds on x86_64 and 318 on aarch64. Four packets in, four out, no drops
and no errors. Every layer below can be proved against itself; this is the one
claim in the stack that no self-test can make.

**Thirteen files, all under `core/`, and `make check-portable` is still clean.**
The whole stack is portable, because every card it can talk to is reached
through a mapped BAR rather than through an instruction only one architecture
has. That is the same boundary that keeps legacy IDE *out* of `core/` (BG-192).

What is here: virtio-net over both transports; packet buffers with headroom, so
a header stack is prepended without copying anything; Ethernet; ARP with a cache
that expires and evicts the oldest rather than the first; IPv4; ICMP echo; UDP
with the pseudo-header checksum; a TCP state machine with the specification's
own state names, random initial sequence numbers, retransmission with backoff,
and a window that follows the free space in the receive buffer; BSD sockets; and
a DHCP client.

**Three bugs, one bump each.**

**BG-194** -- virtio-net used a descriptor index as a ring slot, twice. It
stashed a slot number in `desc[head].next`, which is the field that chains the
head to the frame's descriptor and is read by the device; and it indexed 64
descriptors into a 32-entry transmit table, so two frames in flight shared one
and a buffer would have been freed twice. One mistake in two places: borrowing a
field that already has an owner.

**BG-195** -- a broadcast could not leave a card with no address, which makes
DHCP impossible to run. Right for ordinary traffic and wrong for the one
protocol whose job is to run *before* there is an address. No self-test could
have found it: every test configures its device by hand first, which is the
exact condition under which the bug cannot occur.

**BG-196** -- the ARP expiry test wrote zero to mean "long ago", on a machine
whose clock starts at zero. Four seconds into a boot, an entry stamped zero is
four seconds old, not stale. Fixed by subtracting from *now*, which is correct
even when it underflows -- the same unsigned-difference property TCP uses to
compare sequence numbers across the wrap.

**Deliberately absent, written down rather than discovered:** IPv6; IP
fragmentation and reassembly, refused rather than attempted, because a
reassembly queue is where a decade of security holes lived; out-of-order TCP
segments, dropped rather than queued for the same reason; window scaling, SACK
and timestamps; a measured retransmission timeout; and DHCP lease renewal.

**Not 0.2.0.** The gate is every row in 1.1 to 1.9 Built, and 1.7's USB row is
still Partly -- EHCI and hot-plug. A minor bump here would be a judgement about
how large the change feels, which is what the rule exists to replace.

## 0.1.11 -- 12 September 2026

**Five things built, one bug that had to be found before any of them could be
trusted, and none of it through a verification run yet.**

**`virt_to_phys` answered for addresses it cannot answer for.** (BG-193) The
direct-map test was one-sided, and the kernel image runs *above* the direct map
base -- so a stack pointer subtracted to a plausible-looking physical address
about a hundred and forty terabytes in. `virtio_blk` has always had the right
guard with the right comment, checking for zero; the function it depended on
never returned zero, so the check could not fire. A read was entered, queued,
issued and reported `BLOCK_OK` with the caller's buffer untouched. Invisible
until now because every other caller in the kernel hands drivers pages from the
page allocator, which are direct-map addresses by construction. Same fault and
same constants on aarch64, fixed in the same change.

**Signals, on both architectures.** Delivery happens on the way back to user
mode and nowhere else -- sending records, returning delivers. Default actions,
per-thread masks, and handlers that run in ring 3 on the program's own stack
with a caller-supplied restorer. The return path gained no branch on either
architecture: the same fixed sequence of pops now reads registers that may have
been edited. Where a handler *returns to* turned out to be architectural -- a
word on the stack on x86_64, the link register on aarch64 -- and getting that
wrong let the handler run perfectly and then return to wherever x30 happened to
point.

**procfs.** `/proc` with version, uptime, meminfo, cpuinfo and self. Every entry
is generated once at open, into a buffer reads are served from, so a program
reading `meminfo` in two goes cannot get the first half of one machine and the
second half of another. `self` is the entry that earns the filesystem: it
answers differently depending on who opened it.

**The HPET.** Found through ACPI, 100.0 MHz, 10,000,000 femtoseconds per tick,
and checked against the time stamp counter across a measured wait. The
femtosecond arithmetic is split so it cannot overflow -- the obvious form wraps
five hours after boot. Enabling the counter is not treated as evidence it runs.

**I2C, on the PIIX4 SMBus this machine actually has.** 112 transfers, 104
correctly reporting nobody answered, 0 timed out, 8 devices that did. SPI is
declared with no driver and says so every boot, because there is no SPI
controller on either machine this kernel runs on. Finding the controller took
BG-179 for the third time: `i2c_init` ran before the PCI bus was walked.

**USB hubs.** A disk behind a hub, enumerated and usable. xHCI does not address
a device by the chain of hubs it hangs off -- it wants the root port plus a
twenty-bit route string -- which is why this needed the addressing path
generalised rather than a hub driver bolted on.

**ext2: read, check, and a repair that must be asked for by name.** Reads a
filesystem `mke2fs` wrote: geometry matching `dumpe2fs`, files from the root and
one directory down, and a checker that agrees with `e2fsck` about a clean
volume. Repair rewrites free counts from the bitmaps -- the bitmap is the
evidence and the count is the claim -- and refuses anything needing a guess
about which file a block belongs to. It never happens on mount and requires the
literal word `repair`.

## 0.1.10 -- 12 September 2026

**Found by one assertion added to a boot nothing had been reading.**
`install-then-boot-test.sh` captured the BIOS boot's serial output in full and
grepped it only for the kernel banner and a partition count. Asking whether its
self-tests passed turned up three faults and one gap, two of which had been
failing on every BIOS boot since the checks were written.

**The BIOS loader hands over a clean machine.** (BG-191) `reconboot` clears every
register it does not need; the BIOS loader did not, under a comment stating that
*the kernel must not be able to tell which loader started it*. It could:
`rbx arrived holding something`.

**The processor identity check knows whether there is a map.** (BG-190) On a
machine with no MADT nothing is ever registered, both APIC maps stay zero, and
the check read slot 0 as a real processor holding APIC 0. It reported the map as
broken on a machine that had no map.

**Changing the version rebuilds the kernel.** (BG-189) It did not. `VERSION` is
passed with `-D` and the Makefile was not a prerequisite of any object, so the
binary went on printing 0.1.0 out of a tree that said 0.1.7 -- and matrix 23
passed 951 self-tests against the mislabelled kernel. A clean build was always
right; only incremental builds, which is every build anybody does, were wrong.

**Open, and the largest of the four:** the kernel boots from a disk over BIOS and
then cannot see it (BG-192). The disk is IDE and there is no IDE driver, so a
machine with no UEFI starts ReconOS and has no storage. The machines with no UEFI
are the same machines likely to present their disk that way, which makes this the
configuration the BIOS bootloader exists to serve and the one the kernel can
least use.

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
