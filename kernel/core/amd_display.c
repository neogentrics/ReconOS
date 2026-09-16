/* AMD RDNA2 -- identification, and the mode firmware set.
 *
 * See amd_display.h for what this drives and what it deliberately does not.
 *
 * --- Where the device ids come from -----------------------------------------
 *
 * Two of them were read off the machine this was written on, through Windows'
 * own device list:
 *
 *   PCI\VEN_1002&DEV_73FF&SUBSYS_E4511DA2&REV_C7   AMD Radeon RX 6600
 *   PCI\VEN_1002&DEV_164E&SUBSYS_7D781462&REV_C3   AMD Radeon(TM) Graphics
 *
 * both reporting compatible id `PCI\CC_030000`. The rest of the family, and
 * every name below, is `/usr/share/misc/pci.ids` -- the database `lspci` prints
 * from -- so each line can be checked against a source outside this tree.
 *
 * --- Two things that file makes obvious, and both are in the tests ----------
 *
 * **An identifier means nothing without its vendor.** `73bf` is Navi 21 under
 * AMD and a National Instruments FlexRay controller (1093); `164e` is Raphael under
 * AMD and a Broadcom NetXtreme II under Broadcom. Both collisions are in
 * pci.ids today, and one of them is the integrated graphics in this project's
 * own desktop.
 *
 * **And nothing without its class.** `73a4`, `73c4` and `73e4` are the *USB
 * controllers* AMD puts on Navi 21, 22 and 23 boards -- same vendor, adjacent
 * numbers, on the same card as the display, and not a display. A rule that
 * matched on vendor and identifier alone would drive a USB controller as a
 * screen.
 */
#include <recon/kernel/amd_display.h>
#include <recon/kernel/boot.h>
#include <recon/kernel/console.h>
#include <recon/kernel/display.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/suspend.h>

#define PCI_VENDOR_AMD		0x1002

#define PCI_CLASS_DISPLAY	0x03
#define PCI_SUBCLASS_VGA	0x00

/* RDNA2, and the integrated parts that share its display core.
 *
 * Navi 21, 22, 23 and 24 are the discrete chips; Raphael and Rembrandt are the
 * same generation inside a processor package. Kept to the consumer and
 * workstation display devices: the USB controllers on those boards are
 * deliberately absent and are in the refusal test instead.
 */
static const struct amd_model MODELS[] = {
	/* Navi 21 */
	{ 0x73a5, "Navi 21 [Radeon RX 6950 XT]",                "Navi 21", false },
	{ 0x73af, "Navi 21 [Radeon RX 6900 XT]",                "Navi 21", false },
	{ 0x73bf, "Navi 21 [Radeon RX 6800/6800 XT / 6900 XT]", "Navi 21", false },
	{ 0x73a3, "Navi 21 GL-XL [Radeon PRO W6800]",           "Navi 21", false },

	/* Navi 22 */
	{ 0x73df, "Navi 22 [Radeon RX 6700/6700 XT/6750 XT / 6800M/6850M XT]",
							        "Navi 22", false },

	/* Navi 23 -- 73ff is the card in this project's desktop */
	{ 0x73ef, "Navi 23 [Radeon RX 6650 XT / 6700S / 6800S]", "Navi 23", false },
	{ 0x73ff, "Navi 23 [Radeon RX 6600/6600 XT/6600M]",      "Navi 23", false },
	{ 0x73e3, "Navi 23 WKS-XL [Radeon PRO W6600]",           "Navi 23", false },

	/* Navi 24 */
	{ 0x7422, "Navi 24 [Radeon PRO W6400]",                 "Navi 24", false },
	{ 0x7424, "Navi 24 [Radeon RX 6300]",                   "Navi 24", false },

	/* Integrated -- 164e is the graphics in this project's desktop */
	{ 0x164e, "Raphael",                                    "Raphael", true  },
	{ 0x164d, "Rembrandt",                                  "Rembrandt", true },
	{ 0x1681, "Rembrandt [Radeon 680M]",                    "Rembrandt", true },
};

#define MODEL_COUNT (sizeof(MODELS) / sizeof(MODELS[0]))

/* **Two, because this project's desktop has two.**
 *
 * A Radeon RX 6600 on the bus and Raphael graphics in the processor package,
 * both answering PCI class 030000 on the same machine. A driver written to hold
 * one would have found whichever the bus walk reached first and reported the
 * machine as having a single screen. */
#define AMD_DISPLAY_MAX		2

static struct amd_display {
	const struct amd_model *model;
	struct display *disp;
} adapters[AMD_DISPLAY_MAX];

static unsigned adapter_count;
static unsigned declined_unknown;
static unsigned no_room;

/* --- recognition ----------------------------------------------------------- */

