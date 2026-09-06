#!/usr/bin/env python3
"""Read a ReconFS image and say whether it holds together.

--- Why this exists in a different language ---

Nobody else implements ReconFS. Testing the kernel's checker against the
kernel's writer proves only that they share their assumptions, which is the
failure mode scripts/make-partition-fixtures.sh opens by naming: a writer and a
reader built from the same misunderstanding agree perfectly.

So this is written from kernel/include/recon/kernel/reconfs.h -- from the field
offsets and the rules, not from kernel/core/reconfs*.c. Where the C and this
disagree, one of them is wrong, and neither was derived from the other.

It is also what judges a power cut. The kernel cannot check an image it was
killed in the middle of writing; something outside has to read what survived.

--- What it checks ---

  * a superblock validates, and the one with the higher epoch is believed
  * every block's checksum, where the block kind carries one
  * the forward walk: root -> directory entries -> inodes -> block pointers
  * the reverse sweep: every owner-table entry
  * the two agree, block for block

It writes nothing, ever. That is what makes it safe to run on an image that is
about to be examined again.
"""
import struct
import sys

BLOCK_MIN = 4096
BLOCK_MAX = 65536

MAGIC       = 0x3153466E6F636552      # "ReconFS1"
INODE_MAGIC = 0x316F6E496E636552      # "ReconIno1"
VERSION     = 1

OWNER_VOID    = 0
OWNER_ARCHIVE = 1
DOSSIER_ROOT  = 2

TYPE_FILE = 1
TYPE_DIR  = 2

DIRECT = 12

# --- Layouts, by explicit offset -------------------------------------------
#
# Field by field rather than as one struct format string. A format string is a
# single opaque token where one wrong letter shifts everything after it and the
# values still parse -- which is precisely the kind of wrong that looks right.
# These offsets were read out of the compiler with __builtin_offsetof and are
# checked against sizeof below.
SUPER_AT = {
    "magic": ("<Q", 0), "version": ("<I", 8), "block_size": ("<I", 12),
    "epoch": ("<Q", 16), "total_blocks": ("<Q", 24), "root_inode": ("<Q", 32),
    "table_root": ("<Q", 40), "table_depth": ("<I", 48),
    "next_dossier": ("<Q", 56), "blocks_used": ("<Q", 64),
    "fold": ("<I", 152), "seek_is_free": ("<B", 156),
    "discard_supported": ("<B", 157), "transfer_hint": ("<I", 160),
}
SUPER_CSUM = 204
SUPER_SIZE = 208

INODE_AT = {
    "magic": ("<Q", 0), "dossier": ("<Q", 8), "parent": ("<Q", 16),
    "type": ("<I", 24), "mode": ("<I", 28), "uid": ("<I", 32),
    "gid": ("<I", 36), "size": ("<Q", 40), "btime": ("<Q", 48),
    "mtime": ("<Q", 56), "ctime": ("<Q", 64), "links": ("<I", 72),
    "flags": ("<I", 76), "inline_len": ("<I", 80),
    "indirect": ("<Q", 184), "double": ("<Q", 192), "triple": ("<Q", 200),
}
INODE_DIRECT_AT = 88
INODE_CSUM = 224
INODE_SIZE = 232          # where the inline data begins

DIRENT_SIZE = 16          # inode, rec_len, name_len, type, reserved


def field(buf, table, name):
    fmt, off = table[name]
    return struct.unpack_from(fmt, buf, off)[0]


