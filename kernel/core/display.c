/* The display layer, and the one device it drives.
 *
 * See display.h for why this is portable and why it runs after the bus walk.
 *
 * --- The device -------------------------------------------------------------
 *
 * `1234:1111` is the display adapter QEMU and Bochs present, and it is on every
 * x86 machine in the verification matrix whether or not that machine has any
 * firmware. It is programmed through the **DISPI** registers: sixteen-bit words
 * at a fixed offset inside one of its BARs, one for width, one for height, one
 * for depth, one to turn the result on.
 *
 * There are two ways to reach those registers and only one of them is allowed
 * here. The historical way is the I/O port pair 0x01CE/0x01CF, which is two x86
 * instructions and would put this file in `arch/x86_64/`. The other is a memory
 * window the device exposes through a BAR, which is a load and a store like any
 * other. `make check-portable` enforces the difference; this driver takes the
 * second and compiles for aarch64 untouched, where the same adapter appears
 * under emulation.
 *
 * --- What it does not do ----------------------------------------------------
 *
 * It does not choose a mode for anybody, accelerate anything, or own the
 * screen. It answers "make the glass this many pixels and tell me where they
 * are", which is the whole of what a framebuffer console and, later, a
 * compositor need from it.
 */
#include <recon/kernel/suspend.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/amd_display.h>
#include <recon/kernel/user.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/console.h>
#include <recon/kernel/display.h>
#include <recon/kernel/fbcon.h>
#include <recon/kernel/intel_display.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/vm.h>

/* --- the DISPI register block ---------------------------------------------
 *
 * Sixteen-bit registers, one every two bytes, starting at 0x500 into the
 * device's MMIO window. The numbering is the Bochs interface's own.
 */
#define DISPI_MMIO_OFFSET	0x500

#define DISPI_ID		0x0
#define DISPI_XRES		0x1
#define DISPI_YRES		0x2
#define DISPI_BPP		0x3
#define DISPI_ENABLE		0x4
#define DISPI_VIRT_WIDTH	0x6
#define DISPI_VIRT_HEIGHT	0x7
#define DISPI_X_OFFSET		0x8
#define DISPI_Y_OFFSET		0x9

/* The identity register answers 0xB0C0 plus the interface revision. Anything
 * else is a device that happens to share a PCI id and does not speak this, and
 * writing mode registers into it would be writing into whatever it does have
 * there. */
#define DISPI_ID_BASE		0xB0C0
#define DISPI_ID_MASK		0xFFF0

#define DISPI_DISABLED		0x00
#define DISPI_ENABLED		0x01
#define DISPI_LFB_ENABLED	0x40

/* The sizes this will ask for, largest first.
 *
 * **The range is the point.** A panel may be 1024x600 on something small, or
 * 1080x1280 held in portrait, or 7680x4320 on a desk; all of them are screens
 * somebody will want ReconOS on, and a driver that knows one size is a driver
 * that works on one machine. The first entry that fits in the adapter's own
 * memory is the one taken, so the same code gives 8K to hardware that can hold
 * it and 1024x600 to hardware that cannot.
 *
 * 1280x800 sits in the middle on purpose: it is what the UEFI paths already
 * come up at, so a machine that gains its screen here and a machine that was
 * handed one by firmware land in the same shape wherever memory allows it.
 */
static const struct { u32 w, h; } MODE_LADDER[] = {
	{ 7680, 4320 },		/* 8K UHD           126 MB */
	{ 5120, 2880 },		/* 5K               56 MB  */
	{ 3840, 2160 },		/* 4K UHD           31 MB  */
	{ 2560, 1440 },		/* QHD              14 MB  */
	{ 1920, 1080 },		/* FHD              7.9 MB */
	{ 1280,  800 },		/* what UEFI gives  3.9 MB */
	{ 1024,  768 },		/*                  3.0 MB */
	{  800,  600 },		/*                  1.8 MB */
	{  640,  480 },		/* the last resort  1.2 MB */
};

/* The DISPI registers are sixteen bits each. A size past that does not fail --
 * it **wraps**, and a request for 70000 pixels becomes a mode of 4464 that the
 * hardware accepts and nobody asked for. Refused here instead, because a
 * silently truncated mode is indistinguishable from a granted one. */
#define DISPI_MAX_DIMENSION	0xFFFFu

/* Below this there is no room for even one character cell, and a console with
 * no rows is not a screen anybody gains anything from. */
#define MIN_DIMENSION		64u

/* Four bytes a pixel, and nothing here handles anything else. The DISPI
 * interface can be told 8, 15, 16, 24 or 32; the console and everything above
 * it assume four-byte pixels, so asking for a depth this kernel cannot draw
 * into would be asking for a screen it cannot use. */
#define BPP			32

/* **Four, because two is what this project's own desktop needs.**
 *
 * That machine has a Radeon RX 6600 on the bus *and* Raphael graphics in the
 * processor package, both answering PCI class 030000 -- so a table of two is
 * exactly full before anything else is plugged in, and a virtual machine on it
 * with a virtio-gpu would be refused a slot by a kernel that had found every
 * adapter correctly. Sized from a machine rather than from a guess about how
 * many screens a computer has. */
