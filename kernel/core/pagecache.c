/* One copy of a file's page. See pagecache.h for what this is for. */
#include <recon/kernel/pagecache.h>

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/rootfs.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/wait.h>

/* Small, and a fixed table rather than a growing one.
 *
 * A cache that allocates to remember things is a cache that fails at the
 * moment memory is short, which is the moment it is most wanted -- the same
 * argument that put swap on a partition rather than in a file. When it is
 * full, `pagecache_get` says so and the caller fills a page privately, which
 * is exactly what every caller did before this existed.
 */
#define PAGECACHE_MAX 64

struct entry {
	u64 id;			/* what the filesystem calls this file */
	u64 offset;		/* page-aligned, into the file */
	paddr_t page;
	unsigned refs;		/* mappings holding it */
	bool valid;

	/* The file changed after this was read. Nothing new is served from it
	 * and it goes when the last mapping does -- see pagecache_forget. */
	bool stale;

	/* Somebody has mapped this writably and shared, so the page may hold
	 * something the file does not. */
	bool dirty;

	/* How much of the page the file actually backs. Written back and no
	 * more: the rest is padding this put there, and returning it would
	 * grow the file to a page boundary every time anybody mapped it. */
	u32 bytes;

	/* A reference to the file, so the page can be put back without the
	 * mapping that dirtied it still being around to ask.
	 *
	 * This pins the file open for as long as the page is cached, which is
	 * a real cost and the reason it is only taken for entries that can be
	 * written back at all. A cache that held every file it had ever read
	 * would be a machine that never closes anything. */
	struct file *owner;
};

static struct entry entries[PAGECACHE_MAX];
static struct spinlock lock = SPINLOCK_INIT("pagecache");

/* Held across the read, and separate from the lock above for the reason the
 * mapping fill learned the hard way: a storage driver sleeps now, and a
 * spinlock held across a sleep is held by a thread that is not running. */
static struct mutex fill;

static u64 hits, misses, refills, full, unnamed, forgotten, stale_kept;
static u64 written_back, write_failed;

void pagecache_init(void)
{
	mutex_init(&fill, "pagecache-fill");
}

static struct entry *find(u64 id, u64 offset)
{
	unsigned i;

	for (i = 0; i < PAGECACHE_MAX; i++)
		if (entries[i].valid && !entries[i].stale &&
		    entries[i].id == id && entries[i].offset == offset)
			return &entries[i];

	return NULL;
}

static struct entry *spare(void)
{
	unsigned i;

	for (i = 0; i < PAGECACHE_MAX; i++)
		if (!entries[i].valid)
			return &entries[i];

	/* Nothing free. An entry nobody is holding could be taken -- that is
	 * eviction, and it is the next thing this wants. It is not here yet
	 * because taking a page back needs certainty that every mapping of it
	 * has gone, and the only thing that could say so is the reference
	 * count below, which teardown has only just started maintaining. A
	 * cache that evicted a page somebody still had mapped would hand one
	 * program's file to another. */
	return NULL;
}

/* Puts a dirty page back, and says whether it got there.
 *
 * The caller holds nothing: writing can sleep, and the table lock must not be
 * held across it. Everything this needs is copied out first.
 */
static bool write_back(struct file *f, u64 offset, paddr_t page, u32 bytes)
{
	i64 n;

	if (!f || !f->ops || !f->ops->write_at || !bytes)
		return false;

	n = f->ops->write_at(f, offset, phys_to_virt(page), bytes);

	return n == (i64)bytes;
}

