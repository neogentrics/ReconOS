#include <recon/kernel/heap.h>
#include <recon/kernel/compiler.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/panic.h>

/* Size classes. Powers of two from the alignment up to an eighth of a page:
 * beyond that a slab holds too few objects for its header to be worth carrying,
 * and the page allocator is the better answer. */
static const size_t class_size[] = { 16, 32, 64, 128, 256, 512 };
#define CLASS_COUNT RK_ARRAY_LEN(class_size)
#define MAX_SLAB_OBJECT 512

/* Spelled out rather than written as a hex constant, for two reasons. The
 * portability check flags long hex literals in core/ because that is what a
 * hardware address looks like, and it was right to ask -- weakening the check
 * to admit a magic number would also admit the next real address. And this way
 * the value reads as what it is without anybody decoding it. */
#define SLAB_MAGIC (((u32)'R' << 24) | ((u32)'S' << 16) | \
		    ((u32)'L' << 8)  |  (u32)'A')

struct slab {
	u32 magic;
	u32 obj_size;
	u32 total;		/* objects this slab holds */
	u32 in_use;
	void *free_list;	/* threaded through the free objects */
	struct slab *next;	/* the next slab of the same class */
};

/* A record of one large allocation. Allocated from this heap itself, which is
 * fine and slightly pleasing: the smallest size class is exactly big enough,
 * and it means large allocations cost no fixed table. */
struct large {
	void *addr;
	size_t pages;
	struct large *next;
};

static struct slab  *slabs[CLASS_COUNT];
static struct large *larges;

static size_t bytes_live, bytes_slab, slab_count, large_count;

/* The header sits at the front of the page, rounded up so the first object is
 * aligned. Everything after it is objects. */
#define SLAB_HEADER_SIZE (((sizeof(struct slab) + HEAP_ALIGN - 1) / HEAP_ALIGN) * HEAP_ALIGN)

void heap_init(void)
{
	for (unsigned i = 0; i < CLASS_COUNT; i++)
		slabs[i] = 0;
	larges = 0;
	bytes_live = bytes_slab = slab_count = large_count = 0;
}

static int class_for(size_t size)
{
	for (unsigned i = 0; i < CLASS_COUNT; i++)
		if (size <= class_size[i])
			return (int)i;
	return -1;
}

static struct slab *grow_class(unsigned cls)
{
	paddr_t page = pmm_alloc_page();
	struct slab *s;
	u8 *body;
	u32 obj = (u32)class_size[cls];
	u32 count;

	if (!page)
		return 0;

	s = phys_to_virt(page);
	body = (u8 *)s + SLAB_HEADER_SIZE;
	count = (u32)((PAGE_SIZE - SLAB_HEADER_SIZE) / obj);

	s->magic = SLAB_MAGIC;
	s->obj_size = obj;
	s->total = count;
	s->in_use = 0;
	s->free_list = 0;
	s->next = slabs[cls];
	slabs[cls] = s;

	/* Thread the free list through the objects, backwards, so that the
	 * first object handed out is the first in the page. Allocation order
	 * matching address order is not required by anything, but it makes a
	 * memory dump readable, and readable dumps are how the last three bugs
	 * were found. */
	for (u32 i = count; i-- > 0;) {
		void *obj_ptr = body + (size_t)i * obj;

		*(void **)obj_ptr = s->free_list;
		s->free_list = obj_ptr;
	}

	slab_count++;
	bytes_slab += PAGE_SIZE;
	return s;
}

static void *alloc_large(size_t size)
{
	size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	paddr_t phys = pmm_alloc_pages(pages);
	struct large *rec;

	if (!phys)
		return 0;

	/* The record comes from the heap itself, and has to be taken *after*
	 * the pages, so a failure to record does not leak them silently. */
	rec = kmalloc(sizeof(*rec));
	if (!rec) {
		pmm_free_pages(phys, pages);
		return 0;
	}

	rec->addr = phys_to_virt(phys);
	rec->pages = pages;
	rec->next = larges;
	larges = rec;

	large_count++;
	bytes_live += pages * PAGE_SIZE;
	return rec->addr;
}

void *kmalloc(size_t size)
{
	int cls;
	struct slab *s;
	void *obj;

	if (size == 0)
		return 0;

	cls = class_for(size);
	if (cls < 0)
		return alloc_large(size);

	/* First slab of this class with something free. The list is short in
	 * practice because a freed-empty slab is returned to the page allocator,
	 * so it does not accumulate. */
	for (s = slabs[cls]; s; s = s->next)
		if (s->free_list)
			break;

	if (!s) {
		s = grow_class((unsigned)cls);
		if (!s)
			return 0;
	}

	obj = s->free_list;
	s->free_list = *(void **)obj;
	s->in_use++;
	bytes_live += s->obj_size;

	return obj;
}

void *kzalloc(size_t size)
{
	void *p = kmalloc(size);

	if (p)
		kmemset(p, 0, size);
	return p;
}

/* Returns a slab to the page allocator once nothing in it is used. Without
 * this, a burst of allocations that is then freed leaves every slab it touched
 * held forever -- which is exactly the shape of "the machine gets slower the
 * longer it runs" that this project is meant not to have. */
