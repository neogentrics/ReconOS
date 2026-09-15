/*
 * The allocator.
 *
 * This is the last large piece of the C library and the one everything else
 * was waiting behind: 5 symbols, **430 call sites** in the desktop, measured
 * by `scripts/measure-libc.py` rather than estimated. Until it existed
 * `userland/include/stdlib.h` deliberately did not declare `malloc` at all, so
 * that a caller failed to *link* rather than receiving a stub that returned
 * nothing and crashed an hour later somewhere else.
 *
 * --- Where the memory comes from ---
 *
 * Not from a system call named here. `struct recon_memory_source` is two
 * function pointers -- take a range, give one back -- and everything below is
 * written against those.
 *
 * That split is the same one `userland/init/layout.c` uses for making
 * directories, and it is here for the same reason: the part that can be wrong
 * is the allocator, not the system call. With the source behind a pointer the
 * whole of this file can be exercised on the host against the allocator it
 * replaces, and with a source that refuses, a source that cannot give anything
 * back, and a source that hands out ranges in a deliberately awkward order --
 * none of which can be arranged by booting a kernel.
 *
 * On ReconOS the source is `mem_recon.c`. See it for what the kernel still
 * owes this file.
 *
 * --- The shape ---
 *
 * Boundary tags with coalescing, and free lists segregated by size. Classic,
 * and chosen because every part of it can be checked: every block knows its
 * own size, a free block knows its size at *both* ends so its neighbour can
 * find where it starts, and every block knows whether the block before it is
 * in use. `recon_malloc_audit()` walks the whole arrangement and says whether
 * those agree, which is what the tests assert after every operation rather
 * than at the end of a run.
 *
 *   [ region header ][ block ][ block ][ ... ][ sentinel ]
 *
 * A block is a 16-byte header and a payload. 16 is also the alignment, so
 * payloads are aligned by construction rather than by arithmetic that could be
 * wrong. A *free* block additionally holds two list pointers in the first 16
 * bytes of its payload and a copy of its size in the last 8 -- which is why
 * the smallest block is 32 bytes, and why asking for one byte costs 32.
 *
 * --- Giving it back ---
 *
 * A region whose blocks are all free is handed back to the source. This is not
 * a refinement to add later: `docs/KERNEL-WANTS.md` measured a browser tab at
 * 6.8 MiB, so a window where twelve tabs have been opened and closed has lost
 * eighty-one megabytes if nothing is released -- on a 512 MiB machine, a
 * browser that dies after sixty tabs and cannot say why.
 *
 * **A source that cannot give anything back is a supported configuration**,
 * not a degraded one, and it is the one ReconOS is in today: the kernel has no
 * call that releases a range. Those regions are kept and reused, which is
 * correct, and the suite runs every scenario both ways.
 *
 * --- What this does not do ---
 *
 * **It is not thread-safe.** There is no lock here because there is nothing to
 * lock against: ReconOS has no threads in user mode yet. Said out loud rather
 * than left to be discovered, because the day there are threads this file
 * needs a lock and nothing else will say so.
 */

#include "internal.h"

/* --- the shape of a block -------------------------------------------------
 *
 * A 16-byte header and a payload. 16 is also the alignment, so payloads are
 * aligned by construction.
 *
 * `head` holds the whole block's size -- header included -- with the two flags
 * in its low bits. Sizes are multiples of sixteen, so four bits are going
 * spare and using two of them costs a mask on every read.
 *
 * **That mask is the price of the smallest block being 32 bytes, and it is
 * worth paying.** The first version of this file gave the flags a field of
 * their own and wrote the footer -- a second copy of the size, so the block
 * *after* a free one can find where it starts -- into the last eight bytes of
 * the block. In a 32-byte block the payload is sixteen bytes, which is exactly
 * the two list pointers a free block keeps there, and the last eight of them
 * are the second pointer. Every smallest-size block that went on a free list
 * had its `prev` link overwritten by its own size.
 *
 * So the footer lives in the **next block's** `prev_size` field instead, which
 * is outside this block entirely. It is meaningful only while this block is
 * free, which is what the PREV_USED flag in the next block's head says.
 */

