/* Intel integrated graphics, Gen9 -- identification, and the mode firmware set.
 *
 * See intel_display.h for what this drives and, more importantly, what it
 * deliberately does not: there is no modesetting here, and the reason is
 * written down there rather than left to be inferred from its absence.
 *
 * --- Where the device ids come from -----------------------------------------
 *
 * `/usr/share/misc/pci.ids`, the same database `lspci` prints from, read on the
 * machine this was written on rather than recalled. Every name below is that
 * file's name for that id, unedited -- so a line here can be checked against a
 * source outside this tree, which is the only thing that makes a table of
 * magic numbers auditable.
 *
 * **The list is short on purpose.** It is Gen9 and Gen9 LP, which is what the
 * hardware on this project's desks actually is, plus nothing. An Intel display
 * adapter of another generation is declined by not being listed. That is the
 * same rule `core/ext2.c` follows when it refuses EXTENTS, RECOVER and 64BIT
 * by name: a filesystem read through a wrong assumption about its layout is
 * worse than one that will not mount, and a display engine written to through
 * a wrong assumption about its registers is worse than a machine that says it
 * has no driver.
 */
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/display.h>
#include <recon/kernel/intel_display.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/suspend.h>

#define PCI_VENDOR_INTEL	0x8086

/* Class 3, subclass 0: a VGA-compatible display controller. Intel's integrated
 * graphics report this; so does almost every other display adapter ever made,
 * which is why the class is a precondition here and never the decision. */
#define PCI_CLASS_DISPLAY	0x03
#define PCI_SUBCLASS_VGA	0x00

/* Gen9 and Gen9 LP.
 *
 * Gen9 proper is the Skylake display engine and the two refreshes that kept it
 * -- Kaby Lake and Coffee Lake. Gen9 LP is the same engine in the Atom-derived
 * parts, Apollo Lake and Gemini Lake, with fewer pipes and its own power
 * management. They are one family for the purpose of reading a mode out of a
 * running pipe, which is all this driver does, and the distinction is recorded
 * because it stops being cosmetic the moment modesetting is written.
 *
 * 3185 is the one that matters first: **Gemini Lake, UHD Graphics 600, which is
 * what a Celeron N4020 has** -- the Gateway GWTC116-2BL this project tests real
 * hardware on (KF-214).
 */
static const struct intel_model MODELS[] = {
	/* Gen9 LP -- Apollo Lake and Gemini Lake */
	{ 0x3184, "GeminiLake [UHD Graphics 605]",       9, true  },
	{ 0x3185, "GeminiLake [UHD Graphics 600]",       9, true  },
	{ 0x5a84, "HD Graphics 505",                     9, true  },
	{ 0x5a85, "HD Graphics 500",                     9, true  },

	/* Gen9 -- Skylake */
	{ 0x1902, "HD Graphics 510",                     9, false },
	{ 0x1906, "HD Graphics 510",                     9, false },
	{ 0x190b, "HD Graphics 510",                     9, false },
	{ 0x1912, "HD Graphics 530",                     9, false },
	{ 0x1916, "Skylake GT2 [HD Graphics 520]",       9, false },
	{ 0x191b, "HD Graphics 530",                     9, false },
	{ 0x191e, "HD Graphics 515",                     9, false },
	{ 0x1921, "HD Graphics 520",                     9, false },
	{ 0x1923, "HD Graphics 535",                     9, false },

	/* Gen9 -- Coffee Lake, the last of the line */
	{ 0x3e90, "CoffeeLake-S GT1 [UHD Graphics 610]", 9, false },
	{ 0x3e91, "CoffeeLake-S GT2 [UHD Graphics 630]", 9, false },
	{ 0x3e92, "CoffeeLake-S GT2 [UHD Graphics 630]", 9, false },
	{ 0x3e93, "CoffeeLake-S GT1 [UHD Graphics 610]", 9, false },
	{ 0x3e94, "Coffee Lake-S GT2 [UHD Graphics P630]", 9, false },
};

#define MODEL_COUNT (sizeof(MODELS) / sizeof(MODELS[0]))

/* On Gen9 the first base address register is the register file and the
 * graphics translation table together, and it is sixteen megabytes. Checked
 * rather than assumed: a device answering to a known id with a BAR of the wrong
 * size is not the device this table describes, whatever it calls itself. */
#define GEN9_MMIO_BAR		0
#define GEN9_MMIO_SIZE		(16u * 1024 * 1024)

#define INTEL_DISPLAY_MAX	1

static struct intel_display {
	const struct intel_model *model;
	paddr_t mmio;
	u64 mmio_size;
	struct display *disp;
} adapters[INTEL_DISPLAY_MAX];

