/* virtio-gpu: a screen that is not a window onto device memory.
 *
 * The second display backend, and it exists to disagree with the first.
 *
 * `display_ops` was shaped entirely by the Bochs/VBE adapter, which is one
 * device, and one device cannot show whether an interface abstracts anything.
 * This driver is the disagreement: it is a display that works in a way the
 * first one does not, on a device QEMU runs, so `scripts/verify-kernel.sh`
 * exercises it on every change rather than once on a laptop somebody had to
 * reboot.
 *
 * --- The difference, which is the entire reason this file is interesting ----
 *
 * The Bochs adapter has a **framebuffer BAR**: a range of device memory at a
 * physical address, permanently being scanned out. Writing a pixel into it is
 * writing a pixel onto the glass. There is no third step, and every consumer
 * in this kernel was written against that -- `fbcon` stores characters and they
 * appear, `/dev/fb0` hands a program the pages and the program's stores appear.
 *
 * virtio-gpu has no such aperture. **There is no device memory to write to.**
 * The pixels live in ordinary guest RAM that this driver allocated, and the
 * host cannot see them at all until it is *told*:
 *
 *   1. RESOURCE_CREATE_2D    the host makes a surface of its own
 *   2. RESOURCE_ATTACH_BACKING   ...backed by these guest pages
 *   3. SET_SCANOUT           ...and that surface is what output 0 shows
 *   4. TRANSFER_TO_HOST_2D   copy the guest pages into the host surface
 *   5. RESOURCE_FLUSH        and put it on the screen
 *
 * Steps 1 to 3 happen once, at mode set. **Steps 4 and 5 happen every time the
 * pixels change**, and nothing at all appears without them. A driver that
 * stopped after step 3 would set a mode, report a framebuffer, let the console
 * draw fifty rows into it, pass every test that asks what the mode is -- and
 * show a black screen for ever.
 *
 * That is not a quirk of this device. It is what every display that is not a
 * dumb aperture does: a GPU with a command ring, a remote framebuffer, a
 * virtualised display. Bochs is the special case, not this one.
 *
 * --- What that cost the interface ------------------------------------------
 *
 * `display_ops` grew `flush`, and `struct display` grew nothing, because a null
 * `flush` already means "there is no such step here" in the same way a null
 * `request_interrupt` means it on the virtio transports. The Bochs driver
 * leaves it null and is unchanged.
 *
 * **The half that is not solved here is the mapping**, and it is written down
 * in `docs/SIGNALS.md` rather than guessed at. `/dev/fb0` hands a program the
 * framebuffer pages and then the kernel is not involved again -- which is the
 * whole point of a mapping and the reason a compositor can be fast. On this
 * device the kernel *must* be involved again, once per frame, or nothing the
 * program drew is ever seen. There is no system call for "present" and adding
 * one changes an ABI two other sessions own, so this driver makes the console
 * and the `write` path correct, reports the mapping honestly, and leaves the
 * question where the people who own that ABI will see it.
 */
#ifndef RECON_KERNEL_VIRTIO_GPU_H
#define RECON_KERNEL_VIRTIO_GPU_H

#include <recon/kernel/types.h>

struct virtio_device;

/* Takes a probed virtio device and makes it a display, if that is what it is.
 * False, quietly, for anything else -- the caller probes everything on the bus
 * and most of what it finds is not a screen. */
bool virtio_gpu_attach(const struct virtio_device *probed);

/* How many of them were claimed. */
unsigned virtio_gpu_count(void);

/* What they are, and how the presenting actually went -- transfers, flushes,
 * and anything the host refused. Counted because a display that has quietly
 * stopped presenting looks exactly like a machine nobody has drawn on. */
void virtio_gpu_print_summary(void);

/* That a flush actually reaches the host, and that a refused one is reported
 * as refused. True, with a line saying so, on a machine with no such device --
 * most of the matrix has none, and failing there would be failing the machine
 * for what it is. */
bool virtio_gpu_self_test(void);

#endif /* RECON_KERNEL_VIRTIO_GPU_H */