#define RECON_ALIGN	16u

#define THIS_USED	((size_t)1)
#define PREV_USED	((size_t)2)
#define SIZE_MASK	(~(size_t)(RECON_ALIGN - 1))

struct block {
	size_t prev_size;	/* the block before, while that one is free */
	size_t head;		/* this block's size, with the flags */
};

/* What a free block keeps in the first bytes of its payload. */
struct links {
	struct block *next;
	struct block *prev;
};

#define HEADER		(sizeof(struct block))		/* 16 */
#define MIN_BLOCK	(HEADER + sizeof(struct links))	/* 32 */

static size_t size_of(const struct block *b)
{
	return b->head & SIZE_MASK;
}

static void set_size(struct block *b, size_t bytes)
{
	b->head = bytes | (b->head & ~SIZE_MASK);
}

/* --- regions --------------------------------------------------------------
 *
 * One range obtained from the source. Kept in a list so that `audit` can walk
 * every block the allocator owns, which is the only way to check the
 * invariants rather than trust them.
 */
struct region {
	struct region *next;
	struct region *prev;
	size_t bytes;		/* exactly what the source was asked for */
	size_t whole;		/* nonzero: one allocation owns all of it */
};

/* Padded so the first block after it is aligned. Checked at run time by
 * `audit` rather than assumed, because getting this wrong produces payloads
 * misaligned by eight and a fault on the first double somebody stores. */
#define REGION_HEADER	(((sizeof(struct region) + RECON_ALIGN - 1) \
			  / RECON_ALIGN) * RECON_ALIGN)

/* How much to ask for when the free lists cannot serve a request.
 *
 * A megabyte. Small enough that a program allocating a few kilobytes does not
 * reserve a five-hundredth of a 512 MiB machine, large enough that the
 * desktop's smaller allocations -- the overwhelming majority of those 430
 * sites -- are served without a system call each.
 */
#define REGION_BYTES	((size_t)1024 * 1024)

/* Above this, an allocation gets a region to itself and gives the whole thing
 * back when it is freed.
 *
 * A quarter of a region. The numbers that make this worth having are in
 * KERNEL-WANTS: the largest single request the desktop makes is just under
 * four megabytes -- the link table, two thousand addresses of two kilobytes
 * each -- and a browser tab is 6.8 MiB. Those have no business being carved
 * out of a shared region and leaving a hole that size behind when they go.
 */
#define LARGE_BLOCK	(REGION_BYTES / 4)

/* --- free lists -----------------------------------------------------------
 *
 * Bin k holds free blocks of [32 << k, 32 << (k+1)) bytes. So any block found
 * in a bin *above* the one a request lands in is guaranteed to fit, and only
 * the request's own bin has to be searched rather than taken from.
 */
#define NBINS		32

static struct block *bins[NBINS];
static struct region *regions;

static struct recon_memory_source source;
static int have_source;

/* Counted rather than derived, because the tests assert on them after every
 * operation -- and a number recomputed by walking the heap would be the heap's
 * own opinion of itself. `audit` compares the two, which is the point. */
static size_t taken_bytes;	/* what the source has handed over, live */
static size_t live_bytes;	/* payload bytes currently handed out */
static size_t live_blocks;
static size_t region_count;
static size_t takes, gives, refusals;

/* --- small helpers -------------------------------------------------------- */

static size_t align_up(size_t n)
{
	return (n + (RECON_ALIGN - 1)) & ~((size_t)RECON_ALIGN - 1);
}

static struct block *next_block(struct block *b)
{
	return (struct block *)((unsigned char *)b + size_of(b));
}

static void *payload_of(struct block *b)
{
	return (unsigned char *)b + HEADER;
}

static struct block *block_of(void *p)
{
	return (struct block *)((unsigned char *)p - HEADER);
}

static struct links *links_of(struct block *b)
{
	return (struct links *)payload_of(b);
}