#define DISPLAY_MAX		4

static struct display displays[DISPLAY_MAX];
static unsigned display_count;
static struct display *primary;

/* How many times a mode was set, and how many times one was refused. Counted
 * because a driver that quietly stops working looks exactly like a machine
 * nobody asked to change mode.
 *
 * **Counted here rather than in the driver, and that is a correction.** They
 * were incremented inside `bochs_set_mode`, which is the only place a mode
 * could be refused for as long as there was one backend. The self-test below
 * asks whether an oversized mode was *counted* as refused -- so on a machine
 * whose display is driven by anything else, every refusal was invisible and
 * that check failed on a kernel behaving perfectly (GX-004).
 *
 * A counter that only one implementation of an interface maintains is a
 * counter about that implementation, however much it looks like one about the
 * interface. */
static unsigned modes_set;
static unsigned modes_refused;

struct bochs {
	volatile u8 *regs;	/* the MMIO window, already mapped */
};

/* One per possible display, but reached through `d->ops_private` rather than
 * by indexing this with the display's position in the table.
 *
 * **That indexing is what this comment is for.** It read `&adapters[d -
 * displays]`, which is correct exactly while every display in the table is a
 * Bochs adapter -- and a second backend makes it address another driver's
 * device (GX-002). The storage is still static, because a driver allocating at
 * attach time on a path that runs before the heap is a different problem; what
 * changed is how it is found. */
static struct bochs adapters[DISPLAY_MAX];

/* --- reaching the registers ------------------------------------------------ */

static u16 dispi_read(struct bochs *b, unsigned index)
{
	volatile u16 *r = (volatile u16 *)(b->regs + DISPI_MMIO_OFFSET +
					   index * 2);

	return *r;
}

static void dispi_write(struct bochs *b, unsigned index, u16 value)
{
	volatile u16 *r = (volatile u16 *)(b->regs + DISPI_MMIO_OFFSET +
					   index * 2);

	*r = value;
}

/* --- setting a mode -------------------------------------------------------- */

static bool bochs_set_mode(struct display *d, u32 width, u32 height)
{
	struct bochs *b = d->ops_private;
	u64 needed;

	if (width < MIN_DIMENSION || height < MIN_DIMENSION) {
		kprintf("display: %ux%u is too small to put anything on\n",
			width, height);
		return false;
	}

	/* **Refused, not truncated.** These registers are sixteen bits; the
	 * cast below would turn 70000 into 4464 and the adapter would accept
	 * it, so the caller would be told yes and get a mode nobody chose. */
	if (width > DISPI_MAX_DIMENSION || height > DISPI_MAX_DIMENSION) {
		kprintf("display: %ux%u is past what a sixteen-bit mode "
			"register can hold, and would wrap rather than fail\n",
			width, height);
		return false;
	}

	/* The pixels have to fit in the memory the device actually has. A mode
	 * larger than the BAR is not refused by the hardware -- it is accepted,
	 * and then every row past the end is a write into nothing. */
	needed = (u64)width * height * (BPP / 8);
	if (needed > d->fb_size) {
		kprintf("display: %ux%u needs %u MB and this adapter has %u\n",
			width, height,
			(unsigned)(needed >> 20), (unsigned)(d->fb_size >> 20));
		return false;
	}

	/* Disabled while being reprogrammed. The registers are latched when it
	 * is switched on, and writing width while it is live is a screen made
	 * of half of one mode and half of another. */
	dispi_write(b, DISPI_ENABLE, DISPI_DISABLED);

	dispi_write(b, DISPI_XRES, (u16)width);
	dispi_write(b, DISPI_YRES, (u16)height);
	dispi_write(b, DISPI_BPP, BPP);
	dispi_write(b, DISPI_VIRT_WIDTH, (u16)width);
	dispi_write(b, DISPI_VIRT_HEIGHT, (u16)height);
	dispi_write(b, DISPI_X_OFFSET, 0);
	dispi_write(b, DISPI_Y_OFFSET, 0);

	dispi_write(b, DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);

	/* **Read back, and believe the readback rather than the request.**
	 * The interface clamps a size it cannot do rather than refusing it, so
	 * a driver that records what it asked for describes a screen that does
	 * not exist -- and every row after the first lands at the wrong offset.
	 * This is the same shape as KF-141: enough to notice is not enough to
	 * report. */
	d->mode.width  = dispi_read(b, DISPI_XRES);
	d->mode.height = dispi_read(b, DISPI_YRES);
	d->mode.pitch  = (u32)dispi_read(b, DISPI_VIRT_WIDTH) * (BPP / 8);
	d->mode.base   = d->fb_base;
	d->mode.size   = d->fb_size;

	/* 32bpp on this interface is one byte each of blue, green, red and an
	 * unused fourth -- which is what FB_FORMAT_BGRA already means to the
	 * console. */
	d->mode.format = FB_FORMAT_BGRA;

	if (!d->mode.width || !d->mode.height ||
	    d->mode.pitch < d->mode.width * 4) {
		kprintf("display: asked for %ux%u and the adapter came back "
			"with %ux%u, pitch %u -- not usable\n",
			width, height, d->mode.width, d->mode.height,
			d->mode.pitch);
		dispi_write(b, DISPI_ENABLE, DISPI_DISABLED);
		return false;
	}

	if (d->mode.width != width || d->mode.height != height)
		kprintf("display: asked for %ux%u, got %ux%u\n",
			width, height, d->mode.width, d->mode.height);

	return true;
}

