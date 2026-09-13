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

**The gate was met on 12 September 2026.** Every row in 1.1 through 1.9 is
Built, which is why the version reads 0.2.x. What remains in the audit is
**section 2, the bootloader**, which was never part of it.

This paragraph asked for 1.7's USB row until 13 September, forty lines above an
entry recording that row closing. A file that records a gate being met and goes
on guarding it is worse than one that does neither, because a reader believes
the part they read first.

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

That is the same shape as KF-186, one level up: a number that is only correct
while somebody remembers is a number that will eventually be wrong, and will
look exactly the same when it is.

---

## 0.2.5 -- 13 September 2026

**Checkpoint 24, the tick half: a tick that stops.** Suspend is its own
follow-up and is not in this.

`time.h` has carried the description of this since the tick was written: *a tick
that fires a hundred times a second on an idle machine is a hundred wakeups a
second doing nothing, and that shows up as a laptop that runs warm... making it
stop when there is nothing to wake for belongs with the scheduler.* That is a
promise with a date on it, and this is the date.

| | wakeups in ~200 ms | a fixed tick would cost |
|---|---|---|
| x86_64, 1 processor | **1** | 21 |
| x86_64, 4 processors | 7 | 20 each -- 80 across the machine |
| aarch64, 1 processor | **1** | 20 |
| aarch64, 4 processors | 7 | 80 across the machine |

### What made it possible, and what nearly made it untestable

**Monotonic time is a hardware counter, not the tick.** It keeps counting
through any amount of idleness, which is the single fact this rests on -- with
the time kept by the tick there would be nothing to catch up against.

But the timer wheel *is* turned by the tick count, so a stopped tick is a wheel
that never turns again and every filed timer simply never runs.
`time_tick_resync()` puts the count where the hardware clock says it should be
and the wheel catches up in the loop it already had. Processor 0 only: a
secondary writing it would race the increment, and a lost race moves the count
*backwards*, which files every pending timer into the past and runs all of them
at once.

**And that resync is why the obvious measurement cannot work.** `time_ticks()`
reads the same after a 200 ms sleep whether the machine woke twenty times or
once. A self-test written against it would pass identically with the tick
stopped and with it running flat out -- KF-187's shape, in the test for the
feature. So what is counted is `time_tick_interrupts()`: interrupts actually
taken, by any processor, never resynced.

### How the tick is suspended

**Processor 0 and the secondaries tick from different chips on x86**, which
shapes the whole thing. Processor 0's tick is the 8254 PIT through whichever
controller took the line; a secondary's is its own local APIC timer.

The way back is the same for both: the local APIC timer, one-shot. On processor
0 that timer turned out to be **calibrated at boot and never started** --
`x86_apic_start_timer` has exactly one caller and it is the secondary bring-up
path -- so it was sitting unused and cost nothing to borrow.

**Nothing boot-critical moved.** The PIT goes on running at 100 Hz in mode 2,
routed exactly as before; only a mask bit moves, and only across the halt. That
restraint is deliberate: KF-157 was this tick being moved and coming back at
201 Hz against a 100 Hz constant with every test still passing.

aarch64 is simpler for a structural reason rather than a lucky one: every
processor has its own generic timer in system registers, so the thing that ticks
and the thing that wakes it are the same timer and there is no line to mask.

**The sleep is bounded at one second even with nothing filed.** Preemption comes
from the tick, so a processor that suspended its own tick also suspended the
thing that would notice work arriving elsewhere. A ceiling turns a hundred
wakeups a second into one -- all of the saving to three significant figures --
and leaves no way to build a machine that has stopped listening.

## 0.2.4 -- 13 September 2026

**Checkpoint 21, the first half: the kernel sets its own display mode.** And
**KF-202**, which is what the version number is actually for -- the feature
rides along, because the rule is a patch per bug *fixed*.

### A mode, on a machine firmware gave nothing

Until now the kernel had one relationship with a screen: it drew into the
framebuffer the bootloader had been handed, at whatever size firmware chose,
and where there was none it had no screen at all. That is most of the matrix --
**every PVH and direct-kernel path boots with `framebuffer : none`**, because
there is no firmware on those paths to have set one up. A display adapter is on
the bus regardless. Nobody had ever asked it for a mode.

`core/display.c` drives the `1234:1111` adapter through its DISPI registers,
reached **through a PCI BAR rather than an I/O port** -- so it lives in `core/`,
compiles for aarch64 untouched, and `make check-portable` stays clean. The
same reasoning that keeps virtio, NVMe and AHCI portable and leaves legacy IDE
in `arch/` (KF-192).

