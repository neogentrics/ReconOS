/* See swap.h for why this is a partition and not a file. */
#include <recon/kernel/swap.h>
#include <recon/kernel/block.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/boot.h>

/* How many pages the store can hold.
 *
 * A visible number rather than one derived from the device, because the bitmap
 * below is statically sized and a bitmap sized from a disk is an allocation --
 * on a path whose entire point is that it never allocates. 32768 slots is 128 MB
 * of swap at a 4 KB page, which is more than any machine this has run on has
 * memory. A larger device is used up to this and the rest is reported as unused
 * rather than silently ignored.
 */
#define SWAP_SLOTS_MAX 32768

static struct block_device *store;
static struct spinlock swap_lock = SPINLOCK_INIT("swap");

/* One bit per slot, and the same read-modify-write on a shared byte that the
 * page allocator has -- so it is under the lock for the same reason, and that
 * reason is written down in pmm.c under BG-149. */
static u8 in_use[SWAP_SLOTS_MAX / 8];
static u32 slots;			/* how many the device actually gives */
static u32 next_hint;
static u32 used;

static u64 writes, reads, write_failures, read_failures, full_refusals;

/* Blocks per page. A device whose blocks are larger than a page is refused
 * rather than handled: a page would live inside part of a block, and writing it
 * would mean reading the block first to preserve the rest -- a read on the
 * eviction path, which is the one thing this design exists to avoid. */
static u32 blocks_per_page;

static bool bit_test(u32 slot)
{
	return (in_use[slot / 8] >> (slot % 8)) & 1u;
}

static void bit_set(u32 slot)
{
	in_use[slot / 8] |= (u8)(1u << (slot % 8));
	used++;
}

static void bit_clear(u32 slot)
{
	if (bit_test(slot)) {
		in_use[slot / 8] &= (u8)~(1u << (slot % 8));
		used--;
	}
}

bool swap_attach(struct block_device *dev)
{
	u64 possible;

	if (!dev || !dev->present)
		return false;

	if (dev->read_only) {
		kputs("  swap: that device is read-only\n");
		return false;
	}

	/* Claimed, which is this kernel's way of saying out loud that a caller
	  * means to write over a whole device.
	  *
	  * It is worth being precise about what this does and does not check,
	  * because the name invites the wrong reading. block_claim_raw does not
	  * ask whether the device carries a filesystem -- it is the mechanism
	  * for *permitting* a raw write, and the question it makes somebody ask
	  * is "am I allowed to destroy this disk". Swap answers yes, because
	  * writing over the whole thing is exactly what it is for.
	  *
	  * **Which device is swap is therefore the caller's decision, not this
	  * function's.** Getting it wrong means a machine that destroys its own
	  * system volume at the moment it is busiest, so it is not a decision to
	  * make by guessing: it comes from a partition the installer marked, and
	  * until the installer marks one it comes from the command line, the
	  * same way `reconfs=` and `durability=` already do. There is no code
	  * here that picks a device by looking at it. */
	if (block_claim_raw(dev) != BLOCK_OK) {
		kprintf("  swap: %s could not be claimed for raw writes\n",
			dev->name);
		return false;
	}

	if (dev->block_size > PAGE_SIZE) {
		/* Reading the rest of the block to preserve it would be a read
		 * on the eviction path. Refused rather than worked around. */
		kprintf("  swap: %s has %u-byte blocks, larger than a page\n",
			dev->name, dev->block_size);
		block_release_raw(dev);
		return false;
	}

	blocks_per_page = PAGE_SIZE / dev->block_size;
	possible = dev->block_count / blocks_per_page;

	if (possible == 0) {
		kprintf("  swap: %s is smaller than one page\n", dev->name);
		block_release_raw(dev);
		return false;
	}

	if (possible > SWAP_SLOTS_MAX) {
		kprintf("  swap: %s holds %lu pages; using %u of them\n",
			dev->name, (unsigned long)possible,
			(unsigned)SWAP_SLOTS_MAX);
		possible = SWAP_SLOTS_MAX;
	}

	store = dev;
	slots = (u32)possible;
	next_hint = 0;
	used = 0;
	kmemset(in_use, 0, sizeof(in_use));

	/* Slot zero is never handed out, so that SWAP_NONE can be zero and a
	 * zeroed field means "not swapped" without a second flag beside it to
	 * disagree with. One page of a partition is a cheap price for removing
	 * a whole class of "was this set?" question. */
	bit_set(0);

	return true;
}

bool swap_present(void)
{
	return store != NULL;
}