/* No `flush`. The framebuffer is device memory being scanned out continuously,
 * so a store is a pixel appearing and there is no third step to ask for. A null
 * pointer is how that is said -- see display_ops. */
static const struct display_ops bochs_ops = {
	.set_mode = bochs_set_mode,
};

/* --- the table ------------------------------------------------------------- */

struct display *display_register(const char *name,
				 const struct display_ops *ops, void *priv,
				 paddr_t fb_base, u64 fb_size)
{
	struct display *d;

	if (display_count >= DISPLAY_MAX)
		return NULL;

	d = &displays[display_count];
	kmemset(d, 0, sizeof(*d));

	d->name        = name;
	d->ops         = ops;
	d->ops_private = priv;
	d->fb_base     = fb_base;
	d->fb_size     = fb_size;
	d->present     = true;

	display_count++;

	if (!primary)
		primary = d;

	return d;
}

/* --- attaching -------------------------------------------------------------
 *
 * Matched on the PCI identity rather than on the class, because class 3 is
 * every display adapter ever made and this driver speaks to one of them. A
 * device that says it is a display and is not this one is left alone, which is
 * the right answer: the machine still has no framebuffer and says so, rather
 * than having registers written into it on the strength of a class code.
 */
bool display_attach(const struct pci_device *d)
{
	struct display *disp;
	struct bochs *b;
	u16 id;
	unsigned slot;

	if (d->vendor != 0x1234 || d->device != 0x1111)
		return false;

	if (display_count >= DISPLAY_MAX)
		return false;

	/* BAR 0 is the pixels and BAR 2 is the register window. Both must be
	 * memory: an adapter presenting either through I/O ports is one this
	 * file cannot reach without becoming machine-specific. */
	if (!d->bar[0] || d->bar_is_io[0] || !d->bar_size[0])
		return false;

	if (!d->bar[2] || d->bar_is_io[2] || !d->bar_size[2]) {
		kputs("display: the adapter has no MMIO register window, so "
		      "its mode can only be set through I/O ports -- which "
		      "would make this driver machine-specific\n");
		return false;
	}

	/* The private block is claimed before the display is registered,
	 * because everything below can still fail and a half-registered display
	 * is one `display_primary` may hand to the console. */
	slot = display_count;
	b = &adapters[slot];
	kmemset(b, 0, sizeof(*b));

	{
		paddr_t first = PAGE_ALIGN_DOWN((paddr_t)d->bar[2]);
		u64 span = PAGE_ALIGN_UP(((paddr_t)d->bar[2] - first) +
					 d->bar_size[2]);
		vaddr_t va = (vaddr_t)(uintptr_t)phys_to_virt(first);

		if (!vm_lookup(va) &&
		    !vm_map(va, first, span,
			    VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL)) {
			kputs("display: could not map the adapter's "
			      "registers\n");
			return false;
		}

		b->regs = phys_to_virt((paddr_t)d->bar[2]);
	}

	/* **And the pixels, which are the part that is easy to forget.**
	 *
	 * `vm_init` maps the framebuffer the handoff carried, because it knows
	 * about that one. This adapter's is a BAR, above RAM, and nothing has
	 * mapped it -- so `phys_to_virt` on it yields a direct-map address that
	 * is not backed by anything. The first version of this driver set a
	 * mode, handed the address to the console, and the console drew fifty
	 * rows of characters into nowhere.
	 *
	 * Device memory, like the registers: a pixel that sits in a cache line
	 * is a pixel that does not appear. */
	{
		paddr_t first = PAGE_ALIGN_DOWN((paddr_t)d->bar[0]);
		u64 span = PAGE_ALIGN_UP(((paddr_t)d->bar[0] - first) +
					 d->bar_size[0]);
		vaddr_t va = (vaddr_t)(uintptr_t)phys_to_virt(first);

		if (!vm_lookup(va) &&
		    !vm_map(va, first, span,
			    VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL)) {
			kprintf("display: could not map %u MB of pixels at "
				"%p\n", (unsigned)(d->bar_size[0] >> 20),
				(void *)(uintptr_t)d->bar[0]);
			return false;
		}
	}

	/* Ask it who it is before writing anything into it. */
	id = dispi_read(b, DISPI_ID);
	if ((id & DISPI_ID_MASK) != DISPI_ID_BASE) {
		kprintf("display: the adapter answered %04x where a DISPI "
			"interface answers B0Cx -- not writing mode registers "
			"into a device that does not have them\n", id);
		return false;
	}

	disp = display_register("bochs-display", &bochs_ops, b,
				(paddr_t)d->bar[0], d->bar_size[0]);

	if (!disp) {
		kputs("display: no room in the display table\n");
		return false;
	}

	/* Declared to the suspend layer with no ops, which is the honest
	 * state: this holds hardware state and nothing here can bring it
	 * back yet. Recorded rather than remembered, so the list of what
	 * would not survive a suspend is generated from what is actually in
	 * the machine. */
	suspend_declare(disp->name, 0);

	kprintf("display: %s, DISPI revision %u, %u MB of pixels at %p\n",
		disp->name, (unsigned)(id & 0x0F),
		(unsigned)(disp->fb_size >> 20), (void *)(uintptr_t)disp->fb_base);

	return true;
}

