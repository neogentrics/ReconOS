/* /dev/fb0 -- the screen, as something a program can open and draw on.
 *
 * Checkpoint 21's first half gave the kernel a mode of its own: it asks the
 * display adapter for a size instead of drawing into whatever firmware left.
 * This is the other half. Until now the only thing that could put a pixel on
 * the glass was `fbcon`, which is a text console in the kernel -- and a desktop
 * is not a thing you build out of an eighty-column console.
 *
 * --- Why a mapping, and not just write() ------------------------------------
 *
 * Both are here, and the difference is not convenience.
 *
 * A `write` copies. Drawing a 2560x1440 frame through one is fourteen megabytes
 * *through a system call* per frame, which is the copy plus the boundary
 * crossing plus a second set of dirty cache lines -- for pixels whose only
 * destination is a device that was reachable from the program's own address
 * space the whole time.
 *
 * A mapping hands the program the pages themselves. After it, drawing is stores
 * to memory and the kernel is not involved at all. That is what every compositor
 * that has ever been fast does, and it is the reason this checkpoint is not
 * finished without it.
 *
 * `write` stays because it is the honest fallback: it works on a machine where
 * the mapping is refused, and it gives the self-test a second way to reach the
 * same pixels -- which is the only way to check that the mapping put them where
 * it claimed.
 *
 * --- What makes this different from a file-backed mapping -------------------
 *
 * The kernel has had file-backed mappings since checkpoint 19: a region records
 * a file, a fault allocates a page and fills it from that file. **That is the
 * wrong mechanism here and would look exactly right.** A page filled from a
 * framebuffer is a *copy* of what is on screen; a program drawing into it would
 * see its own pixels read back perfectly and nobody would ever see them.
 *
 * These pages are not filled from the device. They *are* the device. So the
 * file answers a different question -- "what physical memory are you" -- through
 * the `map` operation, and the mapping is built with `addrspace_map`, which has
 * been able to do this since checkpoint 5 and had no caller from user mode.
 */

#include <recon/kernel/boot.h>
#include <recon/kernel/display.h>
#include <recon/kernel/fbcon.h>
#include <recon/kernel/fbdev.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/user.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/vm.h>

/* The screen this machine actually has, or null.
 *
 * Asked of the console rather than of the display driver, and that is
 * deliberate: on a UEFI machine there is no display driver involved at all --
 * firmware set the mode and handed it over. The console is the one place that
 * reconciles "firmware gave us one" with "the driver made one", so it is the
 * one place that knows what screen exists. */
static const struct framebuffer *screen(void)
{
	return fbcon_framebuffer();
}

/* How many bytes of it there are.
 *
 * `pitch * height`, not `size`: a firmware framebuffer's reported size is
 * sometimes the whole BAR and sometimes rounded up, and a program told the
 * screen is larger than it is will draw off the end of it. The rows are the
 * thing that exists. */
static u64 screen_bytes(const struct framebuffer *fb)
{
	return (u64)fb->pitch * fb->height;
}

static i64 fb_read(struct file *f, void *out, u64 len)
{
	const struct framebuffer *fb = screen();
	u64 total, left;

	if (!fb)
		return SYS_ENODEV;
	if (!out)
		return SYS_EFAULT;

	total = screen_bytes(fb);
	if (f->pos >= total)
		return 0;

	/* Subtraction, not addition: `pos + len` past the end of a u64 compares
	 * as comfortably inside, which is the shape every range check in this
	 * kernel is written to avoid. */
	left = total - f->pos;
	if (len > left)
		len = left;

	/* Through the direct map, as device memory. Reading a framebuffer is
	 * slow -- far slower than writing one -- which is why `fbcon` keeps a
	 * shadow rather than reading back what it drew. A program that reads
	 * this is asking for exactly that cost and is entitled to it. */
	kmemcpy(out, (const u8 *)phys_to_virt(fb->base) + f->pos,
		(size_t)len);
	f->pos += len;
	return (i64)len;
}

