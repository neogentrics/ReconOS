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

Two lines on the console, and they are the same two the emulated Intel already
produces:

```
r8169: eth0 at <the card's own MAC>, RTL8168 rev 0c, 32 receive buffers, ...
net: eth0 is 192.168.10.x, via 192.168.10.1
net: the gateway answered in NNN us
```

A DHCP lease from the real network and an ICMP echo answered by the real
OPNsense box. Anything less is a result too — `no address; nothing offered one`
is a true line about a real network and tells us where the driver stopped.

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
- It does not cover the medium itself, which ReconOS **may** write to: it saves
  its boot log to a writable ReconOS volume. That is the USB stick, and that is
  wanted.

---

## The procedure

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

3. **Get a serial cable if there is a header.** The AB350-Gaming has a COM
   header. `arch_console_putc` writes to COM1 at 115200 8N1 on every x86 boot,
   so a USB-to-serial adapter on that header captures **the entire boot log as
   text** — far better evidence than photographing a screen, and it works even
   if the Radeon never gets a usable mode. This is optional and it is the single
   biggest improvement to the quality of the result.

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

8. **At ReconOS's menu, let it start ReconOS.** Do not choose recovery and do
   not choose an installed system.

9. **Read the three things that matter**, in order:

   - `r8169: ethN at <MAC> ...` — the card was found, reset, and its address
     read out of the silicon. If the MAC matches `1c:1b:0d:9e:ff:26` or
     `5c:92:5e:d4:e2:c6` then the register offsets are right, which is the
     largest single unknown in the file.
   - `r8169: ethN link up, 1000 Mb full duplex` — the PHY is being read
     correctly.
   - `net: ethN is 192.168.10.x` and `the gateway answered` — the whole path
     works on real silicon.

10. **Capture it.** Serial log if there is one; photographs of the screen if not.
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
