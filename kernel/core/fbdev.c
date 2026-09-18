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
#include <recon/kernel/console.h>
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

/* Which display a `/dev/fbN` file is about.
 *
 * `f->private` carries it, which is how this kernel says what a file *is*
 * everywhere else -- `struct pipe` in pipe.c, `struct socket` in
 * socket_file.c, read straight back out. devfs puts it there when the file is
 * made.
 *
 * **Null private means the console's screen**, and that is `/dev/fb0` keeping
 * the meaning it has always had: not "display zero" but "the one being drawn
 * on". The two are the same machine until there are two displays, and a change
 * that silently redefined fb0 on the machines that have one would be a change
 * nobody could see going wrong.
 */
static const struct framebuffer *screen_of(struct file *f)
{
	struct display *d = f ? f->private : NULL;

	if (!d)
		return screen();

	if (!d->mode.width || !d->mode.height)
		return NULL;

	return &d->mode;
}

/* And the display itself, for the paths that have to flush it. */
static struct display *display_of(struct file *f)
{
	struct display *d = f ? f->private : NULL;

	return d ? d : display_primary();
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
	const struct framebuffer *fb = screen_of(f);
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
	const struct framebuffer *fb = screen_of(f);
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
	if (display_needs_flush_on(display_of(f)) && fb->pitch) {
		u32 first = (u32)(f->pos / fb->pitch);
		u32 last  = (u32)((f->pos + len - 1) / fb->pitch);

		/* **The display this file is about**, which is the primary only
		 * when the file is /dev/fb0. Writing a row to /dev/fb1 and
		 * presenting the primary would put one screen's pixels on
		 * another's glass -- and on a machine with one display the two
		 * are the same call, so nothing here would have looked wrong. */
		display_flush_on(display_of(f), 0, first, fb->width,
				 last - first + 1);
	}

	f->pos += len;
	return (i64)len;
}

