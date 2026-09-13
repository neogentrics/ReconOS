/* ReconFS -- the layout arithmetic, checked at sizes no disk here can reach.
 *
 * --- Why this is a separate test from the one that formats a volume ---
 *
 * The format-and-check self-test runs on a disk, which means it runs at the
 * sizes a disk in this build environment can be. The largest available here is
 * a few gigabytes, and the arithmetic that decides how deep the owner table's
 * tree is, and how many blocks it needs, is exercised only in its smallest
 * case.
 *
 * That is precisely where a capacity bug lives. This format had one: the owner
 * was a 32-bit block number, capping a volume at 16TiB, and every test passed
 * because every test ran on a volume a hundred thousand times smaller than the
 * limit. The limit was found by being asked about, not by being hit.
 *
 * So the arithmetic is tested separately from the disk, at sizes from a
 * gigabyte to an exabyte, with no device involved. It is fast, it writes
 * nothing, and it runs on every boot -- which means a change that reintroduces
 * a ceiling is caught on the machine the change was made on rather than by
 * somebody with a big drive.
 *
 * --- What "correct" means here ---
 *
 * Three properties, and each one is a way the arithmetic could be wrong:
 *
 *   the tree is deep enough      depth levels of fan-out must cover every leaf,
 *                                or blocks past the end are unaddressable
 *   the tree is not too deep     one level too many is a wasted indirection on
 *                                every single lookup
 *   nothing overflows            the block counts are u64 and the intermediate
 *                                products must stay that way
 */
#include <recon/kernel/reconfs.h>

#include <recon/kernel/console.h>

/* Repeated here rather than shared with reconfs.c on purpose.
 *
 * A test that calls the same function it is testing proves the function agrees
 * with itself. These are written from the definition -- depth is how many times
 * you divide the leaf count by the fan-out before reaching one -- so a mistake
 * in reconfs.c shows up as a disagreement rather than as agreement. */
static u32 expected_depth(u64 blocks, u32 per_leaf, u32 per_index)
{
	u64 leaves = (blocks + per_leaf - 1) / per_leaf;
	u32 depth = 0;

	while (leaves > 1) {
		leaves = (leaves + per_index - 1) / per_index;
		depth++;
	}
	return depth;
}

/* Does a tree of this depth reach every leaf?
 *
 * Computed by multiplying up and stopping at the first sign of overflow, rather
 * than by multiplying and checking afterwards -- which is the bug this is
 * looking for, written into the checker. */
static bool covers(u32 depth, u32 per_leaf, u32 per_index, u64 blocks)
{
	u64 reach = per_leaf;
	u32 i;

	for (i = 0; i < depth; i++) {
		if (reach > 0xFFFFFFFFFFFFFFFFULL / per_index)
			return true;		/* already past any real volume */
		reach *= per_index;
	}

	return reach >= blocks;
}

bool reconfs_layout_self_test(void)
{
	static const u64 sizes[] = {
		1ULL << 30,		/* 1 GiB */
		1ULL << 40,		/* 1 TiB */
		16ULL << 40,		/* 16 TiB -- where the old ceiling was */
		24ULL * 1000 * 1000 * 1000 * 1000,	/* a 24TB drive, as sold */
		1ULL << 50,		/* 1 PiB */
		1ULL << 60,		/* 1 EiB */
	};
	static const u32 block_sizes[] = { 4096, 16384, 65536 };
	unsigned i, j;
	bool ok = true;

	for (i = 0; i < RK_ARRAY_LEN(sizes); i++) {
		for (j = 0; j < RK_ARRAY_LEN(block_sizes); j++) {
			u32 bs = block_sizes[j];
			u32 per_leaf = RECONFS_PER_LEAF_FOR(bs);
			u32 per_index = RECONFS_PER_INDEX_FOR(bs);
			u64 blocks = sizes[i] / bs;
			u32 depth;

			if (!blocks)
				continue;

			depth = expected_depth(blocks, per_leaf, per_index);

			/* The thing actually under test: what reconfs.c would
			 * choose, against what the definition says it should be.
			 *
			 * The first version of this test never called reconfs.c at
			 * all -- it derived the depth locally and then checked its
			 * own derivation, which passes whatever the format does. */
			if (reconfs_depth_for(blocks, per_leaf, per_index) != depth) {
				kprintf("  reconfs layout: %llu blocks of %u -- the "
					"format says depth %u, the definition says "
					"%u\n",
					(unsigned long long)blocks, bs,
					reconfs_depth_for(blocks, per_leaf, per_index),
					depth);
				ok = false;
			}

			if (!covers(depth, per_leaf, per_index, blocks)) {
				kprintf("  reconfs layout: %llu blocks of %u "
					"need more than %u levels\n",
					(unsigned long long)blocks, bs, depth);
				ok = false;
			}

			/* One level shallower must *not* be enough, or the tree
			 * carries an indirection nothing needs. */
			if (depth > 0 &&
			    covers(depth - 1, per_leaf, per_index, blocks)) {
				kprintf("  reconfs layout: %llu blocks of %u "
					"use %u levels where %u would do\n",
					(unsigned long long)blocks, bs, depth,
					depth - 1);
				ok = false;
			}

			/* The volume must be able to hold its own owner table.
			 *
			 * Written as a real computation rather than as a
			 * comparison that cannot fail: the first version of this
			 * check compared a u64 block number against itself stored
			 * in a u64 and called that proof the owner was wide
			 * enough. It was true by construction and tested
			 * nothing -- the same shape of vacuous pass this session
			 * has already found three of. */
			{
				u64 leaves = (blocks + per_leaf - 1) / per_leaf;
				u64 total = leaves;
				u64 level = leaves;
				u32 d;

				for (d = 0; d < depth; d++) {
					level = (level + per_index - 1) / per_index;
					if (total > 0xFFFFFFFFFFFFFFFFULL - level) {
						kputs("  reconfs layout: the table size "
						      "overflows\n");
						ok = false;
						break;
					}
					total += level;
				}

				if (reconfs_blocks_for_table(blocks, depth, per_leaf,
							     per_index) != total) {
					kprintf("  reconfs layout: %llu blocks of %u -- "
						"the format and the definition disagree "
						"about the table size\n",
						(unsigned long long)blocks, bs);
					ok = false;
				}

				if (total >= blocks) {
					kprintf("  reconfs layout: %llu blocks of %u "
						"need %llu for the table alone\n",
						(unsigned long long)blocks, bs,
						(unsigned long long)total);
					ok = false;
				}
			}
		}
	}

	return ok;
}
