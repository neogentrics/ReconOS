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

	/* **Put what has been drawn on the screen.**
	 *
	 * Null on hardware where there is no such step, which is what the
	 * Bochs adapter is: its framebuffer is device memory being scanned out
	 * continuously, so a store *is* a pixel appearing and there is nothing
	 * to ask for. A null pointer here means exactly that, in the same way a
	 * null `request_interrupt` means "this transport has no way to do it"
	 * on the virtio side.
	 *
	 * **This operation exists because it was missing.** Everything above
	 * this interface was written against the Bochs adapter and inherited
	 * its assumption -- that drawing is the whole of drawing. virtio-gpu
	 * keeps its pixels in guest RAM that the host cannot see until it is
	 * told to copy them, so a console that stores characters and stops
	 * leaves a black screen while reporting a working mode. The assumption
	 * was invisible while there was one backend to hold it.
	 *
	 * The rectangle is in pixels and is what changed. A driver may present
	 * more than it was asked to and may not present less. False means the
	 * hardware refused -- worth counting and worth printing, because a
	 * display that has silently stopped presenting looks exactly like a
	 * machine nobody has drawn on.
	 *
	 * **Callers must not hold the console lock.** A flush talks to a device
	 * and may print about it, and printing takes that lock.
	 */
	bool (*flush)(struct display *d, u32 x, u32 y, u32 w, u32 h);

	/* **What size the screen actually is**, where the hardware can say.
	 *
	 * Null on an adapter that has no idea, which the Bochs one does not: it
	 * has a memory size and a set of registers, and nothing that describes
	 * the panel in front of it. That is why `display_init` picks a mode off
	 * a ladder of sizes largest-first -- it is choosing the biggest thing
	 * the *adapter* can hold, because the biggest thing the *screen* can
	 * show is not a question it can ask.
	 *
	 * **Which produced a real wrong answer as soon as a device could
	 * answer.** virtio-gpu reports its scanout, and the host said 1280x800
	 * while the ladder went on choosing 5120x2880 -- four times the pixels
	 * of the display they were being scaled onto, and sixty megabytes of
	 * memory spent to do it (GX-005). The ladder was never wrong; it was
	 * answering the only question it had.
	 *
	 * False means the device was asked and did not know, which is different
	 * from not being askable and is worth keeping apart -- a monitor that
	 * reports nothing is a machine, and a driver with no way to ask is a
	 * gap in this kernel. */
	bool (*preferred_mode)(struct display *d, u32 *width, u32 *height);
};

struct display {
	const char *name;
	const struct display_ops *ops;

	/* Whatever the driver behind `ops` needs to find its own device.
	 *
	 * **This replaced a parallel array**, and the array is worth describing
	 * because it is the shape a single-backend interface grows naturally.
	 * The Bochs driver kept `adapters[DISPLAY_MAX]` beside the display
	 * table and indexed it with `d - displays`, so one driver's private
	 * state was addressed by a display's position in a table it did not
	 * own. That works perfectly for as long as every entry in the table
	 * belongs to that driver, and stops meaning anything the moment one
	 * does not (GX-002). */
	void *ops_private;

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

/* Put a display in the table and return it, or null when the table is full.
 *
 * Every backend comes in this way, including the Bochs one. The alternative --
 * each driver reaching into the table itself -- is what produced the parallel
 * private-state array described on `ops_private`, so the table is only written
 * to from here.
 *
 * `fb_base` and `fb_size` mean subtly different things per backend and the
 * difference is worth knowing: for an adapter with an aperture they are the
 * device's own memory, fixed for the life of the machine. For a device whose
 * pixels are main memory, `fb_base` is zero until a mode is set and `fb_size`
 * is a *budget* the driver chose rather than a measurement of anything.
 */
struct display *display_register(const char *name,
				 const struct display_ops *ops, void *priv,
				 paddr_t fb_base, u64 fb_size);

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

/* Present a rectangle of the primary display.
 *
 * **True where there is nothing to do**, and that is the important half of the
 * contract rather than a convenience. A machine whose screen scans itself out
 * has already shown the pixels by the time this is called, so "no flush
 * operation" and "the flush worked" are the same outcome to every caller and
 * are reported as the same answer. False is reserved for a display that was
 * asked to present and would not -- the only case anybody can act on.
 *
 * Safe on a machine with no display at all, which is most of the matrix.
 */
bool display_flush(u32 x, u32 y, u32 w, u32 h);

/* Whether presenting is a step on this machine at all.
 *
 * For code that must know the *shape* of the screen it has rather than merely
 * getting the right answer from `display_flush` -- `/dev/fb0` has to tell a
 * program mapping the pixels whether its stores will be seen on their own. */
bool display_needs_flush(void);

/* Whether this physical page is part of the screen in force right now.
 *
 * For the address-space teardown, which walks page tables and so knows only
 * physical addresses. It used to decide this by asking whether the page
 * allocator had handed the page out -- correct while every framebuffer was a
 * PCI aperture, wrong the moment one is main memory (GX-001). */
bool display_owns_page(paddr_t pa);

void display_print_summary(void);
bool display_self_test(void);

#endif /* RECON_KERNEL_DISPLAY_H */
