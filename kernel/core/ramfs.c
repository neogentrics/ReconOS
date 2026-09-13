/* Files that live in memory, mounted at /tmp.
 *
 * The third filesystem behind the same interface, and the first one that is
 * *writable and not a disk*. That combination is what it is for.
 *
 * `/dev` proved the interface fits something with no storage under it, but every
 * device there is a fixed thing with a fixed name -- nothing creates one. A file
 * on the volume proved the interface fits a real filesystem, but every write goes
 * through a transaction and a commit. Neither of those is what a program wants
 * when it needs somewhere to put a few kilobytes for a moment.
 *
 * So: names created on demand, contents that grow, and nothing that survives the
 * power going out. A machine with no disk at all has a working /tmp, which is
 * also what makes this testable on every boot in the rig rather than only on the
 * ones that formatted a volume.
 *
 * --- What it deliberately does not do ---
 *
 * **A flat namespace.** `/tmp/notes` is a name, and `/tmp/a/b` is also a name --
 * one containing a slash. There are no directories here, and that is not an
 * oversight to be fixed quietly later: directories bring listing, removal,
 * emptiness and loops, and every one of those is a decision. When something needs
 * a tree in memory it will need those answers too.
 *
 * **No removal.** A name, once made, lasts until the machine stops. That is
 * honest for a first version and it is a real limit, so it is written here rather
 * than discovered when /tmp fills up.
 *
 * **A cap on everything.** A fixed number of files and a fixed size each, both
 * visible numbers. An in-memory filesystem with no limit is a program's typo
 * turning into a machine that stops.
 */
#include <recon/kernel/vfs.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/user.h>

#define RAMFS_FILES_MAX 16
#define RAMFS_NAME_MAX  48
#define RAMFS_BYTES_MAX (64u * 1024u)

struct ram_file {
	bool used;
	char name[RAMFS_NAME_MAX];
	u8 *data;
	u32 len;
	u32 mode;
};

static struct ram_file files[RAMFS_FILES_MAX];
static struct spinlock ramfs_lock = SPINLOCK_INIT("ramfs");
static u64 created, bytes_held;

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

/* The caller holds the lock. */
static struct ram_file *find(const char *name)
{
	unsigned i;
	size_t n = kstrlen(name);

	if (n == 0 || n >= RAMFS_NAME_MAX)
		return NULL;

	for (i = 0; i < RAMFS_FILES_MAX; i++)
		if (files[i].used && kstrlen(files[i].name) == n &&
		    kmemcmp(files[i].name, name, n) == 0)
			return &files[i];

	return NULL;
}

static i64 ram_read(struct file *f, void *out, u64 len)
{
	struct ram_file *r = f->private;
	u64 flags = spin_lock_irq(&ramfs_lock);
	u64 left;

	if (f->pos >= r->len) {
		spin_unlock_irq(&ramfs_lock, flags);
		return 0;
	}

	left = r->len - f->pos;
	if (len > left)
		len = left;

	kmemcpy(out, r->data + f->pos, (size_t)len);
	f->pos += len;

	spin_unlock_irq(&ramfs_lock, flags);
	return (i64)len;
}

static i64 ram_write(struct file *f, const void *in, u64 len)
{
	struct ram_file *r = f->private;
	u64 flags;

	/* Refused rather than trimmed. A program handed a short count knows
	 * something went wrong; one whose write was silently cut in half does
	 * not. */
	if (f->pos > RAMFS_BYTES_MAX || len > RAMFS_BYTES_MAX - f->pos)
		return SYS_ENOSPC;

	flags = spin_lock_irq(&ramfs_lock);

	kmemcpy(r->data + f->pos, in, (size_t)len);
	f->pos += len;

	if (f->pos > r->len) {
		bytes_held += f->pos - r->len;
		r->len = (u32)f->pos;
	}

	spin_unlock_irq(&ramfs_lock, flags);
	return (i64)len;
}

static i64 ram_seek(struct file *f, i64 offset, unsigned from)
{
	struct ram_file *r = f->private;
	i64 base;

	switch (from) {
	case SEEK_START: base = 0; break;
	case SEEK_HERE:  base = (i64)f->pos; break;
	case SEEK_END:   base = (i64)r->len; break;
	default:         return SYS_EINVAL;
	}

	if (offset < -base || base + offset > (i64)RAMFS_BYTES_MAX)
		return SYS_EINVAL;

	f->pos = (u64)(base + offset);
	return (i64)f->pos;
}

