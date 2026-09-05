/* The virtqueue: descriptors, two rings, and the barriers between them.
 *
 * See virtio.h for what the three structures are. This file is the mechanics,
 * and it is portable in the strict sense -- it never touches a register. A
 * transport (memory-mapped, or over PCI) finds a device, configures it, and
 * points it at the physical addresses this file produces. Everything after that
 * is memory that both ends agree on.
 */
#include <recon/kernel/virtio.h>

#include <recon/kernel/vm.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/console.h>

/* The specification requires the three rings to be aligned, and the alignments
 * differ: the descriptor table 16, the available ring 2, the used ring 4. All
 * three are satisfied by putting each on its own page, which also means the
 * device is never handed an address that shares a page with something it should
 * not be able to reach. That is worth a page or two on a machine with
 * megabytes. */
static size_t pages_for(size_t bytes)
{
	return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

bool virtqueue_init(struct virtqueue *q, u16 size)
{
	size_t desc_bytes, avail_bytes, used_bytes;
	size_t desc_pages, avail_pages, used_pages, total;
	paddr_t base;
	u8 *virt;

	if (!size || (size & (size - 1)))
		return false;	/* the ring index arithmetic assumes a power of two */

	kmemset(q, 0, sizeof(*q));

	desc_bytes  = (size_t)size * sizeof(struct vring_desc);
	avail_bytes = sizeof(struct vring_avail) + (size_t)size * sizeof(u16)
		    + sizeof(u16);	/* the trailing used_event, unused but real */
	used_bytes  = sizeof(struct vring_used)
		    + (size_t)size * sizeof(struct vring_used_elem)
		    + sizeof(u16);	/* the trailing avail_event */

	desc_pages  = pages_for(desc_bytes);
	avail_pages = pages_for(avail_bytes);
	used_pages  = pages_for(used_bytes);
	total       = desc_pages + avail_pages + used_pages;

	/* One contiguous allocation, because the device is given three separate
	 * physical addresses and they must each be contiguous in themselves --
	 * a ring split across two non-adjacent physical pages is a ring the
	 * device walks straight off the end of. */
	base = pmm_alloc_pages(total);
	if (!base)
		return false;

	virt = phys_to_virt(base);

	q->size = size;

	q->desc       = (struct vring_desc *)virt;
	q->desc_phys  = base;

	q->avail      = (struct vring_avail *)(virt + desc_pages * PAGE_SIZE);
	q->avail_phys = base + desc_pages * PAGE_SIZE;

	q->used       = (struct vring_used *)(virt + (desc_pages + avail_pages) * PAGE_SIZE);
	q->used_phys  = base + (desc_pages + avail_pages) * PAGE_SIZE;

	q->backing       = base;
	q->backing_pages = total;

	/* Pages arrive cleared, but the free list has to be threaded: every
	 * descriptor points at the next, and the last points nowhere in
	 * particular because free_count is what says when to stop. */
	for (u16 i = 0; i < size; i++)
		q->desc[i].next = (u16)(i + 1);

	q->free_head  = 0;
	q->free_count = size;
	q->last_used  = 0;

	return true;
}

void virtqueue_free(struct virtqueue *q)
{
	if (q->backing)
		pmm_free_pages(q->backing, q->backing_pages);
	kmemset(q, 0, sizeof(*q));
}

u16 virtqueue_avail_index(const struct virtqueue *q)
{
	return q->avail->idx;
}

u16 virtqueue_submit(struct virtqueue *q, const paddr_t *bufs, const u32 *lens,
		     const bool *write, unsigned count)
{
	u16 head, prev = 0;
	u16 idx;

	if (!count || count > q->free_count)
		return 0xFFFF;

	head = q->free_head;

	for (unsigned i = 0; i < count; i++) {
		u16 d = q->free_head;

		q->free_head = q->desc[d].next;
		q->free_count--;

		q->desc[d].addr  = (u64)bufs[i];
		q->desc[d].len   = lens[i];
		q->desc[d].flags = write[i] ? VRING_DESC_F_WRITE : 0;
		q->desc[d].next  = 0;

		if (i > 0) {
			q->desc[prev].flags |= VRING_DESC_F_NEXT;
			q->desc[prev].next = d;
		}

		prev = d;
	}

	/* Publish the head into the available ring at the position the *next*
	 * index names, masked to the ring, and only then bump the index. */
	idx = q->avail->idx;
	q->avail->ring[idx & (q->size - 1)] = head;

	/* THE BARRIER THAT MATTERS ON THE WAY OUT.
	 *
	 * Everything above -- the descriptors, and the ring slot pointing at
	 * them -- must be visible to the device before the index that tells it
	 * to look. Without this the device is entitled to see the new index and
	 * the old descriptor, and it will read whatever was in that slot last
	 * time, which is a physical address that used to be a buffer. */
	__atomic_thread_fence(__ATOMIC_RELEASE);

	q->avail->idx = (u16)(idx + 1);

	return head;
}

bool virtqueue_collect(struct virtqueue *q, u16 *head_out, u32 *len_out)
{
	const struct vring_used_elem *e;
	u16 used_idx;

	/* Read the device's index first, and treat it as the gate. */
	used_idx = q->used->idx;

	/* The counters are 16-bit and wrap at 65536 rather than at the queue
	 * size, so the comparison is on the raw values and only the *indexing*
	 * is masked. Comparing masked values works until the first wrap. */
	if (used_idx == q->last_used)
		return false;

	/* THE BARRIER THAT MATTERS ON THE WAY BACK IN.
	 *
	 * The index said an entry is ready; this makes sure the entry itself is
	 * read afterwards. Reordered, the driver reads a used-ring slot the
	 * device has not written yet and frees a descriptor that is still in
	 * flight. */
	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	e = &q->used->ring[q->last_used & (q->size - 1)];

	if (head_out)
		*head_out = (u16)e->id;
	if (len_out)
		*len_out = e->len;

	q->last_used = (u16)(q->last_used + 1);
	return true;
}

void virtqueue_release(struct virtqueue *q, u16 head)
{
	u16 d = head;

	for (;;) {
		u16 next = q->desc[d].next;
		bool chained = (q->desc[d].flags & VRING_DESC_F_NEXT) != 0;

		/* Back onto the free list, front first, which keeps this O(1)
		 * and keeps recently used descriptors warm. */
		q->desc[d].next = q->free_head;
		q->desc[d].flags = 0;
		q->free_head = d;
		q->free_count++;

		if (!chained)
			break;

		d = next;
	}
}
