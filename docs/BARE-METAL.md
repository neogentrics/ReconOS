# Booting ReconOS on a real machine

The kernel boots on twenty-eight paths in `scripts/verify-kernel.sh` and every
one of them is a virtual machine. This is the procedure for the twenty-ninth,
which is a real computer, and it exists because **a driver for a card nobody has
run is a driver whose faults are opinions.**

Written for one specific machine first — `cycloneserver`, because that is where
the RTL8168s are — and general enough to be the procedure for the next one.

---

## Why this is needed at all

`kernel/core/r8169.c` drives the Realtek gigabit family. Two of those cards are
in the server, bonded. **QEMU emulates no Realtek gigabit part**, so the rig
cannot boot that driver, and it never has.

What is already proved without hardware, and what is not:

| | proved by | |
|---|---|---|
| the descriptor arithmetic, the ring bookkeeping, the length handling, the transmit descriptor | `r8169_self_test`, 10 deliberate breaks all caught | ✔ |
| every register offset in the file | nothing | ✘ |
| the reset sequence | nothing | ✘ |
| that the card raises the interrupts it is asked for | nothing | ✘ |
| that a frame reaches the wire and one comes back | nothing | ✘ |

The test drives the real receive and transmit loops against a page of memory
standing in for the register window. It cannot say the real chip behaves the way
the simulation pretends, and it says so at its own head. **That is the gap this
procedure closes and the only gap it closes.**

## What success looks like

**Two cards, not one.** This is the part an earlier version of this document
got wrong: it showed a single `r8169` line, and the machine has two Realteks
bonded. They are **different revisions** — rev 0c at `05:00.0` and rev 06 at
`08:00.0` — so a run where one works and the other does not is a real possible
outcome and the most informative one available.

```
r8169: eth0 at <the card's own MAC>, RTL8168 rev 0c, 32 receive buffers, ...
r8169: eth1 at <the card's own MAC>, RTL8168 rev 06, 32 receive buffers, ...
net: eth0 is 192.168.10.x, via 192.168.10.1
net: eth1 is 192.168.10.y, via 192.168.10.1
net: the gateway answered in NNN us
```

A DHCP lease from the real network and an ICMP echo answered by the real
OPNsense box.

**Read both `r8169` lines and compare the revisions**, because the driver does
not branch on revision at all. It reads the byte from PCI config, stores it, and
prints it — every RTL8168 is configured identically, which is a choice this
branch made deliberately and has never been able to test. Two cards one revision
apart in one machine is the cheapest experiment anybody will ever get for
whether that choice holds, and it costs nothing extra: they are both already in
there.

**What each outcome means:**

| what appears | what it says |
|---|---|
| both cards, both leases | the driver works on real Realtek silicon, and revision does not matter here |
| both cards named, one lease | **the most interesting result.** The revision the working one reports is the one the common path suits; the other needs per-revision handling this driver does not have |
| both cards named, no lease | the rings or the interrupts, not the revision — the same code failed twice on two different chips |
| one card named | the other was not recognised at probe. Compare its PCI id against the `switch (d->device)` in `r8169_attach` — it accepts 8169, 8168, 8161 and 8136 and declines 8125 by name |
| no cards named | the driver did not attach. `arch/x86_64/storage.c` is where it is bound |

Anything less is a result too — `no address; nothing offered one` is a true line
about a real network and tells us where the driver stopped. So is
`ethN has no cable; not asking for an address`, which is NW-003's line and
means the driver read the link register and believed it: a claim about the
silicon, from a card that has never met this code.

---

## The machine

Measured over SSH on 17 September 2026, not assumed:

| | |
|---|---|
| board | Gigabyte AB350-Gaming, BIOS F3 |
| firmware | **Legacy BIOS**, not UEFI — `/sys/firmware/efi` absent |
| processor | AMD Ryzen 7 1700 |
| graphics | Radeon HD 7790 at `0a:00.0` — a discrete card, because this CPU has no integrated graphics |
| out-of-band | **none.** No BMC, no IPMI device, no IPMI module |
| network | RTL8168 rev 0c at `05:00.0`, RTL8168 rev 06 at `08:00.0`, bonded `balance-alb` |
| disks | `sda` 931 G (root), `sdb` 2.7 T, `sdc`–`sde` 3.6 T each, all mounted |
| uptime | 12 days |
| running | 32 Docker containers |