/* Nothing to commit and nothing to free: the contents are the file, and the file
 * outlives every descriptor of it. That is the difference between this and a
 * file on the volume, where the close is the write. */
/* Defined below, beside the reasoning for it. */
static u64 ramfs_identity(struct file *f);
static i64 ramfs_write_at(struct file *f, u64 offset, const void *in, u64 len);

static const struct file_ops ram_ops = {
	.read  = ram_read,
	.write = ram_write,
	.seek  = ram_seek,
	.identity = ramfs_identity,
	.write_at = ramfs_write_at,
	.name  = "ramfs",
};

struct file *ramfs_open(const char *rest, unsigned flags, u32 mode, i64 *error)
{
	struct ram_file *r;
	u64 lock;
	unsigned i;

	*error = SYS_OK;

	if (kstrlen(rest) == 0 || kstrlen(rest) >= RAMFS_NAME_MAX) {
		*error = SYS_EINVAL;
		return NULL;
	}

	lock = spin_lock_irq(&ramfs_lock);

	r = find(rest);

	if (r && (flags & OPEN_CREATE)) {
		/* The same refusal the volume makes, for the same reason: a
		 * create that silently replaces is how something run twice
		 * destroys what the first run produced. */
		spin_unlock_irq(&ramfs_lock, lock);
		*error = SYS_EEXIST;
		return NULL;
	}

	if (!r) {
		if (!(flags & OPEN_CREATE)) {
			spin_unlock_irq(&ramfs_lock, lock);
			*error = SYS_ENOENT;
			return NULL;
		}

		for (i = 0; i < RAMFS_FILES_MAX; i++)
			if (!files[i].used) {
				r = &files[i];
				break;
			}

		if (!r) {
			spin_unlock_irq(&ramfs_lock, lock);
			*error = SYS_ENOSPC;
			return NULL;
		}

		/* Allocated outside the lock would be the better shape, and it
		 * would mean two threads creating the same name both getting a
		 * slot. The allocation is small and the lock is held briefly;
		 * the alternative is a second lookup and a race to lose. */
		r->data = kzalloc(RAMFS_BYTES_MAX);
		if (!r->data) {
			spin_unlock_irq(&ramfs_lock, lock);
			*error = SYS_ENOSPC;
			return NULL;
		}

		kstrlcpy(r->name, rest, sizeof(r->name));
		r->len = 0;
		r->mode = mode;
		r->used = true;
		created++;
	}

	spin_unlock_irq(&ramfs_lock, lock);

	return file_new_external(&ram_ops, flags, r);
}

/* The slot, plus one so that it is never zero.
 *
 * Stable for the life of the machine, which is the property the cache needs
 * and which this filesystem happens to guarantee for free: a name here, once
 * made, is never removed -- see the header, where the absence of removal is
 * written down as a decision rather than an omission. So a slot is a file, for
 * ever, and two opens of one name find the same slot.
 */
/* A file here *is* memory, so putting bytes back is a copy and nothing else
 * has to happen afterwards. That is the whole reason ramfs can offer a shared
 * writable mapping and the volume cannot: there is no commit to fail.
 *
 * The length is not extended. A shared mapping is a window onto what the file
 * already holds, and a page of it reaching past the end is padding the cache
 * put there -- writing that back would grow the file to a page boundary every
 * time anybody mapped it. */
static i64 ramfs_write_at(struct file *f, u64 offset, const void *in, u64 len)
{
	struct ram_file *r = f ? f->private : NULL;
	u64 flags;

	if (!r)
		return SYS_EBADF;

	if (offset >= RAMFS_BYTES_MAX)
		return SYS_ENOSPC;

	if (len > RAMFS_BYTES_MAX - offset)
		len = RAMFS_BYTES_MAX - offset;

	flags = spin_lock_irq(&ramfs_lock);

	if (offset > r->len)
		len = 0;		/* past the end: nothing to put back */
	else if (len > r->len - offset)
		len = r->len - offset;

	if (len)
		kmemcpy(r->data + offset, in, (size_t)len);

	spin_unlock_irq(&ramfs_lock, flags);
	return (i64)len;
}

static u64 ramfs_identity(struct file *f)
{
	struct ram_file *rf = f ? f->private : NULL;

	if (!rf)
		return 0;

	return (u64)(rf - files) + 1;
}

