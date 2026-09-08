"""Give the Colored Glass tiles the transparent corners they were saved without.

Every file in that pack is RGB with no alpha channel at all, so the rounded
tile sits on an opaque near-black rectangle. Drawn as-is, a desktop icon is a
black square on the wallpaper and a toolbar icon is a black square on the
chrome -- which is not what the picture is of.

The corners are cut geometrically rather than by colour, because the tile's own
darkest pixels are the same near-black as the surround: keying by colour eats
holes out of the middle of a dark icon. Instead the surround is found by
flooding *inward from the border*, so only what is connected to the outside is
removed and an interior shadow is safe by construction.
"""
import os
import sys
from collections import deque

from png8 import read_png, write_png

TOLERANCE = 14           # per channel, against the corner colour
FEATHER = True           # soften the one-pixel step the flood leaves


def surround_colour(rows, w, h, bpp):
    """The four corners, averaged. They are always outside the tile."""
    picks = [(0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)]
    total = [0, 0, 0]
    for x, y in picks:
        at = x * bpp
        for c in range(3):
            total[c] += rows[y][at + c]
    return [v // len(picks) for v in total]


def outside_mask(rows, w, h, bpp, base):
    """True where a pixel is the surround and reachable from the border."""
    seen = bytearray(w * h)
    queue = deque()

    def close_enough(x, y):
        at = x * bpp
        line = rows[y]
        return all(abs(line[at + c] - base[c]) <= TOLERANCE for c in range(3))

    for x in range(w):
        for y in (0, h - 1):
            if not seen[y * w + x] and close_enough(x, y):
                seen[y * w + x] = 1
                queue.append((x, y))
    for y in range(h):
        for x in (0, w - 1):
            if not seen[y * w + x] and close_enough(x, y):
                seen[y * w + x] = 1
                queue.append((x, y))

    while queue:
        x, y = queue.popleft()
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, ny = x + dx, y + dy
            if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx] \
                    and close_enough(nx, ny):
                seen[ny * w + nx] = 1
                queue.append((nx, ny))
    return seen


def cut(src, dest):
    w, h, bpp, rows = read_png(src)
    base = surround_colour(rows, w, h, bpp)
    outside = outside_mask(rows, w, h, bpp, base)

    alpha = bytearray(255 if not outside[i] else 0 for i in range(w * h))

    if FEATHER:
        """
        A flood gives a hard edge, and a hard edge on a curve is a staircase.
        One box pass over the alpha only, applied where a pixel has both a
        transparent and an opaque neighbour, rounds the corner without
        softening the straight sides.
        """
        soft = bytearray(alpha)
        for y in range(h):
            for x in range(w):
                i = y * w + x
                near = []
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        nx, ny = x + dx, y + dy
                        if 0 <= nx < w and 0 <= ny < h:
                            near.append(alpha[ny * w + nx])
                if near and 0 < sum(1 for v in near if v) < len(near):
                    soft[i] = sum(near) // len(near)
        alpha = soft

    out = []
    for y in range(h):
        line = bytearray()
        src_line = rows[y]
        for x in range(w):
            at = x * bpp
            line += bytes((src_line[at], src_line[at + 1], src_line[at + 2],
                           alpha[y * w + x]))
        out.append(line)

    write_png(dest, w, h, out, 4)
    cut_pixels = sum(1 for v in alpha if v == 0)
    return cut_pixels * 100 // (w * h)


def main():
    src_dir, dest_dir = sys.argv[1], sys.argv[2]
    os.makedirs(dest_dir, exist_ok=True)
    for name in sorted(os.listdir(src_dir)):
        if not name.lower().endswith('.png'):
            continue
        share = cut(os.path.join(src_dir, name), os.path.join(dest_dir, name))
        """
        A tile with rounded corners loses a few per cent. Nothing lost means
        the flood found no surround; a lot lost means it leaked into the
        picture. Both are worth seeing rather than discovering on screen.
        """
        flag = '  <-- look' if share < 1 or share > 25 else ''
        print('%s  %2d%% cut%s' % (name, share, flag))


main()
