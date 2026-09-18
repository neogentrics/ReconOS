/* Devices as files, and the second filesystem behind the same interface.
 *
 * This exists to answer a question the VFS could not answer on its own: is
 * `struct file_ops` an interface, or is it ReconFS with the serial numbers filed
 * off? One implementation cannot tell you. The console was a start -- it has no
 * filesystem under it -- but it was reached by a special case rather than by a
 * path.
 *
 * So there is a mount table now, and `/dev` is a filesystem with no disk, no
 * blocks, no transaction and no format. `open("/dev/zero")` and
 * `open("/data/notes")` take the same route through the same function and arrive
 * at implementations that have nothing whatever in common. If the interface only
 * fitted a disk, this file could not exist.
 *
 * --- Why these four ---
 *
 * Each is here because something needs it or will, and none is here to make the
 * list look longer:
 *
 *   /dev/console  what the desktop will write to before it has a display.
 *   /dev/null     where output goes when a program must produce it and nobody
 *                 wants it -- which is what makes a program's output
 *                 *optional* without the program knowing.
 *   /dev/zero     a source of zeroed pages, which is what an anonymous mapping
 *                 is made of.
 *   /dev/random   the generator that already exists, reachable by a program
 *                 that was not written for this kernel's system calls.
 *
 * There is no /dev/tty, no block devices, and no directory listing. A device
 * that cannot be opened by name is not in here pretending to be.
 */
#include <recon/kernel/display.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/fbdev.h>
#include <recon/kernel/input.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/random.h>
#include <recon/kernel/user.h>
#include <recon/kernel/rootfs.h>

/* --- /dev/null ------------------------------------------------------------ */

static i64 null_read(struct file *f, void *out, u64 len)
{
	/* Empty, always. Not an error: a program reading from nothing should
	 * see input that has ended. */
	return 0;
}

static i64 null_write(struct file *f, const void *in, u64 len)
{
	/* Accepted and discarded, and it must report the full count. A short
	 * write here would make a program retry forever. */
	return (i64)len;
}

static const struct file_ops null_ops = {
	.read  = null_read,
	.write = null_write,
	.name  = "null",
};

/* --- /dev/zero ------------------------------------------------------------ */

static i64 zero_read(struct file *f, void *out, u64 len)
{
	kmemset(out, 0, (size_t)len);
	f->pos += len;
	return (i64)len;
}

static const struct file_ops zero_ops = {
	.read  = zero_read,
	.write = null_write,		/* the same discard, and the same reason */
	.name  = "zero",
};

/* --- /dev/random ---------------------------------------------------------- */

static i64 random_read(struct file *f, void *out, u64 len)
{
	/* The generator this kernel already has, rather than a second one.
	 * Two sources of randomness in one machine is two things to be sure
	 * of, and the weaker one is the one that gets used by accident. */
	random_bytes(out, (size_t)len);
	f->pos += len;
	return (i64)len;
}

static const struct file_ops random_ops = {
	.read = random_read,
	.name = "random",
};

/* --- the filesystem ------------------------------------------------------- */

/* One name into the caller's buffer, and the running total either way.
 *
 * The total is kept whether or not there is room, because the total *is* the
 * answer when there is not: a caller that offered nothing is asking how much to
 * offer. Writing only while it fits, and counting always, is what lets one walk
 * serve both questions. */
static void emit(const char *name, char *names, u64 names_len, u64 *needed,
		 unsigned *count)
{
	size_t n = kstrlen(name) + 1;

	if (names && *needed + n <= names_len)
		kmemcpy(names + *needed, name, n);

	*needed += n;

	if (count)
		(*count)++;
}

/* Whether `rest` names the directory itself rather than something in it.
 *
 * This filesystem is flat, so the only directory it has is the one it is
 * mounted at -- `rest` is empty for that and a name for anything else. A name
 * is not a directory and saying so is the honest answer: a caller that listed
 * /dev/null and got an empty listing would believe it had found an empty
 * folder. */
static bool is_the_root(const char *rest)
{
	return !rest || !rest[0];
}

struct device_entry {
	const char *name;
	const struct file_ops *ops;
	unsigned allowed;		/* what it may be opened for */
};

