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

And one that is nobody's fault and has no check in front of it:

- **NW-007** — a receive length four bytes **too short** loses the DHCP lease
  and is caught. Four bytes **too long** produces a lease and a ping reply that
  no instrument in this kernel can tell from correct. Measured both ways. Too
  long is what forgetting to strip the frame check sequence looks like, so the
  boot test catches the mistake nobody makes and misses the one they do.

---

## Three things you need to do on merge

**1. The version.** A new driver is a capability, so this is a **minor** bump:
`0.2.46 → 0.3.0`. I have not touched `kernel/Makefile`. Joshua confirmed the
model in chat on 16 September — major is an official release, minor is an
update, patch is a fix — which is the rule already written above this file's
signal section.

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

## One thing that is not mine

`python scripts/make-issues.py --check` reports four problems on this branch:

```
KF-237 is open and the Open section does not say so
KF-232 is open and the Open section does not say so
KF-225 is open and the Open section does not say so
KF-187 is named as open and its own entry says otherwise
```

All four are **byte-identical on `origin/kernel` at 71a4a5a before any change
of mine** — checked by running the script against `git show HEAD:docs/BUGS.md`.
Flagged so they are not read as arriving with this merge.

## Base

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
