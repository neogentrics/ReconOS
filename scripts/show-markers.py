"""Prints which marker blocks are present, as runs, so the shape of a failure
is visible rather than summarised.

A prefix that stops is an interrupted run. A suffix, or islands, is something
else entirely -- and the difference is the whole diagnosis.
"""

import struct
import sys

MAGIC = 0x4B52414D
BLOCK = 512


def main(path):
    with open(path, "rb") as f:
        data = f.read()

    present = []
    for index in range(len(data) // BLOCK):
        magic, seq, _crc, seq2 = struct.unpack_from("<IIII", data, index * BLOCK)
        if magic == MAGIC:
            present.append((index, seq, seq2))

    if not present:
        print("no markers at all")
        return

    print("%d marker blocks present, of %d blocks in the image"
          % (len(present), len(data) // BLOCK))

    # Collapse into runs of consecutive block indices.
    runs = []
    start = prev = present[0][0]
    for index, _seq, _seq2 in present[1:]:
        if index == prev + 1:
            prev = index
            continue
        runs.append((start, prev))
        start = prev = index
    runs.append((start, prev))

    print("runs of consecutive blocks holding a marker:")
    for lo, hi in runs[:20]:
        print("  %6d .. %-6d  (%d blocks)" % (lo, hi, hi - lo + 1))
    if len(runs) > 20:
        print("  ... and %d more runs" % (len(runs) - 20))

    print()
    print("first few markers, block -> sequence:")
    for index, seq, seq2 in present[:6]:
        print("  block %6d  seq %-6d  tail %d" % (index, seq, seq2))


if __name__ == "__main__":
    main(sys.argv[1])
