/* virtio-gpu: the second display, and the one that disagrees with the first.
 *
 * See virtio_gpu.h for why a second backend was worth writing before a real
 * GPU, and what it cost `display_ops`. The short version: the Bochs adapter
 * scans out a PCI aperture continuously, so drawing is the whole of drawing.
 * This device keeps its pixels in guest RAM the host cannot see, and nothing
 * appears until it is told to look.
 *
 * --- Where the pixels live, which is the other half of the difference -------
 *
 * The Bochs driver maps a BAR: device memory, above RAM, which the page
 * allocator has never heard of and must never be told about. This driver calls
 * `pmm_alloc_pages` -- the framebuffer *is* ordinary memory, from the same
 * allocator everything else comes from.
 *
 * That inverts an assumption that was safe for as long as there was one
 * backend, and `addrspace_release_page` is where it was written down: it
 * refuses to free a page the allocator does not own, which was exactly right
 * for an aperture and is exactly wrong here (GX-001).
 *
 * --- Endianness -------------------------------------------------------------
 *
 * virtio 1.0 is little-endian on the wire. Both architectures this kernel
 * builds for run little-endian, so the structures below are filled by ordinary
 * assignment rather than through byte-swapping accessors. That is a fact about
 * the targets and not about the protocol, which is why it is said here: the day
 * this kernel is built big-endian, every field in this file needs converting
 * and nothing will warn.
 */
#include <recon/kernel/console.h>
#include <recon/kernel/display.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/suspend.h>
#include <recon/kernel/time.h>
#include <recon/kernel/virtio.h>
#include <recon/kernel/virtio_gpu.h>
#include <recon/kernel/vm.h>

/* The device type in a virtio identifier. 16 is the GPU. */
#define VIRTIO_ID_GPU		16

/* The control queue. There is a second, the cursor queue, which this driver
 * does not use: a hardware cursor is a compositor's concern and there is no
 * compositor yet. Declaring a queue nothing feeds is worse than not declaring
 * it -- it looks driven. */
#define GPU_CONTROLQ		0

/* Commands, from the virtio specification's own numbering. Only the 2D set:
 * the 3D commands need a renderer on the host and a context on the guest, and
 * this kernel has neither. */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO		0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D	0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF		0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT		0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH		0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D	0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING	0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING	0x0107

#define VIRTIO_GPU_RESP_OK_NODATA		0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO		0x1101

/* Anything from here up is a refusal. The specific codes are not enumerated
 * because this driver does not act differently on any of them -- it reports the
 * number and stops, which is the honest response to "the host said no" when
 * there is nothing to retry. */
#define VIRTIO_GPU_RESP_ERR_BASE		0x1200

/* One byte each of blue, green, red and an unused fourth, in memory order.
 *
 * **The name reads backwards and that is the specification's convention, not a
 * mistake here.** virtio-gpu names a format by its channels from the most
 * significant byte of a little-endian word downwards, so B8G8R8X8 is the word
 * 0xXXRRGGBB, which is the bytes B,G,R,X -- which is what `FB_FORMAT_BGRA`
 * already means to the console and what the Bochs adapter already produces.
 * Getting this backwards does not fail; it swaps red and blue, on a screen
 * nobody in the verification rig is looking at. */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM	2

/* The scanout and resource this driver uses. One of each: a single screen,
 * which is what `struct display` can describe. A device may offer sixteen
 * scanouts and this drives the first -- see docs/SIGNALS.md, because the
 * interface has no way to say there are others. */
#define GPU_SCANOUT		0
#define GPU_RESOURCE_ID		1

/* How much memory this driver will spend on pixels.
 *
 * The Bochs adapter has an answer to "how large can the screen be" -- the size
 * of its BAR, which is a fact about the hardware. **This device has no such
 * limit and therefore no such answer**: the framebuffer is RAM, and the only
 * ceiling is how much of the machine's memory a screen is worth. So the number
 * is a policy rather than a measurement, and it is written here where it can be
 * argued with rather than buried in a mode table.
 *
 * 64 MiB holds 4K at four bytes a pixel with room over. It is also capped
 * against what is actually free below, because a machine with 128 MiB of RAM
 * should not spend half of it on a screen it has not been asked for. */
#define GPU_BUDGET_MAX		(64ull * 1024 * 1024)