i64 ramfs_list(const char *rest, char *names, u64 names_len, unsigned *count)
{
	u64 needed = 0;
	unsigned i;
	u64 flags;

	/* Flat, so the mount point is the only directory there is -- and a name
	 * here may contain a slash, which is why `/tmp/a/b` is a file rather
	 * than something inside `/tmp/a`. See the header: the absence of
	 * directories is a decision, and this is what it looks like from the
	 * outside. */
	if (rest && rest[0])
		return SYS_ENOENT;

	flags = spin_lock_irq(&ramfs_lock);

	for (i = 0; i < RAMFS_FILES_MAX; i++)
		if (files[i].used)
			emit(files[i].name, names, names_len, &needed, count);

	spin_unlock_irq(&ramfs_lock, flags);

	if (needed > names_len && count)
		*count = 0;

	return (i64)needed;
}

void ramfs_print_summary(void)
{
	kprintf("  /tmp         : %lu file(s), %lu byte(s), up to %u files of "
		"%u KB\n", created, bytes_held, (unsigned)RAMFS_FILES_MAX,
		(unsigned)(RAMFS_BYTES_MAX / 1024));
}

/* --- the self-test --------------------------------------------------------
 *
 * Runs on every machine, including the ones with no disk, which is half the
 * point of it existing.
 *
 * The assertion that matters is the one that separates this from /dev: a name
 * that did not exist is created, and what is written to it is still there when
 * it is opened again through a *different* descriptor. A filesystem that handed
 * back a fresh empty buffer each time would pass every other check here.
 */
bool ramfs_self_test(void)
{
	static const char message[] = "kept in memory, not on a disk";
	struct file *f;
	i64 err = SYS_OK;
	char back[64];
	bool ok = true;

	/* Not there yet, and that is ENOENT rather than a fresh empty file. */
	f = file_open_path("/tmp/notes", OPEN_READ, 0, &err);
	if (f) {
		kputs("  ramfs: a file that was never created opened anyway\n");
		file_release(f);
		ok = false;
	} else if (err != SYS_ENOENT) {
		kprintf("  ramfs: opening a missing file gave %ld rather than "
			"ENOENT\n", (long)err);
		ok = false;
	}

	f = file_open_path("/tmp/notes", OPEN_WRITE | OPEN_CREATE, 0600, &err);
	if (!f) {
		kprintf("  ramfs: could not create a file (%ld)\n", (long)err);
		return false;
	}

	if (f->ops->write(f, message, sizeof(message)) != (i64)sizeof(message)) {
		kputs("  ramfs: a write was short\n");
		ok = false;
	}

	file_release(f);

	/* A second descriptor onto the same name. This is the assertion: the
	 * contents outlive the descriptor that wrote them, which is what makes
	 * it a filesystem rather than a scratch buffer. */
	f = file_open_path("/tmp/notes", OPEN_READ, 0, &err);
	if (!f) {
		kprintf("  ramfs: could not open what was just written "
			"(%ld)\n", (long)err);
		return false;
	}

	if (f->ops->read(f, back, sizeof(back)) != (i64)sizeof(message) ||
	    kmemcmp(back, message, sizeof(message)) != 0) {
		kputs("  ramfs: what came back was not what was written\n");
		ok = false;
	}

	file_release(f);

	/* Creating it again is refused, the same way the volume refuses. */
	f = file_open_path("/tmp/notes", OPEN_WRITE | OPEN_CREATE, 0600, &err);
	if (f) {
		kputs("  ramfs: creating a name that already exists was "
		      "allowed\n");
		file_release(f);
		ok = false;
	} else if (err != SYS_EEXIST) {
		kprintf("  ramfs: creating an existing name gave %ld rather "
			"than EEXIST\n", (long)err);
		ok = false;
	}

	/* And a write past the cap is refused rather than trimmed. */
	f = file_open_path("/tmp/big", OPEN_WRITE | OPEN_CREATE, 0600, &err);
	if (f) {
		if (f->ops->seek(f, RAMFS_BYTES_MAX - 4, SEEK_START) < 0) {
			kputs("  ramfs: seeking to the end of the cap "
			      "failed\n");
			ok = false;
		} else if (f->ops->write(f, message, sizeof(message)) !=
			   SYS_ENOSPC) {
			kputs("  ramfs: a write past the cap was not "
			      "refused\n");
			ok = false;
		}

		file_release(f);
	}

	return ok;
}