static unsigned adapter_count;

/* How many Intel display adapters were seen and not driven, and why. Counted
 * because "this kernel has no Intel driver" and "this kernel has one and does
 * not know your chip" are different facts about a machine with a blank screen,
 * and they want different things done about them. */
static unsigned declined_unknown;
static unsigned unexpected_shape;

/* --- recognition ----------------------------------------------------------- */

const struct intel_model *intel_display_identify(u16 vendor, u16 device,
						 u8 class_code, u8 subclass)
{
	unsigned i;

	if (vendor != PCI_VENDOR_INTEL)
		return NULL;

	/* **The class is checked and is not the decision.**
	 *
	 * Intel's vendor id is on the host bridge, the PCH, the SMBus
	 * controller, every root port and both network cards in this
	 * project's machines. Matching a display by vendor alone would claim
	 * all of them. Matching by class alone would claim every graphics card
	 * ever made. The identifier decides; these two only stop an identifier
	 * from being read out of a device that is not a display at all -- an
	 * id is only unique within a vendor's own numbering, and Intel has
	 * reused numbers across device types. */
	if (class_code != PCI_CLASS_DISPLAY || subclass != PCI_SUBCLASS_VGA)
		return NULL;

	for (i = 0; i < MODEL_COUNT; i++)
		if (MODELS[i].device == device)
			return &MODELS[i];

	return NULL;
}

/* --- the operations -------------------------------------------------------- */

/* **Empty, and every omission is a statement.**
 *
 * `set_mode` is null: this driver cannot change the mode, and the display layer
 * has a way to say exactly that -- which it gained by being shown this shape
 * (GX-006). A driver that offered a `set_mode` returning false would say the
 * same thing less clearly and would be counted differently.
 *
 * `flush` is null: the display engine scans the framebuffer out of memory
 * continuously, so a store is a pixel appearing, the same as the Bochs adapter
 * and unlike virtio-gpu. That is the third data point for the null-means-no-
 * such-step idiom, and the first from hardware rather than from an emulator.
 *
 * `preferred_mode` is null *for now*, and this is the one that is missing
 * rather than inapplicable. The panel's size is knowable -- it is in the pipe's
 * source-size register, and its real timings are in the EDID the panel will
 * hand over across AUX. Both need registers read from a machine rather than
 * remembered, and neither can be checked under QEMU. It is listed in
 * docs/SIGNALS.md as the next thing, with what has to be measured first.
 */
static const struct display_ops intel_ops = {
	.set_mode       = 0,
	.flush          = 0,
	.preferred_mode = 0,
};

/* --- attaching ------------------------------------------------------------- */