paddr_t pagecache_get(struct file *f, u64 offset)
{
	struct entry *e;
	paddr_t page;
	u64 id;
	u64 flags;
	i64 n;

	if (!f || !f->ops || !f->ops->identity)
		return 0;

	if (offset & (PAGE_SIZE - 1))
		return 0;

	id = f->ops->identity(f);

	if (!id) {
		unnamed++;
		return 0;
	}

	flags = spin_lock_irq(&lock);
	e = find(id, offset);

	if (e) {
		e->refs++;
		page = e->page;
		hits++;
		spin_unlock_irq(&lock, flags);
		return page;
	}

	spin_unlock_irq(&lock, flags);

	/* Read outside the table lock, because reading is the slow part and
	 * because it can now sleep. Two faults on the same page can both get
	 * here and both read it -- the loser's copy is freed below rather than
	 * prevented, because the alternative is holding a lock across a disk. */
	page = pmm_alloc_page();

	if (!page)
		return 0;

	kmemset(phys_to_virt(page), 0, PAGE_SIZE);

	mutex_lock(&fill);

	if (f->ops->seek && f->ops->seek(f, (i64)offset, SEEK_START) < 0) {
		mutex_unlock(&fill);
		pmm_free_page(page);
		return 0;
	}

	n = f->ops->read ? f->ops->read(f, phys_to_virt(page), PAGE_SIZE) : -1;

	mutex_unlock(&fill);

	/* Short is not an error: it is the tail of a file, and the rest of the
	 * page is already zero. Failed is. */
	if (n < 0) {
		pmm_free_page(page);
		return 0;
	}

	flags = spin_lock_irq(&lock);

	/* Somebody may have filled it while this was reading. Theirs wins --
	 * not because it is better but because a second entry for one page of
	 * one file is the aliasing this exists to prevent. */
	e = find(id, offset);

	if (e) {
		e->refs++;
		refills++;
		spin_unlock_irq(&lock, flags);
		pmm_free_page(page);
		return e->page;
	}

	e = spare();

	if (!e) {
		full++;
		spin_unlock_irq(&lock, flags);
		pmm_free_page(page);
		return 0;
	}

	e->id = id;
	e->offset = offset;
	e->page = page;
	e->refs = 1;
	e->valid = true;
	e->bytes = (u32)n;

	/* Held only where it could be used. A file with no way to take a write
	 * back can never dirty a page here, so keeping it open would be a
	 * reference nothing would ever read. */
	e->owner = f->ops->write_at ? file_hold(f) : NULL;

	misses++;

	spin_unlock_irq(&lock, flags);
	return page;
}

void pagecache_put(paddr_t page)
{
	unsigned i;
	u64 flags;
	struct file *back_file = NULL;
	u64 back_off = 0;
	paddr_t back_page = 0;
	u32 back_bytes = 0;

	if (!page)
		return;

	flags = spin_lock_irq(&lock);

	for (i = 0; i < PAGECACHE_MAX; i++)
		if (entries[i].valid && entries[i].page == page) {
			if (entries[i].refs)
				entries[i].refs--;

			/* The last mapping has gone and the page may hold
			 * something the file does not. Copied out here and
			 * written after the lock, because writing can sleep. */
			if (!entries[i].refs && entries[i].dirty) {
				back_file = entries[i].owner;
				back_off = entries[i].offset;
				back_page = entries[i].page;
				back_bytes = entries[i].bytes;
				entries[i].dirty = false;
			}

			/* A stale entry is only being kept for the mappings
			 * that still point at it. When the last one lets go
			 * there is nothing left to keep. */
			if (!entries[i].refs && entries[i].stale) {
				pmm_free_page(entries[i].page);
				entries[i].valid = false;
				entries[i].stale = false;
				entries[i].page = 0;
				forgotten++;
			}
			break;
		}

	/* An entry that is not stale stays valid at zero references. It is
	 * still the right contents for that page of that file, and the next
	 * mapping of it should find it rather than read it again -- which is
	 * the whole point. Zero means "may be evicted", not "is empty". */
	spin_unlock_irq(&lock, flags);

	if (back_file) {
		if (write_back(back_file, back_off, back_page, back_bytes))
			written_back++;
		else
			write_failed++;
	}
}

bool pagecache_mark_shared(paddr_t page)
{
	unsigned i;
	u64 flags;
	bool ok = false;

	if (!page)
		return false;

	flags = spin_lock_irq(&lock);

	for (i = 0; i < PAGECACHE_MAX; i++)
		if (entries[i].valid && entries[i].page == page) {
			/* Only where the promise can be kept. Saying yes here
			 * for a file that cannot take the write back is how a
			 * program's changes get accepted and lost. */
			if (entries[i].owner) {
				entries[i].dirty = true;
				ok = true;
			}
			break;
		}

	spin_unlock_irq(&lock, flags);
	return ok;
}