static const struct device_entry devices[] = {
	{ "null",    &null_ops,   OPEN_READ | OPEN_WRITE },
	{ "zero",    &zero_ops,   OPEN_READ | OPEN_WRITE },
	{ "random",  &random_ops, OPEN_READ },
	{ "console", NULL,        OPEN_WRITE },	/* built by the VFS itself */
	{ "input",   &input_file_ops, OPEN_READ },
	/* **One node per display, and they are not all real.**
	 *
	 * `fb0` is the screen the console is on and exists whenever there is
	 * one at all -- that is what it has always meant and changing it would
	 * redefine the device every existing program opens. `fb1` upward name
	 * displays by position and are refused when the machine does not have
	 * them, which is why `devfs_open` asks the display layer rather than
	 * trusting this table, and why `devfs_list` leaves out the ones that
	 * are not there. A device that opens and reads nothing is worse than
	 * one that is absent: the first looks like a broken screen. */
	{ "fb0",     &fb_file_ops, OPEN_READ | OPEN_WRITE },
	{ "fb1",     &fb_file_ops, OPEN_READ | OPEN_WRITE },
	{ "fb2",     &fb_file_ops, OPEN_READ | OPEN_WRITE },
	{ "fb3",     &fb_file_ops, OPEN_READ | OPEN_WRITE },
};

/* The display a `fbN` node names, or null for a node this machine has no
 * display for.
 *
 * `fb0` is special and deliberately so: it is the console's screen rather than
 * display zero. On every machine with one display those are the same thing, and
 * on a machine with two the console's choice is the one a program opening
 * `/dev/fb0` has always got. */
static struct display *fb_node_display(const char *name, bool *is_fb)
{
	unsigned n;

	*is_fb = false;

	if (name[0] != 'f' || name[1] != 'b' || !name[2] || name[3])
		return NULL;

	if (name[2] < '0' || name[2] > '9')
		return NULL;

	*is_fb = true;
	n = (unsigned)(name[2] - '0');

	if (n == 0)
		return display_primary();

	return display_at(n);
}

struct file *devfs_open(const char *rest, unsigned flags, u32 mode, i64 *error)
{
	unsigned i;

	*error = SYS_ENOENT;

	/* Creating a device is refused rather than ignored. A program that
	 * asked to make /dev/something and was quietly handed the existing
	 * thing would be writing to a device it believes it owns. */
	if (flags & OPEN_CREATE) {
		*error = SYS_EPERM;
		return NULL;
	}

	for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
		const struct device_entry *d = &devices[i];
		size_t n = kstrlen(d->name);

		/* The whole name, terminator included, so that "zeroes" does not
		 * match "zero". Compared as bytes because this kernel has no
		 * string compare -- and the one place a prefix match would be
		 * wrong is exactly here. */
		if (kstrlen(rest) != n || kmemcmp(rest, d->name, n) != 0)
			continue;

		/* Asked for something it cannot do. Refused at open, where the
		 * program can still choose something else, rather than at the
		 * first read -- by which time it has usually committed. */
		if (flags & ~d->allowed & (OPEN_READ | OPEN_WRITE)) {
			*error = SYS_EPERM;
			return NULL;
		}

		*error = SYS_OK;

		if (!d->ops)
			return file_open_console();

		/* A framebuffer node carries which display it is about, and a
		 * node for a display this machine does not have is not there.
		 *
		 * Refused at open rather than read: a program that opened
		 * /dev/fb2 on a one-screen machine and got a file it could not
		 * usefully read would spend its time deciding whether the
		 * screen was broken. */
		{
			bool is_fb;
			struct display *disp = fb_node_display(d->name, &is_fb);

			if (is_fb) {
				if (!disp) {
					*error = SYS_ENOENT;
					return NULL;
				}

				return file_new_external(d->ops, flags, disp);
			}
		}

		return file_new_external(d->ops, flags, NULL);
	}

	return NULL;
}

i64 devfs_list(const char *rest, char *names, u64 names_len, unsigned *count)
{
	u64 needed = 0;
	unsigned i;

	if (!is_the_root(rest))
		return SYS_ENOENT;

	for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
		bool is_fb;

		/* A framebuffer node for a display this machine does not have
		 * is not listed, for the same reason it will not open. */
		if (!fb_node_display(devices[i].name, &is_fb) && is_fb)
			continue;

		emit(devices[i].name, names, names_len, &needed, count);
	}

	/* Nothing was written if it would not all fit, and the caller can tell
	 * because the answer is larger than what it offered. Half a listing
	 * reported as a whole one is the failure this shape exists to make
	 * impossible. */
	if (needed > names_len && count)
		*count = 0;

	return (i64)needed;
}

void devfs_print_summary(void)
{
	unsigned i;

	kprintf("  /dev         : ");

	for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++)
		kprintf("%s%s", i ? ", " : "", devices[i].name);

	kputs("\n");
}

