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

#include <recon/kernel/types.h>

/* Reads the framebuffer out of the boot info and clears the screen. Must run
 * after vm_init(), because the framebuffer is reached through the direct map. */
void fbcon_init(void);

/* One character. Understands \n, \r and \t, and scrolls at the bottom. */
void fbcon_putc(char c);

/* Whether anything is being drawn. For code that wants to say something
 * different when the only console is a cable. */
bool fbcon_active(void);

/* Prints what the screen is, in the same shape as the rest of the boot
 * summary. */
void fbcon_describe(void);

#endif /* RECON_KERNEL_FBCON_H */