/* A command and its response share one page: the command at the front, the
 * response half way in. One allocation, and the two are far enough apart that
 * no command this driver sends can reach the response area. The largest is
 * GET_DISPLAY_INFO's reply at 408 bytes. */
#define GPU_RESP_OFFSET		2048

#define VIRTIO_GPU_MAX		2

/* --- the wire structures ---------------------------------------------------
 *
 * Packed, because the device reads them at the offsets the specification names
 * and a compiler entitled to insert padding is a compiler entitled to move
 * every field after the first.
 */
struct gpu_ctrl_hdr {
	u32 type;
	u32 flags;
	u64 fence_id;
	u32 ctx_id;
	u32 padding;
} RK_PACKED;

struct gpu_rect {
	u32 x, y, width, height;
} RK_PACKED;

struct gpu_display_one {
	struct gpu_rect r;
	u32 enabled;
	u32 flags;
} RK_PACKED;

#define VIRTIO_GPU_MAX_SCANOUTS 16

struct gpu_resp_display_info {
	struct gpu_ctrl_hdr hdr;
	struct gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} RK_PACKED;

struct gpu_resource_create_2d {
	struct gpu_ctrl_hdr hdr;
	u32 resource_id;
	u32 format;
	u32 width;
	u32 height;
} RK_PACKED;

struct gpu_resource_unref {
	struct gpu_ctrl_hdr hdr;
	u32 resource_id;
	u32 padding;
} RK_PACKED;

struct gpu_mem_entry {
	u64 addr;
	u32 length;
	u32 padding;
} RK_PACKED;

struct gpu_attach_backing {
	struct gpu_ctrl_hdr hdr;
	u32 resource_id;
	u32 nr_entries;
	/* Followed by nr_entries mem entries. One, here: the framebuffer is
	 * allocated contiguously, so a scatter list would be one entry long
	 * spelled the hard way. */
	struct gpu_mem_entry entry;
} RK_PACKED;

struct gpu_detach_backing {
	struct gpu_ctrl_hdr hdr;
	u32 resource_id;
	u32 padding;
} RK_PACKED;

struct gpu_set_scanout {
	struct gpu_ctrl_hdr hdr;
	struct gpu_rect r;
	u32 scanout_id;
	u32 resource_id;
} RK_PACKED;

struct gpu_transfer_to_host_2d {
	struct gpu_ctrl_hdr hdr;
	struct gpu_rect r;
	u64 offset;
	u32 resource_id;
	u32 padding;
} RK_PACKED;

struct gpu_resource_flush {
	struct gpu_ctrl_hdr hdr;
	struct gpu_rect r;
	u32 resource_id;
	u32 padding;
} RK_PACKED;

/* --- the driver ------------------------------------------------------------ */

struct virtio_gpu {
	struct virtio_device dev;
	struct virtqueue q;

	/* The command and response page. */
	paddr_t scratch_phys;
	u8 *scratch;

	/* The pixels: allocator pages, not an aperture. */
	paddr_t fb_phys;
	size_t fb_pages;

	/* What the host said its screen is, from GET_DISPLAY_INFO. Zero where
	 * it did not say. */
	u32 preferred_w, preferred_h;

	struct display *disp;

	u64 transfers, flushes, refused, timeouts;
};

static struct virtio_gpu devices[VIRTIO_GPU_MAX];
static unsigned device_count;

/* --- one command, run to completion ----------------------------------------
 *
 * Synchronous, and that is a deliberate choice rather than a simplification.
 * Mode setting happens a handful of times in a boot, and a present happens once
 * per burst of console output -- neither is a path where the queue depth buys
 * anything, and a driver that returned before the host had acted would have to
 * answer "did it work" later, from somewhere with nothing to do about it.
 *
 * The two-second limit is the same one virtio-blk uses, for the same reason: a
 * device that has gone silent must not stop the boot.
 */