struct display *display_primary(void)
{
	return primary;
}

bool display_set_mode(u32 width, u32 height)
{
	if (!primary) {
		kputs("display: no adapter, so there is no mode to set\n");
		return false;
	}

	/* **A mode that cannot be asked for was refused, and is counted as
	 * refused.**
	 *
	 * This returned false without touching the counter, which is GX-004
	 * arriving from the other direction: `modes_refused` is the number of
	 * times a mode was asked for and not established, and "there is no way
	 * to ask" is one of those. The self-test checks that an oversized mode
	 * is *counted* as refused, so a display that cannot be set failed it on
	 * a kernel behaving perfectly (GX-006).
	 *
	 * Said once rather than on every attempt. Nine identical lines while a
	 * mode ladder works its way down tell nobody anything the first did
	 * not. */
	if (!primary->ops || !primary->ops->set_mode) {
		static bool said;

		if (!said) {
			kprintf("display: %s cannot be told what mode to be in, "
				"so the mode it has is the one it keeps\n",
				primary->name);
			said = true;
		}

		modes_refused++;
		return false;
	}

	/* **Counted here, once, for every backend.** See the counters' own
	 * comment: they used to be maintained inside the Bochs driver, which
	 * made "how many modes were refused" a question only that driver could
	 * answer -- and the self-test asks it of the interface (GX-004). */
	if (!primary->ops->set_mode(primary, width, height)) {
		modes_refused++;
		return false;
	}

	modes_set++;
	return true;
}

/* How many rectangles were presented, and how many presents were refused.
 * Separate from the mode counters above because they answer a different
 * question: a display can be in a perfectly good mode and have stopped putting
 * anything on it. */
static u64 flushes_done;
static u64 flushes_refused;

/* How many times the address-space teardown was about to hand a page of the
 * live framebuffer back to the page allocator.
 *
 * **Not a fault counter -- a proof that the guard below is reached.** A check
 * that never fires and a check that was never needed look identical in a
 * summary that prints neither, and this one is needed on exactly the machines
 * whoever wrote it is least likely to be running. */
static u64 fb_pages_kept;

/* Whether this physical page is part of the screen right now.
 *
 * **The address space cannot answer this and must not guess.** It tears down by
 * walking page tables, so all it has is a physical address; the rule it used
 * instead was "the allocator never handed this out, so it is a device" -- true
 * for an adapter whose framebuffer is a PCI aperture, and false for one whose
 * framebuffer is main memory (GX-001). On virtio-gpu the first program to map
 * /dev/fb0 and exit hands the live screen back to the allocator, which gives it
 * out as ordinary memory to whatever asks next.
 *
 * Asked of the display because the display is the only thing that knows which
 * pages are pixels *now*. A range recorded when the mapping was made would go
 * stale the next time the mode changed; this cannot, because it is read from
 * the mode in force.
 *
 * **It is narrow on purpose.** /dev/fb0 is the only file in this kernel with a
 * `map` operation, so the framebuffer is the only borrowed range that can reach
 * a user page table today. The general rule -- that pages obtained through
 * `file_ops.map` belong to the file and never to the address space -- wants
 * somewhere better than a display-shaped question, and it is raised in
 * docs/SIGNALS.md rather than settled here by the session that happened to
 * trip over it.
 */
bool display_owns_page(paddr_t pa)
{
	const struct framebuffer *fb;

	if (!primary || !primary->mode.base)
		return false;

	fb = &primary->mode;

	/* `pitch * height` rather than `size`, the same reckoning /dev/fb0
	 * makes: `size` is sometimes the whole aperture and sometimes rounded
	 * up, and the rows are the part that exists. */
	if (pa < fb->base || pa >= fb->base + (u64)fb->pitch * fb->height)
		return false;

	fb_pages_kept++;
	return true;
}

