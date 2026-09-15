/*
 * The allocator.
 *
 * An allocator is the worst possible thing to test from memory of what it
 * should do. Every one of its faults is silent at the moment it happens: a
 * block handed out twice, a free that does not coalesce, a realloc that copies
 * the wrong number of bytes, a header that runs one byte into its neighbour.
 * None of them announces itself; all of them appear later as something else
 * misbehaving, somewhere with no allocator in sight.
 *
 * So this suite does four things, and the first is the one that matters:
 *
 * **It audits the whole heap after every operation.** `recon_malloc_audit`
 * walks every block of every region and checks that what they say about each
 * other agrees -- sizes that are multiples of the alignment, a free block's
 * footer matching its header, every block's PREV_USED matching whether the
 * block before it is in use, no two free blocks side by side, and the counters
 * matching a fresh count. A heap corrupted at operation three and noticed at
 * operation nine thousand is a heap nobody can debug.
 *
 * **It keeps a shadow of what every live block should contain** and checks it.
 * Alignment and audit would both pass on an allocator that hands the same
 * address out twice; only the contents catch that.
 *
 * **It holds the parts with an answer against the host's allocator**, which is
 * how everything else in this library is tested. `realloc(p, 0)`, `malloc(0)`
 * and `calloc` overflow are places where the standard says "implementation
 * defined" and the desktop will be compiled against one and run on the other.
 *
 * **And it runs every scenario twice**: once with a source that can give
 * memory back and once with one that cannot -- because the second is the
 * configuration ReconOS is actually in today, and an allocator that is only
 * correct when it can release is an allocator that is wrong on this kernel.
 *
 * Run with: ./build/recon_libc_malloc_tests
 */

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ReconOS's, renamed by prefix.h when libc/ was compiled. */
struct recon_memory_source {
	void *(*take)(size_t bytes);
	int (*give_back)(void *at, size_t bytes);
};

struct recon_malloc_stats {
	size_t taken_bytes;
	size_t live_bytes;
	size_t live_blocks;
	size_t regions;
	size_t takes;
	size_t gives;
	size_t refusals;
};

void *recon_malloc(size_t bytes);
void *recon_calloc(size_t count, size_t each);
void *recon_realloc(void *p, size_t bytes);
void recon_free(void *p);
char *recon_strdup(const char *text);

void recon_memory_from(const struct recon_memory_source *from);
void recon_malloc_stats(struct recon_malloc_stats *into);
unsigned recon_malloc_audit(void);
void recon_malloc_reset(void);

/* Under AddressSanitizer, `calloc` is ASan's rather than the host's, and its
 * answer to an overflowing one is to kill the process -- which is a reasonable
 * thing for a bug-finding allocator to do and not what the allocator the
 * desktop links does.
 *
 * Told to answer NULL instead, so that the comparison below is against
 * something with the same contract as the real one. Skipping the comparison
 * under a sanitizer was the other option and it was worse: `check.sh` runs
 * this suite sanitized, so "skip under a sanitizer" is "skip", and a check
 * that cannot fail is the shape this project has already paid for twice.
 */
#ifdef __SANITIZE_ADDRESS__
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
	return "allocator_may_return_null=1";
}
#endif

static unsigned long checks;
static unsigned long failures;
static const char *area = "";

static void ok(int condition, const char *what)
{
	checks++;

	if (!condition) {
		failures++;
		if (failures <= 20)
			printf("  FAIL  %s: %s\n", area, what);
	}
}

/* Every operation is followed by this. See the header. */
static void audit(const char *what)
{
	unsigned bad = recon_malloc_audit();

	checks++;

	if (bad) {
		failures++;
		if (failures <= 20)
			printf("  FAIL  %s: the heap disagrees with itself "
			       "in %u places after %s\n", area, bad, what);
	}
}

/* --- the sources ----------------------------------------------------------
 *
 * Three, because the allocator has to be right against all three and only one
 * of them exists on a real machine.
 */

static size_t mapped_now;	/* what the source believes it has handed out */
static size_t map_calls, unmap_calls;
static size_t refuse_after;	/* 0 = never refuse */

static void *source_take(size_t bytes)
{
	void *p;

	if (refuse_after && map_calls >= refuse_after)
		return NULL;

	p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (p == MAP_FAILED)
		return NULL;

	map_calls++;
	mapped_now += bytes;
	return p;
}

static int source_give(void *at, size_t bytes)
{
	if (munmap(at, bytes) != 0)
		return -1;

	unmap_calls++;
	mapped_now -= bytes;
	return 0;
}

