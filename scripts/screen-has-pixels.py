#!/usr/bin/env python3
"""Boot the kernel and ask QEMU what is actually on the screen.

--- Why this exists, and why nothing inside the kernel can replace it ---------

Every other display check in this tree reads the framebuffer back through the
kernel's own eyes. On an adapter whose framebuffer is a scanned-out PCI
aperture that is a real check: the memory *is* the screen, so a read-back that
finds the right pixels found them on the glass.

On virtio-gpu it is not a check at all. The pixels live in ordinary guest RAM
and reach the host only when the driver sends TRANSFER_TO_HOST_2D and
RESOURCE_FLUSH. A kernel that never sends them still writes the pixels, still
reads them back, and still reports every self-test passing -- against a
completely black screen. That is not a hypothetical: it is what this kernel did
on the first boot that drove a virtio-gpu, and it passed `a mode of our own`,
`a screen to draw on` and `a C program ... its pixels are on the screen` while
showing nothing whatsoever (GX-003).

So the question "is anything on the screen" has to be asked of something that
is not the kernel. QEMU's monitor will answer it: `screendump` writes what the
display is presenting, from the host's side of the device.

--- What it asserts ----------------------------------------------------------

That at least `--min-pixels` pixels are not black. Deliberately crude: this is
not checking that the console is legible, it is checking that the present path
works at all, and the difference between "some pixels" and "no pixels" is the
entire failure this catches. A tighter assertion about *which* pixels would
break every time the boot output changed by a line.

Shown to fail before being believed, as everything in this tree is: with
`gpu_flush` stubbed to return true without talking to the device, this reports
0 non-black pixels and exits non-zero, on a kernel whose own self-tests all
pass.

  screen-has-pixels.py --marker "first screen" --min-pixels 1000 -- \
      qemu-system-x86_64 -m 1024M -vga none -device virtio-gpu-pci -kernel ...

The QEMU command is given without `-serial`, `-monitor`, `-display` or
`-no-reboot`: this adds them, because it needs the serial output in a file it
can watch and the monitor on a socket it can drive.
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time


def parse_ppm(path):
    """Width, height, count of non-black pixels, and how many distinct colours.

    A hand-rolled P6 reader rather than a dependency: this runs in the same
    verification rig that refuses to need anything not already installed, and
    the format is a magic number, three integers and the bytes.
    """
    with open(path, "rb") as f:
        data = f.read()

    fields, i = [], 0
    while len(fields) < 4:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":          # a comment runs to end of line
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j

    if fields[0] != b"P6":
        raise ValueError("not a binary PPM: %r" % fields[0])

    i += 1                                  # the single whitespace byte
    width, height = int(fields[1]), int(fields[2])
    px = data[i:]

    nonblack = 0
    counts = {}
    end = min(len(px), width * height * 3) - 2
    for k in range(0, max(end, 0), 3):
        t = bytes(px[k:k + 3])
        if t != b"\x00\x00\x00":
            nonblack += 1
        counts[t] = counts.get(t, 0) + 1

    return width, height, nonblack, len(counts), counts


def run(qemu_cmd, marker, timeout, settle):
    work = tempfile.mkdtemp(prefix="recon-screen-")
    mon = os.path.join(work, "mon.sock")
    serial = os.path.join(work, "serial.log")
    shot = os.path.join(work, "screen.ppm")

    # `-display none` rather than `-nographic`: the display still exists and is
    # still being presented to, there is simply no window. `-nographic` would
    # also seize the serial port, which is wanted in a file here.
    cmd = list(qemu_cmd) + [
        "-no-reboot", "-display", "none",
        "-serial", "file:%s" % serial,
        "-monitor", "unix:%s,server,nowait" % mon,
    ]

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.STDOUT)
    try:
        # Waiting for a line rather than for a fixed time. A boot that is slow
        # because the machine is busy is still a boot; a sleep long enough to
        # cover that on the worst day is a sleep everybody pays for.
        deadline = time.time() + timeout
        seen = False
        while time.time() < deadline:
            if os.path.exists(serial):
                try:
                    with open(serial, "rb") as f:
                        if marker in f.read().decode("utf-8", "replace"):
                            seen = True
                            break
                except OSError:
                    pass
            if proc.poll() is not None:
                break
            time.sleep(0.25)

        if not seen:
            print("the marker %r never appeared on the serial port" % marker)
            return None, serial

        # The marker is printed by the kernel; the present that follows it goes
        # over a virtqueue and is answered by the host. Settling covers that
        # gap rather than racing it.
        time.sleep(settle)

        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(mon)
        time.sleep(0.3)
        try:
            s.recv(65536)                   # the monitor's banner
        except OSError:
            pass
        s.sendall(("screendump %s\n" % shot).encode())
        time.sleep(1.5)
        try:
            s.recv(65536)
        except OSError:
            pass
        s.close()

        if not os.path.exists(shot):
            print("QEMU's monitor produced no screendump")
            return None, serial

        return shot, serial
    finally:
        proc.kill()
        proc.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--marker", required=True,
                    help="serial line to wait for before looking at the screen")
    ap.add_argument("--min-pixels", type=int, default=1000)
    ap.add_argument("--forbid-colour", default=None,
                    help="RRGGBB[:N] -- a colour that must NOT be on the "
                         "screen, or at most N pixels of it")
    ap.add_argument("--require-colour", default=None,
                    help="RRGGBB[:N] -- a colour that must be on the screen, "
                         "and how many pixels of it at least")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--settle", type=float, default=1.5)
    ap.add_argument("qemu", nargs=argparse.REMAINDER)
    args = ap.parse_args()

    qemu = args.qemu
    if qemu and qemu[0] == "--":
        qemu = qemu[1:]
    if not qemu:
        print("no QEMU command given")
        return 2

    shot, serial = run(qemu, args.marker, args.timeout, args.settle)
    if shot is None:
        print("  serial log: %s" % serial)
        return 2

    w, h, nonblack, colours, counts = parse_ppm(shot)

    # **A colour, not a count**, where the caller asks for one.
    #
    # Counting non-black pixels says the screen is not blank. It cannot say
    # *whose* pixels they are, and where the console covers the whole panel
    # that is the entire question: with SYS_PRESENT taken out of the paint
    # program this screen still reports 2,304,000 non-black pixels, because the
    # console is still drawing. What disappears is the program's own ground
    # colour. So the check that can actually fail is the one that names it.
    # **And a colour that must be absent**, which is a different question
    # again.
    #
    # "Is the program's picture there" and "is the console on top of it" are
    # both true at once when the console draws into the middle of a program's
    # screen -- which is the whole of the fault in docs/KERNEL-WANTS.md, and it
    # is why a require-colour check alone passes on a kernel that has it. With
    # the panel claim disabled the init screen still covers 64% of the glass;
    # what appears is 2,126,664 pixels of console paper sitting on top of it.
    if args.forbid_colour:
        spec = args.forbid_colour.split(":")
        unwanted = bytes.fromhex(spec[0])
        allowed = int(spec[1]) if len(spec) > 1 else 0
        got = counts.get(unwanted, 0)

        if got > allowed:
            print("#%s is on %d pixel(s) of the screen and at most %d is "
                  "allowed -- something drew over the program that owns it"
                  % (unwanted.hex(), got, allowed))
            print("  screendump: %s" % shot)
            print("  serial log: %s" % serial)
            return 1

    if args.require_colour:
        spec = args.require_colour.split(":")
        want = bytes.fromhex(spec[0])
        need = int(spec[1]) if len(spec) > 1 else 1
        got = counts.get(want, 0)

        if got < need:
            print("#%s is on %d pixel(s) and this asks for at least %d -- what "
                  "was drawn through the mapping did not reach the display"
                  % (want.hex(), got, need))
            print("  screendump: %s" % shot)
            print("  serial log: %s" % serial)
            return 1

    if nonblack < args.min_pixels:
        print("the screen is %dx%d and %d pixel(s) are not black, which is "
              "fewer than the %d this asks for -- the kernel drew and nothing "
              "reached the display"
              % (w, h, nonblack, args.min_pixels))
        print("  screendump: %s" % shot)
        print("  serial log: %s" % serial)
        return 1

    print("%dx%d, %d non-black pixel(s), %d distinct colour(s)"
          % (w, h, nonblack, colours))
    return 0


if __name__ == "__main__":
    sys.exit(main())