**The two facts that shape everything below are "legacy BIOS" and "no
out-of-band".** BIOS means the medium boots through ReconOS's own loader rather
than the UEFI one. No BMC means that if the machine does not come back, it needs
somebody standing in front of it — there is no remote power button and no remote
console.

## What it costs to try

Thirty-two containers stop. Every service on that box is down from the moment it
is rebooted until it is back on Debian. **That is the real price and it is not
small**; the driver is not worth an unplanned outage, so this happens in a
window Joshua chooses.

---

## Is it safe for the disks?

This is the question that decides whether the procedure is reasonable, and it
was **measured rather than reasoned about.**

Three disk images were built to look like the server's — a GPT partition table,
and on the first a real ext4 filesystem with real contents — checksummed,
attached to ReconOS as ordinary AHCI disks, and checksummed again after a full
boot:

```
=== verdict ===
UNCHANGED: all three disks are byte-identical after the boot

=== what the kernel says it did ===
Block traffic
  transfers    : 14 read, 0 written, 0 flushed
  blocks       : 127 read, 0 written
```

**Zero writes, and the checksums agree with the counter.** Two independent
instruments saying the same thing, which is the point of using two.

The kernel reads partition tables and looks for a volume it understands, finds
none, and says so (`storage: no volume this kernel can read`). The installer
exists and is a deliberate action; nothing on the boot path invokes it. Recovery
likewise — the matrix entry for it reads *"found the damage, kept its hands off
the disk"*.

**The reproduction is kept**, so this claim can be re-checked rather than
believed: `scripts/check-disk-safety.sh`.

### What that measurement does not cover

- It was made on **emulated** AHCI with 256 MB disks. The server's controller
  and its 3.6 TB disks are not the same silicon, and a partition table ReconOS
  misparses is a partition table it still only *reads*.
- It says nothing about what happens if the machine is reset mid-boot.

**And the medium is not written to either**, which is a correction to what this
document first said. An earlier draft claimed ReconOS would save its boot log to
the stick. It does not: the medium is a FAT32 EFI system partition, not a
ReconOS volume, and the boot says so in as many words —

```
boot log     : no ReconOS medium is writable here, so it stayed in memory
transfers    : 1 read, 0 written, 0 flushed
```

Zero writes on the *boot-from-stick* path too, not just the disks-attached one.
Good for safety and bad for evidence: **there will be no log file
afterwards.** Whatever is not captured while it is on screen is gone when the
power goes off. That is the first of the two reasons the serial cable is not
optional; the second is NW-013, and it is under step 3 of the procedure.

---

## The procedure, rehearsed

**Everything below except the card itself has been run**, on the same path the
server will take: legacy BIOS, booting from a USB stick, with a network card
attached. Not the UEFI path, because the server has no UEFI.

```
qemu-system-x86_64 -m 512M \
  -drive if=none,id=stick,format=raw,file=reconos-medium.img \
  -device usb-ehci,id=ehci \
  -device usb-storage,bus=ehci.0,drive=stick,bootindex=0 \
  -netdev user,id=n0 -device e1000,netdev=n0
```

```
ReconOS kernel 0.4.3
e1000: eth0 link up, 1000 Mb full duplex
e1000: eth0 at 52:54:00:12:34:56, 8254x 100E, 32 receive buffers, ...
e1000: eth1 link up, 1000 Mb full duplex
e1000: eth1 at 52:54:00:12:34:57, 8254x 100E, 32 receive buffers, ...
net: eth0 is 10.0.2.15, via 10.0.2.2
net: the gateway answered in 569 us
net: eth1 is 10.0.2.16, via 10.0.2.2
net: the gateway answered in 63 us
```