bool intel_display_attach(const struct pci_device *d)
{
	const struct intel_model *m;
	struct intel_display *a;
	const struct boot_info *info;
	struct display *disp;

	m = intel_display_identify(d->vendor, d->device, d->class_code,
				   d->subclass);

	if (!m) {
		/* An Intel display this kernel does not know is worth one line.
		 * The machine is about to report that it has no display driver,
		 * and the reason is a number somebody can look up. */
		if (d->vendor == PCI_VENDOR_INTEL &&
		    d->class_code == PCI_CLASS_DISPLAY) {
			kprintf("intel-display: %04x:%04x is an Intel display "
				"this kernel has no entry for, so it is left "
				"alone rather than driven on the strength of "
				"the vendor id\n", d->vendor, d->device);
			declined_unknown++;
		}
		return false;
	}

	if (adapter_count >= INTEL_DISPLAY_MAX)
		return false;

	/* **Noted, and no longer a veto.** (GX-007)
	 *
	 * This refused any adapter whose first base address register was
	 * smaller than sixteen megabytes, which is Gen9's register window --
	 * a rule written from a specification, never seen on the hardware, and
	 * guarding nothing, because nothing in this file reads a register.
	 *
	 * Writing the AMD backend is what showed it up. RDNA2 keeps its register
	 * file in a **512 KB fifth BAR behind a 256 MB aperture**, read off this
	 * project's own desktop -- so a rule phrased about "the first BAR,
	 * sixteen megabytes" is true of one family and false of another, and
	 * there was never a reason to believe it would hold for the real Gemini
	 * Lake either.
	 *
	 * The cost of being wrong was the laptop reporting "not the device this
	 * driver knows" while saying nothing about what it actually had: a dead
	 * end that could not be corrected from the evidence it produced. The
	 * layout is printed instead, and the veto belongs with the first
	 * register access -- which is where being wrong about it would finally
	 * matter. */
	if (d->bar_size[GEN9_MMIO_BAR] < GEN9_MMIO_SIZE) {
		kprintf("intel-display: %s has a first register window of "
			"%llu bytes where Gen9 is expected to have 16 MB -- "
			"driven anyway, because nothing here reads a register "
			"and the expectation has never been checked against "
			"this machine\n", m->name,
			(unsigned long long)d->bar_size[GEN9_MMIO_BAR]);
		unexpected_shape++;
	}

	a = &adapters[adapter_count];
	kmemset(a, 0, sizeof(*a));
	a->model     = m;
	a->mmio      = (paddr_t)d->bar[GEN9_MMIO_BAR];
	a->mmio_size = d->bar_size[GEN9_MMIO_BAR];

	/* **The register window is deliberately not mapped.**
	 *
	 * Nothing here reads a register yet, and mapping sixteen megabytes of
	 * device memory that nothing touches is a page table full of a claim
	 * this driver cannot make good on. It is recorded, so the mapping is a
	 * line of code away the day there is something to read -- which is the
	 * day somebody has read those registers on a running machine and can
	 * say what they should contain. */

	info = boot_info();

	/* The framebuffer firmware left, if it left one.
	 *
	 * On the UEFI path this is how the panel is already lit: GOP set a
	 * mode, the loader recorded it, and the console has been drawing into
	 * it since before the bus was walked. Registering with it means the
	 * display layer describes the screen the machine is actually showing
	 * instead of reporting that the machine has none.
	 *
	 * Where firmware left nothing, this registers with a zero mode, which
	 * is the honest state and which the layer now says plainly rather than
	 * printing as `0x0, pitch 0, RGBA` (GX-006). */
	disp = display_register(m->name, &intel_ops, a, info->fb.base,
				info->fb.size);

	if (!disp) {
		kputs("intel-display: no room in the display table\n");
		return false;
	}

	if (info->fb.width && info->fb.height &&
	    info->fb.format != FB_FORMAT_NONE)
		disp->mode = info->fb;

	a->disp = disp;
	adapter_count++;

	/* Nothing to bring back after a suspend, and nothing pretending
	 * otherwise -- the same declaration the other two backends make. */
	suspend_declare(m->name, 0);

	kprintf("intel-display: %s, Gen%u%s, registers at %p\n",
		m->name, m->gen, m->low_power ? " LP" : "",
		(void *)(uintptr_t)a->mmio);

	/* The layout as found, so that the first boot on real hardware is a
	 * measurement rather than a dead end (GX-007). */
	display_print_bars(d);

	if (disp->mode.width)
		kprintf("intel-display: firmware left a %ux%u screen and this "
			"driver keeps it -- there is no modesetting here yet\n",
			disp->mode.width, disp->mode.height);
	else
		kputs("intel-display: firmware left no screen, and this driver "
		      "cannot set one yet, so this machine has an adapter and "
		      "no display\n");

	return true;
}

/* --- the self-test ----------------------------------------------------------
 *
 * **The whole of what can honestly be checked without the hardware, and it is
 * the half most likely to be wrong.**
 *
 * Nothing in this file can be exercised against a real Gen9 under QEMU, which
 * emulates no Intel display engine. What *can* be exercised is the recognition:
 * whether the table says what it means, and -- far more importantly -- whether
 * it declines what it should. A table that claims too much is how a kernel ends
 * up writing display registers into a host bridge.
 *
 * So `intel_display_identify` is a pure function taking four numbers, and this
 * feeds it devices that do not exist on this machine. It runs on every boot in
 * the matrix, on both architectures, on machines with no Intel hardware at all.
 */
