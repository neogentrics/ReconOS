/* One copy of a file's page. See pagecache.h for what this is for. */
#include <recon/kernel/pagecache.h>

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/user.h>
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
	/* **Which filesystem**, and then what it calls the file. Both, because
	 * one is not a key.
	 *
	 * Each filesystem numbers its own files from its own space and has
	 * every right to: ramfs answers with a slot, so small integers, and the
	 * volume answers with a dossier, which starts small too. Keyed on the
	 * number alone, ramfs slot 10 and volume dossier 10 are the same entry
	 * -- and the cache hands one file's pages to the other.
	 *
	 * That is not hypothetical. It is what happened the first time the
	 * eviction test flooded the table: enough ramfs files to reach slot 10,
	 * and a volume file that had been reading correctly all along came back
	 * holding somebody else's bytes. The header above this file warns about
	 * exactly this shape, using the block cache's partition-and-disk as the
	 * example, and the warning was written before the mistake was made.
	 *
	 * The ops pointer is the filesystem: one static table per filesystem,
	 * for the life of the machine. */
	const struct file_ops *fs;

	u64 id;			/* what that filesystem calls this file */
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

	/* Looked at since the hand last passed. Cleared rather than evicted on
	 * the first pass, which is what stops a page that is being used in a
	 * loop from being taken because it happened to be next. */
	bool referenced;

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
static u64 written_back, write_failed, evictions, all_held;

void pagecache_init(void)
{
	mutex_init(&fill, "pagecache-fill");
}

static struct entry *find(const struct file_ops *fs, u64 id, u64 offset)
{
	unsigned i;

	for (i = 0; i < PAGECACHE_MAX; i++)
		if (entries[i].valid && !entries[i].stale &&
		    entries[i].fs == fs && entries[i].id == id &&
		    entries[i].offset == offset)
			return &entries[i];

	return NULL;
}

/* Defined below, beside the write it does. */
static bool write_back(struct file *f, u64 offset, paddr_t page,
                       u32 bytes);

/* Where the clock hand is. Static, because the point of a clock is that it
 * carries on from where it stopped -- restarting at zero every time would give
 * the entries at the front of the table a second chance for ever and the ones
 * at the back none at all. */
static unsigned hand;

/* A slot to fill, taking one back if there is no free one.
 *
 * **The safety property is the whole of this: an entry with a reference is
 * never taken.** A reference means a page table somewhere points at that page,
 * so evicting it would hand one program's file to whoever allocated next --
 * and the failure would not look like a cache bug, it would look like memory
 * corruption in an unrelated program.
 *
 * That is exactly why this could not be written until teardown started
 * maintaining the count. `refs == 0` is now a real statement about the page
 * tables of the whole machine rather than a hopeful one.
 *
 * Second chance, like the block cache's: an entry looked at since the hand last
 * passed gets its bit cleared and is left alone, so a page being used in a loop
 * is not taken merely because it was next. The caller holds the lock, and takes
 * the file reference away to release outside it.
 */
static struct entry *spare(struct file **release)
{
	unsigned i;

	*release = NULL;

	for (i = 0; i < PAGECACHE_MAX; i++)
		if (!entries[i].valid)
			return &entries[i];

	/* Two passes at most: the first clears the bits of everything it
	 * passes, so the second finds a victim unless every entry is held --
	 * which is a cache entirely in use, and an honest no. */
	for (i = 0; i < PAGECACHE_MAX * 2; i++) {
		struct entry *e = &entries[hand];

		hand = (hand + 1) % PAGECACHE_MAX;

		if (e->refs)
			continue;

		if (e->referenced) {
			e->referenced = false;
			continue;
		}

		/* Nobody holds it and nobody has asked for it since the hand
		 * last came round. A dirty page at zero references should not
		 * exist -- the last put writes it back -- but freeing one
		 * without looking would be trusting that rather than checking
		 * it. */
		if (e->dirty && e->owner)
			write_back(e->owner, e->offset, e->page, e->bytes);

		*release = e->owner;

		pmm_free_page(e->page);

		e->valid = false;
		e->stale = false;
		e->dirty = false;
		e->referenced = false;
		e->page = 0;
		e->owner = NULL;
		e->fs = NULL;
		e->id = 0;

		evictions++;
		return e;
	}