/* A free block's size, written again into the header of the block after it, so
 * that block can find where this one starts.
 *
 * In the *next* block rather than at the end of this one, which is the whole
 * of what the first version of this file got wrong -- see the note on the
 * layout above. Meaningful only while this block is free, which is exactly
 * what the next block's PREV_USED flag records. */
static void write_footer(struct block *b)
{
	next_block(b)->prev_size = size_of(b);
}

static struct block *prev_block(struct block *b)
{
	return (struct block *)((unsigned char *)b - b->prev_size);
}

static unsigned bin_of(size_t whole)
{
	unsigned i = 0;
	size_t v = whole / MIN_BLOCK;

	while (v > 1 && i + 1 < NBINS) {
		v >>= 1;
		i++;
	}

	return i;
}

static void bin_insert(struct block *b)
{
	unsigned k = bin_of(size_of(b));
	struct links *l = links_of(b);

	l->prev = 0;
	l->next = bins[k];

	if (bins[k])
		links_of(bins[k])->prev = b;

	bins[k] = b;
}

static void bin_remove(struct block *b)
{
	unsigned k = bin_of(size_of(b));
	struct links *l = links_of(b);

	if (l->prev)
		links_of(l->prev)->next = l->next;
	else
		bins[k] = l->next;

	if (l->next)
		links_of(l->next)->prev = l->prev;
}

/* --- where memory comes from ---------------------------------------------- */

/* Where a source comes from when nobody installed one.
 *
 * Weak, and it does nothing. `mem_recon.c` defines the same name strongly and
 * installs the ReconOS source, so a program that links it gets memory and a
 * program that does not gets an allocator that answers NULL -- rather than a
 * link error in a program that never allocates.
 *
 * Asked from `take_region`, which runs once per region rather than once per
 * allocation, so this costs the common path nothing.
 */
__attribute__((weak)) void recon_memory_default(void)
{
}

void recon_memory_from(const struct recon_memory_source *from)
{
	if (!from || !from->take) {
		have_source = 0;
		return;
	}

	source = *from;
	have_source = 1;
}

/* Takes a region from the source and lays it out.
 *
 * `want` is the number of *block* bytes needed; the region is at least that
 * plus its own header and its sentinel.
 */
static struct region *take_region(size_t want, int whole)
{
	size_t need = REGION_HEADER + want + HEADER;
	size_t ask = whole ? align_up(need) : REGION_BYTES;
	struct region *r;
	struct block *b, *end;

	if (!have_source) {
		recon_memory_default();

		if (!have_source)
			return 0;
	}

	if (ask < need)
		ask = align_up(need);

	r = source.take(ask);
	if (!r) {
		refusals++;
		return 0;
	}

	takes++;
	taken_bytes += ask;
	region_count++;

	r->bytes = ask;
	r->whole = (size_t)whole;
	r->prev = 0;
	r->next = regions;
	if (regions)
		regions->prev = r;
	regions = r;

	b = (struct block *)((unsigned char *)r + REGION_HEADER);
	b->prev_size = 0;
	/* PREV_USED because there is nothing before it inside the region. */
	b->head = (ask - REGION_HEADER - HEADER) | PREV_USED;

	/* The sentinel: a header with no payload, permanently in use, so that
	 * coalescing forward stops at the end of a region without any code
	 * having to compare addresses against the region's bounds. Its
	 * `prev_size` is where the last real block writes its footer. */
	end = next_block(b);
	end->prev_size = 0;
	end->head = THIS_USED;

	write_footer(b);

	if (!whole)
		bin_insert(b);

	return r;
}

static void drop_region(struct region *r)
{
	if (r->prev)
		r->prev->next = r->next;
	else
		regions = r->next;

	if (r->next)
		r->next->prev = r->prev;

	taken_bytes -= r->bytes;
	region_count--;

	if (source.give_back && source.give_back(r, r->bytes) == 0)
		gives++;
}

/* The region a block belongs to, found by walking the list.
 *
 * Linear, and a deliberate trade rather than an oversight: a back pointer in
 * every block would cost eight bytes on every allocation to make one branch of
 * `free` faster, and the list is short -- one entry per megabyte in use, plus
 * one per large allocation. If it ever shows up in a measurement it becomes a
 * sorted array. It is not one on a guess.
 */
