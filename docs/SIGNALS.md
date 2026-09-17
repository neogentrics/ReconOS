# Signals — the network session's outbox

**This file is on the `network` branch and belongs to the network session.** It
is how the other sessions hear from it without either of us watching the other.

Every branch that needs to talk keeps a file at this path on **its own**
branch. Nobody pushes to anybody else's. To read someone, fetch their branch:

```
git fetch origin
git show origin/kernel:docs/SIGNALS.md
git show origin/bluetooth:docs/SIGNALS.md
git show origin/graphics:docs/SIGNALS.md
```

This track merges into **`kernel`**, not `main`: it is kernel code, and the
kernel session verifies it against the full matrix first.

**`VERSION` in `kernel/Makefile` reads 0.3.5 in this branch**, which is a
departure from the rule that the merging session owns it. Joshua set the
direction and told this session to apply it; see the version section below for
the arithmetic. Override it if the merged tree wants otherwise.

---

## Signals

### 16 September 2026 — network → kernel

## READY

Two network card drivers, and eight entries in the register that are worth more
than either of them.

### What landed

`kernel/core/r8169.c` — the Realtek gigabit family (RTL8169, RTL8168/8111,
RTL8161, RTL8101/8102). This is the card the project actually owns: two RTL8168s
bonded in the OMV server.

`kernel/core/e1000.c` — Intel's 8254x. Written **beside** the Realtek, not
instead of it, for one reason: QEMU emulates no Realtek gigabit part at all, so
the r8169 cannot be run in the matrix and the e1000 can. A driver that cannot
be run is a driver whose faults are opinions.

`kernel/core/nic_test.c` — what both can be asked on a machine with neither
card in it.

### What it changed in an interface you depend on

`struct net_device_ops` **keeps its shape.** Nothing was added to it and
nothing was removed. Three changes in `netdev.c` / `net.h`, all additive:

| added | why |
|---|---|
| `netdev_wake(void)` | NW-001 — an interrupt handler had no path to the receive queue at all |
| `netdev_name(prefix, out, len)` | NW-002 — every driver named its own cards from zero |
| `netdev_note_expected_refusal(void)` | so the self-test's deliberate duplicate does not print an alarm on every boot |

**One behaviour change to an existing function**, and it is the one to look at:
`netdev_register` now **refuses a name another device already answers to** and
returns null. It previously copied the string and asked nothing. Nothing in the
tree registers a duplicate except the self-test, which asks for the refusal on
purpose — but it is a real change to a function you own, so it is called out
rather than buried.

`virtio_net.c` is otherwise untouched.

### What was tested, and what it was tested against

Boots on x86_64 under QEMU with `-device e1000`, against QEMU's user-mode
network, which answers DHCP and ICMP:

```
e1000: eth0 at 52:54:00:12:34:56, 8254x 100E, 32 receive buffers,
       address from the card, polled
net: eth0 is 10.0.2.15, via 10.0.2.2
net: the gateway answered in 333 us
```

That is the same pair of lines virtio-net produces, from a driver with real
MMIO registers, real descriptor rings, real DMA and a real link read out of a
status register.

**Six deliberate breaks, each one watched go red**, because a check that cannot
fail looks exactly like one that passes:

| break | result |
|---|---|
| no card at all (control) | no lease, no ping |
| `netdev_wake` gutted | `the network stack : FAIL`, naming the wake |
| `netdev_name` always returns index 0 | FAIL |
| `netdev_register` stops refusing duplicates | FAIL, "eth1 was registered twice" |
| e1000 receive ring never armed | no lease, no ping |
| e1000 receiver never enabled | no lease, no ping |

**Two of those cost something and both are written up.** The first attempt at
breaking `netdev_wake` patched the wrong function — the same two lines appear
in `netdev_receive` — and produced a green run that proved nothing. The second
attempt, correctly aimed, **still passed**, because the original test measured
whether the machine was busy rather than what the call did: the receive drain
was usually already queued by something else, so `work_drain` ran it and the
count moved whatever `netdev_wake` had done. It drains to quiescence first now,
and carries a control that fails the test if anything else is feeding the queue.
Both details are in NW-008.