**Re-run at 0.4.3 with two cards**, because the first rehearsal was 0.3.5 with
one and the machine has two. Both attach, both take their own lease, and both
answer the gateway. `netdev_primary` — NW-010's function, which decides the
address this machine advertises — picks `eth0`, the first that is up, cabled and
addressed, which is what it is supposed to do and had never been watched doing
it outside a self-test.

So the medium builds, the BIOS boot path works from a stick, the kernel starts,
two cards attach and the whole network path runs twice. **Three things are left
that the rehearsal cannot cover**, and they are the whole point of doing it for
real:

- the Realtek's actual registers, which no emulator has;
- whether the Radeon produces a picture;
- the physical stick and that board's BIOS.

### Before the window — no downtime, done in advance

1. **Build the medium.**

   ```bash
   cd kernel && make ARCH=x86_64 && cd .. && scripts/make-medium.sh reconos.img 512M
   ```

   One image carries both firmwares and both architectures: a protective MBR
   with our 440 bytes of boot code for a BIOS machine, and an EFI system
   partition for a UEFI one. This machine reads the first sector and never looks
   at the second partition.

2. **Write it to a stick**, on the Windows box or anywhere:

   ```bash
   dd if=reconos.img of=/dev/sdX bs=4M conv=fsync status=progress
   ```

   **Check `/dev/sdX` twice.** It is the one command in this document that can
   destroy something.

3. **Get a serial cable. It is not optional, and there are now two
   independent reasons.** The AB350-Gaming has a COM header.
   `arch_console_putc` writes to COM1 at 115200 8N1 on every x86 boot, so a
   USB-to-serial adapter on that header captures **the entire boot log as
   text** — far better evidence than photographing a screen, and it works even
   if the Radeon never gets a usable mode.

   **Reason one: nothing is written down.** No ReconOS medium is writable on
   this path, so there is no log file afterwards. Whatever is not captured
   while it is on screen is gone when the power goes off.

   **Reason two, and it is the one that closed the last door: the log port
   cannot be turned on here.** The kernel session's log port would have been
   the second evidence channel — the boot log read over TCP, no disk involved,
   which is exactly what this machine needs. It is switched on by the word
   `logport` on the kernel command line, and **a BIOS boot carries no command
   line at all.** `boot/bios/stage2.c` writes an empty string and never reads
   `\reconos\cmdline`; the UEFI loader does. This machine is legacy BIOS.

   Measured with a control rather than read off the source — one medium, one
   cmdline file, booted both ways: under UEFI the report says
   `command line : logport verbose` and the port listens; under BIOS neither
   line appears. That is **NW-013**, and it is open.

   The same gap takes `noinit` with it, which is what makes `xhci.c` print
   PORTSC for every port as the controller comes up. So the USB diagnostic is
   unavailable on the machine with the USB fault, which is the same shape as
   the fault itself.

   Two independent reasons for one cable is the strongest form that conclusion
   can take. Do not treat this step as a nice-to-have.

4. **Confirm physical access.** Somebody must be able to reach the machine's
   power button and plug in a keyboard. Without a BMC there is no substitute.

5. **Know how to reach the boot menu.** Gigabyte: **F12** at power-on for the
   one-time boot menu. This is the whole rollback story — see below.

### The window itself

6. **Quiesce Debian properly.** Stop the containers rather than pulling the
   power out from under 32 of them:

   ```bash
   docker stop $(docker ps -q)
   sync
   ```

7. **Reboot and press F12.** Choose the USB stick from the **one-time** boot
   menu.

   **Do not change the BIOS boot order.** That is deliberate and it is the
   rollback: nothing persistent has been altered, so any power cycle from here
   returns the machine to Debian without anybody editing a setting under
   pressure.

8. **A menu may or may not appear, and either is correct.** Rehearsed in the
   rig, none did — `boot menu : never offered -- 0 entries found and no menu
   ran`. On the server it probably *will*, because `menu_discover` looks for
   installed systems and there is a Debian on `sda`.

   If it appears: **let the countdown run out, or choose ReconOS.** The default
   is ReconOS and the countdown is short. Do not choose recovery and do not
   choose the installed system.

