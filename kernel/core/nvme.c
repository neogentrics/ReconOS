/* NVMe: the disk controller real hardware has.
 *
 * virtio proved the block layer, and it exists only where there is a
 * hypervisor. Every machine made since about 2016 has this instead, and
 * checkpoint 17 -- boots on real hardware -- is not reachable without it.
 *
 * --- It is a queue protocol, and that is the good news ---
 *
 * NVMe and virtio are shaped alike: a submission queue the driver writes, a
 * completion queue the device writes, and a doorbell register to say something
 * is there. Having built one, the second is mostly a different set of field
 * names. Three things are genuinely different, and all three are below:
 *
 *   THE CONTROLLER MUST BE STOPPED BEFORE IT IS CONFIGURED. Writing the queue
 *   addresses while it is running is ignored, silently. So the sequence is
 *   disable, wait for it to say it has stopped, configure, enable, wait for it
 *   to say it is ready -- with a timeout the controller itself publishes.
 *
 *   THERE ARE TWO KINDS OF QUEUE. An *admin* queue, which exists to create the
 *   others and to ask what the device is, and *I/O* queues, which carry reads
 *   and writes. The admin queue is configured through registers; the I/O queues
 *   are created by sending commands through the admin queue. One of each is
 *   built here, because nothing in this kernel issues two requests at once.
 *
 *   COMPLETIONS ARE FOUND BY A PHASE BIT, NOT BY AN INDEX. The completion queue
 *   is never cleared; instead every entry carries a bit that flips each time the
 *   ring wraps. An entry is new when its phase differs from the one before. This
 *   is cheaper than an index -- there is nothing to read from the device to know
 *   whether work is done -- and it is the detail that makes a first NVMe driver
 *   hang: forget to flip the expected phase on wrap and everything works for
 *   exactly one lap of the ring.
 */
#include <recon/kernel/block.h>
#include <recon/kernel/pci.h>

#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/time.h>
#include <recon/kernel/sched.h>

/* Controller registers, offsets from the first base address register. */
#define NVME_CAP      0x00	/* capabilities; 64-bit */
#define NVME_VS       0x08
#define NVME_INTMS    0x0C
#define NVME_INTMC    0x10
#define NVME_CC       0x14	/* configuration */
#define NVME_CSTS     0x1C	/* status */
#define NVME_AQA      0x24	/* admin queue sizes */
#define NVME_ASQ      0x28	/* admin submission queue address; 64-bit */
#define NVME_ACQ      0x30	/* admin completion queue address; 64-bit */
#define NVME_DOORBELL 0x1000	/* the first of them; the rest are strided */

#define NVME_CC_ENABLE  (1u << 0)
#define NVME_CSTS_READY (1u << 0)
#define NVME_CSTS_FATAL (1u << 1)

/* Admin opcodes. */
#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY  0x06
#define NVME_ADMIN_SET_FEATURES 0x09

/* I/O opcodes. */
#define NVME_IO_FLUSH 0x00
#define NVME_IO_WRITE 0x01
#define NVME_IO_READ  0x02

#define NVME_QUEUE_SIZE 32	/* entries in each queue; see below */
#define NVME_MAX 2

struct nvme_command {
	u32 cdw0;		/* opcode, and the command identifier up top */
	u32 nsid;
	u64 reserved;
	u64 metadata;
	u64 prp1;
	u64 prp2;
	u32 cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} RK_PACKED;

struct nvme_completion {
	u32 result;
	u32 reserved;
	u16 sq_head;
	u16 sq_id;
	u16 cid;
	u16 status;		/* the phase bit is bit 0 */
} RK_PACKED;

struct nvme_queue {
	volatile struct nvme_command    *sq;
	volatile struct nvme_completion *cq;
	paddr_t sq_phys, cq_phys;
	paddr_t backing;
	size_t  backing_pages;

	u16 size;
	u16 sq_tail;
	u16 cq_head;
	u8  phase;		/* what a *new* completion's phase bit will be */