const struct amd_model *amd_display_identify(u16 vendor, u16 device,
					     u8 class_code, u8 subclass)
{
	unsigned i;

	if (vendor != PCI_VENDOR_AMD)
		return NULL;

	/* See the file comment: `73a4`, `73c4` and `73e4` are USB controllers
	 * on Navi boards, and this is what stops them being driven as
	 * screens. */
	if (class_code != PCI_CLASS_DISPLAY || subclass != PCI_SUBCLASS_VGA)
		return NULL;

	for (i = 0; i < MODEL_COUNT; i++)
		if (MODELS[i].device == device)
			return &MODELS[i];

	return NULL;
}

/* --- the operations --------------------------------------------------------
 *
 * All null, and each omission means what it means on the Intel backend:
 *
 *   set_mode        there is no modesetting for RDNA2 here.
 *   flush           the display controller scans out of memory continuously,
 *                   so a store is a pixel appearing.
 *   preferred_mode  knowable, not yet known -- it wants registers read from a
 *                   running machine.
 */
static const struct display_ops amd_ops = {
	.set_mode       = 0,
	.flush          = 0,
	.preferred_mode = 0,
};

/* --- attaching ------------------------------------------------------------- */

bool amd_display_attach(const struct pci_device *d)
{
	const struct amd_model *m;
	struct amd_display *a;
	const struct boot_info *info;
	struct display *disp;

	m = amd_display_identify(d->vendor, d->device, d->class_code,
				 d->subclass);

	if (!m) {
		if (d->vendor == PCI_VENDOR_AMD &&
		    d->class_code == PCI_CLASS_DISPLAY) {
			kprintf("amd-display: %04x:%04x is an AMD display this "
				"kernel has no entry for, so it is left alone "
				"rather than driven on the strength of the "
				"vendor id\n", d->vendor, d->device);
			declined_unknown++;
		}
		return false;
	}

	if (adapter_count >= AMD_DISPLAY_MAX) {
		kprintf("amd-display: %s found and there is no room for a "
			"third\n", m->name);
		no_room++;
		return false;
	}

	a = &adapters[adapter_count];
	kmemset(a, 0, sizeof(*a));
	a->model = m;

	info = boot_info();

	disp = display_register(m->name, &amd_ops, a, info->fb.base,
				info->fb.size);

	if (!disp) {
		kputs("amd-display: no room in the display table\n");
		no_room++;
		return false;
	}

	/* Only the first adapter found can claim the firmware's framebuffer.
	 *
	 * **There is exactly one of those and this machine has two adapters.**
	 * Firmware lit one panel, on one of them, and the handoff says where
	 * the pixels are and not which device is scanning them out. Giving that
	 * mode to the second as well would describe two screens showing the
	 * same memory, which is a claim nobody checked and which would be wrong
	 * on the desktop this was written on. The second is registered in no
	 * mode, which is what is actually known about it. */
	if (adapter_count == 0 && info->fb.width && info->fb.height &&
	    info->fb.format != FB_FORMAT_NONE)
		disp->mode = info->fb;

	a->disp = disp;
	adapter_count++;

	suspend_declare(m->name, 0);

	kprintf("amd-display: %s, %s%s\n", m->name, m->family,
		m->integrated ? ", in the processor package" : ", on a card");

	/* **The base address registers, printed rather than judged.**
	 *
	 * This is the whole of GX-007. The Intel driver used to refuse a device
	 * whose first BAR was not sixteen megabytes; AMD's register window is
	 * 512 KB and is not the first BAR at all -- the first is a 256 MB
	 * aperture onto video memory, with a 2 MB doorbell between them, as
	 * read off this project's own desktop.
	 *
	 * So the layout is a fact about a family, nothing here reads a
	 * register, and a veto that protects nothing can only lose a machine.
	 * Printed instead, because the first boot on real hardware is then a
	 * measurement rather than a dead end -- which is the only way the
	 * assumption gets corrected at all. */
	display_print_bars(d);

	if (disp->mode.width)
		kprintf("amd-display: firmware left a %ux%u screen and this "
			"driver keeps it -- there is no modesetting here yet\n",
			disp->mode.width, disp->mode.height);

	return true;
}

void amd_display_print_summary(void)
{
	unsigned i;

	if (!adapter_count && !declined_unknown && !no_room)
		return;

	for (i = 0; i < adapter_count; i++)
		kprintf("  amd gfx      : %s, %s, no modesetting yet\n",
			adapters[i].model->name, adapters[i].model->family);

	if (declined_unknown || no_room)
		kprintf("               : %u AMD display(s) with no entry, "
			"%u with nowhere to go\n", declined_unknown, no_room);
}

/* --- the self-test ---------------------------------------------------------
 *
 * The same shape as the Intel one, and for the same reason: the hardware cannot
 * be reached from the verification matrix, so what is tested is the
 * recognition -- and the refusals are the half that costs something.
 */
