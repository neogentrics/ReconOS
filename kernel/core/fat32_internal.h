/* Shared between fat32.c and fat32_write.c, and nowhere else.
 *
 * Not in <recon/kernel/fat32.h> on purpose. That header is the interface the
 * rest of the kernel uses, and everything in it is a promise. These are the
 * seams inside one driver, split across two files only because reading and
 * writing a foreign filesystem are different enough jobs to deserve separate
 * files -- the same reason ReconFS is four.
 */
#ifndef RECON_KERNEL_FAT32_INTERNAL_H
#define RECON_KERNEL_FAT32_INTERNAL_H

#include <recon/kernel/fat32.h>

u16 fat_le16(const u8 *p);
u32 fat_le32(const u8 *p);
void fat_put16(u8 *p, u16 v);
void fat_put32(u8 *p, u32 v);

enum fat32_status fat_read_sectors(struct fat32 *fs, u64 vol_sector, u32 count,
				   void *buf);
enum fat32_status fat_write_sectors(struct fat32 *fs, u64 vol_sector, u32 count,
				    const void *buf);

u64 fat_cluster_to_sector(const struct fat32 *fs, u32 cluster);

/* One entry of the allocation table, read from the first copy and checked
 * against the second. See the comment in fat32.c for why it is per sector. */
enum fat32_status fat_entry_get(struct fat32 *fs, u32 cluster, u32 *value);

/* The checksum tying long-name fragments to the 8.3 entry they belong to. */
u8 fat_alias_checksum(const u8 *name11);

/* True when `cluster` is a real data cluster on this volume. */
bool fat_cluster_ok(const struct fat32 *fs, u32 cluster);

#endif /* RECON_KERNEL_FAT32_INTERNAL_H */