**Every size, not one size.** A panel is 1024x600 on something small, 1080x1280
held in portrait, 3440x1440 across a desk, or 7680x4320 on a wall, and a driver
that knows one of those works on one machine. The mode is chosen from a ladder,
largest first, bounded by the memory the adapter actually reports:

| adapter memory | mode taken |
|---|---|
| 4 MB | 1280x800 |
| 16 MB | 2560x1440 |
| 64 MB | 5120x2880 |
| 256 MB | **7680x4320** |

and the self-test sets seven shapes -- 640x480 up to 8K, portrait and
ultra-wide among them -- reading every one **back out of the hardware** rather
than out of the request, because a driver that records what it asked for passes
any test that asks what it recorded (KF-141). Shapes the adapter has no memory
for are skipped and *counted*, because a sweep that silently skipped everything
looks exactly like one that passed (KF-187). A new matrix path gives the
adapter 256 MB so the large end is exercised on every run rather than by hand.

A dimension past the sixteen-bit mode registers is **refused, not truncated**:
70000 becomes 4464 on the way in, and the adapter would accept it.

### And the console scales with the panel

The glyph was doubled, which is right at 1280x800 and wrong at both ends. An
8x8 glyph doubled is sixteen pixels on a 4320-line screen -- a fortieth of the
height, and a 480x270 character grid, most of it clamped away and left dark.
The scale is chosen from the height now, so the console stays about the same
apparent size on any panel. 800 pixels still comes out at exactly two, which is
what keeps every path that already had a framebuffer looking as it did.

| panel | grid |
|---|---|
| 1280x800 | 80 x 50 |
| 2560x1440 | 106 x 60 |
| 5120x2880 | 91 x 51 |
| 7680x4320 | 120 x 67 |

A machine whose firmware already provided a screen **keeps it**. Re-setting a
working mode is a flicker and a chance of ending with nothing, in exchange for
nothing.

### KF-202: the image the UEFI paths boot had frozen

The new self-test appeared on every path except the UEFI ones. The kernel
extracted back out of the install image was **1,804,808 bytes against the
1,822,488 just built**.

`boot/Makefile` builds the ESP with a timestamp rule, and **something else
writes into that file**: OVMF, booted with `-bios`, keeps its firmware
variables in `NvVars` on the very filesystem it is handed. One UEFI boot leaves
the image newer than the kernel it was built from, and `make esp` is a no-op
from then on.

This is KF-144's sentence word for word -- *the rig would have gone on
reporting those paths green against a kernel that no longer existed* -- arriving
through the one door KF-144 did not close. That fix made the **kernel** rule
phony; it said nothing about the image that carries it, and the image is what
the firmware boots. A dependency hardened at one link of a chain does not
harden the chain, and the comment explaining KF-144 sits four lines below the
rule this was in.

Matrices 31 and 32 were honest only because the script that launched them
happened to clear `boot/build` first. Nothing in `verify-kernel.sh` does.

## 0.2.3 -- 13 September 2026

**KF-201.** The bounded-queue self-test raced its own drain, and passed only
where the machine was slow enough to let the queue fill.

Found by matrix 31 -- the run that existed only because KF-200 had earned a
version bump nothing about the kernel required. One path failed, `PVH, 8
processors`, on a tree whose only change since the last green run was a version
string and some comments.

The assertion floods `RX_QUEUE_MAX + 8` frames and requires an overflow to have
been counted. `netdev_receive` enqueues **and then schedules the drain**, and
the drain empties the queue through `rx_take`. Given a processor with nothing
else to do it keeps up, the queue never reaches its bound, and nothing is ever
dropped.

**The boot said so in its own counters.** `ethernet : ... 72 too short`, and 72
is exactly the flood: every frame reached the ethernet layer, which can only
happen if every frame was drained. Not memory -- 510 MB free. Not the card --
`devices : none found` on that path.

So it passed at one, two and four processors and **passing was the wrong answer
at all four**: it had never exercised the drop path it exists to check. aarch64's
own 8-processor path passed in the same run, which is what a race looks like
rather than a threshold.

- the drain is held for the length of the flood, read inside `rx_take` under the
  lock the queue already uses
- released before the test drains its own frames, because that goes through
  `rx_take` too and holding it there would have leaked seventy-two pages
- a flood that still cannot be built now says *that*, rather than reporting that
  the bound does not hold. Two different sentences, and only one was ever true

Verified both ways, which is the standing rule here: the exact failing
configuration ten times for ten passes, then drop counting removed on purpose
and the test answered `net: 72 frames past a bound of 64 and not one was
dropped` -- proving the flood reaches 72 deterministically and that the
assertion can still fire.

### Two harness faults found on the way