static struct region *region_of(struct block *b)
{
	struct region *r;

	for (r = regions; r; r = r->next) {
		unsigned char *lo = (unsigned char *)r;

		if ((unsigned char *)b >= lo &&
		    (unsigned char *)b < lo + r->bytes)
			return r;
	}

	return 0;
}

/* --- carving -------------------------------------------------------------- */

/* Splits `b` down to exactly `want`, returning the rest as a free block.
 *
 * Only when the remainder can hold a block of its own. A remainder smaller
 * than MIN_BLOCK has nowhere to keep its own links and could never be found
 * again, so it stays part of the allocation -- which is where the difference
 * between bytes asked for and bytes charged comes from, and why `live_bytes`
 * counts what a block holds rather than what was requested.
 *
 * The tail is *not* coalesced here, because every caller splits a block whose
 * successor is in use: a free block's successor is in use by the invariant,
 * and `release_block` is what handles the one case where it is not.
 */
static void split(struct block *b, size_t want)
{
	size_t rest = size_of(b) - want;
	struct block *tail;

	if (rest < MIN_BLOCK)
		return;

	set_size(b, want);

	tail = next_block(b);
	tail->head = rest | PREV_USED;
	write_footer(tail);

	bin_insert(tail);
}

static struct block *find_fit(size_t want)
{
	unsigned k = bin_of(want);
	struct block *b;

	/* Only this bin can hold a block that does not fit, so it is the only
	 * one that has to be searched rather than taken from. */
	for (b = bins[k]; b; b = links_of(b)->next) {
		if (size_of(b) >= want)
			return b;
	}

	for (k = k + 1; k < NBINS; k++) {
		if (bins[k])
			return bins[k];
	}

	return 0;
}

/* Marks a block free and puts it back, coalescing on both sides.
 *
 * One function rather than two, because `free` and a `realloc` that shrinks
 * are the same act: a block that is no longer wanted, next to blocks that may
 * or may not be free. Writing it twice is how one of the two comes to leave
 * two free blocks side by side -- which nothing notices until the heap is
 * growing while the program's live set is not.
 *
 * Returns nonzero if the region was handed back, in which case `b` no longer
 * points at anything.
 */
static int release_block(struct block *b, struct region *r)
{
	struct block *after;

	b->head &= ~THIS_USED;

	/* Forward first, because coalescing backwards moves the block and the
	 * one after it would then have to be found all over again. */
	after = next_block(b);
	if (size_of(after) && !(after->head & THIS_USED)) {
		bin_remove(after);
		set_size(b, size_of(b) + size_of(after));
	}

	if (!(b->head & PREV_USED)) {
		struct block *before = prev_block(b);

		bin_remove(before);
		set_size(before, size_of(before) + size_of(b));
		b = before;
	}

	write_footer(b);
	next_block(b)->head &= ~PREV_USED;

	/* A region holding one free block that reaches the sentinel is a
	 * region nobody is using. `b` is in no bin at this point -- it was
	 * never inserted, and both neighbours were removed -- so there is
	 * nothing to unlink before letting it go. */
	if (size_of(b) == r->bytes - REGION_HEADER - HEADER) {
		drop_region(r);
		return 1;
	}

	bin_insert(b);
	return 0;
}

/* --- the four functions --------------------------------------------------- */

void *malloc(size_t bytes)
{
	size_t want;
	struct block *b;
	struct region *r;

	/* A request of zero returns a pointer that can be freed and equals no
	 * other live pointer -- which is what the library this is held against
	 * does, and what callers treating NULL as failure depend on. */
	want = align_up(bytes + HEADER);

	if (want < bytes)		/* the addition wrapped */
		return 0;

	if (want < MIN_BLOCK)
		want = MIN_BLOCK;

	if (want >= LARGE_BLOCK) {
		r = take_region(want, 1);
		if (!r)
			return 0;

		b = (struct block *)((unsigned char *)r + REGION_HEADER);
		b->head |= THIS_USED;
		next_block(b)->head |= PREV_USED;

		live_bytes += size_of(b) - HEADER;
		live_blocks++;
		return payload_of(b);
	}

	b = find_fit(want);

	if (!b) {
		if (!take_region(want, 0))
			return 0;

		b = find_fit(want);
		if (!b)
			return 0;
	}

	bin_remove(b);
	split(b, want);

	b->head |= THIS_USED;
	next_block(b)->head |= PREV_USED;

	live_bytes += size_of(b) - HEADER;
	live_blocks++;

	return payload_of(b);
}

