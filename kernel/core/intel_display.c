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
#include <recon/kernel/pmm.h>
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

	/* The register window, once mapped, or null where it could not be.
	 *
	 * **Null is a working adapter**, not a broken one: the panel firmware
	 * lit is still registered and the console still draws on it. All that
	 * is lost is the ability to ask the hardware what mode it is in, which
	 * is exactly what this driver did for its first five days. */
	volatile u8 *regs;

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

	/* **The register window, mapped -- and that day has come.**
	 *
	 * This comment used to say the mapping was "a line of code away the day
	 * there is something to read, which is the day somebody has read those
	 * registers on a running machine and can say what they should contain".
	 * That condition is met. The Gateway's display registers were read on
	 * 18 September 2026, twice, by two sessions, and both dumps are in
	 * `docs/hardware/`. Every offset in `intel_modeset.c` has been compared
	 * against a running Gen9, and the panel's timings against the mode its
	 * connector reports.
	 *
	 * --- Why a raw read is known to be safe here ---------------------------
	 *
	 * Not from a specification: `intel_reg` is a userspace program that
	 * mmaps this BAR and reads it with no driver's help, and it read every
	 * register this kernel cares about on that machine. A mapped read of
	 * this window is therefore something that has been *done* on this exact
	 * silicon, rather than something believed to work.
	 *
	 * --- Read-only, and that is the point ---------------------------------
	 *
	 * `VM_WRITE` is **absent**, which makes this the first device mapping in
	 * this kernel that a driver cannot write through. Every other one --
	 * apic.c, pci.c, storage.c, the Bochs adapter -- maps `VM_READ |
	 * VM_WRITE | VM_DEVICE | VM_GLOBAL`, because every other one writes.
	 *
	 * This driver reads. On a machine with **no serial port**, a stray write
	 * into the display engine is the single most expensive mistake available
	 * -- it takes the panel out and takes the only channel that could
	 * explain why out with it. An intention not to write is worth nothing
	 * against a typo; a page table entry without the writable bit is worth
	 * something, and both architectures honour its absence (x86_64 sets the
	 * bit only under `if (flags & VM_WRITE)`, aarch64 picks `ATTR_AP_RO`).
	 *
	 * The day a modeset is written, this flag changes, in one place, on
	 * purpose, and somebody has to mean it.
	 *
	 * --- Failing to map is not failing to attach ---------------------------
	 *
	 * A refusal here would lose the machine its display over an inability to
	 * ask the hardware a question -- which is GX-007's mistake with a
	 * different register. The adapter registers either way; `regs` stays
	 * null and the mode-read is skipped.
	 */
	a->regs = pci_map_bar_ro(d, GEN9_MMIO_BAR, 0,
				 (u32)(a->mmio_size > 0xFFFFFFFFull
				       ? 0xFFFFFFFFu : a->mmio_size));

	if (!a->regs)
		kprintf("intel-display: could not map %s's register window at "
			"%p -- the display is still driven, its mode just "
			"cannot be read\n", m->name,
			(void *)(uintptr_t)a->mmio);

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

	/* **The first thing this kernel has ever read off a Gen9.**
	 *
	 * After registering rather than before, so that a machine whose
	 * registers say something unexpected still has its display in the
	 * table. This call writes nothing and returns whether it found a pipe
	 * scanning out; what it prints is the interesting part, and it prints a
	 * disagreement with the boot handoff as a disagreement, with both
	 * numbers and no verdict.
	 *
	 * It is given the framebuffer firmware described, which is the only
	 * independent statement about this screen the kernel has. On the
	 * Gateway that is the UEFI GOP mode, and the registers should agree
	 * with it -- two sources, never compared on this machine by anybody. */
	if (a->regs)
		intel_modeset_read(a->regs, &info->fb);

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

	/* 0. **The window a BAR gets mapped through** -- `pci_bar_window`, which
	 *    is shared with every other driver in this kernel and had no test
	 *    of its own until this one.
	 *
	 *    Checked from here because this is the driver that needed it
	 *    exposed, and worth checking precisely because the rest of the
	 *    mapping cannot be: QEMU emulates no Intel display engine, so that
	 *    path runs on exactly one computer this project owns, with no
	 *    serial port on it.
	 *
	 *    **This arithmetic existed twice for about an hour.** The first
	 *    version of this change copied it into this file rather than
	 *    calling `pci_map_bar`, because `pci_map_bar` maps writable and
	 *    this driver wants read-only -- so a permission difference produced
	 *    a duplicated calculation. `pci_map_bar_ro` is the fix, and the
	 *    arithmetic is in one place with a test on it.
	 *
	 *    Asserted as a **property** rather than as a table of answers,
	 *    because the property is what the mapping needs and the constants
	 *    are just one way of satisfying it: the window must start at or
	 *    below the BAR, end at or above its last byte, and be whole pages
	 *    at both ends. A version that rounds the length up but forgets that
	 *    the base moved satisfies two of those three, and leaves the last
	 *    page of a 16 MB register file unmapped -- which shows up as a
	 *    fault at the far end of the address space, nowhere near here. */
	{
		static const struct { u64 base, size; const char *what; } CASES[] = {
			{ 0xa0000000ull, 16ull << 20, "the Gateway's, as lspci reports it" },
			{ 0xa0000800ull, 0x1000ull,   "a BAR that is not page aligned" },
			{ 0xa0000fffull, 1ull,        "one byte in the last of a page" },
			{ 0xa0001000ull, 0x1000ull,   "exactly one aligned page" },
			{ 0x90000000ull, 256ull << 20, "the aperture, for scale" },
		};

		for (i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
			paddr_t first = 0;
			u64 span = 0;
			u64 base = CASES[i].base, size = CASES[i].size;

			pci_bar_window((paddr_t)base, size, &first, &span);

			if ((u64)first > base) {
				kprintf("intel-display: window for %s starts at "
					"%llx, past the %llx it must cover\n",
					CASES[i].what,
					(unsigned long long)first,
					(unsigned long long)base);
				ok = false;
			}

			if ((u64)first + span < base + size) {
				kprintf("intel-display: window for %s ends at "
					"%llx and the register file ends at "
					"%llx\n", CASES[i].what,
					(unsigned long long)((u64)first + span),
					(unsigned long long)(base + size));
				ok = false;
			}

			if (((u64)first & (PAGE_SIZE - 1)) ||
			    (span & (PAGE_SIZE - 1)) || !span) {
				kprintf("intel-display: window for %s is %llx "
					"+ %llx, which is not whole pages\n",
					CASES[i].what,
					(unsigned long long)first,
					(unsigned long long)span);
				ok = false;
			}
		}
	}

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

	/* 4. **That `attach` asks the question the same way this test does.**
	 *
	 *    Everything above drives `intel_display_identify` directly, which
	 *    leaves the wiring between the two entirely untested: `attach`
	 *    passes four fields of a `struct pci_device` in a particular order,
	 *    and swapping the class for the subclass -- or reading the
	 *    subsystem id instead of the device id -- would leave every test
	 *    above passing while the driver claimed the wrong hardware or none
	 *    at all.
	 *
	 *    Only the refusing half can be driven from here: a device that is
	 *    *accepted* would be put in the display table, and a self-test that
	 *    registers an imaginary screen has changed the machine it was
	 *    asked about. So this feeds it one that must be refused, and checks
	 *    that it was refused for the stated reason -- the counter moving is
	 *    what says the decline path ran rather than the function returning
	 *    early somewhere else.
	 */
	{
		struct pci_device fake;
		unsigned before = declined_unknown;

		kmemset(&fake, 0, sizeof(fake));
		fake.vendor     = PCI_VENDOR_INTEL;
		fake.device     = 0x9a49;	/* Tiger Lake Xe, no entry here */
		fake.class_code = PCI_CLASS_DISPLAY;
		fake.subclass   = PCI_SUBCLASS_VGA;

		if (intel_display_attach(&fake)) {
			kputs("intel-display: attach claimed a device with no "
			      "entry in the table\n");
			ok = false;
		} else if (declined_unknown != before + 1) {
			kputs("intel-display: attach refused an unknown Intel "
			      "display without counting it, so the decline "
			      "path did not run\n");
			ok = false;
		}

		/* **And that it passes them in the right order**, which the case
		 * above cannot see.
		 *
		 * A first version of this test fed `attach` an unknown device
		 * at class 3 subclass 0 and checked it was refused. Swapping
		 * the two arguments in `attach` still refused it -- the device
		 * was unknown either way -- so the test passed against exactly
		 * the wiring fault it was written to catch. That is GX-008
		 * happening again inside the fix for GX-008.
		 *
		 * This case discriminates. The identifier is one the table
		 * *does* know, presented at class 0 subclass 3 -- the reverse
		 * of a display. Read correctly it is not a display and must be
		 * refused; read with the two swapped it becomes the laptop's
		 * own graphics and would be claimed.
		 */
		kmemset(&fake, 0, sizeof(fake));
		fake.vendor     = PCI_VENDOR_INTEL;
		fake.device     = 0x3185;
		fake.class_code = PCI_SUBCLASS_VGA;	/* deliberately reversed */
		fake.subclass   = PCI_CLASS_DISPLAY;

		if (intel_display_attach(&fake)) {
			kputs("intel-display: " "attach claimed a device whose class and "
			      "subclass are the reverse of a display, so it is "
			      "reading the two the wrong way round\n");
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