`check()` printed a failure as `grep -aE ': +FAIL|^  [a-z].*: ' | head -10`. The
processor identity block matches the second branch and comes first in the log,
so a real failure printed ten lines of `architecture : x86_64` and **never
reached the FAIL line it was called to show**. Failures print first and alone.

And the sweep log was `cpus_$label.log`, with no processor count in the name, so
each count overwrote the last. The evidence for KF-201 survived only because
eight is the last count the PVH sweep tries.

## 0.2.2 -- 13 September 2026

**KF-200.** The register's own tooling read the register with literal patterns,
against a document that has more than one convention.

`make-issues.py` split it on a hard-coded em dash and fourteen entries had been
typed with `--`; they were not skipped with a warning, they were **not seen**,
and the run printed *139 entries* against a file holding 153. It decided an
issue was closed by matching one of the four forms the register uses for a fix
line, so **thirty of the kernel's seventy-two entries had their state invisible
to the tool that publishes them** -- twenty-one bugs never filed, six fixed ones
standing open. `make-bug-register.py` had both faults too, and reported *178
entries, highest 178* against a file holding 251.

- the splitter takes either prefix and any of three separators, and
  canonicalises the separator when it builds the title
- `is_fixed()` reads the state instead of matching a substring; a `**Status:**`
  line wins where there is one, and *half fixed* reads as open, because it is
- `--check` fails if the `## Open` section stops naming exactly the entries that
  say they are open. It said *"None"*; five entries said otherwise
- the `AREA` table gained the numbers it was missing, and is keyed by the whole
  identifier -- forty-four numbers want a different area in each track
- the page generator fails rather than publishes if the number of headings in
  the file is not the number it parsed

**Why a bump for a change in `scripts/`.** Nothing here alters a kernel
instruction, and raising `VERSION` rebuilds the tree the previous matrix was run
against -- a real cost, and KF-189 is the entry about what happens when that
dependency is missing. The rule is still a patch per bug fixed. *The change does
not feel large enough* is the reasoning the version comment in `kernel/Makefile`
exists to refuse, and it does not become sound because it is being applied to
tooling.

### The same shape three times, in one register's tools

Each of the three printed a total **derived from its own filter**, so no run of
any of them could disagree with itself. The third was the script written to
clean up after the first two: it moved 383 references from `BG-` to `KF-`,
filtering the walk on a list of file extensions, and reported *383 references in
69 files*. A `Makefile` has no extension. Twenty-one references in
`kernel/Makefile`, `boot/Makefile` and `boot/bios/Makefile` were still `BG-` a
day later, found by reading one of those files for an unrelated reason.

The defence is not a better pattern -- each of the three was written
deliberately and each was nearly right. It is a second count taken a different
way: `grep -c '^### '` for the register, `grep -rl` with no filter for the tree.

## 0.2.1 -- 12 September 2026

**The gate is met.** Every row in sections 1.1 through 1.9 of the blueprint
audit is Built: 55 rows, with 3 partly and 2 not built remaining, and all five of
those in **section 2, the bootloader**, which was never in the gate.

The rule, set on 11 September, was *a patch per bug fixed, and 0.2.0 is not
reached until 1.1 to 1.9 are all built* -- deliberately not a judgement about
what counts as a big change, but a fact about those tables that anybody can
check, including a script. It is checkable, and it checks out.

**It took three verification runs in one day.**

| run | closed | self-tests |
|---|---|---|
| matrix 24 | 1.3 signals, 1.4 the HPET, 1.6 ext2 and procfs, 1.7 I2C/SPI | 1046 |
| matrix 26 | **1.8, the network stack** -- 0 of 4 and the largest single thing left | 1065 |
| matrix 29 | **1.7's USB row**, with hot-plug | 1065 |

Every one green on all twenty-five boots with nothing skipped.

**The count going up is the evidence, not the pass.** 53 self-tests per path
became 54 when the network stack's own test joined them, which is how it is
known to have *run* rather than skipped -- a skipped test and a passing test look
identical in a total, which is what KF-187 was about. Nothing was moved to Built
on the strength of a run reporting the same number as the one before it.

**Two rulings were needed, and both were made in the open.** `ext4` moved to
section 2.3, because reading an ext4 volume is an installing-beside-Linux
question and that is where NTFS, APFS and HFS+ already sit. `EHCI` stays named
in 1.7 and does not gate it, because the row means *USB works on the machines
this kernel runs on* -- and every machine it runs on presents xHCI. Neither
changed what is built; both changed what a row is asking for, which is a
different thing and is why each says so on its own row. **A row renamed quietly
to let a gate pass is how a checklist stops being worth reading.**