swap_slot_t swap_write_page(const void *page)
{
	u32 slot = 0;
	u32 scanned;
	u64 flags;
	enum block_status st;

	if (!store || !page)
		return SWAP_NONE;

	flags = spin_lock_irq(&swap_lock);

	/* One pass from the hint, wrapping once. Bounded by construction: a
	 * full circuit means the store is genuinely full and the answer is
	 * genuinely no. */
	for (scanned = 0; scanned < slots; scanned++) {
		u32 i = (next_hint + scanned) % slots;

		if (!bit_test(i)) {
			slot = i;
			break;
		}
	}

	if (!slot) {
		full_refusals++;
		spin_unlock_irq(&swap_lock, flags);
		return SWAP_NONE;
	}

	/* Claimed before the write, and the lock dropped before it.
	 *
	 * The bit is what stops two processors picking the same slot, so it has
	 * to be set under the lock. The write is a disk operation and must not
	 * be: holding a lock across it would stop every other processor in the
	 * machine from evicting a page for the length of a write to storage --
	 * on the path that runs when memory is short, which is when several
	 * processors are most likely to be on it at once.
	 *
	 * The same shape as the page allocator clearing outside its lock, and
	 * for the same reason. */
	bit_set(slot);
	next_hint = (slot + 1) % slots;
	spin_unlock_irq(&swap_lock, flags);

	st = block_write(store, (u64)slot * blocks_per_page, blocks_per_page,
			 (void *)page);

	if (st != BLOCK_OK) {
		/* Given back, because a slot claimed for a write that failed is
		 * a slot nothing will ever use or free. */
		flags = spin_lock_irq(&swap_lock);
		bit_clear(slot);
		write_failures++;
		spin_unlock_irq(&swap_lock, flags);
		return SWAP_NONE;
	}

	__atomic_add_fetch(&writes, 1, __ATOMIC_RELAXED);
	return (swap_slot_t)slot;
}

bool swap_read_page(swap_slot_t slot, void *page)
{
	enum block_status st;

	if (!store || !page || slot == SWAP_NONE || slot >= slots)
		return false;

	/* Reading a slot nothing wrote is a bug in the caller, not a page of
	 * zeroes. It is checked because the alternative is a program silently
	 * given whatever the partition held before -- which on a reinstalled
	 * machine is the last installation's memory. */
	if (!bit_test(slot)) {
		read_failures++;
		return false;
	}

	st = block_read(store, (u64)slot * blocks_per_page, blocks_per_page,
			page);

	if (st != BLOCK_OK) {
		__atomic_add_fetch(&read_failures, 1, __ATOMIC_RELAXED);
		return false;
	}

	__atomic_add_fetch(&reads, 1, __ATOMIC_RELAXED);
	return true;
}

void swap_free(swap_slot_t slot)
{
	u64 flags;

	if (!store || slot == SWAP_NONE || slot >= slots)
		return;

	flags = spin_lock_irq(&swap_lock);
	bit_clear(slot);
	spin_unlock_irq(&swap_lock, flags);
}

void swap_print_summary(void)
{
	kprintf("\nSwap\n");

	if (!store) {
		/* Ordinary, not a failure: no machine in the rig has a swap
		 * partition, and one booted from install media has not been
		 * installed onto. */
		kputs("  store        : none; nothing will be evicted\n");
		return;
	}

	kprintf("  store        : %s, %u slots of %u KB, %u in use\n",
		store->name, slots, (unsigned)(PAGE_SIZE / 1024), used);
	kprintf("  activity     : %lu written, %lu read", writes, reads);

	if (write_failures || read_failures || full_refusals)
		kprintf(", %lu write failures, %lu read failures, %lu refused "
			"for want of room", write_failures, read_failures,
			full_refusals);

	kputs("\n");
}

/* --- the self-test --------------------------------------------------------
 *
 * A store that wrote nothing and returned a slot would pass "it did not report
 * an error". So every assertion here is about a page coming *back*, and the
 * pages are filled with values that could not be there by accident.
 *
 * The one that matters most is the third: two pages written to two slots must
 * come back as two different pages. A store that returned the same slot twice,
 * or wrote both to the same place, passes every check that only reads one back.
 */