/* The one ReconOS is in today: memory can be had and never handed back. */
static int source_cannot_give(void *at, size_t bytes)
{
	(void)at;
	(void)bytes;
	return -1;
}

static const struct recon_memory_source can_release = {
	source_take, source_give
};

static const struct recon_memory_source cannot_release = {
	source_take, source_cannot_give
};

static void start(const struct recon_memory_source *src, size_t refuse)
{
	recon_malloc_reset();
	mapped_now = 0;
	map_calls = unmap_calls = 0;
	refuse_after = refuse;
	recon_memory_from(src);
}

/* --- a shadow of what should be there ------------------------------------- */

#define SLOTS 4096

struct slot {
	unsigned char *at;
	size_t bytes;
	unsigned char seed;
};

static struct slot live[SLOTS];

static void fill(struct slot *s)
{
	size_t i;

	for (i = 0; i < s->bytes; i++)
		s->at[i] = (unsigned char)(s->seed + (unsigned char)i);
}

static int intact(const struct slot *s)
{
	size_t i;

	for (i = 0; i < s->bytes; i++) {
		if (s->at[i] != (unsigned char)(s->seed + (unsigned char)i))
			return 0;
	}

	return 1;
}

static int all_intact(void)
{
	unsigned i;

	for (i = 0; i < SLOTS; i++) {
		if (live[i].at && !intact(&live[i]))
			return 0;
	}

	return 1;
}

/* Do any two live blocks overlap?
 *
 * Quadratic, so it runs on the small scenarios rather than inside the torture
 * loop -- where `all_intact` catches the same fault by a different route, an
 * address handed out twice being an address whose contents change under the
 * first owner.
 */
static int no_overlaps(void)
{
	unsigned i, j;

	for (i = 0; i < SLOTS; i++) {
		if (!live[i].at)
			continue;

		for (j = i + 1; j < SLOTS; j++) {
			if (!live[j].at)
				continue;

			if (live[i].at < live[j].at + live[j].bytes &&
			    live[j].at < live[i].at + live[i].bytes)
				return 0;
		}
	}

	return 1;
}

/* --- a deterministic sequence, so a failure can be reproduced ------------- */

static unsigned long seed_state = 88172645463325252ull;

static unsigned long roll(void)
{
	seed_state ^= seed_state << 13;
	seed_state ^= seed_state >> 7;
	seed_state ^= seed_state << 17;
	return seed_state;
}

/* --- the scenarios -------------------------------------------------------- */

static void basics(void)
{
	struct recon_malloc_stats st;
	void *a, *b, *c;

	area = "basics";

	a = recon_malloc(1);
	audit("malloc(1)");
	ok(a != NULL, "one byte can be had");
	ok(((size_t)a % 16) == 0, "and it is aligned to sixteen");

	b = recon_malloc(1);
	ok(b != NULL && b != a, "a second byte is somewhere else");

	c = recon_malloc(0);
	audit("malloc(0)");
	ok(c != NULL, "a request for nothing still answers a pointer");
	ok(c != a && c != b, "and it is not somebody else's");

	recon_free(c);
	audit("free of the empty one");
	recon_free(b);
	audit("free b");
	recon_free(a);
	audit("free a");

	recon_malloc_stats(&st);
	ok(st.live_bytes == 0, "nothing is live after freeing everything");
	ok(st.live_blocks == 0, "and no blocks either");

	/* Freeing NULL is defined to do nothing, and is done constantly by
	 * code that frees in a cleanup path. */
	recon_free(NULL);
	audit("free(NULL)");
}

static void alignment_and_sizes(void)
{
	size_t n;
	void *p;

	area = "alignment";

	for (n = 0; n <= 4096; n++) {
		p = recon_malloc(n);
		ok(p != NULL, "an allocation under four kilobytes succeeds");

		if (!p)
			break;

		ok(((size_t)p % 16) == 0, "every allocation is aligned");

		/* Writing every byte is the only way to find out the block is
		 * as big as it claims; a header one byte short shows up here
		 * as the audit failing afterwards rather than as a crash. */
		memset(p, 0xA5, n);
		recon_free(p);
	}

	audit("a sweep of every size to 4096");
}