static bool gpu_command(struct virtio_gpu *g, const void *cmd, u32 cmd_len,
			void *resp, u32 resp_len, const char *what)
{
	paddr_t bufs[2];
	u32 lens[2];
	bool wr[2];
	u16 head;
	u64 deadline;
	struct gpu_ctrl_hdr *rh;

	kmemcpy(g->scratch, cmd, cmd_len);
	kmemset(g->scratch + GPU_RESP_OFFSET, 0, resp_len);

	bufs[0] = g->scratch_phys;
	lens[0] = cmd_len;
	wr[0]   = false;		/* the device reads the command */

	bufs[1] = g->scratch_phys + GPU_RESP_OFFSET;
	lens[1] = resp_len;
	wr[1]   = true;			/* and writes the response */

	head = virtqueue_submit(&g->q, bufs, lens, wr, 2);

	if (head == 0xFFFF) {
		kprintf("virtio-gpu: no descriptors left to send %s\n", what);
		return false;
	}

	g->dev.t->notify(&g->dev, GPU_CONTROLQ);

	deadline = time_monotonic_ns() + 2000000000ULL;

	for (;;) {
		u16 done;

		if (virtqueue_collect(&g->q, &done, 0)) {
			virtqueue_release(&g->q, done);

			/* **Wait for this command's own completion, not for
			 * any completion.** (GX-011)
			 *
			 * This broke out of the loop on the first chain the
			 * device finished, whatever it was -- so a driver that
			 * had ever got one behind stayed one behind for the
			 * life of the machine, each command returning on its
			 * predecessor's answer and reading a response buffer
			 * the device had not written for it.
			 *
			 * It survived for as long as it did because every
			 * command here is small, synchronous and almost always
			 * succeeds: returning early on the previous answer
			 * looks identical to returning on your own when both
			 * say OK. It stopped looking identical the moment a
			 * sixteen-megabyte transfer was followed straight away
			 * by the flush that shows it -- the flush returned
			 * before the transfer had happened, and the screen
			 * kept whatever was on it. */
			if (done == head)
				break;

			continue;
		}

		if (time_monotonic_ns() > deadline) {
			/* The descriptors are deliberately not released: the
			 * device still owns them, and handing them back would
			 * let a later command reuse memory the host may still
			 * be writing into. Two leaked descriptors is the cheap
			 * and correct answer, and it is the same call
			 * virtio-blk makes on the same reasoning. */
			g->timeouts++;
			kprintf("virtio-gpu: %s got no answer in two seconds\n",
				what);
			return false;
		}

		sched_yield();
	}

	kmemcpy(resp, g->scratch + GPU_RESP_OFFSET, resp_len);

	rh = (struct gpu_ctrl_hdr *)resp;

	if (rh->type >= VIRTIO_GPU_RESP_ERR_BASE) {
		/* **Printed with the number, not summarised as "failed".**
		 * The codes distinguish "out of memory" from "no such resource"
		 * from "invalid parameter", and the one that eventually shows
		 * up here will be diagnosed by whoever reads this line. */
		g->refused++;
		kprintf("virtio-gpu: the host refused %s with %04x\n",
			what, rh->type);
		return false;
	}

	return true;
}

/* A command whose only answer is yes or no. */
static bool gpu_simple(struct virtio_gpu *g, const void *cmd, u32 cmd_len,
		       const char *what)
{
	struct gpu_ctrl_hdr resp;

	return gpu_command(g, cmd, cmd_len, &resp, sizeof(resp), what);
}

static void hdr_init(struct gpu_ctrl_hdr *h, u32 type)
{
	kmemset(h, 0, sizeof(*h));
	h->type = type;
}

/* --- releasing whatever mode is currently up ------------------------------- */