void free(void *p)
{
	struct block *b;
	struct region *r;

	if (!p)
		return;

	b = block_of(p);
	r = region_of(b);

	/* Not ours. Refused rather than acted on: a free of a pointer this
	 * allocator never handed out would otherwise write list pointers into
	 * somebody else's memory, and the crash would be somewhere else
	 * entirely. */
	if (!r)
		return;

	live_bytes -= size_of(b) - HEADER;
	live_blocks--;

	if (r->whole) {
		drop_region(r);
		return;
	}

	release_block(b, r);
}

void *calloc(size_t count, size_t each)
{
	size_t total = count * each;
	void *p;

	/* Refused rather than wrapped. A multiplication that overflows here
	 * hands back a buffer far smaller than the caller asked for, and the
	 * caller then writes the size it asked for -- which is the oldest
	 * heap-overflow shape there is. */
	if (each && total / each != count)
		return 0;

	p = malloc(total);
	if (p)
		memset(p, 0, total);

	return p;
}

void *realloc(void *p, size_t bytes)
{
	struct block *b, *after;
	struct region *r;
	size_t want, had;
	void *fresh;

	if (!p)
		return malloc(bytes);

	/* Frees and answers NULL, which is what the library this is held
	 * against does. The standard calls it implementation-defined; a
	 * desktop compiled against one and run on the other would leak or
	 * double-free, so "whatever glibc does" is the only safe reading. */
	if (!bytes) {
		free(p);
		return 0;
	}

	b = block_of(p);
	r = region_of(b);

	if (!r)
		return 0;

	had = size_of(b) - HEADER;

	want = align_up(bytes + HEADER);
	if (want < bytes)
		return 0;
	if (want < MIN_BLOCK)
		want = MIN_BLOCK;

	/* --- it already fits --------------------------------------------- */
	if (size_of(b) >= want && !r->whole) {
		size_t before = size_of(b);

		if (before - want >= MIN_BLOCK) {
			struct block *tail;

			set_size(b, want);

			tail = next_block(b);
			tail->head = (before - want) | PREV_USED | THIS_USED;

			live_bytes -= before - want;

			/* Through the same path a free takes, because the
			 * block after this tail may itself be free -- and two
			 * free blocks side by side is the one arrangement this
			 * allocator must never produce. */
			release_block(tail, r);
		}

		return p;
	}

	/* A shrink inside a region of its own keeps the whole region, which
	 * for a six-megabyte buffer cut to a kilobyte is six megabytes held
	 * for nothing. Small enough to live in a shared region, so move it. */
	if (size_of(b) >= want && r->whole && want >= LARGE_BLOCK)
		return p;

	/* --- grow into the block after it -------------------------------- */
	after = next_block(b);
	if (!r->whole && size_of(after) && !(after->head & THIS_USED) &&
	    size_of(b) + size_of(after) >= want) {
		bin_remove(after);
		set_size(b, size_of(b) + size_of(after));
		next_block(b)->head |= PREV_USED;

		live_bytes += size_of(b) - HEADER - had;
		had = size_of(b) - HEADER;

		if (size_of(b) - want >= MIN_BLOCK) {
			size_t before = size_of(b);
			struct block *tail;

			set_size(b, want);
			tail = next_block(b);
			tail->head = (before - want) | PREV_USED | THIS_USED;

			live_bytes -= before - want;
			release_block(tail, r);
		}

		return p;
	}

	/* --- or somewhere else entirely ---------------------------------- */
	fresh = malloc(bytes);
	if (!fresh)
		return 0;		/* and `p` is still valid, as promised */

	memcpy(fresh, p, had < bytes ? had : bytes);
	free(p);

	return fresh;
}