static void contents_survive(void)
{
	unsigned i;

	area = "contents";

	for (i = 0; i < 512; i++) {
		live[i].bytes = (roll() % 600) + 1;
		live[i].seed = (unsigned char)i;
		live[i].at = recon_malloc(live[i].bytes);

		ok(live[i].at != NULL, "five hundred blocks can be had");
		if (!live[i].at)
			return;

		fill(&live[i]);
	}

	audit("512 allocations");
	ok(no_overlaps(), "no two live blocks overlap");
	ok(all_intact(), "every block still holds what was written into it");

	/* Free every other one, which is the pattern that makes a free list
	 * do some work: the survivors keep the holes from coalescing. */
	for (i = 0; i < 512; i += 2) {
		recon_free(live[i].at);
		live[i].at = NULL;
	}

	audit("freeing every other block");
	ok(all_intact(), "the survivors are untouched by their neighbours "
			 "going");

	for (i = 1; i < 512; i += 2) {
		recon_free(live[i].at);
		live[i].at = NULL;
	}

	audit("freeing the rest");
}

static void coalescing_and_release(int can_give)
{
	struct recon_malloc_stats before, after;
	void *a, *b, *c;

	area = can_give ? "release" : "release (source cannot)";

	recon_malloc_stats(&before);

	a = recon_malloc(200000);
	b = recon_malloc(200000);
	c = recon_malloc(200000);
	audit("three large-ish blocks");
	ok(a && b && c, "three blocks of two hundred kilobytes");

	recon_free(b);
	audit("free the middle one");

	recon_free(a);
	audit("free the first");

	recon_free(c);
	audit("free the last");

	recon_malloc_stats(&after);

	ok(after.live_bytes == before.live_bytes,
	   "nothing is live afterwards");

	if (can_give) {
		/* The whole point of the exercise: three blocks that were
		 * carved out of one region coalesce back into it, and the
		 * region goes home. */
		ok(after.taken_bytes == before.taken_bytes,
		   "and the memory went back to where it came from");
		ok(mapped_now == 0,
		   "the source agrees it has nothing outstanding");
	} else {
		ok(after.taken_bytes >= before.taken_bytes,
		   "a source that cannot release keeps its regions");
		ok(after.gives == before.gives,
		   "and nothing was reported as given back");
	}
}

static void large_allocations(int can_give)
{
	struct recon_malloc_stats st;
	unsigned char *big;

	area = can_give ? "large" : "large (source cannot)";

	/* Larger than the threshold that gives an allocation a region of its
	 * own, and the size KERNEL-WANTS measured for a browser tab. */
	big = recon_malloc(6800u * 1024u);
	audit("a 6.8 MiB allocation");
	ok(big != NULL, "a browser tab's worth can be had in one piece");

	if (!big)
		return;

	memset(big, 0x5A, 6800u * 1024u);
	ok(big[0] == 0x5A && big[6800u * 1024u - 1] == 0x5A,
	   "and every byte of it is writable");

	recon_malloc_stats(&st);
	ok(st.live_bytes >= 6800u * 1024u,
	   "the allocator says it is holding that much");

	recon_free(big);
	audit("freeing it");

	recon_malloc_stats(&st);
	ok(st.live_bytes == 0, "and nothing afterwards");

	if (can_give)
		ok(st.taken_bytes == 0,
		   "a large allocation gives its whole region back");
}

static void realloc_behaviour(void)
{
	unsigned char *p;
	size_t i;

	area = "realloc";

	/* NULL is malloc, which a good deal of code relies on for its first
	 * round of a growing buffer. */
	p = recon_realloc(NULL, 100);
	audit("realloc(NULL, 100)");
	ok(p != NULL, "realloc of nothing is an allocation");

	for (i = 0; i < 100; i++)
		p[i] = (unsigned char)i;

	/* Growing, over and over, which is the shape every string builder and
	 * every read-until-EOF loop has. */
	for (i = 200; i <= 200000; i *= 2) {
		unsigned char *grown = recon_realloc(p, i);
		size_t k;

		ok(grown != NULL, "a growing buffer keeps growing");
		if (!grown)
			return;

		p = grown;
		audit("a realloc that grows");

		for (k = 0; k < 100; k++)
			ok(p[k] == (unsigned char)k,
			   "and the first hundred bytes are still there");
	}

	/* And shrinking, which has to keep the contents too. */
	p = recon_realloc(p, 64);
	audit("a realloc that shrinks");
	ok(p != NULL, "shrinking succeeds");

	for (i = 0; i < 64; i++)
		ok(p[i] == (unsigned char)i,
		   "and what fits is still what it was");

	/* The host frees and answers NULL. The standard calls it
	 * implementation-defined, which is exactly why it is worth pinning:
	 * the desktop will be compiled against one and run on the other. */
	ok(recon_realloc(p, 0) == NULL,
	   "realloc to nothing answers NULL, as the host's does");
	audit("realloc(p, 0)");
}