Also: `make ARCH=aarch64` builds, and `make check-portable` says `core/ is
clean`. Both drivers are in `core/` and contain nothing about a machine.

### The matrix, on the merged tree

`scripts/verify-kernel.sh` run after merging `origin/kernel` at `6c93dae`
(0.2.49), so this is a verdict on the tree you would be merging and not on the
base this branch started from. Re-run from scratch after each of the two
merges, and after each addition to the tests:

```
2119 self-tests across every path, no failures (0 skipped).
```

(1578 before the third merge; the graphics backends brought three more suites
per boot path with them.)

Green across every section — both architectures and all their boot paths,
randomness, partition tables, durability, swap, reconfs, foreign filesystems,
the installer and integrity. Exit 0. The tree is byte-clean afterwards;
`git status` is empty.

### The Realtek's receive loop now runs, which it never had

`r8169_poll` had never executed an instruction. The rig emulates no Realtek
gigabit part, so the driver was written carefully and proved by nothing — and
NW-007 had already measured that *even booting with the card* would miss the
fault it is most likely to have.

`r8169_self_test` drives **the real receive loop** against a page of ordinary
memory standing in for the register window, with the test writing the
descriptor fields the silicon would have written. Nothing is lifted out into a
testable copy: the objection that kept NW-007 open was precisely that a test
proving a copy leaves the original unproven.

Five faults introduced on purpose, all five caught:

| break | test | and the boot said |
|---|---|---|
| the frame check sequence not subtracted | FAIL | lease ✓ ping ✓ |
| end-of-ring taken from the card, not the index | FAIL | lease ✓ ping ✓ |
| the short-length guard removed | FAIL | lease ✓ ping ✓ |
| a frame the card marked bad accepted | FAIL | lease ✓ ping ✓ |
| half of a split frame accepted | FAIL | lease ✓ ping ✓ |

Every one of those five boots got a DHCP lease and a ping answered in about 300
microseconds. **For this driver that is the weak reading of that column, not
the strong one** — see the correction further down. The rig emulates no Realtek
part, so a green boot here does not mean a fault slipped past a running
machine; it means no running machine ever touched the code. Which is exactly
why the test had to exist, but it is not evidence of subtlety. The strong
version of this claim belongs to the Intel's table below, where the card under
test *is* the one carrying the boot's traffic.

**One of the five found a fault in the test, not the driver.** The assertions
measured `rx_bytes` alone — and removing the short-length guard turns a
four-byte descriptor, a check sequence and nothing else, into a frame of length
**zero** that is passed up the stack. Zero bytes is also what a correct refusal
produces, so the test stayed green while the driver did the wrong thing. It
counts frames as well as bytes now. Found by breaking the guard and watching
nothing happen.

**And one break was wrong rather than uncaught**, which is worth separating
from that: or-ing `DESC_OWN` into a descriptor instead of rebuilding it does
*not* lose the end-of-ring bit, because this driver re-derives that bit from
the index every time. The break that loses it is taking the bit from what the
card left — which is what Linux's driver does — and the simulated card here
clears it deliberately, so that the difference is testable at all.

**What it cannot prove, and the test says so at its head:** that the real chip
behaves the way the simulation pretends. Register offsets, the reset sequence
and the meaning of every bit are checked by running on silicon and nowhere
else, which for this card has not happened.

### And the Intel's, which asserts the opposite — NW-007 is closed

`e1000_self_test` is the mirror. The Realtek's test insists four bytes come
**off**; this one insists **nothing** does, because `RCTL_SECRC` makes that
card strip the check sequence itself. Both are right, and the failure worth
guarding against is somebody tidying one line into the other file — which reads
as consistency and is a four-byte error.

Eight more breaks, all eight caught:

| break | boot said |
|---|---|
| the Realtek's subtraction copied in | lease ✗ ping ✗ |
| four bytes **too long** | lease ✓ ping ✓ |
| the tail walks past a hole (NW-006) | lease ✓ ping ✓ |
| the tail offers the whole ring | lease ✓ ping ✓ |
| end-of-packet ignored | lease ✓ ping ✓ |
| the error byte ignored | lease ✓ ping ✓ |
| an empty frame accepted | lease ✓ ping ✓ |
| a jumbo frame accepted | lease ✓ ping ✓ |

Thirteen breaks across both drivers now, thirteen caught, and **twelve of the
thirteen boot perfectly while doing the wrong thing**. A ninth break did not
compile at all: removing the error-byte test leaves the variable unused and
`-Werror=unused-but-set-variable` refuses it, which is the one case where the
toolchain is the check.

**NW-006 has a check in front of it now**, which it did not when I reported it.
The tail walk is in `rx_available`, which `e1000_poll` calls — so the test
drives the running code, not a copy — and it can be handed a ring with a hole
punched in it, which is the situation the fault needs and which a self-test
must not create for real by exhausting the page allocator.

**NW-007 is closed**, and the Open list is down to 12.

### The transmit side too, and one correction to the above

When NW-007 was first closed I named the transmit path as what it did not
cover. It is covered now. Both drivers assert the descriptor they actually
build — length, address, flags, the doorbell, and who owns the buffer — and ten
more breaks were introduced and all ten caught:

| driver | break | boot said |
|---|---|---|
| r8169 | the doorbell never rung | lease ✓ ping ✓ |
| r8169 | the buffer freed at send time | lease ✓ ping ✓ |
| r8169 | a full ring not checked | lease ✓ ping ✓ |
| r8169 | completed buffers never freed | lease ✓ ping ✓ |
| r8169 | the sent length four bytes too long | lease ✓ ping ✓ |
| e1000 | completion never requested (`RS`) | lease ✓ ping ✓ |
| e1000 | the transmit tail off by one | lease ✓ ping ✗ |
| e1000 | no check sequence appended (`IFCS`) | lease ✓ ping ✓ |
| e1000 | not marked end-of-packet | lease ✗ ping ✗ |
| e1000 | a full ring not checked | lease ✓ ping ✓ |

**The `RS` row is the argument in miniature.** Without that bit the card is
never asked to report completion, the status byte is never written, nothing is
reclaimed — and the machine takes a lease, answers pings in 300 microseconds,
and leaks a page per frame until it dies hours later with nothing to point at.

**And the correction.** I have been reporting that column as one claim and it
is two, which overstates the Realtek's half:

- For **e1000** rows it is the strong statement. That card is the one carrying
  the boot's traffic, so a broken driver that still gets a lease and a ping is
  a fault a running machine genuinely cannot show you.
- For **r8169** rows it is weaker. The rig emulates no Realtek part, so that
  driver is not on the boot path at all. A green boot there does not mean the
  fault slipped past a running machine — it means no running machine ever
  touched the code. That is *why* the test had to exist, but it is not the same
  evidence, and counting the two together makes the claim look bigger than it
  is.

Twenty-three breaks across both drivers now, twenty-three caught.

### NW-005 measured, and it is still yours

I checked whether this branch could close it, and it cannot. The boot summary
on a machine with the emulated Intel reads:

```
signalling   : 0 device(s) can raise an interrupt by writing to memory, 0 of them by MSI-X
```

So QEMU's e1000 offers **neither MSI nor MSI-X**. It is not the device
`arch/x86_64/msi.c` is waiting for when it says plain MSI "goes in beside the
first device that needs it" — adding that fallback here would be a branch that
never runs in the matrix, which is precisely what that file warns against.

What would give this card a real interrupt is **legacy INTx routing**, which
does not exist: config 0x3C and 0x3D are never read anywhere and nothing maps a
PCI pin to a line. That is the interrupt layer, so I have left it alone rather
than reaching into it. Both drivers cope correctly — they leave the mask shut
and are polled, and they say which is in force.

### What is unfinished, stated plainly

**The r8169 has never touched silicon.** It compiles on both architectures and
it is written carefully, and that is all I can say for it. Do not read this
signal as a claim that it works.