9. **Read the three things that matter**, in order:

   - `r8169: ethN at <MAC> ...` — the card was found, reset, and its address
     read out of the silicon. If the MAC matches `1c:1b:0d:9e:ff:26` or
     `5c:92:5e:d4:e2:c6` then the register offsets are right, which is the
     largest single unknown in the file.
   - `r8169: ethN link up, 1000 Mb full duplex` — the PHY is being read
     correctly.
   - `net: ethN is 192.168.10.x` and `the gateway answered` — the whole path
     works on real silicon.

10. **Capture it.** The serial log — and photographs of the screen as well,
    not instead, because the two fail in different ways and neither can be
    taken again once the power goes off.
    The boot summary at the end carries the counters — `N in, N out, N stocked,
    N interrupt(s)` — and those are what say whether the card is interrupting or
    being carried by the poll loop.

### Getting back

11. **Power off, remove the stick, power on.** Debian boots because nothing was
    changed. Start the containers:

    ```bash
    docker start $(docker ps -aq)
    ```

Expected total downtime if it goes well: **ten to fifteen minutes.** Most of it
is Debian shutting down and starting back up, not ReconOS.

---

## What can go wrong, honestly

| | likelihood | what happens | what to do |
|---|---|---|---|
| The stick does not boot | low — the matrix covers `the install medium boots from a USB stick, 8 of 8` on both firmwares | BIOS falls through to the next device, which is Debian | nothing; try again with a different stick |
| ReconOS boots, no video | **plausible** — the Radeon is not a card ReconOS's display layer recognises, and the loader relies on firmware setting a mode | the machine runs and shows nothing | this is why the serial cable matters; without it the run produces no evidence |
| The card is found and the driver is wrong | this is the point of the exercise | some line short of a lease | read the counters and fix the driver |
| ReconOS hangs | possible | no further output | power cycle; Debian returns |
| The machine does not come back to Debian | **low, and this is the one that hurts** | needs hands on the machine | nothing was changed persistently, so the recovery is a power cycle and, at worst, re-selecting the boot device in the BIOS |

**The failure mode that would be serious — ReconOS damaging a disk — is the one
with a measurement behind it rather than an argument.** Zero writes, checksums
unchanged.

## What this does not attempt

- **No installation.** Nothing is written to `sda`. ReconOS is not going on that
  machine; it is being booted on it for as long as it takes to read three lines.
- **Not the RTL8125 in the desktop.** `r8169_attach` declines that part by name
  and says why: its interrupt registers moved and it is untested. A separate
  exercise.
- **Not the firewall.** OPNsense is doing a job.
- **Not unattended.** This is not a thing to run while nobody is watching, and
  it is not a thing to run on a schedule.

---

## What was checked without booting anything, 18 September 2026

**The table above says four things are *proved by nothing*. One of them no
longer is**, and closing it cost an SSH session rather than a server outage.

`cycloneserver` was read while it was running, over SSH, as root, **without
writing a single register, unbinding a driver or changing a mode**. Everything
below comes from sysfs, `lspci`'s own decode of config space, and `ethtool -d`
-- which dumps the card's register window *and labels every offset itself*,
because the Linux driver knows what they are.

That last part is what makes this evidence rather than a second opinion from
the same memory: **ethtool's labels come from Realtek's driver, not from ours.**
Two independent readings of the same silicon agreeing is worth more than either
being checked against a datasheet somebody transcribed.

### The two cards

```
05:00.0  10ec:8168 rev 0c   RTL8168g/8111g   enp5s0  1000 Mb full
08:00.0  10ec:8168 rev 06   RTL8168e/8111e   ens1    1000 Mb full
```

Both are the `8168` device id this driver claims, and `REALTEK_VENDOR 0x10EC`
reads straight out of config space as `ec 10` -- little-endian, as it should be.

### The base address register, which was the one that could have been wrong

The card offers **three**:

```
BAR0  io   256 bytes
BAR2  mem  4096 bytes      <- 64-bit, at 0xfe500000
BAR4  mem  16384 bytes     <- 64-bit, prefetchable
```