	all_held++;
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
	struct file *evicted_owner = NULL;

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
	e = find(f->ops, id, offset);

	if (e) {
		e->refs++;
		e->referenced = true;
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
	e = find(f->ops, id, offset);

	if (e) {
		e->refs++;
		refills++;
		spin_unlock_irq(&lock, flags);
		pmm_free_page(page);
		return e->page;
	}

	e = spare(&evicted_owner);

	if (!e) {
		full++;
		spin_unlock_irq(&lock, flags);
		pmm_free_page(page);
		return 0;
	}

	e->fs = f->ops;
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
	e->referenced = true;

	misses++;

	spin_unlock_irq(&lock, flags);

	/* Outside the lock: the last reference to a file commits it, which
	 * reaches a disk. */
	if (evicted_owner)
		file_release(evicted_owner);

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

void pagecache_forget(struct file *f)
{
	unsigned i;
	u64 id;
	u64 flags;

	if (!f || !f->ops || !f->ops->identity)
		return;

	id = f->ops->identity(f);

	if (!id)
		return;

	flags = spin_lock_irq(&lock);

	for (i = 0; i < PAGECACHE_MAX; i++) {
		/* The filesystem as well as the number, for the same reason
		 * `find` needs both: forgetting by number alone would drop
		 * another filesystem's file that happens to share it. */
		if (!entries[i].valid || entries[i].fs != f->ops ||
		    entries[i].id != id)
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
			entries[i].fs = NULL;
			entries[i].id = 0;
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

/* --- and that a rewritten file is not served from the copy before it -------
 *
 * The half a stable key makes necessary. Under a key that moved when the file
 * did, a rewrite was invalidation by accident; a dossier survives it, so the
 * cache has to be told -- and if it were not, every check would still pass
 * while the pages handed out were the ones from before the write.
 *
 * **This could not be tested through a real rewrite until today.** The first
 * attempt was written the obvious way and reported a file rewritten as
 * "second" reading back as "first", which looks exactly like stale pages. It
 * was not: `rootfs_create_file` answered ERR_EXISTS and the file was never
 * rewritten. The cache was right and the test was wrong. Replacing a file is
 * its own call now, asked for with its own flag, so the obvious way is finally
 * the correct one.
 */
void pagecache_run(void)
{
	const char *path = "/rewritten";
	struct file *f;
	paddr_t before = 0, after = 0;
	i64 err = 0;
	bool ok = true;
	char seen[8];

	if (!rootfs())
		return;

	/* Writes on every boot, by design -- it has to rewrite a file to prove
	 * the cache noticed. That is exactly what a recovery boot must not do,
	 * and this test writing during recovery is what the recovery harness
	 * caught by hashing the disk. */
	if (rootfs_is_read_only()) {
		kputs("  a rewrite is noticed : not run, the volume is "
		      "read-only on a recovery boot\n");
		return;
	}

	f = file_open_path(path, OPEN_WRITE | OPEN_CREATE, 0600, &err);

	if (f) {
		f->ops->write(f, "first", 5);
		file_release(f);
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

	if (before)
		pagecache_put(before);
	if (f)
		file_release(f);

	/* A real rewrite, through the ordinary path a program would use. The
	 * close is the commit, and the commit is where the cache is told. */
	f = file_open_path(path, OPEN_WRITE | OPEN_REPLACE, 0, &err);

	if (!f) {
		kprintf("  pagecache: the file could not be reopened to "
			"replace it (%ld)\n", (long)err);
		ok = false;
	} else {
		f->ops->write(f, "second", 6);

		if (file_release(f) != SYS_OK) {
			kputs("  pagecache: replacing the file failed\n");
			ok = false;
		}
	}

	f = file_open_path(path, OPEN_READ, 0, &err);
	after = f ? pagecache_get(f, 0) : 0;

	if (ok && !after) {
		kputs("  pagecache: nothing came back after the rewrite\n");
		ok = false;
	} else if (ok) {
		kmemcpy(seen, phys_to_virt(after), 6);
		seen[6] = 0;

		/* The contents, which is the assertion that matters now that a
		 * rewrite is real: a cache that was never told would hand back
		 * the page it read before the write, and it would look
		 * perfectly healthy doing it. */
		if (kmemcmp(seen, "second", 6) != 0) {
			kprintf("  pagecache: a file rewritten as \"second\" "
				"reads back as \"%s\" -- the cache was never "
				"told it changed\n", seen);
			ok = false;
		}
	}

	if (after)
		pagecache_put(after);
	if (f)
		file_release(f);

	kprintf("  a rewrite is noticed : %s\n", ok ? "pass" : "FAIL");
}

/* --- that it makes room, and never out of a page somebody holds ------------
 *
 * Two claims, and the second is the one worth the code:
 *
 *   asking for more pages than the table has keeps working -- otherwise the
 *     cache stops being a cache the moment a machine is busy;
 *   and a page with a reference is never taken -- otherwise a program is
 *     reading a file through memory that has been handed to somebody else,
 *     which does not present as a cache bug. It presents as corruption
 *     somewhere unrelated.
 *
 * The second is checked by holding one page across the whole flood and asking
 * for it again at the end. If eviction ignored references, the held key would
 * have been taken and re-read into a *different* page -- so the assertion is on
 * the address, which is the only thing that changes. The contents would match
 * either way.
 */
bool pagecache_eviction_self_test(void)
{
	struct file *f;
	paddr_t held, again;
	unsigned i, made = 0;
	u64 before = evictions;
	i64 err = 0;
	bool ok = true;

	f = file_open_path("/tmp/evict-test", OPEN_WRITE | OPEN_CREATE,
			   0600, &err);

	if (!f) {
		kprintf("  pagecache: no file to flood with (%ld)\n",
			(long)err);
		return false;
	}

	f->ops->write(f, "held", 4);
	file_release(f);

	f = file_open_path("/tmp/evict-test", OPEN_READ, 0, &err);

	if (!f) {
		kputs("  pagecache: could not reopen the file\n");
		return false;
	}

	held = pagecache_get(f, 0);

	if (!held) {
		kputs("  pagecache: nothing to hold\n");
		file_release(f);
		return false;
	}

	/* More distinct keys than the table has entries, and spread over
	 * several files because one is not enough: a file here holds 64KB, so
	 * sixteen pages, and seeking past that is refused. The first version of
	 * this flooded one file and reported "only 16 of 72 asks found room" --
	 * which was the test running out of file, not the cache running out of
	 * room.
	 *
	 * Offsets past the end of a file are still keys: the read comes back
	 * short, the rest of the page is zero, and the entry is as real as any
	 * other. */
	for (i = 0; i < 8 && ok; i++) {
		char name[32];
		struct file *g;
		unsigned k;

		kstrlcpy(name, "/tmp/evict-0", sizeof(name));
		name[11] = (char)('0' + i);

		g = file_open_path(name, OPEN_WRITE | OPEN_CREATE, 0600, &err);

		if (g) {
			g->ops->write(g, "x", 1);
			file_release(g);
		}

		g = file_open_path(name, OPEN_READ, 0, &err);

		if (!g)
			continue;

		for (k = 0; k < 10; k++) {
			paddr_t p = pagecache_get(g, (u64)k * PAGE_SIZE);

			if (p) {
				made++;
				pagecache_put(p);
			}
		}

		file_release(g);
	}

	if (made < PAGECACHE_MAX) {
		kprintf("  pagecache: only %u of 80 asks found room, so the "
			"cache stopped making any\n", made);
		ok = false;
	}

	if (evictions == before) {
		kputs("  pagecache: the table was flooded and nothing was "
		      "ever taken back\n");
		ok = false;
	}

	/* The held one is still the held one. */
	again = pagecache_get(f, 0);

	if (again != held) {
		kprintf("  pagecache: a page that was still held was taken "
			"anyway -- it was %p and came back %p\n",
			(void *)(uintptr_t)held, (void *)(uintptr_t)again);
		ok = false;
	}

	if (again)
		pagecache_put(again);

	pagecache_put(held);
	file_release(f);

	return ok;
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

	if (evictions)
		kprintf("  evicted      : %lu page(s) taken back to make "
			"room\n", (unsigned long)evictions);

	if (full || all_held)
		kprintf("  full         : %lu fault(s) filled a private page "
			"because every entry was still held\n",
			(unsigned long)full);

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