/* Print a device's base address registers, as they were actually found.
 *
 * **Because the shape of them is a fact about a family, not about displays.**
 * The Intel backend refused any adapter whose first BAR was smaller than
 * sixteen megabytes, which is Gen9's register window. AMD's register window is
 * 512 KB and is not the first BAR at all -- the first is a 256 MB aperture onto
 * video memory, with a 2 MB doorbell between them, read off this project's own
 * desktop. Both rules are correct about one family and wrong about the other
 * (GX-007).
 *
 * So a driver that reads no registers should not be refusing hardware on the
 * layout of registers it does not touch. It prints what it found instead, and
 * the first boot on a real machine becomes a measurement rather than a dead
 * end -- which is the only way an assumption made from a specification ever
 * gets corrected.
 */
void display_print_bars(const struct pci_device *d)
{
	unsigned i;

	for (i = 0; i < 6; i++) {
		if (!d->bar_size[i])
			continue;

		kprintf("               bar%u  %-6s %10llu bytes at %p\n",
			i, d->bar_is_io[i] ? "i/o" : "memory",
			(unsigned long long)d->bar_size[i],
			(void *)(uintptr_t)d->bar[i]);
	}
}

bool display_needs_flush(void)
{
	return primary && primary->ops && primary->ops->flush;
}

bool display_flush(u32 x, u32 y, u32 w, u32 h)
{
	/* **Both of these are "yes", not "no", and the distinction is the
	 * whole reason this wrapper exists.**
	 *
	 * A machine with no display has shown everything it was going to show.
	 * A machine whose framebuffer is scanned out continuously showed it at
	 * the moment of the store. Neither is a failure, and returning false
	 * for either would make every caller either check for two conditions it
	 * cannot do anything about, or -- far likelier -- ignore the return
	 * value entirely, which is how a real refusal would then go unnoticed.
	 *
	 * False is kept for the one case worth acting on: a display that was
	 * asked to present and would not. */
	if (!display_needs_flush())
		return true;

	if (!w || !h)
		return true;

	if (!primary->ops->flush(primary, x, y, w, h)) {
		flushes_refused++;
		return false;
	}

	flushes_done++;
	return true;
}

/* --- bringing it up --------------------------------------------------------- */

void display_init(void)
{
	const struct boot_info *info = boot_info();
	unsigned i;

	/* Every display driver is offered every function, and each answers for
	 * its own devices. The order carries no meaning: `display_attach` takes
	 * the Bochs adapter and nothing else, `intel_display_attach` takes an
	 * Intel display engine it has an entry for and nothing else, and both
	 * decline quietly.
	 *
	 * virtio-gpu is not here: it is found during the bus walk, because it
	 * has to be probed as a virtio device before anyone can tell it is a
	 * display at all. That is a difference between transports rather than
	 * between displays. */
	for (i = 0; i < pci_device_count(); i++) {
		const struct pci_device *pd = pci_device_at(i);

		if (display_attach(pd))
			continue;

		if (intel_display_attach(pd))
			continue;

		amd_display_attach(pd);
	}

	if (!primary)
		return;

	/* **A machine that already has a screen keeps it.**
	 *
	 * Firmware set a mode, the loader recorded it, the console has been
	 * drawing into it since before the heap existed. Re-setting it would
	 * flicker, and would risk ending up with nothing in exchange for a
	 * screen that already works. The case this exists for is the other one:
	 * a machine with a display adapter and no framebuffer, which is every
	 * PVH and direct-kernel path in the matrix. */
	if (info->fb.width && info->fb.height && info->fb.base &&
	    info->fb.format != FB_FORMAT_NONE) {
		primary->mode = info->fb;
		return;
	}

	/* **A display that cannot be told anything keeps what it has.**
	 *
	 * The ladder below asks for nine sizes in turn. On a backend with no
	 * `set_mode` that is nine refusals and nine lines, ending in a summary
	 * blaming the adapter's memory -- "would not take any of the 9 sizes
	 * this driver knows -- it has 256 MB" -- for a display that was never
	 * asked anything (GX-006). The memory was not the reason and the
	 * adapter did not refuse.
	 *
	 * This is the shape a display inherited from firmware has: the mode is
	 * whatever the firmware left and there is no way to change it. It is
	 * what an Intel adapter will be here before there is modesetting for
	 * it, and it is a third shape this interface had never been shown. */
	if (!primary->ops || !primary->ops->set_mode) {
		if (primary->mode.width && primary->mode.height) {
			kprintf("display: %s cannot be told a mode, and is "
				"already in %ux%u\n", primary->name,
				primary->mode.width, primary->mode.height);
			goto adopt;
		}

		kprintf("display: %s cannot be told a mode and firmware left "
			"none, so this machine has an adapter and no screen\n",
			primary->name);
		return;
	}

	/* **Ask the screen first, and only guess when it cannot say.**
	 *
	 * The ladder below chooses the largest mode the *adapter* has memory
	 * for, which is the best answer available from hardware that cannot
	 * describe its own panel -- and the wrong one from hardware that can.
	 * A virtio-gpu host reporting a 1280x800 screen was being driven at
	 * 5120x2880 because nothing asked it (GX-005).
	 *
	 * A refused preferred mode falls through to the ladder rather than
	 * leaving the machine dark: the device knowing what it wants and this
	 * driver being unable to give it is a reason to try something else, not
	 * a reason to stop. */
	if (primary->ops && primary->ops->preferred_mode) {
		u32 pw = 0, ph = 0;

		if (primary->ops->preferred_mode(primary, &pw, &ph) &&
		    display_set_mode(pw, ph)) {
			kprintf("display: %s reports its screen is %ux%u, and that "
				"is the mode it is in\n", primary->name, pw, ph);
			goto adopt;
		}

		if (pw && ph)
			kprintf("display: %s reports its screen is %ux%u and would "
				"not take that mode, so a size is being chosen "
				"instead\n", primary->name, pw, ph);
	}

	/* The largest size this adapter's memory can actually hold, rather than
	 * one size that happens to work on the machine it was written on. An
	 * adapter with 16 MB lands on 1920x1080; one given 256 MB lands on 8K;
	 * one with 2 MB lands on 800x600 -- same code, and nothing above here
	 * has to know which. */
	{
		unsigned n;
		bool set = false;

		for (n = 0; n < sizeof(MODE_LADDER) / sizeof(MODE_LADDER[0]);
		     n++) {
			u64 needed = (u64)MODE_LADDER[n].w *
				     MODE_LADDER[n].h * (BPP / 8);

			if (needed > primary->fb_size)
				continue;

			if (display_set_mode(MODE_LADDER[n].w,
					     MODE_LADDER[n].h)) {
				set = true;
				break;
			}
		}

		/* Every size on the ladder was either too big for the memory
		 * or refused by the hardware. Said outright: an adapter that
		 * is present and cannot be put into any mode at all is a
		 * different fact from no adapter, and wants different things
		 * done about it. */
		if (!set) {
			kprintf("display: %s would not take any of the %u "
				"sizes this driver knows -- it has %u MB\n",
				primary->name,
				(unsigned)(sizeof(MODE_LADDER) /
					   sizeof(MODE_LADDER[0])),
				(unsigned)(primary->fb_size >> 20));
			return;
		}
	}

adopt:
	/* The console has been serial-only until this line.
	 *
	 * Said either way. A mode that was set and then not taken up leaves a
	 * machine with a configured adapter and a blank screen, and the first
	 * version of this printed only on success -- so the interesting case
	 * was the silent one. That is the shape this kernel keeps finding and
	 * it is not going to be introduced by the file that says so. */
	if (fbcon_adopt(&primary->mode)) {
		kprintf("display: the console has a screen now, %ux%u\n",
			primary->mode.width, primary->mode.height);

		/* Said again, because the first time it was said there was no
		 * screen to describe. `fbcon_describe` runs at init, which is
		 * before the bus has been walked -- so on every path that
		 * gains its display here, the only description printed was
		 * "none". */
		fbcon_describe();
	}
	else
		kprintf("display: set %ux%u, pitch %u, and the console would "
			"not take it -- the screen stays dark and everything "
			"keeps going to the serial port\n",
			primary->mode.width, primary->mode.height,
			primary->mode.pitch);
}

