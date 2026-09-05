/* Reading a partition table, for its geometry and nothing else.
 *
 * --- Why this is in the kernel at all ---
 *
 * The desktop-facing API used to say that partition tables were the caller's
 * business, because GPT and MBR are data formats and a kernel that parses one
 * has taken on a parser it did not need. That rule was written before there was
 * a block layer, and the repository had already argued with it: the kernel
 * parses a device tree because it has to find its own memory, and ACPI tables
 * because it has to find its own bus. Where a partition begins is that kind of
 * question -- one the kernel must answer before anything exists above it to
 * answer it.
 *
 * So the rule is narrowed rather than abandoned. The kernel reads what it must
 * to answer a question it has to answer first, and nothing else.
 *
 * --- Where the line inside the format is, and why it is stable ---
 *
 * The kernel reads the fixed-width integers that say *where sectors are*: the
 * starting and ending block of each entry, how many entries there are and how
 * far apart, and where the two copies of the header live. Every disk already
 * written froze those, and they cannot move.
 *
 * It does not read type GUIDs, partition names, or attribute bits. Those are
 * specified to grow -- a standards body or a vendor can add a meaning without
 * changing the format -- and a kernel you have to ship again to recognise a new
 * partition type is the wrong shape.
 *
 * The test for any field a later change wants to add: can somebody change what
 * this field *means* without changing the format? If yes, it belongs to the
 * caller. Apple's partition map is entirely the caller's and stays there, which
 * is what makes this a seam rather than a rebrand.
 *
 * --- What it costs, said plainly ---
 *
 * This is a parser running with kernel privilege over numbers chosen by
 * whoever last wrote the disk, which on the machines this exists for is
 * Windows. So: it allocates nothing, it never writes, every field is bounded
 * before the field after it is read, and it is tested against disks partitioned
 * by tools that share no code with it.
 */
#ifndef RECON_KERNEL_PARTITION_H
#define RECON_KERNEL_PARTITION_H

#include <recon/kernel/types.h>

struct block_device;

/* Reads the table on one whole device and registers each partition as a block
 * device of its own. Does nothing to a device that is already a slice.
 *
 * Never fails in a way a caller has to handle: a disk with no table, an
 * unreadable table and a disk that could not be read all leave the device
 * usable as a whole device, with `scheme` saying which happened. */
void partition_scan(struct block_device *dev);

bool partition_self_test(void);

#endif /* RECON_KERNEL_PARTITION_H */