char *strdup(const char *text)
{
	size_t n;
	char *out;

	if (!text)
		return 0;

	n = strlen(text) + 1;
	out = malloc(n);

	if (out)
		memcpy(out, text, n);

	return out;
}

/* --- what the tests ask it ------------------------------------------------ */

void recon_malloc_stats(struct recon_malloc_stats *into)
{
	if (!into)
		return;

	into->taken_bytes = taken_bytes;
	into->live_bytes = live_bytes;
	into->live_blocks = live_blocks;
	into->regions = region_count;
	into->takes = takes;
	into->gives = gives;
	into->refusals = refusals;
}

/* Walks every block of every region and checks that what they say about each
 * other agrees.
 *
 * Called by the tests after *every* operation rather than at the end of a run,
 * because a heap corrupted at operation three and noticed at operation nine
 * thousand is a heap nobody can debug. Returns how many disagreements it
 * found, so a caller can report the number rather than that there were some.
 */
unsigned recon_malloc_audit(void)
{
	struct region *r;
	unsigned bad = 0;
	size_t counted_live = 0, counted_blocks = 0;

	for (r = regions; r; r = r->next) {
		unsigned char *base = (unsigned char *)r;
		struct block *b = (struct block *)(base + REGION_HEADER);
		int prev_used = 1;
		size_t span = REGION_HEADER;

		if ((size_t)(base + REGION_HEADER) % RECON_ALIGN)
			bad++;		/* payloads would be misaligned */

		while (size_of(b)) {
			if (size_of(b) % RECON_ALIGN)
				bad++;
			if (size_of(b) < MIN_BLOCK)
				bad++;

			if (!!(b->head & PREV_USED) != prev_used)
				bad++;

			if (b->head & THIS_USED) {
				counted_live += size_of(b) - HEADER;
				counted_blocks++;
			} else {
				/* The footer, which lives in the next block's
				 * header. If it disagrees, coalescing
				 * backwards would land in the middle of a
				 * block -- which is what the first version of
				 * this allocator did. */
				if (next_block(b)->prev_size != size_of(b))
					bad++;

				/* Two free blocks side by side is a free that
				 * did not coalesce -- the fault that shows up
				 * later as a heap growing while the program's
				 * live set does not. */
				if (!prev_used)
					bad++;
			}

			prev_used = !!(b->head & THIS_USED);
			span += size_of(b);
			b = next_block(b);

			if (span > r->bytes) {
				bad++;
				break;
			}
		}

		if (span + HEADER != r->bytes)
			bad++;
	}

	if (counted_live != live_bytes)
		bad++;
	if (counted_blocks != live_blocks)
		bad++;

	return bad;
}

/* Everything back to the source, and the allocator back to how it started.
 *
 * For the tests, which run many independent scenarios in one process and would
 * otherwise measure each one on top of the last. Not offered to programs:
 * freeing everything a program is still using is not a service.
 */
void recon_malloc_reset(void)
{
	unsigned i;

	while (regions) {
		struct region *r = regions;

		regions = r->next;
		if (source.give_back)
			source.give_back(r, r->bytes);
	}

	for (i = 0; i < NBINS; i++)
		bins[i] = 0;

	taken_bytes = live_bytes = 0;
	live_blocks = region_count = 0;
	takes = gives = refusals = 0;
}

/* --- What a program can ask about its own heap ---------------------------
 *
 * `recon_malloc_stats` fills a structure and is declared in `internal.h`,
 * which a program must not include -- that header exists so the library's
 * files can call each other without dragging in the public headers, and the
 * whole host comparison depends on the two staying apart.
 *
 * So the three numbers a program actually reports are readable one at a time
 * through `<stdlib.h>`. Thin on purpose: no structure to agree about, nothing
 * to keep in step, and a program built against an older library still links.
 */

size_t recon_malloc_held(void)
{
	return taken_bytes;
}

size_t recon_malloc_live(void)
{
	return live_bytes;
}

size_t recon_malloc_refused(void)
{
	return refusals;
}
