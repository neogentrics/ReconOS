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
#include <recon/kernel/console.h>
#include <recon/kernel/display.h>
#include <recon/kernel/fbcon.h>
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

#define DISPLAY_MAX		2

static struct display displays[DISPLAY_MAX];
static unsigned display_count;
static struct display *primary;

/* How many times a mode was set, and how many times one was refused. Counted
 * because a driver that quietly stops working looks exactly like a machine
 * nobody asked to change mode. */
static unsigned modes_set;
static unsigned modes_refused;

struct bochs {
	volatile u8 *regs;	/* the MMIO window, already mapped */
};

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
	struct bochs *b = &adapters[d - displays];
	u64 needed;

	if (width < MIN_DIMENSION || height < MIN_DIMENSION) {
		kprintf("display: %ux%u is too small to put anything on\n",
			width, height);
		modes_refused++;
		return false;
	}

	/* **Refused, not truncated.** These registers are sixteen bits; the
	 * cast below would turn 70000 into 4464 and the adapter would accept
	 * it, so the caller would be told yes and get a mode nobody chose. */
	if (width > DISPI_MAX_DIMENSION || height > DISPI_MAX_DIMENSION) {
		kprintf("display: %ux%u is past what a sixteen-bit mode "
			"register can hold, and would wrap rather than fail\n",
			width, height);
		modes_refused++;
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
		modes_refused++;
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
		modes_refused++;
		return false;
	}

	if (d->mode.width != width || d->mode.height != height)
		kprintf("display: asked for %ux%u, got %ux%u\n",
			width, height, d->mode.width, d->mode.height);

	modes_set++;
	return true;
}

static const struct display_ops bochs_ops = {
	.set_mode = bochs_set_mode,
};

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

	slot = display_count;
	disp = &displays[slot];
	b = &adapters[slot];
	kmemset(disp, 0, sizeof(*disp));
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

	disp->name    = "bochs-display";
	disp->ops     = &bochs_ops;
	disp->fb_base = (paddr_t)d->bar[0];
	disp->fb_size = d->bar_size[0];
	disp->present = true;

	display_count++;
	if (!primary)
		primary = disp;

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

	if (!primary->ops || !primary->ops->set_mode) {
		kprintf("display: %s cannot be told what mode to be in\n",
			primary->name);
		return false;
	}

	return primary->ops->set_mode(primary, width, height);
}

/* --- bringing it up --------------------------------------------------------- */

void display_init(void)
{
	const struct boot_info *info = boot_info();
	unsigned i;

	for (i = 0; i < pci_device_count(); i++)
		display_attach(pci_device_at(i));

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
	if (!primary) {
		kputs("  display      : none -- no adapter this kernel can "
		      "drive\n");
		return;
	}

	kprintf("  display      : %s, %ux%u, pitch %u, %s\n",
		primary->name, primary->mode.width, primary->mode.height,
		primary->mode.pitch,
		primary->mode.format == FB_FORMAT_BGRA ? "BGRA" : "RGBA");

	kprintf("               : %u mode(s) set, %u refused\n",
		modes_set, modes_refused);
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
