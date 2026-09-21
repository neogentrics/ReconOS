/* The framebuffer console: what the kernel says, on the screen.
 *
 * The serial port is where the verification rig listens. This is where a person
 * standing in front of an installed machine is looking, and until it existed
 * everything after the bootloader's last line was invisible to them.
 *
 * Every one of these is safe to call when there is no framebuffer, which is the
 * normal case on a serial-only machine and on every aarch64 boot in the matrix,
 * because AAVMF provides none. `fbcon_putc` on a machine with no screen is not
 * an error to be checked for; it is nothing happening.
 */
#ifndef RECON_KERNEL_FBCON_H
#define RECON_KERNEL_FBCON_H

#include <recon/kernel/boot.h>
#include <recon/kernel/types.h>

/* Reads the framebuffer out of the boot info and clears the screen. Must run
 * after vm_init(), because the framebuffer is reached through the direct map. */
void fbcon_init(void);

/* Point the console at a framebuffer somebody else obtained.
 *
 * The display driver calls this after the bus walk, on a machine where firmware
 * left no framebuffer and it set a mode itself. False means the description was
 * not one this console will draw into, and whatever was there before is
 * untouched -- a refused framebuffer must not cost the caller a working screen.
 */
bool fbcon_adopt(const struct framebuffer *given);

/* One character. Understands \n, \r and \t, and scrolls at the bottom. */
void fbcon_putc(char c);

/* Put what has been drawn onto the screen, where that is a separate step.
 *
 * On a display whose framebuffer is scanned out continuously this does nothing
 * and costs a branch. On one that has to be told -- virtio-gpu, and every real
 * GPU -- it is the difference between a console that works and a black screen
 * that reports success.
 *
 * **Call it without the console lock held.** Presenting talks to a device and
 * prints when that fails, and printing takes the lock.
 */
void fbcon_present(void);

/* Whether anything is being drawn. For code that wants to say something
 * different when the only console is a cable. */
bool fbcon_active(void);

/* The screen this machine actually has, or null where it has none.
 *
 * The *panel*, not the console's window onto it -- the console bounds itself
 * to 1920x1200 however large the glass is (KF-203), and a program mapping
 * /dev/fb0 gets all of it.
 *
 * Asked of the console because the console is the one place that reconciles
 * the two ways a machine comes to have a screen: firmware set a mode and
 * passed it through the handoff, or the display driver asked for one after the
 * bus walk. Everything else would have to ask both and know which won. */
const struct framebuffer *fbcon_framebuffer(void);

/* Prints what the screen is, in the same shape as the rest of the boot
 * summary. */
void fbcon_describe(void);

#endif /* RECON_KERNEL_FBCON_H */