static i64 fb_seek(struct file *f, i64 offset, unsigned from)
{
	const struct framebuffer *fb = screen_of(f);
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
/* --- who owns the panel ----------------------------------------------------
 *
 * **Both the console and a program write to these pixels, through different
 * paths, and the last one wins.** A program maps the screen and fills it; the
 * kernel prints its next line; the console draws characters straight over the
 * top of the picture -- not all of it, only the cells it has text in, so what
 * is left is the program's background showing round the edges of a block of
 * kernel log. It is the second entry in docs/KERNEL-WANTS.md and it was found
 * by photographing a panel, because the serial line said the program had
 * succeeded and it had.
 *
 * It is fatal for a compositor rather than untidy: a compositor owns every
 * pixel, and something else drawing into the middle of that is not a cosmetic
 * problem -- it is the compositor being wrong about what is displayed, and it
 * will not repaint the damage because nothing told it there was any.
 *
 * --- What is claimed, and what is not --------------------------------------
 *
 * The **panel**, and only the panel. The serial port and the log ring keep
 * everything, always, because the verification rig reads the serial port and a
 * rig that cannot see is a rig that cannot fail. A person debugging is reading
 * the cable anyway; a person looking at the glass is looking at the program.
 *
 * --- Tied to the mapping, not to a promise ---------------------------------
 *
 * The claim is taken when a program maps the framebuffer and given back when
 * the file is closed -- and `fd_close_all` closes every descriptor when a
 * process ends, so **a program that dies gives the panel back without having
 * to ask**. That is the whole reason it hangs on the mapping rather than on a
 * call the program makes: a machine whose console went silent because a
 * program crashed is worse than the fault this fixes.
 *
 * Counted rather than a flag, and the count is per-file: two programs may hold
 * mappings, and a file closed without ever having been mapped must not release
 * somebody else's claim. `close` runs on the last reference drop, so a
 * descriptor that was duplicated releases once.
 */
static unsigned panel_claims;

/* Which files are holding a claim.
 *
 * **This used to live in `f->private`**, as a sentinel meaning "this file has
 * mapped the screen". That field now carries which display the file is about,
 * which is what `private` means everywhere else in this kernel -- `struct pipe`
 * in pipe.c, `struct socket` in socket_file.c -- so the claim needed somewhere
 * of its own.
 *
 * A fixed table rather than a per-open allocation, and the reason is the shape
 * of the thing being recorded: pipe and socket allocate because the file *is*
 * that object, whereas here the object is a display, which is shared and
 * outlives every file. An allocation existing only to hold one bool would have
 * to be freed on a path that currently cannot fail.
 *
 * Bounded, and the bound is visible: a claim that will not fit is refused and
 * said out loud rather than silently not taken, because a claim nobody recorded
 * is a console that draws over a program with nothing to explain it. Four
 * displays and a few files each is generous for a machine whose whole point is
 * that one program owns the screen.
 */
#define PANEL_CLAIMS_MAX	8

static struct file *panel_claimed_by[PANEL_CLAIMS_MAX];

static bool panel_claim_take(struct file *f)
{
	unsigned i;

	for (i = 0; i < PANEL_CLAIMS_MAX; i++)
		if (panel_claimed_by[i] == f)
			return true;		/* already holds one */

	for (i = 0; i < PANEL_CLAIMS_MAX; i++) {
		if (panel_claimed_by[i])
			continue;

		panel_claimed_by[i] = f;
		panel_claims++;
		return true;
	}

	kputs("fbdev: no room to record that a program has taken the screen, "
	      "so the console will keep drawing on it\n");
	return false;
}

static bool panel_claim_release(struct file *f)
{
	unsigned i;

	for (i = 0; i < PANEL_CLAIMS_MAX; i++) {
		if (panel_claimed_by[i] != f)
			continue;

		panel_claimed_by[i] = NULL;

		if (panel_claims)
			panel_claims--;

		return true;
	}

	return false;
}

/* A file that is holding one. Any non-null value will do; the address of the
 * counter is used so that a stray pointer landing here is obviously wrong. */
bool fbdev_panel_claimed(void)
{
	return panel_claims != 0;
}

static i64 fb_close(struct file *f)
{
	if (!panel_claim_release(f))
		return SYS_OK;

	/* **And that is all it does.**
	 *
	 * An earlier version repainted the console's window here, reasoning
	 * that leaving the program's last frame on the glass with the console
	 * resuming into it was the original fault arriving one line later. That
	 * is a cosmetic argument and it broke a real check: `user_framebuffer
	 * _test` writes two markers through a mapping, closes the file and then
	 * reads the screen back -- and a repaint on close overwrote the markers
	 * before the read, so the test reported having been handed "a mapping
	 * of something that is not the framebuffer".
	 *
	 * What the want asks for is that the console stop while a program owns
	 * the screen and start again afterwards. It does not ask for the screen
	 * to be taken back the instant the program lets go, and doing that
	 * makes the kernel fight anything that wants to look at what was drawn.
	 */

	return SYS_OK;
}

static bool fb_map(struct file *f, paddr_t *pa, u64 *len, unsigned *flags)
{
	const struct framebuffer *fb = screen_of(f);

	if (!fb)
		return false;

	/* The program is taking the pixels, so the console stops drawing on
	 * them. Taken here rather than at `open`, because opening /dev/fb0 to
	 * ask its geometry or to write a row through it is not taking the
	 * screen -- mapping it is. */
	if (f)
		panel_claim_take(f);

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
	.close = fb_close,
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

/* --- that the nodes name different screens ---------------------------------
 *
 * **The assertion is not that `/dev/fb1` opens.** A kernel where `fb1` is an
 * alias for `fb0` opens it perfectly, reads the same pixels from it, and passes
 * any check that only asks whether the device is there -- which is the failure
 * this is written to catch, because it is what a half-finished version of this
 * change looks like.
 *
 * So what is checked is that each node resolves to the display at its own
 * position, that a node for a display the machine does not have is **absent**
 * rather than empty, and -- on a machine with two -- that the first two are not
 * the same object.
 *
 * It runs on every boot in the matrix. On a machine with one display that is
 * three assertions about `fb0` and three absences; on the two-adapter path it
 * is the whole thing.
 */
bool fbdev_nodes_self_test(void)
{
	unsigned total = display_total();
	unsigned n;
	bool ok = true;
	struct display *seen[4];

	if (!total) {
		kputs("fbdev: no display on this machine, so there are no "
		      "framebuffer nodes to check\n");
		return true;
	}

	for (n = 0; n < 4; n++) {
		char name[4] = { 'f', 'b', (char)('0' + n), 0 };
		i64 err = 0;
		struct file *f;

		seen[n] = NULL;
		f = devfs_open(name, OPEN_READ, 0, &err);

		if (n < total) {
			if (!f) {
				kprintf("fbdev: this machine has %u display(s) "
					"and /dev/%s would not open (%d)\n",
					total, name, (int)err);
				ok = false;
				continue;
			}

			seen[n] = f->private ? f->private : display_primary();

			/* fb0 is the console's screen rather than display
			 * zero, which is the meaning it has always had; every
			 * node above it names a display by position. */
			if (n && seen[n] != display_at(n)) {
				kprintf("fbdev: /dev/%s resolved to a display "
					"that is not the one at position %u\n",
					name, n);
				ok = false;
			}

			file_release(f);
			continue;
		}

		/* Past the end. Absent, not empty. */
		if (f) {
			kprintf("fbdev: this machine has %u display(s) and "
				"/dev/%s opened anyway\n", total, name);
			file_release(f);
			ok = false;
		}
	}

	/* **And that the first two are not the same screen.**
	 *
	 * This is the one that catches an alias, and it can only be asked on a
	 * machine that has two -- which the matrix provides by giving QEMU a
	 * virtio-gpu alongside the adapter it supplies by default. */
	if (total > 1 && seen[0] && seen[1] && seen[0] == seen[1]) {
		kputs("fbdev: /dev/fb0 and /dev/fb1 are the same display, so "
		      "naming a second screen does nothing\n");
		ok = false;
	}

	kprintf("fbdev: %u framebuffer node(s) for %u display(s), and the "
		"ones past the end are absent\n", total, total);

	return ok;
}


int fbdev_describe(struct fb_info *out)
{
	/* The console's screen, not a file's: this answers SYS_SCREEN, which
	 * takes no descriptor. Making it per-display is half of the proposal in
	 * docs/SIGNALS.md and is the kernel session's call, because SYS_SCREEN
	 * is theirs. */
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