bool swap_self_test(void)
{
	static const u32 marks[3] = { 0x50414745u, 0x53574150u, 0x53544F52u };
	paddr_t page;
	u32 *buf;
	swap_slot_t slot[3];
	unsigned i, j;
	bool ok = true;

	if (!store) {
		kputs("  swap: no store on this machine to test against\n");
		return true;
	}

	page = pmm_alloc_page();
	if (!page) {
		kputs("  swap: no page to test with\n");
		return false;
	}

	buf = phys_to_virt(page);

	/* Three pages, each filled end to end with its own mark. Filled to the
	 * end rather than at the front, because a store that wrote only the
	 * first block of a page would pass a check that read only the first
	 * word -- and the whole point of blocks_per_page is that a page is
	 * more than one block on most devices. */
	for (i = 0; i < 3; i++) {
		for (j = 0; j < PAGE_SIZE / sizeof(u32); j++)
			buf[j] = marks[i] ^ j;

		slot[i] = swap_write_page(buf);

		if (slot[i] == SWAP_NONE) {
			kprintf("  swap: writing page %u found nowhere to put "
				"it\n", i);
			pmm_free_page(page);
			return false;
		}
	}

	/* Three distinct slots. A store handing out the same one twice loses a
	 * page silently, and every read-back below would still succeed. */
	if (slot[0] == slot[1] || slot[1] == slot[2] || slot[0] == slot[2]) {
		kprintf("  swap: three writes got slots %u, %u, %u\n",
			slot[0], slot[1], slot[2]);
		ok = false;
	}

	/* Read back out of order, so that a store which simply returned the
	 * last thing written would be caught. */
	for (i = 0; i < 3; i++) {
		unsigned which = (i + 2) % 3;

		kmemset(buf, 0xA5, PAGE_SIZE);

		if (!swap_read_page(slot[which], buf)) {
			kprintf("  swap: reading slot %u back failed\n",
				slot[which]);
			ok = false;
			continue;
		}

		for (j = 0; j < PAGE_SIZE / sizeof(u32); j++)
			if (buf[j] != (marks[which] ^ j)) {
				kprintf("  swap: page %u came back wrong at "
					"word %u\n", which, j);
				ok = false;
				break;
			}
	}

	/* A slot nobody wrote is refused rather than answered with whatever the
	 * partition held. On a reinstalled machine that would be the previous
	 * installation's memory. */
	{
		swap_slot_t free_one = slot[1];

		swap_free(free_one);

		if (swap_read_page(free_one, buf)) {
			kputs("  swap: reading a slot that was given back was "
			      "allowed\n");
			ok = false;
		}
	}

	/* Slot zero is never handed out, and there is deliberately no check for
	  * it here: SWAP_NONE *is* zero, so a returned zero was already caught
	  * above as a write that found nowhere to go. The property is structural
	  * rather than asserted, and a check for it could not fail. */
	swap_free(slot[0]);
	swap_free(slot[2]);

	pmm_free_page(page);
	return ok;
}

/* --- which device -------------------------------------------------------
 *
 * `swap=<name>` on the command line, and nothing else. There is no code here
 * that picks a device by looking at it, because this writes over a whole device
 * from the first eviction and a machine that guessed wrong would destroy its own
 * system volume at the moment it was busiest.
 *
 * The installer will mark a partition for it, and then this reads the mark
 * instead. Until then the command line is how the rig says which one, exactly as
 * `reconfs=` and `durability=` already do.
 *
 * THIS IS THE THIRD COPY OF THIS PARSER. The first one's comment said "a parser
 * invented before its second caller gets its shape wrong", which was right and
 * has now been overtaken: there are three callers and they agree. It should be
 * one function. It is deliberately not being made into one *in this change* --
 * factoring code that the matrix covers, inside the same commit that adds two
 * new subsystems, means a red run with two candidate causes. It is written down
 * here so the next person to add a fourth does it instead of copying.
 */
static const char *wanted_device(void)
{
	static char name[BLOCK_NAME_MAX];
	const char *p = boot_info()->cmdline;
	static const char key[] = "swap=";

	if (!p)
		return 0;

	while (*p) {
		const char *k = key;
		const char *q = p;

		while (*k && *q == *k) {
			q++;
			k++;
		}

		if (!*k) {
			size_t n = 0;

			while (q[n] && q[n] != ' ' && n + 1 < sizeof(name)) {
				name[n] = q[n];
				n++;
			}
			name[n] = '\0';
			return n ? name : 0;
		}

		while (*p && *p != ' ')
			p++;
		while (*p == ' ')
			p++;
	}

	return 0;
}

void swap_init_from_cmdline(void)
{
	const char *want = wanted_device();
	unsigned i;

	if (!want)
		return;

	for (i = 0; i < block_device_count(); i++) {
		struct block_device *d = block_device_at(i);

		if (!d || !d->present)
			continue;

		if (kstrlen(d->name) == kstrlen(want) &&
		    kmemcmp(d->name, want, kstrlen(want)) == 0) {
			if (swap_attach(d))
				kprintf("  swap: using %s\n", d->name);
			return;
		}
	}

	kprintf("  swap: no device called %s\n", want);
}