bool intel_display_self_test(void)
{
	bool ok = true;
	unsigned i;

	/* 1. The things it must recognise, with the answer it must give.
	 *
	 *    3185 is checked by name because it is the chip in the laptop: a
	 *    table that quietly stopped matching it would leave that machine
	 *    reporting no display driver, which is exactly the state this
	 *    driver exists to end, and it would look like nothing at all from
	 *    here. */
	{
		static const struct { u16 dev; u8 gen; bool lp; } KNOWN[] = {
			{ 0x3185, 9, true  },	/* the test laptop */
			{ 0x3184, 9, true  },
			{ 0x1916, 9, false },	/* Skylake GT2 */
			{ 0x3e92, 9, false },	/* Coffee Lake */
		};

		for (i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
			const struct intel_model *m;

			m = intel_display_identify(PCI_VENDOR_INTEL,
						   KNOWN[i].dev,
						   PCI_CLASS_DISPLAY,
						   PCI_SUBCLASS_VGA);

			if (!m) {
				kprintf("intel-display: %04x is in the table "
					"and was not recognised\n",
					KNOWN[i].dev);
				ok = false;
				continue;
			}

			if (m->gen != KNOWN[i].gen ||
			    m->low_power != KNOWN[i].lp) {
				kprintf("intel-display: %04x came back as "
					"Gen%u%s\n", KNOWN[i].dev, m->gen,
					m->low_power ? " LP" : "");
				ok = false;
			}
		}
	}

	/* 2. **The refusals, which are the half that costs something.**
	 *
	 *    Each of these is a device that really exists on the machines this
	 *    project runs on, and each would be claimed by a rule that is one
	 *    step too loose:
	 *
	 *      - the wrong vendor entirely;
	 *      - Intel, and not a display: the Gemini Lake host bridge, whose
	 *        id 31f0 sits in the same numbering as the graphics device in
	 *        the same package;
	 *      - Intel, a display, and a generation with no entry here;
	 *      - the right identifier at the wrong class, which is what reading
	 *        an id without checking what kind of device it belongs to
	 *        would accept.
	 */
	{
		static const struct {
			u16 vendor, device; u8 cls, sub; const char *what;
		} REFUSE[] = {
			{ 0x1234, 0x1111, 0x03, 0x00,
			  "the Bochs adapter, which is a display and not ours" },
			{ 0x8086, 0x31f0, 0x06, 0x00,
			  "the Gemini Lake host bridge, beside the graphics" },
			{ 0x8086, 0x1237, 0x06, 0x00,
			  "the i440FX host bridge every QEMU machine has" },
			{ 0x8086, 0x100e, 0x02, 0x00,
			  "an Intel network card" },
			{ 0x8086, 0x0046, 0x03, 0x00,
			  "Arrandale graphics, Gen5, with no entry here" },
			{ 0x8086, 0x9a49, 0x03, 0x00,
			  "Tiger Lake Xe, Gen12, with no entry here" },
			{ 0x8086, 0x3185, 0x06, 0x00,
			  "the laptop's own id at the class of a bridge" },
			{ 0x8086, 0x3185, 0x03, 0x80,
			  "the laptop's own id at a non-VGA display subclass" },

			/* **The only case the vendor check alone can catch, and it had
			 * to be constructed.** (GX-008)
			 *
			 * Every other refusal here is caught by the class, the subclass
			 * or simply by not being in the table, so with the vendor check
			 * deleted this test still passed -- the rule had no failing
			 * case. 1916 really is two devices, Intel's Skylake GT2 and a
			 * Dini Group accelerator board, but that board is not a display
			 * and the class check refuses it first.
			 *
			 * So this one is built rather than found, and says so. The rule
			 * it states is real: an identifier means nothing without its
			 * vendor. */
			{ 0x1002, 0x1916, 0x03, 0x00,
			  "AMD's vendor id carrying Intel's Skylake number at "
			  "display class -- constructed, see the comment" },
		};

		for (i = 0; i < sizeof(REFUSE) / sizeof(REFUSE[0]); i++) {
			if (intel_display_identify(REFUSE[i].vendor,
						   REFUSE[i].device,
						   REFUSE[i].cls,
						   REFUSE[i].sub)) {
				kprintf("intel-display: %04x:%04x [%u/%u] was "
					"claimed -- %s\n",
					REFUSE[i].vendor, REFUSE[i].device,
					REFUSE[i].cls, REFUSE[i].sub,
					REFUSE[i].what);
				ok = false;
			}
		}
	}

	/* 3. No two entries may claim the same identifier.
	 *
	 *    A duplicate is invisible: the first match wins and the second is
	 *    dead, so a table with one chip listed twice under different
	 *    generations behaves exactly like a correct one until somebody
	 *    edits the first copy. */
	{
		unsigned j;

		for (i = 0; i < MODEL_COUNT; i++)
			for (j = i + 1; j < MODEL_COUNT; j++)
				if (MODELS[i].device == MODELS[j].device) {
					kprintf("intel-display: %04x is in the "
						"table twice\n",
						MODELS[i].device);
					ok = false;
				}
	}

	kprintf("intel-display: %u model(s) known, recognition and refusal "
		"both checked\n", (unsigned)MODEL_COUNT);

	return ok;
}

void intel_display_print_summary(void)
{
	if (!adapter_count && !declined_unknown && !unexpected_shape)
		return;

	if (adapter_count)
		kprintf("  intel gfx    : %s, Gen%u%s, no modesetting yet\n",
			adapters[0].model->name, adapters[0].model->gen,
			adapters[0].model->low_power ? " LP" : "");

	if (declined_unknown || unexpected_shape)
		kprintf("               : %u Intel display(s) with no entry, "
			"%u with an unexpected register window\n",
			declined_unknown, unexpected_shape);
}