	volatile u32 *sq_doorbell;
	volatile u32 *cq_doorbell;
};

struct nvme {
	volatile u8 *regs;
	struct nvme_queue admin;
	struct nvme_queue io;

	/* One page for identify results and one for the list of pages a large
	 * transfer is scattered across. Allocated once: both are needed on a
	 * path that must not allocate, and a driver that allocates per request
	 * fails at the worst possible moment. */
	paddr_t scratch_phys;
	void   *scratch;
	paddr_t prp_list_phys;
	u64    *prp_list;

	u32 namespace_id;
	u32 block_size;
	u64 block_count;
	u16 next_cid;

	struct block_device *bdev;
};

static struct nvme controllers[NVME_MAX];
static unsigned controller_count;

static u32 reg32(struct nvme *n, u32 off)
{
	return *(volatile u32 *)(n->regs + off);
}

static void wreg32(struct nvme *n, u32 off, u32 v)
{
	*(volatile u32 *)(n->regs + off) = v;
}

static u64 reg64(struct nvme *n, u32 off)
{
	/* Two 32-bit reads. The capability register is genuinely 64 bits wide
	 * and a controller is permitted to require it be read as one 64-bit
	 * access -- but it is also permitted to be behind a bridge that splits
	 * it, and reading halves works on both. */
	return (u64)*(volatile u32 *)(n->regs + off)
	     | ((u64)*(volatile u32 *)(n->regs + off + 4) << 32);
}

static void wreg64(struct nvme *n, u32 off, u64 v)
{
	*(volatile u32 *)(n->regs + off)     = (u32)v;
	*(volatile u32 *)(n->regs + off + 4) = (u32)(v >> 32);
}

/* --- Queues ---------------------------------------------------------------
 *
 * Thirty-two entries each, which is small and deliberate: a submission entry is
 * sixty-four bytes and a completion sixteen, so a queue pair is well under a
 * page, and nothing in this kernel issues more than one request at a time. A
 * driver with a queue depth of 1024 and a caller with a queue depth of one is
 * two pages of reserved memory doing nothing.
 */
