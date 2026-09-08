"""The two names no icon pack answers, drawn in the Glass set's own terms.

`application` is what a program without an icon of its own gets, and it was the
one thing on a themed desktop still wearing a drawn blue window -- a coloured
picture sitting in a row of silhouettes that take the skin's ink. `file-font`
was left out on the rule that a wrong picture on a file type is worse than a
plain sheet, which is true and does not apply to a page with a letter on it.

Ours, so there is nothing to license. Same shape as every other file in that
folder: 96 pixels square, pure white, the drawing entirely in the alpha
channel, falling from about 212 at the top to 130 at the bottom -- which is
what gives the set its lit-from-above look and is why a new icon that is merely
also white does not belong to it.
"""
import math
import os
import struct
import sys
import zlib

SIZE = 96
SS = 4


def coverage(fn):
    out = [[0] * SIZE for _ in range(SIZE)]
    for py in range(SIZE):
        for px in range(SIZE):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    if fn(px + (sx + 0.5) / SS, py + (sy + 0.5) / SS):
                        hits += 1
            out[py][px] = hits * 255 // (SS * SS)
    return out


def in_round_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return math.hypot(x - cx, y - cy) <= r or (x0 + r <= x <= x1 - r) or \
        (y0 + r <= y <= y1 - r)


def ring(x, y, x0, y0, x1, y1, r, w):
    """Inside the rounded rect and not inside the one w smaller."""
    return in_round_rect(x, y, x0, y0, x1, y1, r) and not in_round_rect(
        x, y, x0 + w, y0 + w, x1 - w, y1 - w, max(1.0, r - w))


def application(x, y):
    """A window: a frame, a filled title bar, two lines of something."""
    X0, Y0, X1, Y1, R = 12.0, 20.0, 84.0, 78.0, 9.0

    if ring(x, y, X0, Y0, X1, Y1, R, 7.0):
        return True
    # The title bar, filled, so the shape still reads at sixteen pixels.
    if in_round_rect(x, y, X0, Y0, X1, Y1, R) and y <= Y0 + 17.0:
        return True
    # Two lines inside.
    for top in (48.0, 62.0):
        if 26.0 <= x <= 70.0 and top <= y <= top + 7.0:
            return True
    return False


def file_font(x, y):
    """A page with a letter on it, which is what a font file is."""
    X0, Y0, X1, Y1, R = 22.0, 10.0, 74.0, 86.0, 6.0

    if ring(x, y, X0, Y0, X1, Y1, R, 6.0):
        return True

    # A capital A: two legs meeting at the top, and a crossbar.
    APEX_X, APEX_Y = 48.0, 28.0
    FOOT_Y, SPREAD = 70.0, 15.0
    if APEX_Y <= y <= FOOT_Y:
        t = (y - APEX_Y) / (FOOT_Y - APEX_Y)
        for side in (-1.0, 1.0):
            centre = APEX_X + side * SPREAD * t
            if abs(x - centre) <= 4.5:
                return True
        if 56.0 <= y <= 62.0 and abs(x - APEX_X) <= SPREAD * t + 4.0:
            return True
    return False


def ramp(y):
    return 212 - (212 - 130) * y // (SIZE - 1)


def write_png(path, alpha):
    rows = []
    for y in range(SIZE):
        line = bytearray()
        for x in range(SIZE):
            line += bytes((255, 255, 255, alpha[y][x] * ramp(y) // 255))
        rows.append(line)
    raw = b''.join(b'\x00' + bytes(r) for r in rows)

    def chunk(kind, body):
        c = kind + body
        return struct.pack('>I', len(body)) + c + struct.pack(
            '>I', zlib.crc32(c))
    out = b'\x89PNG\r\n\x1a\n'
    out += chunk(b'IHDR', struct.pack('>IIBBBBB', SIZE, SIZE, 8, 6, 0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(raw, 9))
    out += chunk(b'IEND', b'')
    open(path, 'wb').write(out)


SHAPES = {'application': application, 'file-font': file_font}

if __name__ == '__main__':
    dest = sys.argv[1] if len(sys.argv) > 1 else '.'
    for name, fn in SHAPES.items():
        write_png(os.path.join(dest, name + '.png'), coverage(fn))
        print('wrote %s.png' % name)