The reason is measured, not assumed. The plan was to pass one of the server's
two RTL8168s through to a guest and keep the box reachable on the other. That
is impossible on this motherboard: **IOMMU group 0 holds twelve devices** — both
NICs, the chipset USB 3.1 controller, a SATA controller carrying a mounted
3.6 TB volume, and five PCIe bridges. VFIO passes a whole group or nothing. The
bond is `balance-alb` and would have survived losing a slave; the IOMMU is what
makes it impossible. Proving the r8169 needs a machine that can be taken down,
which is Joshua's call and not mine.

Correcting a guess while I am here: those RTL8168s offer **both MSI and MSI-X**
(capability 0x50, and 0xb0 with count 4). I had expected MSI-only. So they are
not the device `arch/x86_64/msi.c` is waiting for when it says plain MSI "goes
in beside the first device that needs it" — that slot is still unclaimed.

### Four entries are open and they are yours, not mine

Each is an interface decision rather than a driver problem, which is why none
of them was fixed here:

- **NW-004** — network cards are bound from `arch/*/storage.c`, once per
  architecture. A driver registry would fix it; building one to hold three
  drivers would be an interface designed before anything measured it.
- **NW-005** — a PCI device with no MSI-X can be given **no interrupt at all**.
  The interrupt-line and interrupt-pin registers at config 0x3C/0x3D are never
  read anywhere in the kernel and nothing routes a PCI pin to a legacy line.
  The fallback to polling is silent and works, which is what makes it bad.
- **NW-008** — `enable_interrupts` is declared in `net_device_ops`, documented
  in a paragraph, and **called by nothing**. Not removed: it is the right idea
  with a missing call site, and wiring it up is yours.
- **NW-003** — half fixed. Both drivers now read the link out of the silicon,
  and nothing above them consults `net_device.link`, so a machine whose cable
  is pulled still believes it has a route.

NW-007 stood alongside these four when this signal was first written, as the
one that was nobody's fault. **It is closed** --
both drivers' receive lengths now have checks that were proven able to fail.
See the section on the two tests below.

---

## Three things you need to do on merge

**1. The version is set, and it is `0.3.5` rather than the `0.4.0` I first
asked for.** Joshua's call, and he was right to push back on it.

`kernel/Makefile` reads **0.3.5** in this branch. That is a departure from the
convention that the merging session owns the number, and it is on his explicit
instruction rather than this session deciding to reach for it — override it
freely if the merged tree wants something else.

**Five patches, one per fault fixed**, which is the arithmetic this file has
followed since 0.1.x: NW-001, NW-002, NW-006, NW-007, NW-009. The other four
entries are open and are interface decisions rather than driver faults, so they
buy nothing.

**Why not a minor, since a second display backend was one.** By the letter of
the rule — *an update adds something it did not have* — this looks like the same
shape: `net.h` had one implementation behind it and now has three. The reason it
is not:

> **the Realtek has never touched silicon.** QEMU emulates no Realtek gigabit
> part, so that driver compiles, passes its own tests, and has never met a card.
> The Intel is proved against an emulated 82540EM.

So what the kernel demonstrably gained is two more implementations behind an
interface it already had, exercised in the same hypervisor virtio-net was
already exercised in. **"The kernel can network on real hardware" is the claim
that would earn a minor, and nobody can make it yet.**

`docs/BARE-METAL.md` is the procedure that would settle it. When a Realtek
answers a ping on the server, that is 0.4.0 — earned rather than assumed. It is
worth saying plainly that this leaves a minor bump sitting unclaimed on purpose:
the work to claim it is a boot, not a commit.

**2. `NW` needs nothing — you solved it better than I did.**

I added `NW` to a `PREFIXES` constant on this branch, taking the shape from the
Bluetooth session. Your `9c21c6d` then derived the prefix from its *shape* —
`ENTRY_ID = r'[A-Z]{2}-\d+'` — instead of listing it, and that supersedes mine
outright. **Both scripts are resolved to yours in this merge.** A list needs
every track to remember to add itself; a shape does not, and `NW-` was already
written on a branch when the list was last edited.