**Named, absent, and outside the gate:** IPv6, which the blueprint adds to 1.8's
layer 3; EHCI, above; and KF-192, a kernel that boots from a disk over BIOS and
cannot see it, because that disk is IDE and there is no IDE driver.

The `.1` is KF-199's fix, which landed with it.

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
has. That is the same boundary that keeps legacy IDE *out* of `core/` (KF-192).

What is here: virtio-net over both transports; packet buffers with headroom, so
a header stack is prepended without copying anything; Ethernet; ARP with a cache
that expires and evicts the oldest rather than the first; IPv4; ICMP echo; UDP
with the pseudo-header checksum; a TCP state machine with the specification's
own state names, random initial sequence numbers, retransmission with backoff,
and a window that follows the free space in the receive buffer; BSD sockets; and
a DHCP client.

**Three bugs, one bump each.**

**KF-194** -- virtio-net used a descriptor index as a ring slot, twice. It
stashed a slot number in `desc[head].next`, which is the field that chains the
head to the frame's descriptor and is read by the device; and it indexed 64
descriptors into a 32-entry transmit table, so two frames in flight shared one
and a buffer would have been freed twice. One mistake in two places: borrowing a
field that already has an owner.

**KF-195** -- a broadcast could not leave a card with no address, which makes
DHCP impossible to run. Right for ordinary traffic and wrong for the one
protocol whose job is to run *before* there is an address. No self-test could
have found it: every test configures its device by hand first, which is the
exact condition under which the bug cannot occur.

**KF-196** -- the ARP expiry test wrote zero to mean "long ago", on a machine
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

**`virt_to_phys` answered for addresses it cannot answer for.** (KF-193) The
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
KF-179 for the third time: `i2c_init` ran before the PCI bus was walked.

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

**The BIOS loader hands over a clean machine.** (KF-191) `reconboot` clears every
register it does not need; the BIOS loader did not, under a comment stating that
*the kernel must not be able to tell which loader started it*. It could:
`rbx arrived holding something`.

**The processor identity check knows whether there is a map.** (KF-190) On a
machine with no MADT nothing is ever registered, both APIC maps stay zero, and
the check read slot 0 as a real processor holding APIC 0. It reported the map as
broken on a machine that had no map.

**Changing the version rebuilds the kernel.** (KF-189) It did not. `VERSION` is
passed with `-D` and the Makefile was not a prerequisite of any object, so the
binary went on printing 0.1.0 out of a tree that said 0.1.7 -- and matrix 23
passed 951 self-tests against the mislabelled kernel. A clean build was always
right; only incremental builds, which is every build anybody does, were wrong.

**Open, and the largest of the four:** the kernel boots from a disk over BIOS and
then cannot see it (KF-192). The disk is IDE and there is no IDE driver, so a
machine with no UEFI starts ReconOS and has no storage. The machines with no UEFI
are the same machines likely to present their disk that way, which makes this the
configuration the BIOS bootloader exists to serve and the one the kernel can
least use.

## 0.1.7 -- 11 September 2026

**Recovery does not write to the volume, and the volume is what refuses.**
(KF-188) Recovery promised to look and not touch, and nothing enforced it -- the
promise was kept by every caller happening not to write. A self-test that
replaces a file broke that on every boot. The volume is mounted read-only on a
recovery boot now, refused at the one place a change can begin rather than by
each caller remembering.

Cleaning up after the write would not have been enough: ReconFS is
copy-on-write, so a file created and then deleted still moves the root and still
changes the disk. Only not writing keeps a byte-for-byte promise.

**Five self-tests need a volume and no matrix path has one.** (KF-187) Every
boot path attaches a blank disk, so they print "no volume on this machine" and
are counted as having run. A skipped test and a passing test look identical in a
total.

**Requests reach the disk in an order**, and the block layer's transfer counters
are printed. (KF-186) They had been incremented since the block layer was
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

- **KF-186** -- transfer counters incremented and printed nowhere.
- **KF-185** -- the loader's ELF reader trusted a signature check the default
  build does not perform. One byte changed in a file on the EFI partition faults
  the firmware before the kernel starts.
- **KF-184** -- the page cache key named the file but not the filesystem, so
  ramfs slot 10 and volume dossier 10 collided.
- **KF-183** -- the reapers had no callers. Both were written, both correct, and
  each had exactly one caller: a self-test.
- **KF-182** -- tearing down an address space freed the page tables and not the
  pages they pointed at, under a comment saying the memory was somebody else's
  job. There was no somebody else. Twelve pages leaked per program.
- **KF-163** -- two disks of one kind stalled the boot. A legacy interrupt line
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