`r8169_attach` takes **BAR2** and maps `0x100` of it. That is correct, and it
is correct for the reason the code already gives -- BAR0 is the same registers
as I/O ports, which `core/` may not touch because port access is an x86
instruction. **A wrong BAR here is the exact fault that produced three wrong
hypotheses about xHCI in one evening**, and it is now settled for this driver
by measurement rather than by reading.

### Every register offset the driver uses, against ethtool's own labels

| `r8169.c` | offset | what the card's own driver calls it | |
|---|---|---|---|
| `R_IDR0` | 0x00 | MAC Address | ✔ |
| `R_MAR0` | 0x08 | Multicast Address Filter (two words = eight bytes) | ✔ |
| `R_TNPDS` | 0x20 | Tx Normal Priority Ring Addr (two words = 64-bit) | ✔ |
| `R_CR` | 0x37 | Command | ✔ |
| `R_TPPOLL` | 0x38 | *(between Command and Interrupt Mask, unlabelled in this dump)* | — |
| `R_IMR` | 0x3C | Interrupt Mask, 16-bit | ✔ |
| `R_ISR` | 0x3E | Interrupt Status, 16-bit | ✔ |
| `R_TCR` | 0x40 | Tx Configuration, 32-bit | ✔ |
| `R_RCR` | 0x44 | Rx Configuration, 32-bit | ✔ |
| `R_CFG9346` | 0x50 | EEPROM Command | ✔ |
| `R_PHYSTATUS` | 0x6C | PHY status | ✔ |
| `R_RMS` | 0xDA | RX packet maximum size, 16-bit | ✔ |
| `R_CPLUSCMD` | 0xE0 | C+ Command, 16-bit | ✔ |
| `R_RDSAR` | 0xE4 | Rx Ring Addr, 64-bit | ✔ |
| `R_MTPS` | 0xEC | Early Tx threshold | ✔ |

**Fourteen agreements out of fifteen rows, none of them previously run against
hardware.**

**This said "fifteen offsets, fifteen agreements" and that was wrong**, for
twelve lines, directly beneath a table whose own `R_TPPOLL` row reads
*unlabelled in this dump*. That offset had nothing to agree with, so the count
was of rows rather than of confirmations — a tally that included the line it
could not check.

Corrected with the reason attached rather than the number quietly changed,
because the shape has now appeared four times in this project (KF-187, KF-247,
KF-261 and here) and this is the first time in a document rather than a script.
The session that wrote it had just written the disclaimer standing beside it.

**And the kernel session's own caution, which belongs next to the table rather
than after it:** *fifteen offsets from one part in one firmware state at one
moment is not a specification.* This is one RTL8168, at one revision, as its
Linux driver had left it. It is far better evidence than a map written from
memory and it is not a datasheet — the failure mode to guard against is a value
that is true on this machine becoming a constant in a driver for every machine.

### Two bit-level checks, which are better than the offsets

**The command register.** The card reads `0x37: Command = 0x0c`, and ethtool
decodes that as *Rx on, Tx on*. This driver defines `CR_RX_ENABLE 0x08` and
`CR_TX_ENABLE 0x04`. `0x08 | 0x04` is `0x0c`. The two bits are confirmed
individually, by a card that is currently using them.

**The PHY status decode, which is the stronger of the two** because a third,
independent source can be asked. The card reads `0x6C: PHY status = 0xf3`:

```
0xf3 = 1111 0011
        bit 0  set  -> PHY_FULL_DUPLEX  (0x01)
        bit 1  set  -> PHY_LINK_OK      (0x02)
        bit 2  clear -> not 10M         (0x04)
        bit 3  clear -> not 100M        (0x08)
        bit 4  set  -> PHY_1000M        (0x10)
```

Decoded by this driver's constants that is *link up, full duplex, gigabit* --
and Linux, which was never consulted about the decode, reports the same
interface as **1000 Mb, full duplex**. Three sources agree: our constants, the
raw register, and the operating system driving the card.

### The one offset that could not be checked, and what it will look like if it is wrong

Fourteen of the fifteen agreed. **`R_TPPOLL` at `0x38` is the exception** — the
dump does not label it, so there was nothing to agree with.