void pagecache_forget(u64 id)
{
	unsigned i;
	u64 flags;

	if (!id)
		return;

	flags = spin_lock_irq(&lock);

	for (i = 0; i < PAGECACHE_MAX; i++) {
		if (!entries[i].valid || entries[i].id != id)
			continue;

		if (entries[i].refs) {
			/* Somebody has it mapped. Marked rather than taken:
			 * pulling the page would leave a program reading
			 * memory that had been handed to somebody else. */
			entries[i].stale = true;
			stale_kept++;
		} else {
			pmm_free_page(entries[i].page);
			entries[i].valid = false;
			entries[i].page = 0;
			forgotten++;
		}
	}

	spin_unlock_irq(&lock, flags);
}

bool pagecache_owns(paddr_t page)
{
	unsigned i;
	u64 flags;
	bool mine = false;

	if (!page)
		return false;

	flags = spin_lock_irq(&lock);

	for (i = 0; i < PAGECACHE_MAX; i++)
		if (entries[i].valid && entries[i].page == page) {
			mine = true;
			break;
		}

	spin_unlock_irq(&lock, flags);
	return mine;
}

/* --- the test -------------------------------------------------------------
 *
 * The claim is not "a page came back". It is that **the same page came back**,
 * which is the only thing that distinguishes a cache from the private fill it
 * replaced -- and a test that only checked the contents would pass either way
 * while the machine held two copies.
 *
 * Written against /tmp, because ramfs is the filesystem that can name a file:
 * a slot there is a file for the life of the machine, since names are never
 * removed. The volume says zero for now, and the test says so rather than
 * quietly not covering it.
 */
bool pagecache_self_test(void)
{
	struct file *one, *two;
	paddr_t a, b, c;
	i64 err = 0;
	bool ok = true;

	one = file_open_path("/tmp/cache-test", OPEN_WRITE | OPEN_CREATE,
			     0600, &err);

	if (!one) {
		kprintf("  pagecache: could not make a file to map (%ld)\n",
			(long)err);
		return false;
	}

	one->ops->write(one, "shared", 6);
	file_release(one);

	/* Two separate opens. That is the whole point: a `struct file *` is
	 * per-open, so keying on the pointer would give these two nothing in
	 * common. */
	one = file_open_path("/tmp/cache-test", OPEN_READ, 0, &err);
	two = file_open_path("/tmp/cache-test", OPEN_READ, 0, &err);

	if (!one || !two) {
		kputs("  pagecache: could not open the file twice\n");
		return false;
	}

	a = pagecache_get(one, 0);
	b = pagecache_get(two, 0);

	if (!a) {
		kputs("  pagecache: the first mapping got no page\n");
		ok = false;
	} else if (a != b) {
		kprintf("  pagecache: two opens of one file were given "
			"different pages (%p and %p), so nothing is "
			"shared\n", (void *)(uintptr_t)a,
			(void *)(uintptr_t)b);
		ok = false;
	}

	/* And the contents are the file's, not zeroes -- a cache that shared
	 * one blank page between every mapping would satisfy the check above
	 * perfectly. */
	if (ok && a && kmemcmp(phys_to_virt(a), "shared", 6) != 0) {
		kputs("  pagecache: the shared page does not hold what the "
		      "file does\n");
		ok = false;
	}

	/* A different offset is a different page. Keyed on the pair, not on
	 * the file. */
	c = pagecache_get(one, PAGE_SIZE);

	if (c && c == a) {
		kputs("  pagecache: two offsets of one file came back as the "
		      "same page\n");
		ok = false;
	}

	if (c)
		pagecache_put(c);

	if (a)
		pagecache_put(a);
	if (b)
		pagecache_put(b);

	file_release(one);
	file_release(two);

	return ok;
}