static void calloc_behaviour(void)
{
	unsigned char *p;
	size_t i;

	area = "calloc";

	p = recon_calloc(1000, 7);
	audit("calloc(1000, 7)");
	ok(p != NULL, "seven thousand bytes can be had");

	if (p) {
		int zeroed = 1;

		for (i = 0; i < 7000; i++) {
			if (p[i])
				zeroed = 0;
		}

		ok(zeroed, "and every byte of it is zero");
		recon_free(p);
	}

	/* The overflow. A multiplication that wraps here hands back a buffer
	 * far smaller than asked for, and the caller then writes the size it
	 * asked for. Both allocators must refuse. */
	{
		size_t huge = (size_t)-1 / 2 + 1;

		ok(recon_calloc(huge, 4) == NULL,
		   "a count times a size that overflows is refused");

		ok(calloc(huge, 4) == NULL,
		   "and the host refuses it too");
	}

	audit("after the overflow cases");
}

static void strdup_behaviour(void)
{
	char *a, *b;

	area = "strdup";

	a = recon_strdup("the quick brown fox");
	audit("strdup");
	ok(a != NULL, "a string can be copied");
	ok(a && strcmp(a, "the quick brown fox") == 0,
	   "and it is the same string");

	b = strdup("the quick brown fox");
	ok(b && a && strcmp(a, b) == 0, "the host's copy says the same");

	recon_free(a);
	free(b);

	a = recon_strdup("");
	ok(a != NULL && a[0] == '\0', "an empty string copies to an empty one");
	recon_free(a);
	audit("strdup of an empty string");
}

static void a_source_that_refuses(void)
{
	void *p;
	struct recon_malloc_stats st;
	unsigned i;

	area = "refusal";

	/* One region, then nothing. Everything that fits in the first is
	 * served; the first request that does not must answer NULL rather
	 * than anything else. */
	start(&can_release, 1);

	p = recon_malloc(64);
	ok(p != NULL, "the first region is served");
	audit("one allocation against a source about to refuse");

	/* Ask for more than a region holds, over and over, until the source
	 * is asked a second time and says no. */
	for (i = 0; i < 40; i++) {
		void *q = recon_malloc(200000);

		if (!q)
			break;
	}

	p = recon_malloc(200000);
	ok(p == NULL, "and when the source refuses, malloc answers NULL");

	recon_malloc_stats(&st);
	ok(st.refusals > 0, "the refusal is counted rather than swallowed");

	audit("after a refusal");

	/* And the heap is still usable for anything that fits in what it
	 * already has -- a refusal is not a broken allocator. */
	p = recon_malloc(16);
	ok(p != NULL, "a small allocation still works after a refusal");
	recon_free(p);
	audit("after recovering from a refusal");
}

/* --- the torture ----------------------------------------------------------
 *
 * A long pseudo-random sequence of allocate, free, realloc and calloc against
 * a shadow of what every live block should hold, auditing throughout.
 *
 * Deterministic on purpose: the generator is written out here, so a failure at
 * operation N is a failure anybody can reproduce by running it again. A test
 * that uses the clock as a seed finds a fault once and never again.
 */
