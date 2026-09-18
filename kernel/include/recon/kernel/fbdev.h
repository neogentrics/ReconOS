/* /dev/fb0 -- the screen as a file, and what a program has to know to use one.
 *
 * See core/fbdev.c for why a mapping is the point and `write` is the fallback.
 */
#ifndef RECON_KERNEL_FBDEV_H
#define RECON_KERNEL_FBDEV_H

#include <recon/kernel/types.h>
#include <recon/kernel/user.h>

struct file_ops;

/* The operations behind /dev/fb0. Listed in devfs's table, like every other
 * device: the subsystem owns its file_ops and devfs owns the name. */
extern const struct file_ops fb_file_ops;

/* Fills `out` with the screen's shape. SYS_ENODEV where there is no screen,
 * which is the ordinary answer on a serial-only machine and not a failure. */
int fbdev_describe(struct fb_info *out);

/* Whether a program currently holds the screen through a mapping.
 *
 * The console asks this before drawing on the panel. It keeps writing to the
 * serial port and the log ring regardless -- those are the instruments, and a
 * rig that cannot see is a rig that cannot fail. See core/fbdev.c for why the
 * claim hangs on the mapping's lifetime rather than on anything the program
 * promises. */
bool fbdev_panel_claimed(void);

/* That each /dev/fbN names the display at its own position, and that a node for
 * a display this machine does not have is absent rather than empty.
 *
 * The assertion that matters is the last one: a kernel where fb1 is an alias
 * for fb0 opens it, reads the same pixels, and passes anything that only asks
 * whether the device exists. */
bool fbdev_nodes_self_test(void);

#endif /* RECON_KERNEL_FBDEV_H */