static bool queue_init(struct nvme *n, struct nvme_queue *q, u16 index, u16 size)
{
	size_t sq_bytes = (size_t)size * sizeof(struct nvme_command);
	size_t cq_bytes = (size_t)size * sizeof(struct nvme_completion);
	size_t sq_pages = (sq_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	size_t cq_pages = (cq_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
	paddr_t base;
	u32 stride;

	kmemset(q, 0, sizeof(*q));

	base = pmm_alloc_pages(sq_pages + cq_pages);
	if (!base)
		return false;

	q->backing       = base;
	q->backing_pages = sq_pages + cq_pages;

	q->sq      = phys_to_virt(base);
	q->sq_phys = base;
	q->cq      = (volatile struct nvme_completion *)
		     ((u8 *)phys_to_virt(base) + sq_pages * PAGE_SIZE);
	q->cq_phys = base + sq_pages * PAGE_SIZE;

	q->size  = size;
	q->phase = 1;	/* the queue is zeroed, so the first new entry reads 1 */

	/* Doorbells are strided, and the stride is per controller. Two per
	 * queue -- submission then completion -- so queue N's pair starts at
	 * 2N. Getting the stride wrong rings the wrong queue's bell, which is
	 * a controller that never answers rather than an error. */
	stride = 4u << (u32)((reg64(n, NVME_CAP) >> 32) & 0xF);
	q->sq_doorbell = (volatile u32 *)(n->regs + NVME_DOORBELL
					  + (u32)(2 * index) * stride);
	q->cq_doorbell = (volatile u32 *)(n->regs + NVME_DOORBELL
					  + (u32)(2 * index + 1) * stride);

	return true;
}

static void queue_free(struct nvme_queue *q)
{
	if (q->backing)
		pmm_free_pages(q->backing, q->backing_pages);
	kmemset(q, 0, sizeof(*q));
}

/* Sends one command and waits for its completion.
 *
 * Synchronous for the same reason the block layer is: there is no way for a
 * thread to wait yet. The timeout is the controller's own published one rather
 * than a number chosen here -- a controller that says it may take twenty
 * seconds to become ready is telling the truth, and a driver that gives it two
 * calls working hardware broken. */
static bool submit(struct nvme *n, struct nvme_queue *q,
		   const struct nvme_command *cmd, u32 *result_out, u16 *status_out)
{
	u16 cid = n->next_cid++;
	volatile struct nvme_completion *c;
	u64 deadline;
	u16 status;

	q->sq[q->sq_tail] = *cmd;
	q->sq[q->sq_tail].cdw0 = (cmd->cdw0 & 0xFFFFu) | ((u32)cid << 16);

	/* The entry must be visible before the doorbell that points at it, for
	 * exactly the reason the virtqueue's available index needs a barrier. */
	__atomic_thread_fence(__ATOMIC_RELEASE);

	q->sq_tail = (u16)((q->sq_tail + 1) % q->size);
	*q->sq_doorbell = q->sq_tail;

	c = &q->cq[q->cq_head];

	/* Sixty seconds, which is not generosity -- a controller in the middle
	 * of an internal recovery is allowed to take that long, and the failure
	 * this guards against is a wedged one, not a slow one. */
	deadline = time_monotonic_ns() + 60000000000ULL;

	for (;;) {
		status = c->status;

		if ((status & 1) == q->phase)
			break;

		if (time_monotonic_ns() > deadline)
			return false;

		sched_yield();
	}

	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	if (result_out)
		*result_out = c->result;
	if (status_out)
		*status_out = (u16)(status >> 1);

	/* The phase flips when the ring wraps, and only then. Forgetting this
	 * gives a driver that works for exactly one lap. */
	q->cq_head = (u16)((q->cq_head + 1) % q->size);
	if (q->cq_head == 0)
		q->phase ^= 1;

	*q->cq_doorbell = q->cq_head;

	if (c->cid != cid) {
		/* Only possible with more than one command in flight, which
		 * this driver never does. Checked anyway, because the day that
		 * changes this is the assumption that will be silently wrong. */
		kputs("nvme: a completion arrived for a command nobody sent\n");
		return false;
	}

	return true;
}

/* Waits for the controller's ready bit to reach `want`. */
static bool wait_ready(struct nvme *n, bool want)
{
	/* The timeout field is in half-second units, and is the controller
	 * telling the driver how patient to be. */
	u64 limit_ns = ((reg64(n, NVME_CAP) >> 24) & 0xFF) * 500000000ULL;
	u64 deadline;

	if (!limit_ns)
		limit_ns = 5000000000ULL;

	deadline = time_monotonic_ns() + limit_ns;

	for (;;) {
		u32 csts = reg32(n, NVME_CSTS);

		if (csts & NVME_CSTS_FATAL) {
			kputs("nvme: the controller reports a fatal error\n");
			return false;
		}

		if (!!(csts & NVME_CSTS_READY) == want)
			return true;

		if (time_monotonic_ns() > deadline)
			return false;

		sched_yield();
	}
}

/* --- Transfers ------------------------------------------------------------
 *
 * NVMe describes the memory for a transfer as a list of *pages*, not as an
 * address and a length. One page goes in the first pointer; a second page goes
 * in the second; three or more and the second pointer becomes the address of a
 * list of the rest. That is the whole of the scheme, and the boundary between
 * the second and third case is where a driver written against the two-page case
 * quietly corrupts memory.
 */
static bool build_prp(struct nvme *n, paddr_t buf, u32 len, u64 *prp1, u64 *prp2)
{
	u64 first_page_bytes;
	u32 remaining;
	paddr_t p;
	unsigned entries = 0;

	*prp1 = buf;

	/* How much of the transfer the first pointer covers. A buffer that does
	 * not start on a page boundary covers less than a page, which is legal
	 * and is the case that makes the count off by one if it is assumed
	 * away. */
	first_page_bytes = PAGE_SIZE - (buf & (PAGE_SIZE - 1));

	if (len <= first_page_bytes) {
		*prp2 = 0;
		return true;
	}

	p = (buf + first_page_bytes);
	remaining = (u32)(len - first_page_bytes);

	if (remaining <= PAGE_SIZE) {
		*prp2 = p;
		return true;
	}

	/* Three pages or more: the second pointer names a list. */
	while (remaining) {
		if (entries >= PAGE_SIZE / sizeof(u64))
			return false;	/* beyond what one list page describes */

		n->prp_list[entries++] = p;
		p += PAGE_SIZE;
		remaining = remaining > PAGE_SIZE ? remaining - PAGE_SIZE : 0;
	}

	*prp2 = n->prp_list_phys;
	return true;
}

static enum block_status transfer(struct nvme *n, u8 opcode, u64 lba, u32 count,
				  paddr_t buf)
{
	struct nvme_command cmd;
	u16 status;
	u32 bytes = count * n->block_size;
	u64 prp1, prp2;

	kmemset(&cmd, 0, sizeof(cmd));
	cmd.cdw0  = opcode;
	cmd.nsid  = n->namespace_id;
	cmd.cdw10 = (u32)lba;
	cmd.cdw11 = (u32)(lba >> 32);
	cmd.cdw12 = count - 1;	/* zero-based, and getting this wrong is one
				 * extra block written past the end */

	/* Through locals rather than straight into the command, because the
	 * command is a packed structure and the address of a packed member is
	 * not a pointer the compiler will let anyone hold. The layout here
	 * happens to be naturally aligned; the packing is there so that it
	 * stays that way if a field is ever added. */
	if (!build_prp(n, buf, bytes, &prp1, &prp2))
		return BLOCK_ERR_RANGE;

	cmd.prp1 = prp1;
	cmd.prp2 = prp2;

	if (!submit(n, &n->io, &cmd, 0, &status))
		return BLOCK_ERR_TIMEOUT;

	return status ? BLOCK_ERR_IO : BLOCK_OK;
}

static enum block_status nvme_read(struct block_device *dev, u64 lba, u32 count,
				   void *buf)
{
	struct nvme *n = dev->driver;
	paddr_t phys = virt_to_phys(buf);

	if (!phys)
		return BLOCK_ERR_ALIGN;

	return transfer(n, NVME_IO_READ, lba, count, phys);
}

static enum block_status nvme_write(struct block_device *dev, u64 lba, u32 count,
				    const void *buf)
{
	struct nvme *n = dev->driver;
	paddr_t phys = virt_to_phys(buf);

	if (!phys)
		return BLOCK_ERR_ALIGN;

	return transfer(n, NVME_IO_WRITE, lba, count, phys);
}

static enum block_status nvme_flush(struct block_device *dev)
{
	struct nvme *n = dev->driver;
	struct nvme_command cmd;
	u16 status;

	kmemset(&cmd, 0, sizeof(cmd));
	cmd.cdw0 = NVME_IO_FLUSH;
	cmd.nsid = n->namespace_id;

	if (!submit(n, &n->io, &cmd, 0, &status))
		return BLOCK_ERR_TIMEOUT;

	return status ? BLOCK_ERR_IO : BLOCK_OK;
}

static const struct block_ops nvme_ops = {
	.read  = nvme_read,
	.write = nvme_write,
	.flush = nvme_flush,
};

/* Reads one identify page into the scratch buffer. */
static bool identify(struct nvme *n, u32 cns, u32 nsid)
{
	struct nvme_command cmd;
	u16 status;

	kmemset(n->scratch, 0, PAGE_SIZE);
	kmemset(&cmd, 0, sizeof(cmd));

	cmd.cdw0  = NVME_ADMIN_IDENTIFY;
	cmd.nsid  = nsid;
	cmd.prp1  = n->scratch_phys;
	cmd.cdw10 = cns;

	if (!submit(n, &n->admin, &cmd, 0, &status))
		return false;

	return status == 0;
}

static bool create_io_queues(struct nvme *n)
{
	struct nvme_command cmd;
	u16 status;

	if (!queue_init(n, &n->io, 1, NVME_QUEUE_SIZE))
		return false;

	/* The completion queue first. A submission queue names the completion
	 * queue it reports into, so the other order asks the controller to
	 * point at something that does not exist. */
	kmemset(&cmd, 0, sizeof(cmd));
	cmd.cdw0  = NVME_ADMIN_CREATE_CQ;
	cmd.prp1  = n->io.cq_phys;
	cmd.cdw10 = ((u32)(NVME_QUEUE_SIZE - 1) << 16) | 1;	/* size, id */
	cmd.cdw11 = 1;			/* physically contiguous; no interrupts */

	if (!submit(n, &n->admin, &cmd, 0, &status) || status) {
		kputs("nvme: the controller refused an I/O completion queue\n");
		return false;
	}

	kmemset(&cmd, 0, sizeof(cmd));
	cmd.cdw0  = NVME_ADMIN_CREATE_SQ;
	cmd.prp1  = n->io.sq_phys;
	cmd.cdw10 = ((u32)(NVME_QUEUE_SIZE - 1) << 16) | 1;
	cmd.cdw11 = (1u << 16) | 1;	/* reports into completion queue 1 */

	if (!submit(n, &n->admin, &cmd, 0, &status) || status) {
		kputs("nvme: the controller refused an I/O submission queue\n");
		return false;
	}

	return true;
}

/* Reads the first namespace's size and block size out of its identify page.
 *
 * Namespace 1 only. An NVMe device may carve itself into several, and every
 * consumer drive presents exactly one; enumerating the rest needs the active
 * namespace list and is worth doing when something has more than one. Said out
 * loud so that a machine whose disk is namespace 2 is a puzzle for a minute
 * rather than an afternoon. */
static bool read_namespace(struct nvme *n)
{
	const u8 *id;
	u64 blocks;
	u8 flbas, lba_index, lbads;

	if (!identify(n, 0, 1))
		return false;

	id = n->scratch;

	/* Size in blocks, at offset 0. Read byte by byte: the page is a device
	 * structure with no alignment promises this kernel should rely on. */
	blocks = 0;
	for (unsigned i = 0; i < 8; i++)
		blocks |= (u64)id[i] << (i * 8);

	if (!blocks)
		return false;

	/* Which of the sixteen possible block formats is in use, and how big
	 * that format's block is. The size is a power of two given as its
	 * exponent, which is why a drive with 4096-byte blocks reports 12. */
	flbas     = id[26];
	lba_index = (u8)(flbas & 0x0F);
	lbads     = id[128 + lba_index * 4 + 2];

	if (lbads < 9 || lbads > 16)
		return false;	/* 512 bytes to 64KB; anything else is nonsense */

	n->namespace_id = 1;
	n->block_count  = blocks;
	n->block_size   = 1u << lbads;
	return true;
}

/* Takes a PCI function that says it is an NVMe controller and makes it a block
 * device. Returns false, with a reason, on anything that goes wrong. */
bool nvme_attach(const struct pci_device *d)
{
	struct nvme *n;
	char name[BLOCK_NAME_MAX];
	paddr_t page;
	u64 cap;
	u16 max_entries;
	u32 cc;

	/* Class 1 subclass 8 is "non-volatile memory"; programming interface 2
	 * is the NVMe register set specifically. The last of the three matters:
	 * subclass 8 alone also covers controllers that are not NVMe. */
	if (d->class_code != 0x01 || d->subclass != 0x08 || d->prog_if != 0x02)
		return false;

	if (controller_count >= NVME_MAX)
		return false;

	n = &controllers[controller_count];
	kmemset(n, 0, sizeof(*n));

	{
		paddr_t base = (paddr_t)d->bar[0];
		paddr_t first;
		u64 span;
		vaddr_t va;

		if (!base || d->bar_is_io[0] || !d->bar_size[0])
			return false;

		first = PAGE_ALIGN_DOWN(base);
		span  = PAGE_ALIGN_UP((base - first) + d->bar_size[0]);
		va    = (vaddr_t)(uintptr_t)phys_to_virt(first);

		if (!vm_lookup(va) &&
		    !vm_map(va, first, span,
			    VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL)) {
			kputs("nvme: could not map the controller's registers\n");
			return false;
		}

		n->regs = phys_to_virt(base);
	}

	cap = reg64(n, NVME_CAP);
	max_entries = (u16)((cap & 0xFFFF) + 1);

	/* A controller that cannot hold as many entries as we intend to give it
	 * would silently truncate the queue and then report completions for
	 * entries it never read. */
	if (max_entries < NVME_QUEUE_SIZE) {
		kprintf("nvme: the controller's queues hold %u entries, fewer "
			"than the %u this driver uses\n",
			max_entries, (unsigned)NVME_QUEUE_SIZE);
		return false;
	}

	/* Stop it before configuring it. A running controller ignores writes to
	 * its queue registers, and ignores them quietly. */
	cc = reg32(n, NVME_CC);
	wreg32(n, NVME_CC, cc & ~NVME_CC_ENABLE);
	if (!wait_ready(n, false)) {
		kputs("nvme: the controller would not stop\n");
		return false;
	}

	if (!queue_init(n, &n->admin, 0, NVME_QUEUE_SIZE)) {
		kputs("nvme: no memory for the admin queue\n");
		return false;
	}

	page = pmm_alloc_page();
	if (!page)
		goto no_memory;
	n->scratch_phys = page;
	n->scratch      = phys_to_virt(page);

	page = pmm_alloc_page();
	if (!page)
		goto no_memory;
	n->prp_list_phys = page;
	n->prp_list      = phys_to_virt(page);

	/* Sizes are zero-based here as well. */
	wreg32(n, NVME_AQA, ((u32)(NVME_QUEUE_SIZE - 1) << 16)
			  | (u32)(NVME_QUEUE_SIZE - 1));
	wreg64(n, NVME_ASQ, n->admin.sq_phys);
	wreg64(n, NVME_ACQ, n->admin.cq_phys);

	/* Entry sizes are exponents: 6 is sixty-four bytes for a submission
	 * entry, 4 is sixteen for a completion. The page size field is likewise
	 * an exponent above 4096, so zero means 4KB, which is what this kernel
	 * uses. */
	cc = (6u << 16) | (4u << 20) | NVME_CC_ENABLE;
	wreg32(n, NVME_CC, cc);

	if (!wait_ready(n, true)) {
		kputs("nvme: the controller would not start\n");
		goto fail;
	}

	if (!identify(n, 1, 0)) {
		kputs("nvme: the controller would not identify itself\n");
		goto fail;
	}

	if (!create_io_queues(n))
		goto fail;

	if (!read_namespace(n)) {
		kputs("nvme: no usable namespace on this controller\n");
		goto fail;
	}

	kstrlcpy(name, "nvme0n1", sizeof(name));
	name[4] = (char)('0' + controller_count);

	n->bdev = block_register(name, &nvme_ops, n, n->block_size, n->block_count);
	if (!n->bdev)
		goto fail;

	/* One list page describes 512 more pages, and the first pointer covers
	 * one. Stated as a limit so the block layer splits rather than the
	 * driver failing a large request that a caller had every right to
	 * make. */
	/* NVMe's Flush command is mandatory for a controller with a volatile
	 * write cache and harmless on one without, and this driver issues it
	 * and waits for the completion. */
	n->bdev->flush_is_durable = true;

	n->bdev->max_blocks_per_request =
		(u32)(((PAGE_SIZE / sizeof(u64)) + 1) * PAGE_SIZE / n->block_size);

	controller_count++;
	return true;

no_memory:
	kputs("nvme: no memory for the driver's buffers\n");
fail:
	if (n->scratch_phys)
		pmm_free_page(n->scratch_phys);
	if (n->prp_list_phys)
		pmm_free_page(n->prp_list_phys);
	queue_free(&n->admin);
	queue_free(&n->io);
	return false;
}