/* --- the self-test --------------------------------------------------------
 *
 * Everything here is reached by *path*, through the same function a file on a
 * disk goes through. Calling devfs_open directly would test this file; going
 * through file_open_path tests the claim, which is that a program does not have
 * to know which filesystem it is talking to.
 */
bool devfs_self_test(void)
{
	struct file *f;
	i64 err = SYS_OK;
	u8 buf[16];
	unsigned i;
	bool ok = true;

	/* Reads as zeroes, and the buffer is dirtied first so that "it was
	 * already zero" cannot pass for "it was zeroed". */
	kmemset(buf, 0xA5, sizeof(buf));

	f = file_open_path("/dev/zero", OPEN_READ, 0, &err);
	if (!f) {
		kprintf("  devfs: /dev/zero would not open (%ld)\n", (long)err);
		return false;
	}

	if (f->ops->read(f, buf, sizeof(buf)) != (i64)sizeof(buf)) {
		kputs("  devfs: /dev/zero gave the wrong length\n");
		ok = false;
	}

	for (i = 0; i < sizeof(buf); i++)
		if (buf[i] != 0) {
			kputs("  devfs: /dev/zero returned something other "
			      "than zeroes\n");
			ok = false;
			break;
		}

	file_release(f);

	/* Discards, and says it took everything. A short count here makes a
	 * caller loop forever. */
	f = file_open_path("/dev/null", OPEN_WRITE, 0, &err);
	if (!f) {
		kprintf("  devfs: /dev/null would not open (%ld)\n", (long)err);
		return false;
	}

	if (f->ops->write(f, buf, sizeof(buf)) != (i64)sizeof(buf)) {
		kputs("  devfs: /dev/null did not accept everything\n");
		ok = false;
	}

	file_release(f);

	/* And reads as ended, which is different from failing. */
	f = file_open_path("/dev/null", OPEN_READ, 0, &err);
	if (f) {
		if (f->ops->read(f, buf, sizeof(buf)) != 0) {
			kputs("  devfs: reading /dev/null was not the end of "
			      "input\n");
			ok = false;
		}
		file_release(f);
	}

	/* Two reads that come back identical are a generator that is not
	 * advancing -- which every other check here would pass. */
	{
		u8 a[16], b[16];

		f = file_open_path("/dev/random", OPEN_READ, 0, &err);
		if (!f) {
			kprintf("  devfs: /dev/random would not open (%ld)\n",
				(long)err);
			return false;
		}

		f->ops->read(f, a, sizeof(a));
		f->ops->read(f, b, sizeof(b));

		if (kmemcmp(a, b, sizeof(a)) == 0) {
			kputs("  devfs: two reads of /dev/random were "
			      "identical\n");
			ok = false;
		}

		file_release(f);
	}

	/* Opened for something it cannot do, refused at open. */
	f = file_open_path("/dev/random", OPEN_WRITE, 0, &err);
	if (f) {
		kputs("  devfs: /dev/random was opened for writing\n");
		file_release(f);
		ok = false;
	} else if (err != SYS_EPERM) {
		kprintf("  devfs: opening /dev/random for writing gave %ld "
			"rather than a refusal\n", (long)err);
		ok = false;
	}

	/* A name that is not there. ENOENT and not, say, ENODEV: there is no
	 * volume involved and a program that read "no filesystem" would go
	 * looking for a disk. */
	f = file_open_path("/dev/nothing-like-this", OPEN_READ, 0, &err);
	if (f) {
		kputs("  devfs: a device that does not exist was opened\n");
		file_release(f);
		ok = false;
	} else if (err != SYS_ENOENT) {
		kprintf("  devfs: a missing device gave %ld rather than "
			"ENOENT\n", (long)err);
		ok = false;
	}

	/* AND THE ONE THAT PROVES THE MOUNT TABLE.
	 *
	 * /dev is reached without a volume being mounted, and a path outside it
	 * is not. If both went to the same place, one of these two answers
	 * would be wrong -- and on a machine with no disk at all this is the
	 * only thing that can tell a working mount table from a lucky one. */
	if (!rootfs()) {
		f = file_open_path("/somewhere/else", OPEN_READ, 0, &err);

		if (f) {
			kputs("  devfs: a path outside /dev opened on a "
			      "machine with no volume\n");
			file_release(f);
			ok = false;
		} else if (err != SYS_ENODEV) {
			kprintf("  devfs: a path outside /dev gave %ld rather "
				"than \"no filesystem\"\n", (long)err);
			ok = false;
		}
	}

	return ok;
}