void display_print_summary(void)
{
	unsigned n;

	if (!primary) {
		kputs("  display      : none -- no adapter this kernel can "
		      "drive\n");
		return;
	}

	/* **An adapter with no mode is not an adapter in a mode of 0x0.**
	 *
	 * This printed "0x0, pitch 0, RGBA" for a display that had been found
	 * and never configured, which reads like a mode somebody chose -- and
	 * RGBA is not even a guess, it is what the enum happens to be at zero
	 * (GX-006). A display in that state is normal on a machine whose
	 * firmware left no framebuffer and whose adapter cannot be told one. */
	if (!primary->mode.width || !primary->mode.height) {
		kprintf("  display      : %s, found and not in any mode\n",
			primary->name);
		return;
	}

	kprintf("  display      : %s, %ux%u, pitch %u, %s\n",
		primary->name, primary->mode.width, primary->mode.height,
		primary->mode.pitch,
		primary->mode.format == FB_FORMAT_BGRA ? "BGRA" : "RGBA");

	kprintf("               : %u mode(s) set, %u refused\n",
		modes_set, modes_refused);

	/* Said either way, and the "scans itself out" line is the one worth
	 * having: a machine that presents nothing because it never needs to and
	 * a machine that presents nothing because its driver forgot look
	 * identical in a summary that only prints a count. */
	if (display_needs_flush())
		kprintf("               : %llu present(s), %llu refused\n",
			(unsigned long long)flushes_done,
			(unsigned long long)flushes_refused);
	else
		kputs("               : scans itself out, so nothing is "
		      "presented\n");

	/* Said only when it happened, because zero is the ordinary answer on a
	 * machine where no program ever mapped the screen -- and a line reading
	 * "0 kept" on every boot is a line nobody reads on the one boot it
	 * matters. */
	if (fb_pages_kept)
		kprintf("               : %llu page(s) of screen kept out of the "
			"allocator when a program that had mapped it exited\n",
			(unsigned long long)fb_pages_kept);

	/* **Every display in the table, not only the one being drawn on.**
	 *
	 * This reported `primary` and stopped, which is a complete description
	 * of a machine with one adapter and was never anything else while there
	 * was one backend. On a machine with two it names one and is silent
	 * about the other -- and the suspend summary two lines below has been
	 * listing both the whole time, so the boot report contradicted itself
	 * for anybody reading it closely (GX-009).
	 *
	 * It is not a hypothetical machine. QEMU with `-device virtio-gpu-pci`
	 * and no `-vga none` is one, and so is the desktop this was written on:
	 * a Radeon RX 6600 and Raphael graphics, both class 030000. */
	for (n = 0; n < display_count; n++) {
		struct display *d = &displays[n];

		if (d == primary)
			continue;

		if (d->mode.width && d->mode.height)
			kprintf("  also         : %s, %ux%u, not the one being "
				"drawn on\n", d->name, d->mode.width,
				d->mode.height);
		else
			kprintf("  also         : %s, found and not in any mode\n",
				d->name);
	}
}

