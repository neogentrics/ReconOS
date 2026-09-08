"""Read and write 8-bit PNGs, with nothing else in it.

Split out of sheet.py because that file runs its own main() on import, so
anything wanting the reader got the contact sheet as a side effect.
"""
import struct
import zlib


def read_png(path):
    """-> (width, height, bytes_per_pixel, [row bytes, ...]). RGB or RGBA."""
    d = open(path, 'rb').read()
    i = 8
    w = h = ct = 0
    idat = b''
    while i < len(d):
        ln = struct.unpack('>I', d[i:i + 4])[0]
        kind = d[i + 4:i + 8]
        body = d[i + 8:i + 8 + ln]
        i += 12 + ln
        if kind == b'IHDR':
            w, h, bd, ct, _c, _f, interlace = struct.unpack('>IIBBBBB', body)
            if bd != 8 or ct not in (2, 6) or interlace != 0:
                raise ValueError('%s: depth %d type %d interlace %d'
                                 % (path, bd, ct, interlace))
        elif kind == b'IDAT':
            idat += body

    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    rows = []
    prev = bytearray(w * bpp)
    pos = 0
    for _y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + w * bpp])
        pos += w * bpp
        for x in range(w * bpp):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, bpp, rows


def write_png(path, width, height, rows, bpp):
    body = bytearray()
    for row in rows:
        body += b'\x00' + bytes(row)

    def chunk(kind, b):
        c = kind + b
        return struct.pack('>I', len(b)) + c + struct.pack('>I', zlib.crc32(c))

    kind = 6 if bpp == 4 else 2
    out = b'\x89PNG\r\n\x1a\n'
    out += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, kind,
                                      0, 0, 0))
    out += chunk(b'IDAT', zlib.compress(bytes(body), 9))
    out += chunk(b'IEND', b'')
    open(path, 'wb').write(out)
