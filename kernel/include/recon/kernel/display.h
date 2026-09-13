/* Display: a mode somebody chose, rather than whatever firmware left.
 *
 * Until this existed the kernel had exactly one relationship with a screen --
 * it drew into the framebuffer the bootloader had been handed, at the size the
 * firmware happened to pick, and where there was none it had no screen at all.
 * That is most of the verification matrix: **every PVH and direct-kernel path
 * boots with `framebuffer : none`**, because there is no firmware on those
 * paths to have set one up.
 *
 * A display controller is sitting on the bus on those machines regardless. So
 * the gap is not "the kernel cannot draw"; it is that nobody ever asked the
 * hardware for a mode.
 *
 * --- Why this is in core/ ---------------------------------------------------
 *
 * The one device driven here is reached entirely through its PCI BARs: the
 * registers through one, the pixels through another. There is no port I/O and
 * no machine-specific instruction anywhere in it, which is the same reason
 * virtio, NVMe and AHCI are portable and legacy IDE is not (KF-192). A display
 * that has to be poked through an x86 I/O port belongs in arch/; this one does
 * not.
 *
 * --- What a mode is here ----------------------------------------------------
 *
 * Width, height, pitch and a pixel format, plus where the pixels live. The same
 * four facts `struct framebuffer` already carries through the handoff, which is
 * deliberate: a mode this sets and a mode firmware set must be the same kind of
 * thing, or every consumer needs to know which it got.
 */
#ifndef RECON_KERNEL_DISPLAY_H
#define RECON_KERNEL_DISPLAY_H

#include <recon/kernel/boot.h>
#include <recon/kernel/types.h>

struct pci_device;
struct display;

struct display_ops {
	/* Ask for a size. Returns the mode actually established, which may not
	 * be the one asked for -- a caller that assumes otherwise draws off the
	 * end of the first row. Null where the hardware cannot be told. */
	bool (*set_mode)(struct display *d, u32 width, u32 height);
};

struct display {
	const char *name;
	const struct display_ops *ops;

	/* The pixels, as physical address and length. Mapped as device memory
	 * by whoever draws; this layer does not map it for them. */
	paddr_t fb_base;
	u64 fb_size;

	struct framebuffer mode;

	bool present;
};

/* Offer a PCI function to the display drivers. Returns true if one claimed it.
 * Declines quietly for anything that is not a display it can drive, exactly as
 * nvme_attach and ahci_attach do. */
bool display_attach(const struct pci_device *d);

/* Runs after the bus has been walked.
 *
 * **Not at fbcon_init time, and that ordering is the whole point.** The console
 * starts on whatever the handoff carried, because it must work before there is
 * a PCI bus to ask. This runs later, and only where the machine still has no
 * framebuffer does it set one up and hand it to the console. A machine whose
 * firmware already provided a screen keeps it: re-setting a working mode is a
 * visible flicker and a chance to end up with nothing, in exchange for nothing.
 */
void display_init(void);

/* The primary display, or null where there is none. */
struct display *display_primary(void);

/* Change the mode on the primary display. False where there is no display, or
 * where it cannot be told, or where the hardware refused -- and the three are
 * distinguished in what it prints, because "no display" and "the display said
 * no" want different things done about them. */
bool display_set_mode(u32 width, u32 height);

void display_print_summary(void);
bool display_self_test(void);

#endif /* RECON_KERNEL_DISPLAY_H */