/* That a program which maps the screen and exits does not give the screen away.
 *
 * **This is the check that a single backend could not have needed.** The
 * address space tears down by walking page tables, so all it has at that moment
 * is a physical address -- and the rule it applied was "the page allocator
 * never handed this out, so it belongs to a device". That is a complete and
 * correct answer for an adapter whose framebuffer is a PCI aperture, which is
 * the only kind this kernel had. It is the wrong answer for one whose
 * framebuffer is `pmm_alloc_pages`, and the wrong answer is to hand the live
 * screen back to be allocated to somebody else (GX-001).
 *
 * --- Why two spaces rather than one measurement --------------------------
 *
 * "Did the framebuffer page get freed" cannot be read off the free-page count
 * directly, because destroying an address space also frees its page tables and
 * that count is not fixed. So two spaces are built and destroyed that differ in
 * exactly one thing: which physical page is mapped at the same address. Their
 * page tables are identical in shape, so that cost cancels, and what is left is
 * the one page.
 *
 * The control half matters as much as the subject: a run where *neither* page
 * comes back would also show a difference of zero, and would mean the test had
 * stopped measuring anything rather than that the guard worked.
 */
static bool release_keeps_the_screen(void)
{
	struct display *d = display_primary();
	struct addrspace *as;
	paddr_t ordinary;
	u64 before, after_fb, after_ordinary;
	const vaddr_t at = USER_BASE;

	if (!d || !d->mode.base || !d->mode.height)
		return true;	/* no screen: nothing to give away */

	/* Only meaningful where the framebuffer is memory the allocator owns.
	 * On an adapter with an aperture the teardown never had a decision to
	 * make, and saying so is better than a test that quietly passes for a
	 * reason unrelated to what it is named after. */
	if (!pmm_owns(d->mode.base)) {
		kputs("display: this framebuffer is an aperture, so the "
		      "teardown has nothing it could give away\n");
		return true;
	}

	/* 1. A space that maps one page of the screen, then goes away. */
	as = addrspace_create();

	if (!as) {
		kputs("display: could not build a space to map the screen "
		      "into\n");
		return false;
	}

	if (!addrspace_map(as, at, d->mode.base, PAGE_SIZE,
			   VM_READ | VM_WRITE | VM_USER)) {
		kputs("display: could not map the screen into a space\n");
		addrspace_release(as);
		return false;
	}

	before = pmm_free_page_count();
	addrspace_release(as);
	after_fb = pmm_free_page_count() - before;

	/* 2. The same thing with an ordinary page, which *must* come back. */
	ordinary = pmm_alloc_page();

	if (!ordinary) {
		kputs("display: no page to compare the screen against\n");
		return false;
	}

	as = addrspace_create();

	if (!as) {
		kputs("display: could not build the second space\n");
		pmm_free_page(ordinary);
		return false;
	}

	if (!addrspace_map(as, at, ordinary, PAGE_SIZE,
			   VM_READ | VM_WRITE | VM_USER)) {
		kputs("display: could not map an ordinary page into a space\n");
		addrspace_release(as);
		pmm_free_page(ordinary);
		return false;
	}

	before = pmm_free_page_count();
	addrspace_release(as);
	after_ordinary = pmm_free_page_count() - before;

	/* The control must have given its page back. If it did not, the two
	 * numbers below would agree for a reason that has nothing to do with
	 * the screen, and this test would report a pass it had not earned. */
	if (after_ordinary != after_fb + 1) {
		kprintf("display: tearing down a space holding the screen gave "
			"back %llu page(s) and one holding an ordinary page "
			"gave back %llu -- the screen is being freed with "
			"it\n",
			(unsigned long long)after_fb,
			(unsigned long long)after_ordinary);
		return false;
	}

	kprintf("display: a space holding the screen gave back %llu page(s) "
		"and an identical one holding ordinary memory gave back %llu, "
		"so the screen stayed out of the allocator\n",
		(unsigned long long)after_fb,
		(unsigned long long)after_ordinary);

	return true;
}

/* --- the self-test ---------------------------------------------------------
 *
 * Three things, and the third is the one worth having.
 */