static void release_resource(struct virtio_gpu *g)
{
	struct gpu_detach_backing detach;
	struct gpu_resource_unref unref;

	if (!g->fb_phys)
		return;

	/* **Detached before the pages are freed, and that order is the whole
	 * of it.** The host holds these physical addresses and will read them
	 * on the next transfer. Freeing first leaves the allocator free to hand
	 * the same pages to something else while the host still believes they
	 * are a screen -- which is a use-after-free with a device on the other
	 * end of it, and it would present as unrelated memory appearing on the
	 * glass rather than as anything that looks like a fault. */
	hdr_init(&detach.hdr, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
	detach.resource_id = GPU_RESOURCE_ID;
	detach.padding = 0;
	gpu_simple(g, &detach, sizeof(detach), "detach backing");

	hdr_init(&unref.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
	unref.resource_id = GPU_RESOURCE_ID;
	unref.padding = 0;
	gpu_simple(g, &unref, sizeof(unref), "unref resource");

	pmm_free_pages(g->fb_phys, g->fb_pages);
	g->fb_phys = 0;
	g->fb_pages = 0;
}

/* --- setting a mode -------------------------------------------------------- */

static bool gpu_set_mode(struct display *d, u32 width, u32 height)
{
	struct virtio_gpu *g = d->ops_private;
	struct gpu_resource_create_2d create;
	struct gpu_attach_backing attach;
	struct gpu_set_scanout scanout;
	u64 bytes;
	size_t pages;
	paddr_t mem;

	/* The same floor the Bochs driver uses, and for the same reason: below
	 * this there is not room for one character cell. */
	if (width < 64 || height < 64) {
		kprintf("virtio-gpu: %ux%u is too small to put anything on\n",
			width, height);
		return false;
	}

	/* **No sixteen-bit register to wrap here**, unlike DISPI -- the
	 * dimensions go out as 32-bit fields. The ceiling is the budget below
	 * instead, which catches the same class of request for a different
	 * reason, so a caller cannot tell the two devices apart by how they
	 * refuse something absurd. */
	bytes = (u64)width * height * 4;

	if (bytes > d->fb_size) {
		kprintf("virtio-gpu: %ux%u wants %u MB and this driver will "
			"spend %u\n", width, height,
			(unsigned)(bytes >> 20), (unsigned)(d->fb_size >> 20));
		return false;
	}

	pages = (size_t)(PAGE_ALIGN_UP(bytes) / PAGE_SIZE);

	/* Allocated before the old one is released, so that a refused mode
	 * leaves the screen that was working still working. The Bochs driver
	 * gets this for free -- its framebuffer is a fixed aperture and there
	 * is nothing to lose. Here the memory can simply not be there, and a
	 * driver that freed first would answer "no" from a machine whose screen
	 * it had already taken away. */
	mem = pmm_alloc_pages(pages);

	if (!mem) {
		kprintf("virtio-gpu: %ux%u needs %u contiguous pages and the "
			"allocator has no such run\n",
			width, height, (unsigned)pages);
		return false;
	}

	release_resource(g);

	g->fb_phys = mem;
	g->fb_pages = pages;

	/* Cleared, because these are recycled pages and whatever was in them is
	 * now a screen. A framebuffer showing the last program's heap is a
	 * privacy fault as much as an ugly one. */
	kmemset(phys_to_virt(mem), 0, (size_t)(pages * PAGE_SIZE));

	hdr_init(&create.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
	create.resource_id = GPU_RESOURCE_ID;
	create.format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
	create.width       = width;
	create.height      = height;

	if (!gpu_simple(g, &create, sizeof(create), "create resource"))
		goto give_back;

	hdr_init(&attach.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
	attach.resource_id  = GPU_RESOURCE_ID;
	attach.nr_entries   = 1;
	attach.entry.addr   = (u64)g->fb_phys;
	attach.entry.length = (u32)(pages * PAGE_SIZE);
	attach.entry.padding = 0;

	if (!gpu_simple(g, &attach, sizeof(attach), "attach backing"))
		goto give_back;

	hdr_init(&scanout.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
	scanout.r.x = 0;
	scanout.r.y = 0;
	scanout.r.width  = width;
	scanout.r.height = height;
	scanout.scanout_id  = GPU_SCANOUT;
	scanout.resource_id = GPU_RESOURCE_ID;

	if (!gpu_simple(g, &scanout, sizeof(scanout), "set scanout"))
		goto give_back;

	/* **What the device was told, and there is nothing to read back.**
	 *
	 * The Bochs driver reads its mode registers after writing them, because
	 * that interface clamps a size it cannot do and a driver that recorded
	 * its request would describe a screen that does not exist (KF-141).
	 * This one cannot do that and does not need to: the host either
	 * accepted the resource at the size asked for or answered with an error
	 * code, and both were checked above. A refusal here is a refusal, not a
	 * silently different mode.
	 *
	 * That is a real difference between the two backends rather than a
	 * corner cut, and it is written down because the next person to add a
	 * display will want to know which kind theirs is. */
	d->mode.width  = width;
	d->mode.height = height;
	d->mode.pitch  = width * 4;
	d->mode.base   = g->fb_phys;
	d->mode.size   = (u64)pages * PAGE_SIZE;
	d->mode.format = FB_FORMAT_BGRA;

	d->fb_base = g->fb_phys;

	return true;

give_back:
	/* The host did not take it, so nothing here is a screen. Handed back
	 * rather than leaked: a mode sweep that tries seven shapes would
	 * otherwise lose the memory for every one the host declined. */
	pmm_free_pages(g->fb_phys, g->fb_pages);
	g->fb_phys = 0;
	g->fb_pages = 0;
	return false;
}

/* --- presenting ------------------------------------------------------------
 *
 * The operation that did not exist before this driver did.
 */
static bool gpu_flush(struct display *d, u32 x, u32 y, u32 w, u32 h)
{
	struct virtio_gpu *g = d->ops_private;
	struct gpu_transfer_to_host_2d xfer;
	struct gpu_resource_flush flush;

	if (!g->fb_phys)
		return false;

	/* Clipped rather than refused. A console that scrolls computes a
	 * rectangle from its own geometry, and an off-by-one at the bottom row
	 * should cost the bottom row rather than the whole present -- the
	 * device would answer with an error and the screen would simply stop
	 * updating, which is the failure this whole file exists to avoid. */
	if (x >= d->mode.width || y >= d->mode.height)
		return true;

	if (x + w > d->mode.width)
		w = d->mode.width - x;
	if (y + h > d->mode.height)
		h = d->mode.height - y;

	if (!w || !h)
		return true;

	/* **The stores have to be visible to the host before it is told to
	 * copy them.**
	 *
	 * On these two architectures the framebuffer is ordinary cacheable
	 * memory and the "device" is the hypervisor reading the same coherent
	 * RAM, so a release fence is the whole of what is needed -- it stops
	 * the compiler and the processor from moving the pixel stores after the
	 * descriptor publication below.
	 *
	 * It would *not* be enough on a machine whose display engine is not
	 * coherent with the processor's caches, which is most real hardware.
	 * That machine needs the lines written back, and this is the line it
	 * will need changing. Said here rather than discovered there. */
	__atomic_thread_fence(__ATOMIC_RELEASE);

	hdr_init(&xfer.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
	xfer.r.x = x;
	xfer.r.y = y;
	xfer.r.width  = w;
	xfer.r.height = h;
	/* Where in the backing the first pixel of that rectangle is. The host
	 * walks the rest by the resource's own width, so this is the only place
	 * the driver's idea of a row and the host's have to agree. */
	xfer.offset = (u64)y * d->mode.pitch + (u64)x * 4;
	xfer.resource_id = GPU_RESOURCE_ID;
	xfer.padding = 0;

	if (!gpu_simple(g, &xfer, sizeof(xfer), "transfer to host"))
		return false;

	g->transfers++;

	/* **And the second command, which is the one people forget.**
	 *
	 * The transfer copies guest pages into the host's surface. It does not
	 * put the surface on the screen. A driver that stopped here would have
	 * a host-side copy of every pixel, perfectly up to date, and a display
	 * showing the frame it was created with. */
	hdr_init(&flush.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
	flush.r.x = x;
	flush.r.y = y;
	flush.r.width  = w;
	flush.r.height = h;
	flush.resource_id = GPU_RESOURCE_ID;
	flush.padding = 0;

	if (!gpu_simple(g, &flush, sizeof(flush), "resource flush"))
		return false;

	g->flushes++;
	return true;
}

/* What the host says its screen is.
 *
 * Read once at attach and remembered, rather than asked again here: this is
 * called while choosing a mode, and a device that answered differently between
 * the question and the mode set would be a device changing its screen underneath
 * a driver -- which is a hot-plug event, not a mode query, and this kernel has
 * no way to be told about one yet.
 */
static bool gpu_preferred_mode(struct display *d, u32 *width, u32 *height)
{
	struct virtio_gpu *g = d->ops_private;

	if (!g->preferred_w || !g->preferred_h)
		return false;

	*width  = g->preferred_w;
	*height = g->preferred_h;
	return true;
}

static const struct display_ops gpu_ops = {
	.set_mode       = gpu_set_mode,
	.flush          = gpu_flush,
	.preferred_mode = gpu_preferred_mode,
};

/* --- asking the host what its screen is ------------------------------------
 *
 * The Bochs adapter cannot be asked this. It has a memory size and a set of
 * registers, and the driver picks a mode off a ladder and hopes it is a
 * sensible one for the panel -- which on a real machine it frequently is not.
 *
 * This device knows. `GET_DISPLAY_INFO` reports, per scanout, the rectangle the
 * host is actually presenting and whether it is enabled. That is a capability
 * `display_ops` has nowhere to put, so it is recorded on the driver and printed,
 * and raised in docs/SIGNALS.md rather than bolted on here.
 */
static void read_display_info(struct virtio_gpu *g)
{
	struct gpu_ctrl_hdr req;
	struct gpu_resp_display_info info;

	hdr_init(&req, VIRTIO_GPU_CMD_GET_DISPLAY_INFO);

	if (!gpu_command(g, &req, sizeof(req), &info, sizeof(info),
			 "get display info"))
		return;

	if (info.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
		kprintf("virtio-gpu: asked what the screen is and got %04x "
			"where a display list answers 1101\n", info.hdr.type);
		return;
	}

	if (!info.pmodes[GPU_SCANOUT].enabled) {
		kputs("virtio-gpu: the host's first scanout is not enabled\n");
		return;
	}

	g->preferred_w = info.pmodes[GPU_SCANOUT].r.width;
	g->preferred_h = info.pmodes[GPU_SCANOUT].r.height;

	/* How many others there are, counted rather than assumed. A machine
	 * with three screens attached is a machine this kernel drives one of,
	 * and the summary should say so rather than let a single-headed
	 * interface quietly imply there was only ever one. */
	{
		unsigned i, enabled = 0;

		for (i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++)
			if (info.pmodes[i].enabled)
				enabled++;

		kprintf("virtio-gpu: the host offers %u enabled scanout(s), "
			"the first is %ux%u, and this driver uses that one\n",
			enabled, g->preferred_w, g->preferred_h);
	}
}

/* --- attaching ------------------------------------------------------------- */

bool virtio_gpu_attach(const struct virtio_device *probed)
{
	struct virtio_gpu *g;
	struct display *d;
	u16 qsize;
	u64 budget, free_bytes;

	if (probed->device_id != VIRTIO_ID_GPU)
		return false;

	if (device_count >= VIRTIO_GPU_MAX)
		return false;

	g = &devices[device_count];
	kmemset(g, 0, sizeof(*g));
	g->dev = *probed;

	/* No feature is asked for beyond the 1.0 baseline.
	 *
	 * VIRTIO_GPU_F_VIRGL is 3D and needs a renderer this kernel has no use
	 * for; VIRTIO_GPU_F_EDID would let the host hand over a monitor's real
	 * capability block, which is worth having the day a display can be
	 * asked for its modes and is dead weight until then. Asking for a
	 * feature nothing uses is a way to be refused by a device that would
	 * otherwise have worked. */
	if (!virtio_begin(&g->dev, 0)) {
		kputs("virtio-gpu: the device would not negotiate\n");
		return false;
	}

	qsize = g->dev.t->queue_size(&g->dev, GPU_CONTROLQ);

	if (!qsize) {
		kputs("virtio-gpu: no control queue, so there is no way to "
		      "tell it anything\n");
		virtio_give_up(&g->dev);
		return false;
	}

	/* Small on purpose. Every command this driver sends is synchronous, so
	 * at most two descriptors are ever in flight; a ring of the device's
	 * maximum would be pages of memory to hold one outstanding request. */
	if (qsize > 16)
		qsize = 16;

	if (!virtqueue_init(&g->q, qsize)) {
		kputs("virtio-gpu: could not allocate the control queue\n");
		virtio_give_up(&g->dev);
		return false;
	}

	if (!g->dev.t->setup_queue(&g->dev, GPU_CONTROLQ, &g->q)) {
		kputs("virtio-gpu: the device would not take its queue\n");
		virtqueue_free(&g->q);
		virtio_give_up(&g->dev);
		return false;
	}

	g->scratch_phys = pmm_alloc_page();

	if (!g->scratch_phys) {
		kputs("virtio-gpu: no page for its command buffer\n");
		virtqueue_free(&g->q);
		virtio_give_up(&g->dev);
		return false;
	}

	g->scratch = phys_to_virt(g->scratch_phys);
	kmemset(g->scratch, 0, PAGE_SIZE);

	virtio_ready(&g->dev);

	read_display_info(g);

	/* What this driver will spend on pixels. See GPU_BUDGET_MAX for why
	 * this is a policy and not a measurement. */
	free_bytes = (u64)pmm_free_page_count() * PAGE_SIZE;
	budget = free_bytes / 4;

	if (budget > GPU_BUDGET_MAX)
		budget = GPU_BUDGET_MAX;

	d = display_register("virtio-gpu", &gpu_ops, g, 0, budget);

	if (!d) {
		kputs("virtio-gpu: no room in the display table\n");
		virtqueue_free(&g->q);
		pmm_free_page(g->scratch_phys);
		virtio_give_up(&g->dev);
		return false;
	}

	g->disp = d;
	device_count++;

	/* Declared with no ops, which is the honest state: this holds a host
	 * resource and a scanout binding, and nothing here can rebuild them
	 * after a suspend. The same thing the Bochs driver says about itself. */
	suspend_declare("virtio-gpu", 0);

	kprintf("virtio-gpu: control queue of %u, up to %u MB of pixels from "
		"main memory\n", qsize, (unsigned)(budget >> 20));

	return true;
}

unsigned virtio_gpu_count(void)
{
	return device_count;
}

void virtio_gpu_print_summary(void)
{
	unsigned i;

	if (!device_count)
		return;

	for (i = 0; i < device_count; i++) {
		struct virtio_gpu *g = &devices[i];

		kprintf("  virtio-gpu   : %llu transfer(s), %llu flush(es), "
			"%llu refused, %llu timed out\n",
			(unsigned long long)g->transfers,
			(unsigned long long)g->flushes,
			(unsigned long long)g->refused,
			(unsigned long long)g->timeouts);

		if (g->preferred_w)
			kprintf("               : the host's screen is %ux%u\n",
				g->preferred_w, g->preferred_h);
	}
}

/* --- the self-test ---------------------------------------------------------
 *
 * The thing worth testing here is the one the Bochs adapter cannot fail: that
 * a present actually reaches the host, and that a refused one is reported as
 * refused rather than counted as a success.
 */
bool virtio_gpu_self_test(void)
{
	struct virtio_gpu *g;
	bool ok = true;
	u64 flushes_before, refused_before;

	/* No such device is a normal answer. Most of the matrix has none, and a
	 * test that failed here would be failing the machine for what it is --
	 * the same rule display_self_test follows, and the reason KF-187 is
	 * cited in it. */
	if (!device_count) {
		kputs("virtio-gpu: no such device on this machine\n");
		return true;
	}

	g = &devices[0];

	if (!g->fb_phys) {
		kputs("virtio-gpu: the device is here and has no mode, so "
		      "there is nothing to present\n");
		return true;
	}

	flushes_before = g->flushes;
	refused_before = g->refused;

	/* 1. A present of the whole screen reaches the host and is counted.
	 *
	 *    Counted rather than merely returning true, because a flush
	 *    implemented as `return true` passes any test that only asks what
	 *    it returned -- which is the shape of KF-141 and KF-187 both. */
	if (!display_flush(0, 0, g->disp->mode.width, g->disp->mode.height)) {
		kputs("virtio-gpu: presenting the whole screen was refused\n");
		ok = false;
	} else if (g->flushes != flushes_before + 1) {
		kprintf("virtio-gpu: a present reported success and the device "
			"count went from %llu to %llu\n",
			(unsigned long long)flushes_before,
			(unsigned long long)g->flushes);
		ok = false;
	}

	/* 2. A rectangle entirely off the screen is not sent to the host at
	 *    all. It must not be refused either -- a console that scrolls past
	 *    the bottom row would then stop updating the screen. */
	{
		u64 was = g->transfers;

		if (!display_flush(g->disp->mode.width + 16, 0, 8, 8)) {
			kputs("virtio-gpu: a rectangle past the right edge was "
			      "reported as a failure rather than ignored\n");
			ok = false;
		}

		if (g->transfers != was) {
			kputs("virtio-gpu: a rectangle past the right edge was "
			      "sent to the host anyway\n");
			ok = false;
		}
	}

	/* 3. A rectangle that runs off the edge is clipped and still presented,
	 *    rather than being sent whole for the host to reject. */
	{
		u64 was_refused = g->refused;

		if (!display_flush(g->disp->mode.width - 4, 0, 64, 4)) {
			kputs("virtio-gpu: a rectangle overlapping the right "
			      "edge was refused rather than clipped\n");
			ok = false;
		}

		if (g->refused != was_refused) {
			kputs("virtio-gpu: a rectangle overlapping the right "
			      "edge reached the host unclipped and was "
			      "refused\n");
			ok = false;
		}
	}

	kprintf("virtio-gpu: %llu present(s) reached the host, %llu refused\n",
		(unsigned long long)(g->flushes - flushes_before),
		(unsigned long long)(g->refused - refused_before));

	return ok;
}