static void torture(int can_give, unsigned long rounds)
{
	unsigned long i;
	unsigned long audits = 0;

	area = can_give ? "torture" : "torture (source cannot release)";

	for (i = 0; i < rounds; i++) {
		unsigned slot = (unsigned)(roll() % SLOTS);
		unsigned what = (unsigned)(roll() % 100);

		if (live[slot].at && what < 35) {
			if (!intact(&live[slot])) {
				ok(0, "a live block changed under its owner");
				return;
			}

			recon_free(live[slot].at);
			live[slot].at = NULL;
		} else if (live[slot].at && what < 55) {
			size_t want = (roll() % 2000) + 1;
			unsigned char *grown =
				recon_realloc(live[slot].at, want);

			if (!grown) {
				ok(0, "realloc refused with memory available");
				return;
			}

			/* The overlap of old and new must have survived. */
			{
				size_t keep = live[slot].bytes < want
					    ? live[slot].bytes : want;
				size_t k;

				for (k = 0; k < keep; k++) {
					if (grown[k] !=
					    (unsigned char)(live[slot].seed +
							    (unsigned char)k)) {
						ok(0, "realloc lost the bytes "
						      "it was meant to keep");
						return;
					}
				}
			}

			live[slot].at = grown;
			live[slot].bytes = want;
			fill(&live[slot]);
		} else if (!live[slot].at) {
			size_t want = (roll() % 3000) + 1;

			live[slot].bytes = want;
			live[slot].seed = (unsigned char)(roll() & 0xFF);

			if (what & 1)
				live[slot].at = recon_calloc(want, 1);
			else
				live[slot].at = recon_malloc(want);

			if (!live[slot].at) {
				ok(0, "an allocation failed with memory "
				      "available");
				return;
			}

			if (what & 1) {
				size_t k;

				for (k = 0; k < want; k++) {
					if (live[slot].at[k]) {
						ok(0, "calloc handed back "
						      "memory that was not "
						      "zero");
						return;
					}
				}
			}

			fill(&live[slot]);
		}

		/* Auditing every single operation makes a long run very slow
		 * and is still worth doing often. Every sixteenth, plus a full
		 * contents check every thousandth. */
		if ((i & 15) == 0) {
			audits++;
			if (recon_malloc_audit()) {
				ok(0, "the heap disagreed with itself");
				return;
			}
		}

		if ((i % 1000) == 0 && !all_intact()) {
			ok(0, "a live block was damaged");
			return;
		}
	}

	ok(1, "the sequence ran to the end with the heap consistent");
	ok(all_intact(), "and every live block still holds what it was given");
	audit("the end of the sequence");

	/* Give everything back and check the allocator agrees it is empty. */
	{
		unsigned s;
		struct recon_malloc_stats st;

		for (s = 0; s < SLOTS; s++) {
			if (live[s].at) {
				recon_free(live[s].at);
				live[s].at = NULL;
			}
		}

		audit("freeing everything the sequence left");
		recon_malloc_stats(&st);

		ok(st.live_bytes == 0, "nothing is live at the end");
		ok(st.live_blocks == 0, "and no blocks are outstanding");

		if (can_give) {
			ok(st.taken_bytes == 0,
			   "and every region went back to the source");
			ok(mapped_now == 0,
			   "which the source confirms");
		}

		printf("  %-34s %6lu audits, %lu regions taken, %lu given "
		       "back\n", area, audits,
		       (unsigned long)st.takes, (unsigned long)st.gives);
	}
}

/* --- what it costs --------------------------------------------------------
 *
 * Not a pass or a fail. A number printed every run, because the thing an
 * allocator is silently bad at is fragmentation, and a suite that only says
 * "correct" would never notice it doubling.
 */
static void report_fragmentation(void)
{
	struct recon_malloc_stats st;
	unsigned i;
	size_t asked = 0;

	area = "cost";
	start(&can_release, 0);

	/* A pattern with the shape a real program has: many small, some
	 * medium, a few large, and half of them freed in a different order
	 * from the one they were made in. */
	for (i = 0; i < 2000; i++) {
		size_t want = (roll() % 100 < 80) ? (roll() % 200) + 1
						  : (roll() % 20000) + 1;

		live[i].bytes = want;
		live[i].seed = (unsigned char)i;
		live[i].at = recon_malloc(want);
		asked += want;

		if (live[i].at)
			fill(&live[i]);
	}

	for (i = 0; i < 2000; i += 3) {
		if (live[i].at) {
			asked -= live[i].bytes;
			recon_free(live[i].at);
			live[i].at = NULL;
		}
	}

	recon_malloc_stats(&st);

	printf("  %-34s %lu KiB asked for, %lu KiB held, %lu KiB in blocks "
	       "(%lu%% overhead)\n",
	       "a mixed pattern",
	       (unsigned long)(asked / 1024),
	       (unsigned long)(st.taken_bytes / 1024),
	       (unsigned long)(st.live_bytes / 1024),
	       asked ? (unsigned long)((st.taken_bytes - asked) * 100 / asked)
		     : 0ul);

	for (i = 0; i < 2000; i++) {
		if (live[i].at) {
			recon_free(live[i].at);
			live[i].at = NULL;
		}
	}

	audit("after the mixed pattern");
}

int main(void)
{
	printf("The allocator\n\n");

	/* --- with a source that can give memory back --- */
	start(&can_release, 0);
	basics();
	alignment_and_sizes();
	contents_survive();
	coalescing_and_release(1);
	large_allocations(1);
	realloc_behaviour();
	calloc_behaviour();
	strdup_behaviour();

	start(&can_release, 0);
	torture(1, 200000);

	/* --- and with the one ReconOS actually has --- */
	start(&cannot_release, 0);
	basics();
	contents_survive();
	coalescing_and_release(0);
	large_allocations(0);
	realloc_behaviour();
	calloc_behaviour();

	start(&cannot_release, 0);
	torture(0, 200000);

	a_source_that_refuses();

	report_fragmentation();

	printf("\n%lu checks, %lu failures\n", checks, failures);
	return failures ? 1 : 0;
}
