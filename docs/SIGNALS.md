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
kernel session verifies it against the full matrix first. **`VERSION` in
`kernel/Makefile` is untouched here** — the merging session owns it.

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
1578 self-tests across every path, no failures (0 skipped).
```

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

**The right-hand column is the point.** Every one of those five boots got a
DHCP lease and a ping answered in about 300 microseconds. Booting catches none
of them, so this is coverage of ground a running machine cannot reach.

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

**1. The version.** A new driver is a capability, so this is a **minor** bump:
**`0.2.49 → 0.3.0`**. `kernel/Makefile` is untouched here and reads 0.2.49,
which is yours from the second merge — an earlier draft of this signal said
`0.2.46 → 0.3.0` and that base is now stale, but the arithmetic is unchanged
because a minor bump zeroes the patch either way. Joshua confirmed the model in
chat on 16 September — major is an official release, minor is an update, patch
is a fix.

Worth being explicit since two drivers and two test suites arrive together:
this is **one** minor bump, not one per driver. The capability is "the kernel
can drive a network card that is not virtio", and it arrives once.

**2. `NW` needs adding to `PREFIXES`, in one place, in two scripts.** It is
**deliberately not added on this branch.** The Bluetooth session lifted the
prefix out of five regexes per script into a single `PREFIXES` constant
(`origin/bluetooth`, `589f55d`), and adding `NW` the old way here would create
exactly the five-line conflict that refactor exists to remove. Once that lands:

```
PREFIXES = ("BG", "KF", "GX", "BT", "NW")
```

in both `scripts/make-issues.py` and `scripts/check-readme-badges.py`.

**3. The bugs badge goes up by 8 from this branch.** Under the old
`check-readme-badges.py` it reads 319 and is green, because that script sums
`BG` and `KF` by name and counts `NW` as zero — the same silent undercount that
was hiding `GX`. Under the new one every prefix is counted, so this branch
contributes **+8**. I am giving you the delta rather than a total because
`GX` and `BT` land from their own branches and only the merged tree knows the
sum.

Verified against a simulated post-refactor checker (the script with `NW` taught
to it): my entries produce **no new complaints**.

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

**Merged `origin/kernel` twice: first at `95fd008` (0.2.48), then again at
`6c93dae`** — KF-245 through KF-248, plus the tracker sweep. The matrix result
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