**It is the worst one to be left holding**, and worth knowing before the boot
rather than during it. `0x38` is the register this driver writes to say *look at
the transmit ring now*:

```c
__atomic_thread_fence(__ATOMIC_RELEASE);
w8(r, R_TPPOLL, TPPOLL_NPQ);
```

If that offset is wrong the write lands somewhere harmless, the card is never
told to look, and **every frame this machine tries to send sits in the ring with
its OWN bit set.** Nothing is transmitted and nothing says so. DHCP would report
`no address; nothing offered one` — which is also exactly what a machine on a
dead network reports.

**Two things narrow it, and neither is proof.** `0x37` and `0x3C` both agreed,
so `0x38` sits between two confirmed offsets and a shifted map is ruled out. And
the value is a *queue selector* rather than an address, so a wrong offset fails
completely rather than subtly.

**The boot report distinguishes the two cases by itself**, which is the reason
this section exists:

| | `N out` | `ring full` | `N in` |
|---|---|---|---|
| **`0x38` is wrong** | climbs to **32** and stops | appears, and keeps climbing | 0 |
| **the network is dead** | keeps climbing past 32 | stays absent | 0 |

The ring is 32 deep. `collect_tx` stops at the first descriptor the card still
owns, so if the card never walks the ring, `tx_tail` never advances: after 32
frames `tx_head - tx_tail >= TX_RING` and every further send increments
`tx_ring_full`, which the summary prints only when it is non-zero. On a merely
unreachable network the card *does* send — the descriptors come back, the ring
drains, and that counter stays at zero.

**So: `32 out` with `ring full` growing means the register, not the wire.** Read
that line before concluding anything about the network.

### Reading the other three rows from the boot report

The same trick as above, applied to what the register read could not close. The
point is not prediction for its own sake — it is that a maintenance window is
one shot, and *"it did not work"* is worth very little afterwards while *"out
stalled at 32 and ring full climbed"* is worth a fix.

Every line below is one the kernel already prints. Nothing here needs new code.

| what failed | what the report shows | strength |
|---|---|---|
| **the reset** | `r8169: the card at B:D.F did not finish a reset` and **no `r8169:` summary line for that card at all** — attach returns false | **decisive**, it says so in words |
| **the transmit poll (`0x38`)** | `32 out`, `ring full` present and climbing, `0 in` | **decisive**, see the section above |
| **the receive ring** | `0 in`, **and no `bad`/`no buffer` at all** — those three counters print together and only when one is non-zero | **suggestive**, not decisive |
| **interrupts** | `0 interrupt(s)` and the boot's own line: *"N card(s) can raise one, M cannot and are polled"* | **decisive**, and expected — NW-005 |

**The receive row is the weak one and it is worth saying why**, because it is the
row most likely to be over-read. `0 in` is also what a healthy card on a network
that never answers reports. What narrows it is the company it keeps: the PHY
status is *confirmed* against the silicon, so if the report says `link up,
1000 Mb` and frames are going out and **nothing whatever comes back — no frames,
no errors, no dropped buffers** — then the card is not walking the receive ring,
because a card that is walking it and seeing rubbish would be counting rubbish.

`stocked` does **not** help here and should not be read as if it does. It counts
descriptors currently holding a buffer, and it is 32 both when the ring is
working and being refilled and when it has never been touched. A healthy boot on
the emulated Intel prints `4 in, 4 out, 32 stocked` — the 32 is not evidence of
anything.

**Capture the summary lines verbatim.** They are three lines and they carry every
counter named above; a photograph of them is worth more than any description of
what the machine seemed to do.

### What this does not close, and it is most of the risk

**The reset sequence, the interrupts, and a frame on the wire are all
untouched.** Reading a register window says the addresses are right; it says
nothing about whether writing `CR_RST` and waiting produces a card that comes
back, whether the vector this driver asks for is one the card raises, or
whether a descriptor ring this driver fills is one the card walks.

Those need the machine to be running **this** kernel, and the procedure above
is still the way to find out. What has changed is the odds: the class of fault
most likely to make a first bare-metal boot produce nothing at all -- a wrong
window and a register map read from memory -- has been eliminated beforehand.