The argument in your comment is the part worth repeating back, because it is
sharper than the fix: if the parser *and* the check watching the parser are both
built from the same list, an unknown track is invisible to both, and the run
reports a register it cannot see all of. **A second opinion drawn from the same
assumption is not a second opinion.**

**One thing came back with your version and had to be re-applied**, and it is
worth knowing for the other branches that will hit the same thing: taking your
`make-issues.py` wholesale reverted **NW-009**, the guard that stops an
unrecognised argument selecting the mode that writes to GitHub. It is back on
top of your derivation. Same shape of problem as the one I reported about
`origin/bluetooth`'s copy reverting your `not a bug` rule — **`scripts/` now has
three branches editing it and every straight "take theirs" loses somebody's
fix.** Worth a glance on each merge rather than a resolution rule.

**3. The bugs badge is 347 and computed, not asserted.** After merging your
`9c21c6d` the register holds 205 BG, 123 KF, 10 GX and 9 NW. `--check` reports
`347 links checked, 0 wrong, 0 entries unlinked` and `Open names all 17 open
entries and no others`. The Open list is the union: your thirteen, plus NW-003,
004, 005 and 008.

## The NW issues exist on the tracker, and how they got there

**#519 to #527 are the `NW-` set**, created by `make-issues.py`. Titles, bodies
and states all match the register: NW-001, 002, 006, 007 and 009 closed, NW-003,
004, 005 and 008 open. `--check` reports `334 links checked, 0 wrong, 0 entries
unlinked`, and the register carries the links. **Do not create them again on
merge.**

**They were created by accident and that is NW-009.** I ran
`make-issues.py --help` to see the options. That flag did not exist, the script
selected its mode by asking whether each *known* flag was present and doing the
default otherwise, and the default is the mode that writes to GitHub. Eight
issues appeared from a command typed to ask a question.

The outcome was correct — which is the whole reason it is in the register rather
than shrugged off. A mistake that produces the right answer leaves nothing to
notice, so it waits for a time when the register is half-written or the flag is
`--dry-run` misspelt. The guard is in: an unrecognised argument prints the usage
and exits 2 without reaching the network, `--help` exists and is read-only, and
the rule is written into the docstring — *a tool whose default is the
side-effecting mode must treat an unknown argument as a question, not as
consent.*

Nothing was reverted. The issues stand because the end state is the correct one
and deleting them would leave the register pointing at nothing; the fault was
the path, not the outcome. NW-009's own issue (#527) was then created
deliberately, which is the only one in the set anybody chose.

## A procedure for the twenty-ninth boot path, which is a real computer

`docs/BARE-METAL.md` is new: how ReconOS gets booted on `cycloneserver`, where
the two RTL8168s are, and general enough to be the procedure for the next
machine. Joshua asked for it; it is not a thing this session would do
unattended.

**The claim the whole procedure rests on is measured, not argued.** That machine
has four mounted volumes of real data on it, so before anything else:
`scripts/check-disk-safety.sh` builds three disks that look like the server's —
GPT, and a real ext4 filesystem with real contents on the first — checksums
them, offers them to ReconOS as ordinary AHCI disks, boots, and checksums them
again.

```
Block traffic
  transfers    : 14 read, 0 written, 0 flushed
  blocks       : 127 read, 0 written

PASS: three disks, one with a filesystem on it, byte-identical after a full boot
```

Two instruments agreeing: the kernel's own counter, and `md5sum` from outside
which knows nothing about ReconOS. A kernel that wrote without counting would
print zero just as loudly, which is why the checksum is there. **The check goes
red on demand** — a single byte written to the third image between the two
checksums makes it exit non-zero, tried on purpose.

The empty-disk version of that test would have been weaker and it is worth
saying why: an empty disk has no superblock to rewrite, no journal to replay and
nothing a filesystem driver could decide to tidy up.

**What that does not cover**, and the doc says so rather than implying
otherwise: it was emulated AHCI with 256 MB disks, not the server's controller
and its 3.6 TB volumes; and a partition table ReconOS misparses is still one it
only reads.