bool amd_display_self_test(void)
{
	bool ok = true;
	unsigned i;

	/* 1. What it must recognise. The two named are the adapters in this
	 *    project's desktop; a table that stopped matching them would report
	 *    that machine as having no AMD graphics, which is exactly the state
	 *    this driver exists to end and which looks like nothing from
	 *    here. */
	{
		static const struct { u16 dev; bool integrated; } KNOWN[] = {
			{ 0x73ff, false },	/* the RX 6600 in the desktop */
			{ 0x164e, true  },	/* the 7700X's own graphics */
			{ 0x73bf, false },
			{ 0x7424, false },
		};

		for (i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
			const struct amd_model *m;

			m = amd_display_identify(PCI_VENDOR_AMD, KNOWN[i].dev,
						 PCI_CLASS_DISPLAY,
						 PCI_SUBCLASS_VGA);

			if (!m) {
				kprintf("amd-display: %04x is in the table and "
					"was not recognised\n", KNOWN[i].dev);
				ok = false;
				continue;
			}

			if (m->integrated != KNOWN[i].integrated) {
				kprintf("amd-display: %04x came back as %s\n",
					KNOWN[i].dev,
					m->integrated ? "integrated"
						      : "discrete");
				ok = false;
			}
		}
	}

	/* 2. **The refusals, every one of them a device that really exists.**
	 *
	 *    The two identifier collisions are the interesting pair: both
	 *    numbers belong to an AMD display *and* to somebody else's
	 *    hardware, and one of them is the integrated graphics in this
	 *    project's desktop. A rule that read an identifier without its
	 *    vendor would drive a Broadcom network card as a screen.
	 *
	 *    The three USB controllers are the other half: AMD's own, on the
	 *    same boards as the displays, at adjacent numbers.
	 */
	{
		static const struct {
			u16 vendor, device; u8 cls, sub; const char *what;
		} REFUSE[] = {
			{ 0x14e4, 0x164e, 0x02, 0x00,
			  "Broadcom NetXtreme II, which shares 164e with "
			  "Raphael" },
			{ 0x1093, 0x73bf, 0x0b, 0x40,
			  "a FlexRay controller, which shares 73bf with "
			  "Navi 21" },
			{ 0x1002, 0x73a4, 0x0c, 0x03,
			  "Navi 21 USB -- AMD's own, on the graphics card" },
			{ 0x1002, 0x73c4, 0x0c, 0x03, "Navi 22 USB" },
			{ 0x1002, 0x73e4, 0x0c, 0x03, "Navi 23 USB" },
			{ 0x1002, 0x1640, 0x04, 0x03,
			  "the Rembrandt audio controller beside the "
			  "graphics" },
			{ 0x8086, 0x3185, 0x03, 0x00,
			  "Intel's Gemini Lake graphics, which is a display "
			  "and not ours" },
			{ 0x1002, 0x73ff, 0x0c, 0x03,
			  "the desktop's own card at the class of a USB "
			  "controller" },
			{ 0x1002, 0x164e, 0x03, 0x80,
			  "the desktop's own graphics at a non-VGA display "
			  "subclass" },
			{ 0x1002, 0x9999, 0x03, 0x00,
			  "an AMD display with no entry here" },

			/* **The only case the vendor check alone can catch, and it had
			 * to be constructed.** (GX-008)
			 *
			 * Every real collision above is refused by the *class* check
			 * before the vendor is ever considered -- Broadcom's 164e is a
			 * network card and the FlexRay is not a display -- so with the
			 * vendor check deleted this test still passed, and the rule it
			 * was supposed to be protecting had no failing case at all.
			 *
			 * No such device exists: display-class hardware comes from few
			 * enough vendors that no real cross-vendor collision is also a
			 * display. The rule under test is real regardless -- an
			 * identifier means nothing without its vendor -- so the case is
			 * built to state it, and labelled here rather than left looking
			 * like a device somebody found. */
			{ 0x8086, 0x73ff, 0x03, 0x00,
			  "Intel's vendor id carrying the desktop card's number at "
			  "display class -- constructed, see the comment" },
		};

		for (i = 0; i < sizeof(REFUSE) / sizeof(REFUSE[0]); i++) {
			if (amd_display_identify(REFUSE[i].vendor,
						 REFUSE[i].device,
						 REFUSE[i].cls,
						 REFUSE[i].sub)) {
				kprintf("amd-display: %04x:%04x [%u/%u] was "
					"claimed -- %s\n",
					REFUSE[i].vendor, REFUSE[i].device,
					REFUSE[i].cls, REFUSE[i].sub,
					REFUSE[i].what);
				ok = false;
			}
		}
	}

	/* 3. No identifier twice. A duplicate is invisible: the first match
	 *    wins and the second is dead code that looks like a table entry. */
	{
		unsigned j;

		for (i = 0; i < MODEL_COUNT; i++)
			for (j = i + 1; j < MODEL_COUNT; j++)
				if (MODELS[i].device == MODELS[j].device) {
					kprintf("amd-display: %04x is in the "
						"table twice\n",
						MODELS[i].device);
					ok = false;
				}
	}

	kprintf("amd-display: %u model(s) known, recognition and refusal both "
		"checked\n", (unsigned)MODEL_COUNT);

	return ok;
}