/* --- and that being told a file changed actually drops it -----------------
 *
 * The half a stable key makes necessary. Under a key that moved when the file
 * did, a rewrite was invalidation by accident; a dossier survives the rewrite,
 * so a cache that was never told would go on handing out the contents from
 * before it.
 *
 * **This tests the mechanism and not the path to it, and the difference is
 * worth stating.** `pagecache_forget` is called from the commit in
 * `disk_close` -- and that commit cannot currently replace an existing file:
 * `rootfs_create_file` answers `ERR_EXISTS` and changes nothing. So the wiring
 * is real and the caller cannot yet fire it, which was found by writing this
 * test the obvious way and watching a file rewritten as "second" read back as
 * "first". The cache was right; the test was wrong.
 *
 * What is checked here is that forgetting *works*, so that the day a file can
 * be overwritten the only new thing is the overwriting.
 *
 * The assertion is on the **page**, not the contents. The file does not change,
 * so the bytes are the same either way -- and a test that compared them would
 * pass whether or not anything had been dropped. A different physical page is
 * the only visible difference between a cache that re-read and one that did
 * not.
 */
void pagecache_run(void)
{
	const char *path = "/rewritten";
	struct file *f;
	paddr_t before = 0, after = 0;
	u64 id = 0;
	i64 err = 0;
	bool ok = true;

	if (!rootfs())
		return;

	f = file_open_path(path, OPEN_WRITE | OPEN_CREATE, 0600, &err);

	if (f) {
		f->ops->write(f, "first", 5);
		file_release(f);
	}

	if (rootfs_owner_of(path, 0, 0, 0, &id) != RECONFS_OK || !id) {
		kputs("  pagecache: the volume could not name the file it "
		      "just made\n");
		return;
	}

	f = file_open_path(path, OPEN_READ, 0, &err);
	before = f ? pagecache_get(f, 0) : 0;

	if (!before) {
		kputs("  pagecache: the file was not cached at all\n");
		ok = false;
	} else if (kmemcmp(phys_to_virt(before), "first", 5) != 0) {
		kputs("  pagecache: the cached page is not what was "
		      "written\n");
		ok = false;
	}

	/* Let go first, so the entry has no mappings and can be dropped
	 * outright rather than kept stale. Both paths matter; this is the one
	 * with a visible answer. */
	if (before)
		pagecache_put(before);

	pagecache_forget(id);

	after = f ? pagecache_get(f, 0) : 0;

	if (ok && !after) {
		kputs("  pagecache: nothing came back after the file was "
		      "forgotten\n");
		ok = false;
	} else if (ok && after == before) {
		kprintf("  pagecache: the same page came back after the file "
			"was forgotten (%p), so nothing was dropped\n",
			(void *)(uintptr_t)before);
		ok = false;
	} else if (ok && kmemcmp(phys_to_virt(after), "first", 5) != 0) {
		kputs("  pagecache: the page read again does not hold what "
		      "the file does\n");
		ok = false;
	}

	if (after)
		pagecache_put(after);
	if (f)
		file_release(f);

	kprintf("  a file can be forgotten : %s\n", ok ? "pass" : "FAIL");
}

void pagecache_print_summary(void)
{
	unsigned i, used = 0, held = 0;

	for (i = 0; i < PAGECACHE_MAX; i++)
		if (entries[i].valid) {
			used++;
			if (entries[i].refs)
				held++;
		}

	kprintf("\nPage cache\n");
	kprintf("  pages        : %u of %u, %u still mapped\n",
		used, (unsigned)PAGECACHE_MAX, held);
	kprintf("  faults       : %lu found it, %lu read it in\n",
		(unsigned long)hits, (unsigned long)misses);

	if (refills)
		kprintf("  raced        : %lu read a page somebody else had "
			"finished first\n", (unsigned long)refills);

	if (full)
		kprintf("  full         : %lu fault(s) filled a private page "
			"because there was no room\n", (unsigned long)full);

	if (unnamed)
		kprintf("  unnamed      : %lu fault(s) on a file whose "
			"filesystem cannot name it\n", (unsigned long)unnamed);

	if (written_back || write_failed)
		kprintf("  written back : %lu shared page(s) put back into "
			"their file%s\n", (unsigned long)written_back,
			write_failed ? ", and some could not be" : "");

	if (forgotten || stale_kept)
		kprintf("  rewritten    : %lu page(s) dropped because the file "
			"changed, %lu kept for mappings that still hold "
			"them\n",
			(unsigned long)forgotten, (unsigned long)stale_kept);
}
