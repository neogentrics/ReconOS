"""Reads a disk image the kernel was killed while writing, and says what survived.

The kernel writes one marker per block, counting from zero, flushing after each.
Two things are checked, and they answer two different questions:

  ORDER.  The markers present must form an unbroken run from zero. A gap means a
          write issued and flushed later reached the medium before an earlier
          one did -- which would mean this kernel cannot promise ordering, and
          every filesystem design resting on that assumption is wrong the same
          way.

  TEARING.  Each block carries its own sequence number at both ends. If the two
          ends disagree, the block was half-written. That decides whether a
          format may put two facts in one block and rely on them agreeing -- and
          if blocks can tear, a great many simple designs stop working.

Written in Python, on the host, deliberately: it is a second reader of the same
bytes, and it shares no code with the kernel that wrote them.
"""

import struct
import sys

MAGIC = 0x4B52414D  # 'MARK'
BLOCK = 512


def read_marker(block):
    """Returns (present, sequence, tail_sequence) for one block."""
    magic, seq, _crc, seq_again = struct.unpack_from("<IIII", block, 0)

    if magic != MAGIC:
        return (False, None, None)

    # The tail copy sits in the last sixteen bytes of the block.
    tmagic, tseq, _tcrc, tseq_again = struct.unpack_from("<IIII", block, BLOCK - 16)

    if tmagic != MAGIC:
        # Front written, back not: a torn block, and an unambiguous one.
        return (True, seq, None)

    if seq != seq_again or tseq != tseq_again or seq != tseq:
        return (True, seq, tseq)

    return (True, seq, seq)


def main(path):
    with open(path, "rb") as f:
        data = f.read()

    present = []
    torn = []

    for index in range(len(data) // BLOCK):
        block = data[index * BLOCK:(index + 1) * BLOCK]
        ok, seq, tail = read_marker(block)

        if not ok:
            continue

        if tail is None or tail != seq:
            torn.append((index, seq, tail))
            continue

        # The sequence number must also be the block it is in. A marker that
        # moved is a different failure from one that is missing, and worth
        # telling apart.
        if seq != index:
            torn.append((index, seq, tail))
            continue

        present.append(index)

    if torn:
        index, seq, tail = torn[0]
        print("torn block %d: front says %s, back says %s (%d torn in all)"
              % (index, seq, tail, len(torn)))
        return 1

    if not present:
        print("empty no markers were written")
        return 0

    present.sort()
    highest = present[-1]
    expected = set(range(highest + 1))
    missing = sorted(expected - set(present))

    if missing:
        print("gap markers up to %d survived, but %d are missing (first: %d)"
              % (highest, len(missing), missing[0]))
        return 1

    print("ok %d markers, an unbroken run from 0" % (highest + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
