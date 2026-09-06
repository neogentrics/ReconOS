#!/usr/bin/env python3
"""A second reader for FAT32, and a way to break one on purpose.

Written from the specification and from `fat32.h`, not from `fat32.c`. That is
the point: a reader and a checker built from the same misunderstanding agree
perfectly, so the only opinion worth having here is one that shares no code.

It does two jobs:

  --show            parse the volume and print what it is, so the kernel's
                    answer can be compared against an answer computed
                    independently.

  --damage <mode>   break it in one specific way, so the kernel's reader can be
                    shown a fault and watched to refuse it. A reader that has
                    never been seen to fail is not a reader yet -- this project
                    has shipped three checks that reported success while doing
                    nothing.

The damage modes each target a *different* refusal, so passing all of them
means the refusals are distinct rather than one catch-all:

  signature   zero the 0xAA55 at the end of the boot sector   -> not a FAT volume
  fats        flip one byte in the second allocation table    -> the tables disagree
  chain       point a cluster in use at a cluster that is not -> a chain that cannot be true
  size        claim the volume is longer than the device      -> not a FAT volume
"""
import argparse
import struct
import sys


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Volume:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.boot = f.read(512)

        if u16(self.boot, 510) != 0xAA55:
            raise SystemExit("no boot signature: not a FAT volume")

        self.bytes_per_sector = u16(self.boot, 11)
        self.sectors_per_cluster = self.boot[13]
        self.reserved = u16(self.boot, 14)
        self.fat_count = self.boot[16]
        self.root_entries = u16(self.boot, 17)
        tot16 = u16(self.boot, 19)
        fat16 = u16(self.boot, 22)
        self.fat_sectors = fat16 or u32(self.boot, 36)
        self.total_sectors = tot16 or u32(self.boot, 32)
        self.root_cluster = u32(self.boot, 44) & 0x0FFFFFFF

        root_dir_sectors = ((self.root_entries * 32) +
                            (self.bytes_per_sector - 1)) // self.bytes_per_sector
        self.data_start = (self.reserved +
                           self.fat_count * self.fat_sectors + root_dir_sectors)
        self.data_clusters = ((self.total_sectors - self.data_start) //
                              self.sectors_per_cluster)

        # The type is the cluster count and nothing else. The string at offset
        # 82 reads "FAT32   " and the specification says outright that it must
        # not be used to decide.
        if self.data_clusters <= 4084:
            self.kind = "FAT12"
        elif self.data_clusters <= 65524:
            self.kind = "FAT16"
        else:
            self.kind = "FAT32"

    def fat_offset(self, copy, cluster):
        sector = self.reserved + copy * self.fat_sectors
        return (sector * self.bytes_per_sector) + cluster * 4

    def read_fat(self, cluster, copy=0):
        with open(self.path, "rb") as f:
            f.seek(self.fat_offset(copy, cluster))
            return u32(f.read(4), 0) & 0x0FFFFFFF

    def first_used_chain(self):
        """A cluster that is in use and has a successor -- something whose
        chain the kernel will actually walk."""
        for c in range(2, min(self.data_clusters + 2, 5000)):
            nxt = self.read_fat(c)
            if 2 <= nxt < self.data_clusters + 2:
                return c
        raise SystemExit("no multi-cluster chain found to damage")

    def poke(self, offset, data):
        with open(self.path, "r+b") as f:
            f.seek(offset)
            f.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--damage",
                    choices=("signature", "fats", "chain", "size"))
    args = ap.parse_args()

    v = Volume(args.image)

    if args.show or not args.damage:
        print("kind              : %s" % v.kind)
        print("bytes per sector  : %d" % v.bytes_per_sector)
        print("sectors/cluster   : %d" % v.sectors_per_cluster)
        print("allocation tables : %d of %d sectors" % (v.fat_count,
                                                        v.fat_sectors))
        print("data starts at    : sector %d" % v.data_start)
        print("data clusters     : %d" % v.data_clusters)
        print("root cluster      : %d" % v.root_cluster)

    if not args.damage:
        return

    if args.damage == "signature":
        v.poke(510, b"\x00\x00")
        print("damaged: boot signature zeroed")

    elif args.damage == "fats":
        if v.fat_count < 2:
            raise SystemExit("this volume has one table; nothing to disagree")
        c = v.first_used_chain()
        off = v.fat_offset(1, c)
        with open(args.image, "rb") as f:
            f.seek(off)
            was = f.read(4)
        v.poke(off, bytes([was[0] ^ 0xFF]) + was[1:])
        print("damaged: second table's entry for cluster %d differs" % c)

    elif args.damage == "chain":
        c = v.first_used_chain()
        # A cluster number past the end of the volume: in use, and impossible.
        v.poke(v.fat_offset(0, c), struct.pack("<I", v.data_clusters + 9999))
        if v.fat_count > 1:
            v.poke(v.fat_offset(1, c), struct.pack("<I", v.data_clusters + 9999))
        print("damaged: cluster %d now points outside the volume" % c)

    elif args.damage == "size":
        # Longer than the device. A reader that trusts it walks off the end.
        v.poke(32, struct.pack("<I", 0xFFFFFFF0))
        print("damaged: the volume claims to be longer than the device")


if __name__ == "__main__":
    sys.exit(main())