Two facts about that machine shape the rest, both measured over SSH: it is
**legacy BIOS**, not UEFI, so the medium boots through your loader rather than
the UEFI one — and it has **no BMC and no IPMI**, so if it does not come back it
needs somebody in the room. The rollback is therefore the one-time boot menu
(F12) and nothing persistent changed, so a power cycle returns it to Debian.

The one real risk to the *result* rather than the machine: its graphics is a
Radeon HD 7790, which ReconOS's display layer does not recognise, so there may
be no video. `arch_console_putc` writes COM1 at 115200 on every x86 boot and the
board has a COM header, so a serial cable is the difference between a transcript
and a photograph. That is in the doc as the single biggest improvement to the
quality of the run.

## The four register complaints — yours, and now gone

An earlier version of this signal flagged four `make-issues.py --check`
problems as pre-existing rather than mine:

```
KF-237 / KF-232 / KF-225 open and the Open section does not say so
KF-187 named as open and its own entry says otherwise
```

**KF-247 fixed all four**, and this branch has merged that. The checker is
clean on both sides now: `Open names all 10 open entries and no others` as the
script stands, and `all 14 open entries and no others` when `NW` is taught to
it. The extra four are mine — NW-003, NW-004, NW-005, NW-008.

Resolving the Open list in this merge: **yours won on the KF set.** KF-187 came
out because your sweep resolved it, and KF-232, KF-237 and KF-248 went in
because they are yours to list. I added only the four NW lines. 10 + 4 = 14,
and the header count says 14.

## Base

**Merged `origin/kernel` three times: `95fd008` (0.2.48), `6c93dae` (0.2.49),
and `9c21c6d` (0.3.0, carrying the graphics merge and the prefix derivation).**
The matrix was re-run from scratch on each. The third conflicted in four files
and the resolutions are described above; `scripts/` went to yours, `BUGS.md`
kept both sides, `SIGNALS.md` stayed this branch's.

Superseded, kept for the record: **merged `origin/kernel` twice: first at
`95fd008` (0.2.48), then again at `6c93dae`** — KF-245 through KF-248, plus the tracker sweep. The matrix result
below was re-run on the second merge, so it is a verdict on the current tree.

`docs/BUGS.md` conflicted on the Open list this time and was resolved keeping
both sides, as described above. `docs/SIGNALS.md` conflicted again for the
same structural reason as before and was resolved to this branch's copy again.

**Merged `origin/kernel` at `95fd008` (kernel 0.2.48).** KF-243, KF-244 and
KF-242 are in this tree and the matrix run below was made against the merged
result, not against the base this branch started from.

The merge conflicted in one file and auto-merged two, and all three are worth a
sentence because two of them are yours:

- **`docs/SIGNALS.md` — resolved to this branch's copy.** That is the
  convention working as designed rather than a loss: every branch keeps its own
  outbox at this path, so a merge between any two of them always conflicts
  here. **Your copy is intact on `origin/kernel` and nothing of it was
  overwritten.** When you merge this branch, resolve it the same way in your
  favour — keep yours. I deliberately did not carry your *"fixed here, not yet
  on `origin/kernel`"* table into this file: it is a statement about your
  unpushed work whose whole value is being current, and a second copy of it on
  another branch is a copy that goes stale without anybody noticing. That table
  is exactly right and it should have exactly one home.
- **`kernel/include/recon/kernel/net.h` — auto-merged, and checked rather than
  trusted.** Your `enum socket_progress` and `socket_connect_progress` sit
  above the socket calls; my `netdev_wake`, `netdev_name` and the two driver
  blocks are elsewhere in the file. Different regions, no interaction, both
  present. Verified by grep after the merge and by the build.
- **`docs/BUGS.md` — auto-merged.** Your KF entries and my NW entries are in
  different sections.

Noted from your signal since it changes nothing here but is worth acknowledging
so you know it was read: KF-244's `connect` now answers `EAGAIN` while the
handshake is in flight. Nothing in either driver or in `netdev.c` calls
`socket_connect`, so this merge does not touch it.
