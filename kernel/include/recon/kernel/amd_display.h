/* AMD RDNA2: the fourth display, and the second one made of silicon.
 *
 * Written against the two adapters in this project's desktop, read off that
 * machine rather than recalled:
 *
 *   1002:73ff  Navi 23 [Radeon RX 6600/6600 XT/6600M]   discrete
 *   1002:164e  Raphael                                   the 7700X's integrated
 *
 * Both report PCI class 030000, and both are RDNA2 -- one on a card and one in
 * the processor package. That pairing is the reason this is worth having early:
 * a driver that works on a discrete card and not on an integrated part, or the
 * other way round, is a driver that has learned one machine.
 *
 * --- What it does, which is what the Intel one does -------------------------
 *
 * It identifies the adapter and keeps the mode firmware set. **It does not set
 * a mode.** Modesetting on RDNA2 is Display Core Next 2.x -- the DCHUB, the
 * pipes, the DPP and MPC blocks, the stream encoders, link training, and a
 * DMCUB microcontroller that normally wants a firmware blob to do anything
 * useful. It is larger than Gen9's display engine, not smaller, and none of it
 * can be exercised under QEMU.
 *
 * See intel_display.h for the argument in full. It is the same argument and it
 * has not got weaker for being made twice.
 *
 * --- What having two of these proved ----------------------------------------
 *
 * The Intel driver vetoed any device whose first base address register was
 * smaller than sixteen megabytes, on the grounds that Gen9's is that size. AMD's
 * register window is **512 KB, and it is not the first BAR** -- the first is a
 * 256 MB aperture onto video memory, with a 2 MB doorbell between them.
 *
 * So the shape of a display adapter's base address registers is a fact about
 * its family and not about display adapters, and a driver that vetoes on one
 * family's shape will refuse another family's hardware for a reason that is
 * true of neither (GX-007). Both drivers report the layout now and neither
 * refuses on it, because neither reads a register: a gate that protects nothing
 * and can be wrong is only a way to lose a machine.
 */
#ifndef RECON_KERNEL_AMD_DISPLAY_H
#define RECON_KERNEL_AMD_DISPLAY_H

#include <recon/kernel/types.h>

struct pci_device;

struct amd_model {
	u16 device;
	const char *name;	/* pci.ids' name for it, unedited */
	const char *family;	/* the silicon, which is what a driver cares about */
	bool integrated;	/* in the processor package rather than on a card */
};

/* Whether this is an AMD display engine this kernel knows, and which.
 *
 * Pure, and separate from attaching, for the same reason as the Intel one: none
 * of this can be run against the hardware under QEMU, so the recognition is
 * made into a function of four numbers that a self-test can drive with devices
 * the machine does not have. */
const struct amd_model *amd_display_identify(u16 vendor, u16 device,
					     u8 class_code, u8 subclass);

bool amd_display_attach(const struct pci_device *d);
void amd_display_print_summary(void);
bool amd_display_self_test(void);

#endif /* RECON_KERNEL_AMD_DISPLAY_H */