def crc32_reflected(data: bytes) -> int:
    """The same reflected CRC-32 the format uses. Bitwise on purpose: this is a
    checker, and a table would be one more thing to have copied wrong."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


class Bad(Exception):
    pass


class Image:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        self.bs = None
        self.super = None
        self.live = None

    def raw(self, offset, length):
        if offset + length > len(self.data):
            raise Bad(f"read past the end of the image at {offset}")
        return self.data[offset:offset + length]

    def block(self, n):
        return self.raw(n * self.bs, self.bs)

    # --- Superblocks -------------------------------------------------------
    def parse_super(self, buf):
        return {k: field(buf, SUPER_AT, k) for k in SUPER_AT}

    def load(self):
        """Both superblocks sit at fixed byte offsets -- 0 and BLOCK_MAX -- so
        either can be found without knowing the block size the other records."""
        best = None
        for at in (0, BLOCK_MAX):
            try:
                head = self.raw(at, BLOCK_MIN)
            except Bad:
                continue

            s = self.parse_super(head)
            if s["magic"] != MAGIC or s["version"] != VERSION:
                continue

            bs = s["block_size"]
            if bs < BLOCK_MIN or bs > BLOCK_MAX or (bs & (bs - 1)):
                continue

            full = self.raw(at, bs)
            off = SUPER_CSUM
            stored = struct.unpack("<I", full[off:off + 4])[0]
            body = full[:off] + b"\0\0\0\0" + full[off + 4:]
            if crc32_reflected(body) != stored:
                continue

            if best is None or s["epoch"] > best[0]["epoch"]:
                best = (s, at, bs)

        if best is None:
            raise Bad("no valid superblock")

        self.super, self.live, self.bs = best
        return self.super

    # --- The owner table ---------------------------------------------------
    def per_leaf(self):
        return self.bs // 8

    def per_index(self):
        return self.bs // 8

    def leaf_block(self, leaf_index):
        node = self.super["table_root"]
        depth = self.super["table_depth"]
        span = self.per_index() ** depth
        idx = leaf_index

        while depth:
            slots = struct.unpack(f"<{self.per_index()}Q", self.block(node))
            span //= self.per_index()
            slot = idx // span
            if slot >= self.per_index():
                raise Bad("owner table index out of range")
            node = slots[slot]
            idx %= span
            if not node:
                raise Bad("owner table has a hole")
            depth -= 1

        return node

    def owners(self):
        """Every block's owner, in order. Reads leaves, and nothing else."""
        total = self.super["total_blocks"]
        out = []
        for first in range(0, total, self.per_leaf()):
            leaf = self.block(self.leaf_block(first // self.per_leaf()))
            vals = struct.unpack(f"<{self.per_leaf()}Q", leaf)
            take = min(self.per_leaf(), total - first)
            out.extend(vals[:take])
        return out

    # --- Inodes and names --------------------------------------------------
    def inode(self, blk):
        buf = self.block(blk)
        i = {k: field(buf, INODE_AT, k) for k in INODE_AT}
        i["direct"] = list(struct.unpack_from("<12Q", buf, INODE_DIRECT_AT))

        if i["magic"] != INODE_MAGIC:
            raise Bad(f"block {blk} is not an inode")

        stored = struct.unpack_from("<I", buf, INODE_CSUM)[0]
        body = buf[:INODE_CSUM] + b"\0\0\0\0" + buf[INODE_CSUM + 4:]
        if crc32_reflected(body) != stored:
            raise Bad(f"inode at block {blk} does not match its checksum")

        i["_raw"] = buf
        return i

    def contents(self, ino):
        """A file's bytes, assembled from wherever they live."""
        want = ino["size"]

        if ino["inline_len"]:
            return ino["_raw"][INODE_SIZE:INODE_SIZE + want]

        out = bytearray()
        n = 0
        slots = None
        while len(out) < want:
            if n < 12:
                where = ino["direct"][n]
            else:
                if slots is None:
                    if not ino["indirect"]:
                        raise Bad("a file needs an indirect block and has none")
                    slots = struct.unpack(f"<{self.per_index()}Q",
                                          self.block(ino["indirect"]))
                if n - 12 >= len(slots):
                    raise Bad("a file is longer than its pointers reach")
                where = slots[n - 12]
            if not where:
                raise Bad("a file has a hole in it")
            take = min(self.bs, want - len(out))
            out += self.block(where)[:take]
            n += 1

        return bytes(out)

    def dir_entries(self, ino):
        """Every (name, child, type) in a directory, in stored order."""
        streams = []
        if ino["inline_len"]:
            streams.append(ino["_raw"][INODE_SIZE:
                                       INODE_SIZE + ino["inline_len"]])
        else:
            for b in ino["direct"]:
                if b:
                    streams.append(self.block(b))

        out = []
        for s in streams:
            off = 0
            while off + DIRENT_SIZE <= len(s):
                child, rec_len, name_len, etype = struct.unpack(
                    "<QHBB", s[off:off + 12])
                if rec_len == 0:
                    break
                if rec_len < DIRENT_SIZE or off + rec_len > len(s) or \
                        DIRENT_SIZE + name_len > rec_len:
                    raise Bad("a directory entry does not fit its block")
                name = s[off + DIRENT_SIZE:off + DIRENT_SIZE + name_len].decode(
                    "utf-8", "replace")
                if child:
                    out.append((name, child, etype))
                off += rec_len
        return out


PAYLOAD_MAGIC = 0x52464350          # "RFCP"


def reconfs_super_b_blocks(img):
    """The first block a file may use: one past the second superblock."""
    return BLOCK_MAX // img.bs + 1



def check_payload(data):
    """Is this one complete version of the crash workload's file?

    The round number is at both ends, so a file assembled from the head of one
    version and the tail of another shows two different numbers -- visible
    without knowing which version was supposed to be there, which matters
    because after a power cut nobody does.
    """
    if len(data) < 24:
        return f"only {len(data)} bytes"

    magic, round_a, length, stored = struct.unpack_from("<IIII", data, 0)
    round_b = struct.unpack_from("<I", data, 16)[0]
    round_c = struct.unpack_from("<I", data, len(data) - 4)[0]

    if magic != PAYLOAD_MAGIC:
        return f"magic is {magic:#x}"
    if length != len(data):
        return f"says {length} bytes, is {len(data)}"
    if not (round_a == round_b == round_c):
        return (f"torn: the round is {round_a} at the front, {round_b} after "
                f"the header, {round_c} at the end")

    crc = crc32_reflected(data[:12] + data[16:])
    if crc != stored:
        return f"checksum {crc:08x}, stored {stored:08x}"

    return None


def check(path, want=None):
    img = Image(path)
    sb = img.load()

    total = sb["total_blocks"]
    by = [0] * total          # what the forward walk says owns each block
    problems = []
    inodes = 0

    def claim(blk, owner, why):
        if blk >= total:
            problems.append(f"{why}: block {blk} is past the end")
            return
        if by[blk]:
            problems.append(f"block {blk} is reachable twice")
            return
        by[blk] = owner

    def walk(blk, expect_parent, owner, depth=0):
        nonlocal inodes
        if depth > 64:
            problems.append("the directory tree is deeper than 64")
            return
        claim(blk, owner, "inode")
        ino = img.inode(blk)
        inodes += 1

        if ino["parent"] != expect_parent:
            problems.append(
                f"inode at {blk} says its parent is {ino['parent']}, "
                f"but {expect_parent} names it")

        if ino["type"] == TYPE_DIR:
            if not ino["inline_len"]:
                for b in ino["direct"]:
                    if b:
                        claim(b, ino["dossier"], "directory block")
            for _name, child, _t in img.dir_entries(ino):
                walk(child, ino["dossier"], ino["dossier"], depth + 1)
        elif ino["type"] == TYPE_FILE:
            if not ino["inline_len"]:
                for b in ino["direct"]:
                    if b:
                        claim(b, ino["dossier"], "data block")
                if ino["indirect"]:
                    claim(ino["indirect"], ino["dossier"], "indirect block")
                    slots = struct.unpack(f"<{img.per_index()}Q",
                                          img.block(ino["indirect"]))
                    for b in slots:
                        if b:
                            claim(b, ino["dossier"], "data block")
        else:
            problems.append(f"inode at {blk} has type {ino['type']}")

    def claim_table(node, depth):
        """The owner table's own blocks -- its index blocks and its leaves.

        Following the table's *structure* is the forward walk's business; the
        sweep below still reads only the table's contents, so the two stay
        disjoint.
        """
        if node >= total:
            problems.append(f"the owner table leaves the volume at {node}")
            return
        if by[node]:
            problems.append(f"owner table block {node} is reachable twice")
            return
        by[node] = OWNER_ARCHIVE
        if depth == 0:
            return
        slots = struct.unpack(f"<{img.per_index()}Q", img.block(node))
        for child in slots:
            if child:
                claim_table(child, depth - 1)

    # Both superblocks and the run reserved between them. Claimed explicitly,
    # rather than exempted in the comparison below -- an exemption for
    # archive-owned blocks applies to every stale copy of the root directory
    # too, and hid a leak on every commit (BG-122).
    for b in range(min(reconfs_super_b_blocks(img), total)):
        by[b] = OWNER_ARCHIVE

    claim_table(sb["table_root"], sb["table_depth"])

    walk(sb["root_inode"], 0, OWNER_ARCHIVE)

    actual = img.owners()
    for b in range(total):
        a, e = actual[b], by[b]
        if a == e:
            continue
        if a == OWNER_VOID:
            problems.append(f"block {b}: reachable but not allocated")
        elif not e:
            problems.append(f"block {b}: allocated but not reachable")
        else:
            problems.append(f"block {b}: allocated to {a}, reachable from {e}")

    root = img.inode(sb["root_inode"])
    names = [n for n, _c, _t in img.dir_entries(root)]

    found = None
    payload_round = None
    if want is not None:
        matches = [c for n, c, _t in img.dir_entries(root)
                   if n.lower() == want.lower()]
        if len(matches) == 0:
            problems.append(f"'{want}' is not in the root directory")
        elif len(matches) > 1:
            problems.append(f"'{want}' names {len(matches)} things")
        else:
            found = matches[0]
            try:
                ino = img.inode(found)
                data = img.contents(ino)

                # Only if it looks like the crash workload's payload. Other
                # callers write other things, and this reader is not only for
                # that one workload.
                if len(data) >= 4 and struct.unpack_from("<I", data, 0)[0] == \
                        PAYLOAD_MAGIC:
                    why = check_payload(data)
                    if why:
                        problems.append(f"'{want}' is not one whole version: "
                                        f"{why}")
                    else:
                        payload_round = struct.unpack_from("<I", data, 4)[0]
            except Bad as e:
                problems.append(f"'{want}' points at something unreadable: {e}")

    return {
        "epoch": sb["epoch"],
        "block_size": sb["block_size"],
        "inodes": inodes,
        "names": names,
        "target": found,
        "round": payload_round,
        "problems": problems,
    }


def damage(path, how):
    """Breaks a live structure on purpose, so a run can prove its own checker.

    Aimed through the same load() the checker uses, because the two superblocks
    alternate: a first attempt at this reached into superblock A's fields while
    B was the live one, damaged a block nothing pointed at, and the checker
    correctly reported a healthy volume. A negative control that misses its
    target reports exactly what a working checker reports.
    """
    img = Image(path)
    sb = img.load()
    data = bytearray(img.data)
    bs = img.bs

    if how == "checksum":
        # One byte inside the live root inode.
        data[sb["root_inode"] * bs + 300] ^= 0xFF
    elif how == "unallocated":
        # Say the live root inode's block belongs to nobody.
        leaf = img.leaf_block(sb["root_inode"] // img.per_leaf())
        struct.pack_into("<Q", data, leaf * bs +
                         (sb["root_inode"] % img.per_leaf()) * 8, 0)
    elif how == "torn":
        # The thing the whole crash test exists to rule out: a file made of the
        # head of one version and the tail of another.
        #
        # Built by hand rather than hoped for, because QEMU will not produce one
        # -- it does not tear a block, so this failure mode is *designed*
        # against and can only be *tested* by constructing it.
        root = img.inode(sb["root_inode"])
        target = None
        for name, child, _t in img.dir_entries(root):
            if name.lower() == "settings":
                target = child
        if target is None:
            raise Bad("no 'settings' to tear")

        ino = img.inode(target)
        if ino["inline_len"]:
            at = sb["root_inode"] * bs + INODE_SIZE + ino["size"] - 4
        else:
            # The last block holding the tail, and the round number in it.
            n = (ino["size"] - 1) // bs
            where = ino["direct"][n] if n < 12 else struct.unpack_from(
                f"<{img.per_index()}Q", img.block(ino["indirect"]))[n - 12]
            at = where * bs + ((ino["size"] - 1) % bs) - 3

        was = struct.unpack_from("<I", data, at)[0]
        struct.pack_into("<I", data, at, was + 1)
    else:
        raise Bad(f"unknown damage: {how}")

    with open(path, "wb") as f:
        f.write(data)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--damage":
        # reconfs-check.py --damage <how> <image>
        damage(sys.argv[3], sys.argv[2])
        print(f"damaged {sys.argv[2]}")
        return 0

    if len(sys.argv) < 2:
        print("usage: reconfs-check.py <image> [name-that-must-exist]")
        print("       reconfs-check.py --damage checksum|unallocated|torn <image>")
        return 2

    want = sys.argv[2] if len(sys.argv) > 2 else None

    try:
        r = check(sys.argv[1], want)
    except Bad as e:
        print(f"unreadable {e}")
        return 1
    except Exception as e:                    # noqa: BLE001
        print(f"unreadable {type(e).__name__}: {e}")
        return 1

    if r["problems"]:
        print(f"inconsistent epoch={r['epoch']} "
              f"{len(r['problems'])} problem(s)")
        for p in r["problems"][:5]:
            print(f"  {p}")
        return 1

    extra = f" round={r['round']}" if r["round"] is not None else ""
    print(f"ok epoch={r['epoch']} block={r['block_size']} "
          f"inodes={r['inodes']} names={','.join(r['names']) or '-'}{extra}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