static i64 fb_write(struct file *f, const void *in, u64 len)
{
	const struct framebuffer *fb = screen();
	u64 total, left;

	if (!fb)
		return SYS_ENODEV;
	if (!in)
		return SYS_EFAULT;

	total = screen_bytes(fb);
	if (f->pos >= total)
		return 0;

	left = total - f->pos;
	if (len > left)
		len = left;

	kmemcpy((u8 *)phys_to_virt(fb->base) + f->pos, in, (size_t)len);

	/* **And then tell the screen, where the screen has to be told.**
	 *
	 * On an adapter that scans out an aperture this is a branch and a
	 * return: the bytes were on the glass the moment they were copied. On
	 * virtio-gpu the copy went into ordinary memory and nothing has seen
	 * it, so a `write` that stopped at the line above would return the full
	 * byte count, having drawn nothing, for ever.
	 *
	 * The rectangle is computed from the byte range rather than being the
	 * whole screen: a program writing one row should not cost a transfer of
	 * the entire panel. Whole rows rather than a tighter box, because a
	 * write is a run of bytes and a run of bytes that crosses a row boundary
	 * is not a rectangle. */
	if (display_needs_flush() && fb->pitch) {
		u32 first = (u32)(f->pos / fb->pitch);
		u32 last  = (u32)((f->pos + len - 1) / fb->pitch);

		display_flush(0, first, fb->width, last - first + 1);
	}

	f->pos += len;
	return (i64)len;
}

static i64 fb_seek(struct file *f, i64 offset, unsigned from)
{
	const struct framebuffer *fb = screen();
	i64 base, total;

	if (!fb)
		return SYS_ENODEV;

	total = (i64)screen_bytes(fb);

	switch (from) {
	case SEEK_START: base = 0; break;
	case SEEK_HERE:  base = (i64)f->pos; break;
	case SEEK_END:   base = total; break;
	default:         return SYS_EINVAL;
	}

	/* Refused rather than clamped. A seek past the end that silently lands
	 * on the end is a caller drawing somewhere it does not think it is. */
	if (offset < -base || base + offset > total)
		return SYS_EINVAL;

	f->pos = (u64)(base + offset);
	return (i64)f->pos;
}

/* What physical memory this file is.
 *
 * The whole of what makes /dev/fb0 different from every other file here.
 *
 * **Write-combining, not uncached, and not write-back.** Uncached is correct
 * and slow: every pixel becomes its own transaction on the bus, which is a
 * compositor running at a speed nobody would accept. Write-back is fast and
 * *wrong*: a pixel sitting in a cache line is a pixel that is not on the
 * screen, and it reaches the screen whenever the cache feels like it. Write
 * combining is the answer the hardware provides for exactly this -- stores are
 * gathered and then pushed through, in order, without being held.
 *
 * The page attribute table that makes that mean anything was programmed in
 * checkpoint 5, on every processor, before any framebuffer was mapped.
 */
static bool fb_map(struct file *f, paddr_t *pa, u64 *len, unsigned *flags)
{
	const struct framebuffer *fb = screen();

	(void)f;

	if (!fb)
		return false;

	*pa    = fb->base;
	*len   = screen_bytes(fb);
	*flags = VM_READ | VM_WRITE | VM_WRITE_COMBINE;
	return true;
}

const struct file_ops fb_file_ops = {
	.read  = fb_read,
	.write = fb_write,
	.seek  = fb_seek,
	.map   = fb_map,
	.name  = "fb",
};

/* What a program needs to know before it can draw anything, and the reason
 * this is here rather than left for the program to work out.
 *
 * A framebuffer is not self-describing. Bytes at an address say nothing about
 * how wide a row is, and `width * 4` is wrong on most real hardware -- a pitch
 * is padded to whatever the adapter wanted. A program that assumed otherwise
 * draws a picture that shears diagonally, which is a memorable way to learn
 * about stride and a poor one.
 *
 * Answered through the same call that answers everything else about the
 * machine, rather than through an ioctl. This kernel has no ioctl and is not
 * getting one: a single entry point taking a number and a pointer, meaning
 * anything at all depending on the number, is the one interface design that
 * cannot be checked.
 */
int fbdev_describe(struct fb_info *out)
{
	const struct framebuffer *fb = screen();

	if (!out)
		return SYS_EFAULT;
	if (!fb)
		return SYS_ENODEV;

	out->width  = fb->width;
	out->height = fb->height;
	out->pitch  = fb->pitch;
	out->format = (u32)fb->format;
	out->bytes  = screen_bytes(fb);
	return SYS_OK;
}