bool display_self_test(void)
{
	struct display *d = display_primary();
	u32 was_w, was_h;
	bool ok = true;

	/* A machine with no adapter is a normal answer, not a failure. Half
	 * the matrix has no framebuffer and some of it has no adapter either;
	 * a test that failed here would be failing the machine for what it is.
	 * Saying so is what keeps this from being a test that quietly does
	 * nothing (KF-187). */
	if (!d) {
		kputs("display: no adapter on this machine\n");
		return true;
	}

	/* First, because it is the only check here that is about what happens
	 * after a program has finished with the screen rather than about the
	 * modes themselves -- and the sweep below moves the framebuffer seven
	 * times, so running it afterwards would test a screen nothing had
	 * mapped. */
	if (!release_keeps_the_screen())
		ok = false;

	was_w = d->mode.width;
	was_h = d->mode.height;

	/* 1. A mode too large for the adapter's memory must be refused rather
	 *    than accepted and drawn past the end of. */
	{
		unsigned before = modes_refused;

		if (display_set_mode(16384, 16384)) {
			kputs("display: a mode far larger than this adapter's "
			      "memory was accepted\n");
			ok = false;
		} else if (modes_refused == before) {
			kputs("display: an oversized mode was refused and not "
			      "counted as refused\n");
			ok = false;
		}
	}

	/* 2. A mode of nothing is not a mode. */
	if (display_set_mode(0, 0)) {
		kputs("display: a zero-sized mode was accepted\n");
		ok = false;
	}

	/* 2b. A dimension past the sixteen-bit registers must be refused rather
	 *     than wrapped. 70000 becomes 4464 on the way into the hardware,
	 *     and the adapter would accept that happily. */
	if (display_set_mode(70000, 600)) {
		kprintf("display: 70000x600 was accepted, and the adapter is "
			"now in %ux%u\n", d->mode.width, d->mode.height);
		ok = false;
	}

	/* 3. **Every shape, not one size.**
	 *
	 *    A panel is 1024x600 on something small, 1080x1280 held in
	 *    portrait, 3440x1440 across a desk, or 7680x4320 on a wall. Each
	 *    is asked for, and each is checked against what the *hardware*
	 *    reports rather than against what was requested -- a driver that
	 *    records its own request passes any test that asks what it
	 *    recorded, which is no test at all (KF-141).
	 *
	 *    A size this adapter has no memory for is skipped rather than
	 *    failed: it is a fact about the machine, not about the driver. The
	 *    count of what was actually exercised is printed, because a sweep
	 *    that silently skipped everything looks exactly like one that
	 *    passed (KF-187).
	 */
	if (d->ops && d->ops->set_mode) {
		static const struct { u32 w, h; const char *what; } SHAPES[] = {
			{  640,  480, "the smallest thing worth having" },
			{ 1024,  600, "a small panel" },
			{ 1080, 1280, "a phone panel, held in portrait" },
			{ 1920, 1080, "the common one" },
			{ 3440, 1440, "ultra-wide" },
			{ 3840, 2160, "4K" },
			{ 7680, 4320, "8K" },
		};
		unsigned i, tried = 0, skipped = 0;

		for (i = 0; i < sizeof(SHAPES) / sizeof(SHAPES[0]); i++) {
			u64 needed = (u64)SHAPES[i].w * SHAPES[i].h * 4;

			if (needed > d->fb_size) {
				skipped++;
				continue;
			}

			if (!display_set_mode(SHAPES[i].w, SHAPES[i].h)) {
				kprintf("display: %ux%u (%s) was refused and "
					"this adapter has the memory for it\n",
					SHAPES[i].w, SHAPES[i].h,
					SHAPES[i].what);
				ok = false;
				continue;
			}

			tried++;

			if (d->mode.width != SHAPES[i].w ||
			    d->mode.height != SHAPES[i].h) {
				kprintf("display: asked for %ux%u (%s) and the "
					"adapter reports %ux%u\n",
					SHAPES[i].w, SHAPES[i].h,
					SHAPES[i].what,
					d->mode.width, d->mode.height);
				ok = false;
			} else if (d->mode.pitch < d->mode.width * 4) {
				kprintf("display: %ux%u came back with a pitch "
					"of %u, which cannot hold a row\n",
					SHAPES[i].w, SHAPES[i].h,
					d->mode.pitch);
				ok = false;
			}
		}

		kprintf("display: %u shape(s) set and read back, %u skipped "
			"for want of memory on a %u MB adapter\n",
			tried, skipped, (unsigned)(d->fb_size >> 20));

		/* Not one shape exercised is not a pass. */
		if (!tried) {
			kputs("display: no shape could be tested at all\n");
			ok = false;
		}

		/* Put back whatever was there, so a boot that runs this test
		 * leaves the same screen a boot that does not would have --
		 * and the console is pointed at it again, because the mode it
		 * was drawing into is several modes ago by now. */
		if (was_w && was_h && display_set_mode(was_w, was_h))
			fbcon_adopt(&d->mode);
	}

	return ok;
}