static void release_slab(unsigned cls, struct slab *s)
{
	struct slab **link = &slabs[cls];

	while (*link && *link != s)
		link = &(*link)->next;

	if (!*link)
		panic("heap: a slab was not in its own class list");

	*link = s->next;

	slab_count--;
	bytes_slab -= PAGE_SIZE;
	pmm_free_page(virt_to_phys(s));
}

static void free_large(void *ptr)
{
	struct large **link = &larges;

	while (*link && (*link)->addr != ptr)
		link = &(*link)->next;

	if (!*link)
		panic("heap: asked to free a page-aligned pointer this heap never gave out");

	{
		struct large *rec = *link;

		*link = rec->next;
		bytes_live -= rec->pages * PAGE_SIZE;
		large_count--;
		pmm_free_pages(virt_to_phys(rec->addr), rec->pages);
		kfree(rec);
	}
}

void kfree(void *ptr)
{
	struct slab *s;

	if (!ptr)
		return;

	/* Page-aligned means it came from the page allocator, because a slab
	 * object can never be page-aligned -- the slab header occupies the start
	 * of the page. This is the whole discriminator, and it needs no magic
	 * number read from memory that might not be a header. */
	if (((uintptr_t)ptr & (PAGE_SIZE - 1)) == 0) {
		free_large(ptr);
		return;
	}

	s = (struct slab *)(uintptr_t)((uintptr_t)ptr & ~(uintptr_t)(PAGE_SIZE - 1));

	if (s->magic != SLAB_MAGIC)
		panic("heap: asked to free a pointer that is not in a slab");

	if (s->in_use == 0)
		panic("heap: a slab was asked to free more objects than it holds");

	*(void **)ptr = s->free_list;
	s->free_list = ptr;
	s->in_use--;
	bytes_live -= s->obj_size;

	if (s->in_use == 0) {
		int cls = class_for(s->obj_size);

		if (cls >= 0)
			release_slab((unsigned)cls, s);
	}
}

void heap_print_summary(void)
{
	kprintf("\nKernel heap\n");
	kprintf("  size classes : ");
	for (unsigned i = 0; i < CLASS_COUNT; i++)
		kprintf("%lu%s", (u64)class_size[i],
			i + 1 == CLASS_COUNT ? " bytes\n" : ", ");
	kprintf("  slabs        : %lu (%lu KB), %lu large allocations\n",
		(u64)slab_count, (u64)(bytes_slab / 1024), (u64)large_count);
	kprintf("  live         : %lu bytes\n", (u64)bytes_live);
}

bool heap_self_test(void)
{
	size_t live_before = bytes_live;
	size_t slabs_before = slab_count;
	void *p[64];
	bool ok = true;

	/* Every size class, plus one larger than any of them, and each written
	 * with a pattern derived from its index so that an overlap between two
	 * allocations shows up as the wrong pattern rather than as nothing. */
	for (unsigned i = 0; i < 64; i++) {
		size_t size = class_size[i % CLASS_COUNT];

		p[i] = kmalloc(size);
		if (!p[i]) {
			kputs("  heap: an allocation failed with memory available\n");
			return false;
		}
		kmemset(p[i], (int)(i & 0xff), size);
	}

	for (unsigned i = 0; i < 64; i++) {
		size_t size = class_size[i % CLASS_COUNT];
		const u8 *b = p[i];

		for (size_t j = 0; j < size; j++)
			if (b[j] != (u8)(i & 0xff)) {
				kputs("  heap: two allocations overlapped\n");
				ok = false;
				i = 64;
				break;
			}
	}

	for (unsigned i = 0; i < 64; i++)
		kfree(p[i]);

	/* A large allocation, which takes the other path entirely. */
	{
		void *big = kmalloc(3 * PAGE_SIZE);

		if (!big) {
			kputs("  heap: a large allocation failed\n");
			ok = false;
		} else {
			if (((uintptr_t)big & (PAGE_SIZE - 1)) != 0) {
				kputs("  heap: a large allocation was not page aligned, "
				      "so kfree cannot tell it apart from a slab object\n");
				ok = false;
			}
			kmemset(big, 0xA5, 3 * PAGE_SIZE);
			kfree(big);
		}
	}

	{
		void *z = kzalloc(200);

		if (!z) {
			kputs("  heap: kzalloc failed\n");
			ok = false;
		} else {
			for (size_t i = 0; i < 200; i++)
				if (((u8 *)z)[i] != 0) {
					kputs("  heap: kzalloc returned memory that was not zero\n");
					ok = false;
					break;
				}
			kfree(z);
		}
	}

	if (bytes_live != live_before) {
		kprintf("  heap: %ld bytes did not come back\n",
			(i64)bytes_live - (i64)live_before);
		ok = false;
	}

	/* And the slabs themselves were handed back, which is the difference
	 * between a heap that returns memory and one that merely stops using it. */
	if (slab_count != slabs_before) {
		kprintf("  heap: %ld slabs were kept after everything in them was freed\n",
			(i64)slab_count - (i64)slabs_before);
		ok = false;
	}

	return ok;
}
