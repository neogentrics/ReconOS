/* Intel integrated graphics: the third display, and the first real one.
 *
 * The Bochs adapter and virtio-gpu are both devices designed to be emulated.
 * This is a display engine in silicon, in a laptop on a desk, and it is the
 * backend the other two were written to prepare for -- against an interface
 * that has now been made to disagree with itself twice rather than never.
 *
 * --- What this drives, and what it does not ---------------------------------
 *
 * **It identifies the adapter and takes the mode firmware left. It does not set
 * a mode**, and that is a deliberate stopping point rather than an unfinished
 * one.
 *
 * Modesetting on Gen9 is the display engine's whole surface: power wells, the
 * DPLLs and the shared LCPLL, DDI port training, AUX transactions to the panel,
 * eDP panel-power sequencing with its own timing rules, the transcoder, the
 * pipe, the plane, and the watermark levels -- which on Skylake-era parts are
 * their own arithmetic, and which produce underruns and corruption rather than
 * an error when they are wrong. It is thousands of lines, none of it can be
 * exercised under QEMU, and every register in it would have been written from
 * memory of a specification rather than from a machine.
 *
 * This kernel's rule is that a wrong answer is worse than an absent one. A
 * modeset written blind is the largest possible wrong answer: it drives real
 * hardware into a state nobody has checked, on the one machine in the matrix
 * that has no serial port to say what happened (KF-214 was found by
 * photographing the panel).
 *
 * So the first increment is the part that is true on arrival: **the machine has
 * an Intel display engine, here is which one, and the screen it is already
 * showing is the screen firmware set up.**
 *
 * --- Why that is worth having on its own ------------------------------------
 *
 * On the test laptop today the kernel reports `display : none -- no adapter
 * this kernel can drive`, and then draws on the panel perfectly -- because UEFI
 * set a mode and the handoff carried it. The screen works and the display layer
 * does not know the adapter exists. Every question anybody asks it afterwards
 * is answered about a machine with no display.
 *
 * --- The third shape --------------------------------------------------------
 *
 * Each backend answers `display_ops` differently, and the differences are the
 * reason there is more than one:
 *
 *              set_mode   preferred_mode   flush
 *   bochs      yes        no               no    -- an aperture, scanned out
 *   virtio-gpu yes        yes              yes   -- guest RAM, told to present
 *   intel      **no**     later            no    -- firmware's mode, scanned out
 *
 * A backend that can be read and not set is a shape this interface had never
 * been shown, and showing it found GX-006 before this driver existed: the mode
 * ladder asked nine times and blamed the adapter's memory, the summary printed
 * `0x0, pitch 0, RGBA` as though somebody had chosen it, and the self-test
 * failed on a kernel behaving correctly.
 */
#ifndef RECON_KERNEL_INTEL_DISPLAY_H
#define RECON_KERNEL_INTEL_DISPLAY_H

#include <recon/kernel/boot.h>
#include <recon/kernel/types.h>

struct pci_device;

/* What an Intel display device is, once it has been recognised. */
struct intel_model {
	u16 device;
	const char *name;	/* the marketing name, as `lspci` prints it */
	u8 gen;			/* the display generation: 9 here */
	bool low_power;		/* the Atom-derived line: Gen9 LP */
};

/* Whether this is an Intel display engine this kernel knows, and which.
 *
 * **Pure, and separate from attaching, so that it can be tested.** Nothing in
 * this kernel can attach a real Gen9 under QEMU, so the table and the rules
 * around it would otherwise be code that runs for the first time on the one
 * machine that cannot report what happened. Split out, the whole of the
 * recognition can be driven from a self-test with made-up devices on every
 * boot -- including the cases that must be *declined*, which is where the cost
 * of being wrong is.
 *
 * Null for anything not in the table, and that includes Intel display adapters
 * of other generations. Refused by not being listed rather than by being
 * driven on the strength of a vendor id: writing display registers into a
 * device on the assumption that all Intel graphics are alike is how a machine
 * ends up with no screen and no explanation.
 */
const struct intel_model *intel_display_identify(u16 vendor, u16 device,
						 u8 class_code, u8 subclass);

/* Offer a PCI function. True if it was claimed. Declines quietly for anything
 * that is not an Intel display this kernel knows, exactly as the other
 * attaches do. */
bool intel_display_attach(const struct pci_device *d);

/* What was found, and what was seen and declined. Prints nothing on a machine
 * with no Intel graphics, which is every machine in the verification matrix. */
void intel_display_print_summary(void);

/* --- the register map, in core/intel_modeset.c ---------------------------
 *
 * A mode expressed as the numbers Gen9's registers take. Pure functions of
 * integers, no hardware in them, so the arithmetic that a modeset would rest on
 * is checked by known answers on every boot in the matrix -- on machines with
 * no Intel display at all, which is all of them.
 *
 * Every field in this engine stores one *less* than the number it describes,
 * which is the thing most likely to be wrong and the thing nothing catches: a
 * pipe told it is 1921 pixels wide accepts it, and the panel looks almost
 * right. */
u32 intel_pipe_srcsz(u32 width, u32 height);
u32 intel_plane_size(u32 width, u32 height);
u32 intel_trans_timing(u32 total, u32 active);

/* And back again. The encoder has been here since the file was written; these
 * arrived when the hardware handed over eight real values to assert against,
 * and reading was being done inline where nothing could check it. */
u32 intel_trans_total(u32 reg);
u32 intel_trans_active(u32 reg);
u32 intel_plane_ctl_bgra(void);

/* The plane stride, in the sixty-four byte chunks the register counts, or
 * **zero for a pitch it cannot express**. Checked rather than rounded: 1366 is
 * a very common panel width and 1366 * 4 is not a multiple of 64, so its
 * natural pitch has no representation here and a driver has to pad. */
u32 intel_plane_stride(u32 pitch_bytes);

/* Read the mode the hardware is actually in, and compare it with the one
 * firmware handed over.
 *
 * Writes nothing, which is what makes it safe to be the first thing run on a
 * machine that cannot report what happened. Self-validating: agreement between
 * the pipe registers and the handoff is evidence the register map is right
 * about this silicon, and that is a great deal to learn from a read. */
bool intel_modeset_read(volatile u8 *mmio, const struct framebuffer *firmware);

/* That the register map and its arithmetic say what they mean. */
bool intel_modeset_self_test(void);

/* That the recognition table says what it means, including what it refuses. */
bool intel_display_self_test(void);

#endif /* RECON_KERNEL_INTEL_DISPLAY_H */
