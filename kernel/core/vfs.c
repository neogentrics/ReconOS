#include <recon/kernel/vfs.h>
#include <recon/kernel/pagecache.h>
#include <recon/kernel/identity.h>
#include <recon/kernel/process.h>
#include <recon/kernel/rootfs.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/user.h>

/* One lock for every descriptor table in the machine.
 *
 * Not one per process, and that is a choice with a reason rather than an
 * oversight. A per-process lock is the right answer once descriptor operations
 * are frequent enough to contend, and nothing in this kernel opens files in a
 * loop yet. What a single lock buys today is that there is one place to reason
 * about, and the ordering rule -- this lock is never held across a read, a
 * write or a close -- is visible in one file instead of being a convention.
 *
 * That rule is what matters. A close commits a transaction to a disk; holding a
 * lock across it would stop every other process in the machine from touching a
 * descriptor for the length of a write to storage. */
static struct spinlock fd_lock = SPINLOCK_INIT("fds");

static u64 opens, closes, reads, writes;

/* --- references ---------------------------------------------------------- */

struct file *file_hold(struct file *f)
{
	if (f)
		__atomic_add_fetch(&f->refs, 1, __ATOMIC_RELAXED);

	return f;
}

i64 file_release(struct file *f)
{
	i64 st = SYS_OK;

	if (!f)
		return SYS_EINVAL;

	/* The last drop closes it. Acquire on the way down so that everything
	 * the other holders did is visible to whoever runs the close: the
	 * decrement is the handover, and a close that ran before the previous
	 * holder's writes were visible would commit a stale buffer. */
	if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) != 0)
		return SYS_OK;

	if (f->ops->close)
		st = f->ops->close(f);

	closes++;
	kfree(f);
	return st;
}

/* Common to every implementation: allocate the file, not the thing it is a
 * file of. */
static struct file *file_new(const struct file_ops *ops, unsigned flags,
			     void *private)
{
	struct file *f = kzalloc(sizeof(*f));

	if (!f)
		return NULL;

	f->ops = ops;
	f->refs = 1;
	f->flags = flags;
	f->pos = 0;
	f->private = private;

	opens++;
	return f;
}

struct file *file_new_external(const struct file_ops *ops, unsigned flags,
			       void *private)
{
	return file_new(ops, flags, private);
}

/* --- the console --------------------------------------------------------- */

/* A file with no filesystem under it at all.
 *
 * It is here first, and deliberately: an interface that fits only the thing it
 * was written for is not an interface. The console has no path, no size, no
 * position and nothing to read, and it has to work through the same four
 * functions as a file on a disk. Everything it leaves null -- read, seek -- is
 * a question the layer above has to answer once rather than a stub returning an
 * error in every implementation.
 */
static i64 console_write(struct file *f, const void *in, u64 len)
{
	const char *p = in;
	u64 i;

	for (i = 0; i < len; i++)
		kputc(p[i]);

	return (i64)len;
}

static const struct file_ops console_ops = {
	.write = console_write,
	.name  = "console",
};

struct file *file_open_console(void)
{
	return file_new(&console_ops, OPEN_WRITE, NULL);
}

/* --- nothing ------------------------------------------------------------- */

/* What descriptor 0 is until there is something to read from.
 *
 * A read returns zero, which is the end of input, and that is a real answer
 * rather than a placeholder: a program reading from a machine with no keyboard
 * should see input that has ended, not an error it has no way to interpret.
 */
static i64 empty_read(struct file *f, void *out, u64 len)
{
	return 0;
}

static const struct file_ops empty_ops = {
	.read = empty_read,
	.name = "empty",
};

static struct file *file_open_empty(void)
{
	return file_new(&empty_ops, OPEN_READ, NULL);
}

/* --- a file on the mounted volume ---------------------------------------- */

/* THE WHOLE FILE IS HELD IN MEMORY, and this is the honest limitation of this
 * first VFS rather than a detail.
 *
 * ReconFS reads and writes whole files: `reconfs_read_named` refuses a file
 * larger than the buffer given rather than returning part of it, and there is
 * no call that reads from an offset. So a descriptor over one has to hold the
 * contents, and the size a program can open is bounded by what the heap will
 * give.
 *
 * A file too large is **refused at open**, with an error the program can read,
 * rather than opened and then found to be short. The alternative -- opening it
 * and serving the first N bytes -- is the shape of fault this project keeps
 * deciding not to build: a caller handed part of a file with a success status
 * cannot tell.
 *
 * Writes are buffered and committed by close, in one transaction, because that
 * is what the filesystem underneath offers. **That makes close the place where
 * a write fails**, which the header says loudly and which every caller here
 * checks.
 */
#define VFS_FILE_MAX (256u * 1024u)

struct disk_file {
	char path[VFS_PATH_MAX];

	/* What the filesystem calls this object, resolved once when it is
	 * opened rather than on every fault: the page cache asks for it per
	 * page, and walking a path down from the root to answer would make
	 * reading a mapped file slower than not caching it at all.
	 *
	 * Zero for a file that did not exist yet -- one being created has no
	 * dossier until it is committed, and nothing can have mapped it. */
	u64 dossier;
	u8 *data;
	u32 len;		/* how much is valid */
	u32 capacity;
	u32 mode;
	bool dirty;
};

static i64 disk_read(struct file *f, void *out, u64 len)
{
	struct disk_file *d = f->private;
	u64 left;

	if (f->pos >= d->len)
		return 0;

	left = d->len - f->pos;
	if (len > left)
		len = left;

	kmemcpy(out, d->data + f->pos, (size_t)len);
	f->pos += len;
	return (i64)len;
}

static i64 disk_write(struct file *f, const void *in, u64 len)
{
	struct disk_file *d = f->private;

	/* Past the end of what was allocated is refused, not grown. Growing
	 * would mean a reallocation inside a write, and a write that can fail
	 * for a reason unrelated to the disk. The cap is stated at open. */
	if (f->pos > d->capacity || len > d->capacity - f->pos)
		return SYS_ENOSPC;

	kmemcpy(d->data + f->pos, in, (size_t)len);
	f->pos += len;

	if (f->pos > d->len)
		d->len = (u32)f->pos;

	d->dirty = true;
	return (i64)len;
}

static i64 disk_seek(struct file *f, i64 offset, unsigned from)
{
	i64 base;

	switch (from) {
	case SEEK_START: base = 0; break;
	case SEEK_HERE:  base = (i64)f->pos; break;
	case SEEK_END:   base = (i64)((struct disk_file *)f->private)->len; break;
	default:         return SYS_EINVAL;
	}

	/* Before the start is refused. After the end is allowed as far as the
	 * buffer goes, because a program that seeks forward and writes is
	 * ordinary; past the buffer it would be a position no write could ever
	 * use, so it is refused where it is asked rather than later. */
	if (offset < -base)
		return SYS_EINVAL;

	if (base + offset > (i64)((struct disk_file *)f->private)->capacity)
		return SYS_EINVAL;

	f->pos = (u64)(base + offset);
	return (i64)f->pos;
}

static u64 disk_identity(struct file *f)
{
	struct disk_file *d = f ? f->private : NULL;

	return d ? d->dossier : 0;
}

static i64 disk_close(struct file *f)
{
	struct disk_file *d = f->private;
	i64 st = SYS_OK;

	if (d->dirty) {
		/* Replace if it was already there when this was opened, create
		 * if it was not.
		 *
		 * Decided by the dossier, which is exactly the question being
		 * asked: a file that had one at open is a file that existed.
		 * And the two are separate calls on purpose -- create refuses
		 * to overwrite, because a create that silently replaces is how
		 * running a key-generation routine twice destroys the key that
		 * was working. Something has to choose, and this is the layer
		 * that knows which the caller meant: it opened a name that was
		 * already there. */
		enum reconfs_status r =
			(f->flags & OPEN_REPLACE)
			? rootfs_replace_file(d->path, d->data, d->len)
			: rootfs_create_file(d->path, d->mode, d->data,
					     d->len);

		st = user_status_from_reconfs(r);

		/* The contents changed and the name for them did not, which is
		 * the whole reason a dossier can be a cache key and the whole
		 * reason the cache has to be told. Under a block number this
		 * line would be unnecessary and the cache would be useless.
		 *
		 * After the commit rather than before: a write that failed
		 * changed nothing, and throwing away good pages because a
		 * commit did not happen is a slower machine for no reason. */
		if (d->dossier && r == RECONFS_OK)
			pagecache_forget(f);
	}

	kfree(d->data);
	kfree(d);
	return st;
}

static const struct file_ops disk_ops = {
	.read  = disk_read,
	.write = disk_write,
	.seek  = disk_seek,
	.close = disk_close,
	.identity = disk_identity,
	.name  = "reconfs",
};

static struct file *reconfs_open(const char *path, unsigned flags, u32 mode,
				 i64 *error)
{
	struct disk_file *d;
	struct file *f;
	u32 got = 0;

	*error = SYS_OK;

	/* "There is no filesystem" and "no such file" are different answers,
	 * and a program that cannot tell them apart cannot decide what to do.
	 * The first is the ordinary state of a machine booted from install
	 * media. */
	if (!rootfs()) {
		*error = SYS_ENODEV;
		return NULL;
	}

	if (kstrlen(path) >= VFS_PATH_MAX) {
		*error = SYS_EINVAL;
		return NULL;
	}

	d = kzalloc(sizeof(*d));
	if (!d) {
		*error = SYS_ENOSPC;
		return NULL;
	}

	kstrlcpy(d->path, path, sizeof(d->path));
	d->mode = mode;

	/* Resolved once, here, because the page cache asks per page and a walk
	 * from the root on every fault would cost more than the copying it
	 * saves. Zero for a file that does not exist yet, which cannot have
	 * been mapped by anybody. */
	if (rootfs_owner_of(path, 0, 0, 0, &d->dossier) != RECONFS_OK)
		d->dossier = 0;

	/* Said now rather than at the close. A program that asked to replace a
	 * file which is not there has made a mistake it can still do something
	 * about; told two hundred lines later, when it has already written its
	 * contents into a buffer, it cannot. */
	if ((flags & OPEN_REPLACE) && !d->dossier) {
		kfree(d);
		*error = SYS_ENOENT;
		return NULL;
	}


	/* **Asked before anything is read.**
	 *
	 * A file that exists is looked at first: its mode, its owner and its
	 * group, and whether whoever is running may do what they asked for.
	 * Reading the contents and *then* checking is not a permission check --
	 * the bytes are already in the kernel and one mistake later they are in
	 * the program.
	 *
	 * A file being created has no owner yet, so there is nothing to ask
	 * about; it gets the mode the caller passed and belongs to whoever made
	 * it. Directories above it are not consulted, which identity.h says out
	 * loud rather than leaving to be assumed. */
	if (!(flags & OPEN_CREATE)) {
		u32 fmode = 0, fuid = UID_KERNEL, fgid = UID_KERNEL;

		if (rootfs_owner_of(path, &fmode, &fuid, &fgid, 0) == RECONFS_OK &&
		    !identity_may(fmode, fuid, fgid,
				  identity_want_from_open(flags))) {
			kfree(d->data);
			kfree(d);
			*error = SYS_EPERM;
			return NULL;
		}
	}
	d->capacity = VFS_FILE_MAX;

	d->data = kmalloc(d->capacity);
	if (!d->data) {
		kfree(d);
		*error = SYS_ENOSPC;
		return NULL;
	}

	if (flags & OPEN_CREATE) {
		/* Nothing is read and nothing is written yet. The file appears
		 * on the volume when the descriptor is closed, carrying its
		 * mode, in one transaction -- which is the property
		 * rootfs_create_file exists to provide and the reason this does
		 * not create an empty file now and fill it in later. */
		d->len = 0;
		d->dirty = true;
	} else {
		enum reconfs_status r =
			rootfs_read_file(path, d->data, d->capacity, &got,
					 &d->mode);

		if (r != RECONFS_OK) {
			*error = user_status_from_reconfs(r);
			kfree(d->data);
			kfree(d);
			return NULL;
		}

		d->len = got;
	}

	f = file_new(&disk_ops, flags, d);
	if (!f) {
		kfree(d->data);
		kfree(d);
		*error = SYS_ENOSPC;
		return NULL;
	}

	return f;
}

/* --- the mount table ------------------------------------------------------
 *
 * Which filesystem a path belongs to, decided in one place by the longest
 * prefix that matches. It is four lines of table and it is what turns a file
 * interface into a *virtual* file system: before it, "open" meant ReconFS and
 * the console was reached by a special case; after it, a path is looked up and
 * whatever answers is what the program talks to.
 *
 * Longest prefix wins, so "/" can be the root without swallowing "/dev".
 * Written as a length rather than as an ordering, because a table that only
 * works in the order it happens to be written is a table somebody will
 * reorder.
 *
 * There is no mount() call and no unmounting. Both are real and neither is
 * needed yet: this kernel has one volume, found at boot, and inventing the
 * calls before something wants them gets their arguments wrong.
 */
struct mount {
	const char *prefix;
	struct file *(*open)(const char *rest, unsigned flags, u32 mode,
			     i64 *error);

	/* Null on a filesystem that cannot be listed at all, which is a
	 * different answer from one that lists nothing -- and there is no such
	 * filesystem here yet, so this is the shape rather than a case. */
	i64 (*list)(const char *rest, char *names, u64 names_len,
		    unsigned *count);

	const char *name;
};

/* Listing the mounted volume, which is the only one of the three with owners
 * to consult.
 *
 * The check is here rather than in rootfs.c for the same reason every other
 * one is: the policy lives in identity.c and is asked in one place. Reading a
 * directory is a read, so that is what is asked for -- and it is asked *before*
 * the names are fetched, because names already in a kernel buffer are one
 * mistake away from being in the caller's. */
static i64 reconfs_list_path(const char *rest, char *names, u64 names_len,
			     unsigned *count)
{
	u64 needed = 0;
	u32 mode = 0, uid = UID_KERNEL, gid = UID_KERNEL;
	enum reconfs_status st;

	if (!rootfs())
		return SYS_ENODEV;

	if (rootfs_owner_of(rest, &mode, &uid, &gid, 0) == RECONFS_OK &&
	    !identity_may(mode, uid, gid, ACCESS_READ))
		return SYS_EPERM;

	st = rootfs_list(rest, names, names_len, &needed, count);

	if (st == RECONFS_ERR_NOT_FOUND)
		return SYS_ENOENT;

	if (st != RECONFS_OK)
		return SYS_EIO;

	return (i64)needed;
}

static const struct mount mounts[] = {
	{ "/dev/", devfs_open,   devfs_list,   "devfs"  },
	{ "/proc/", procfs_open, procfs_list,  "procfs" },
	{ "/tmp/", ramfs_open,   ramfs_list,   "ramfs"  },
	{ "/",     reconfs_open, reconfs_list_path, "reconfs" },
};

/* Which mount owns a path. Split out of file_open_path rather than copied into
 * the listing below, because two longest-prefix matches would eventually
 * disagree about which filesystem owns a name -- and the way they would
 * disagree is a file that can be opened through one and listed through the
 * other. */
static const struct mount *mount_for(const char *path, size_t *prefix_len)
{
	unsigned i;
	size_t best = 0;
	const struct mount *chosen = NULL;

	for (i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
		size_t n = kstrlen(mounts[i].prefix);

		if (n >= best && kmemcmp(path, mounts[i].prefix, n) == 0) {
			best = n;
			chosen = &mounts[i];
		}
	}

	if (chosen && prefix_len)
		*prefix_len = best;

	return chosen;
}

i64 file_list_path(const char *path, char *names, u64 names_len,
		   unsigned *count)
{
	const struct mount *m;
	size_t best = 0;

	if (count)
		*count = 0;

	if (!path || path[0] != '/')
		return SYS_EINVAL;

	/* A buffer is only required to exist if one was offered. Asking with no
	 * room at all is the documented way to find out how much is needed. */
	if (names_len && !names)
		return SYS_EFAULT;

	m = mount_for(path, &best);

	if (!m || !m->list)
		return SYS_ENOENT;

	return m->list(path + (best > 1 ? best : 0), names, names_len, count);
}

struct file *file_open_path(const char *path, unsigned flags, u32 mode,
			    i64 *error)
{
	size_t best = 0;
	const struct mount *chosen;

	*error = SYS_EINVAL;

	if (!path || path[0] != '/')
		return NULL;

	chosen = mount_for(path, &best);

	if (!chosen)
		return NULL;

	/* The rest of the path, after the prefix. A filesystem is handed what it
	 * owns and never sees where it was mounted, which is what would let the
	 * same one be mounted twice. The root keeps its leading slash, because
	 * for it the prefix *is* the slash. */
	return chosen->open(path + (best > 1 ? best : 0), flags, mode, error);
}

/* --- the descriptor table ------------------------------------------------ */

int fd_install(struct process *p, struct file *f)
{
	u64 flags;
	int i;

	if (!p || !f)
		return SYS_EINVAL;

	flags = spin_lock_irq(&fd_lock);

	/* The lowest free number, which is a promise programs rely on: a
	 * program that closes descriptor 0 and opens something expects the new
	 * thing to be descriptor 0. */
	for (i = 0; i < PROCESS_FDS_MAX; i++)
		if (!p->fds[i]) {
			p->fds[i] = file_hold(f);
			spin_unlock_irq(&fd_lock, flags);
			return i;
		}

	spin_unlock_irq(&fd_lock, flags);
	return SYS_EMFILE;
}

struct file *fd_get(struct process *p, int fd)
{
	struct file *f = NULL;
	u64 flags;

	if (!p || fd < 0 || fd >= PROCESS_FDS_MAX)
		return NULL;

	flags = spin_lock_irq(&fd_lock);

	/* A reference is taken *under the lock*, and that is the whole point of
	 * this function existing rather than callers reading the array.
	 * Without it, another thread of the same process closing that
	 * descriptor between the read and the use would free the file while it
	 * was being read from. */
	if (p->fds[fd])
		f = file_hold(p->fds[fd]);

	spin_unlock_irq(&fd_lock, flags);
	return f;
}

i64 fd_close(struct process *p, int fd)
{
	struct file *f;
	u64 flags;

	if (!p || fd < 0 || fd >= PROCESS_FDS_MAX)
		return SYS_EINVAL;

	flags = spin_lock_irq(&fd_lock);
	f = p->fds[fd];
	p->fds[fd] = NULL;
	spin_unlock_irq(&fd_lock, flags);

	if (!f)
		return SYS_EINVAL;

	/* Released outside the lock, because the last release commits to a
	 * disk. */
	return file_release(f);
}

void fd_close_all(struct process *p)
{
	int i;

	for (i = 0; i < PROCESS_FDS_MAX; i++)
		fd_close(p, i);
}

void fd_open_standard(struct process *p)
{
	struct file *in = file_open_empty();
	struct file *out = file_open_console();
	struct file *err = file_open_console();

	/* Installed in order, so they land on 0, 1 and 2 -- and each is
	 * released afterwards because install took its own reference. A file
	 * that could not be made leaves a hole rather than shifting the others
	 * down, since a program writing to descriptor 2 must not find itself
	 * writing to descriptor 1's thing. */
	if (in) {
		fd_install(p, in);
		file_release(in);
	}

	if (out) {
		fd_install(p, out);
		file_release(out);
	}

	if (err) {
		fd_install(p, err);
		file_release(err);
	}
}

void vfs_print_summary(void)
{
	kprintf("\nOpen files\n");
	kprintf("  activity     : %lu opened, %lu closed, %lu reads, "
		"%lu writes\n", opens, closes, reads, writes);
	kprintf("  a process    : up to %u descriptors, shared by its threads\n",
		(unsigned)PROCESS_FDS_MAX);
}

void vfs_note_read(void)  { reads++; }
void vfs_note_write(void) { writes++; }

/* --- the self-test --------------------------------------------------------
 *
 * Two of them, because two different things are worth proving and they need
 * different machines.
 *
 * This one runs on every boot and needs no filesystem at all. That is not a
 * convenience -- it is the assertion. If the descriptor layer needed a volume to
 * be tested, it would be a filesystem interface rather than a file interface,
 * and the console would not fit through it.
 *
 * It uses an implementation defined here, in the test, which the rest of the
 * kernel has never heard of. A `struct file_ops` that only the code that
 * invented it can satisfy is not an interface, and the cheapest way to find that
 * out is to write a fourth implementation from outside.
 */
struct counted {
	unsigned closes;
	unsigned reads;
	u64 value;
};

static struct counted test_counted;

static i64 counted_read(struct file *f, void *out, u64 len)
{
	struct counted *c = f->private;

	if (len < sizeof(c->value))
		return SYS_EINVAL;

	kmemcpy(out, &c->value, sizeof(c->value));
	c->reads++;
	f->pos += sizeof(c->value);
	return (i64)sizeof(c->value);
}

static i64 counted_close(struct file *f)
{
	((struct counted *)f->private)->closes++;
	return SYS_OK;
}

static const struct file_ops counted_ops = {
	.read  = counted_read,
	.close = counted_close,
	.name  = "counted",
};

/* --- listing ---------------------------------------------------------------
 *
 * Every assertion here is about the *shape* of the answer rather than its
 * contents, because the contents differ by machine -- a boot with no volume
 * lists no volume -- and a test that depended on them would pass on one path
 * and fail on another for no fault of the kernel's.
 *
 * The one that matters: **a listing that does not fit writes nothing.** An
 * implementation that filled the buffer as far as it went and reported the
 * full size would satisfy every other check here, and would hand a file
 * manager the first half of a folder with no way to tell.
 */
bool vfs_list_self_test(void)
{
	char small[4];
	char big[512];
	unsigned count = 0;
	i64 whole, again;
	bool ok = true;
	unsigned i;

	/* /dev is the one directory every boot has, whether or not a disk was
	 * ever found, which is why the checks below are written against it. */
	whole = file_list_path("/dev/", NULL, 0, &count);

	if (whole <= 0) {
		kprintf("  vfs: listing /dev asked for %lld bytes\n",
			(long long)whole);
		return false;
	}

	/* Asking with no room is how a caller finds out how much it needs, so
	 * it must not have been treated as an empty listing. */
	if (count != 0) {
		kputs("  vfs: a listing nobody had room for reported names "
		      "anyway\n");
		ok = false;
	}

	/* Too small, by construction: /dev has more than four bytes of names.
	 * Poisoned first, so that anything written shows up. */
	for (i = 0; i < sizeof(small); i++)
		small[i] = (char)0xA5;

	again = file_list_path("/dev/", small, sizeof(small), &count);

	if (again != whole) {
		kprintf("  vfs: the size of a listing changed between two "
			"asks, %lld then %lld\n",
			(long long)whole, (long long)again);
		ok = false;
	}

	for (i = 0; i < sizeof(small); i++)
		if (small[i] != (char)0xA5) {
			kputs("  vfs: a listing too big for the buffer wrote "
			      "into it anyway, so a caller gets half a "
			      "directory and a success\n");
			ok = false;
			break;
		}

	if (count != 0) {
		kputs("  vfs: a listing that did not fit reported a count\n");
		ok = false;
	}

	/* And with room, all of it arrives: as many NUL-terminated names as the
	 * count claims, ending exactly where the size said. */
	count = 0;
	again = file_list_path("/dev/", big, sizeof(big), &count);

	if (again != whole || count == 0) {
		kprintf("  vfs: listing /dev with room gave %lld bytes and %u "
			"names\n", (long long)again, count);
		return false;
	}

	{
		u64 walked = 0;
		unsigned seen = 0;

		while (walked < (u64)whole && seen <= count) {
			walked += kstrlen(big + walked) + 1;
			seen++;
		}

		if (walked != (u64)whole || seen != count) {
			kprintf("  vfs: %u names walked to %llu bytes, but the "
				"listing said %u names in %lld\n",
				seen, (unsigned long long)walked, count,
				(long long)whole);
			ok = false;
		}
	}

	/* A name is not a directory. Listing one has to say so rather than
	 * answering with nothing, which would read as an empty folder. */
	if (file_list_path("/dev/null", big, sizeof(big), &count) >= 0) {
		kputs("  vfs: /dev/null listed as though it were a "
		      "directory\n");
		ok = false;
	}

	if (file_list_path("nowhere", big, sizeof(big), &count) >= 0) {
		kputs("  vfs: a path that does not start at the root was "
		      "listed\n");
		ok = false;
	}

	return ok;
}

bool vfs_self_test(void)
{
	struct process *p;
	struct file *f;
	struct file *held;
	int a, b;
	u64 back = 0;
	bool ok = true;

	p = process_create("vfs-test", 0, 0, 0);
	if (!p) {
		kputs("  vfs: no process to hold descriptors\n");
		return false;
	}

	/* Every process is born with three, and they are the three programs
	 * expect. A program that writes to descriptor 2 and finds it is not
	 * there writes its error message nowhere. */
	if (!p->fds[0] || !p->fds[1] || !p->fds[2]) {
		kputs("  vfs: a new process did not get its first three "
		      "descriptors\n");
		ok = false;
	}

	/* The console is write-only, and that is expressed by *absence* rather
	 * than by a stub. A read function that returns an error would make
	 * "cannot read" indistinguishable from "the read failed". */
	if (p->fds[1] && p->fds[1]->ops->read) {
		kputs("  vfs: the console claims it can be read\n");
		ok = false;
	}

	test_counted.closes = 0;
	test_counted.reads = 0;
	test_counted.value = 0x5645524946494544ULL;	/* "VERIFIED" */

	f = file_new(&counted_ops, OPEN_READ, &test_counted);
	if (!f) {
		kputs("  vfs: no memory for a file\n");
		process_thread_ended(NULL, 0);
		return false;
	}

	/* Installed twice, which is what a pipe and what dup will both do: one
	 * file, two descriptors. */
	a = fd_install(p, f);
	b = fd_install(p, f);

	if (a < 0 || b < 0 || a == b) {
		kprintf("  vfs: two installs gave %d and %d\n", a, b);
		ok = false;
	}

	/* The lowest free number, which programs rely on. Three are taken, so
	 * these are 3 and 4. */
	if (a != 3 || b != 4) {
		kprintf("  vfs: the first free descriptors were %d and %d "
			"rather than 3 and 4\n", a, b);
		ok = false;
	}

	file_release(f);		/* the two descriptors hold it now */

	/* A descriptor that names nothing, and one out of range. Both are
	 * errors a program can act on rather than a fault. */
	if (fd_get(p, 9) || fd_get(p, -1) || fd_get(p, PROCESS_FDS_MAX)) {
		kputs("  vfs: a descriptor that names nothing returned a "
		      "file\n");
		ok = false;
	}

	held = fd_get(p, a);
	if (!held) {
		kputs("  vfs: a descriptor that was just installed named "
		      "nothing\n");
		ok = false;
	} else {
		if (held->ops->read(held, &back, sizeof(back)) !=
		    (i64)sizeof(back) || back != test_counted.value) {
			kputs("  vfs: reading through a descriptor gave the "
			      "wrong bytes\n");
			ok = false;
		}
		file_release(held);
	}

	/* CLOSING ONE OF TWO MUST NOT CLOSE THE FILE.
	 *
	 * This is the assertion the reference count exists for, and it is the
	 * one a naive implementation fails: a pipe whose reader closes must
	 * stay open for the writer, and a file whose second descriptor is
	 * closed must still be readable through the first. Counting closes
	 * rather than checking a pointer, because a use-after-free reads
	 * correct-looking memory most of the time. */
	fd_close(p, b);

	if (test_counted.closes != 0) {
		kputs("  vfs: closing one of two descriptors closed the file\n");
		ok = false;
	}

	held = fd_get(p, a);
	if (!held) {
		kputs("  vfs: the surviving descriptor stopped naming the "
		      "file\n");
		ok = false;
	} else {
		file_release(held);
	}

	/* Closing it twice is an error the second time, not a second close. */
	if (fd_close(p, b) != SYS_EINVAL) {
		kputs("  vfs: closing an already-closed descriptor was "
		      "allowed\n");
		ok = false;
	}

	if (test_counted.closes != 0) {
		kputs("  vfs: a double close reached the file\n");
		ok = false;
	}

	fd_close(p, a);

	if (test_counted.closes != 1) {
		kprintf("  vfs: the file was closed %u times rather than "
			"once\n", test_counted.closes);
		ok = false;
	}

	/* And everything a process holds goes when the process does. A
	 * descriptor left open by a program that has ended is a file that is
	 * never committed and memory that is never returned. */
	fd_close_all(p);
	p->state = PROCESS_ENDED;

	return ok;
}

/* --- and the same interface over a real volume ----------------------------
 *
 * Run from rootfs_run, which is the only place in the boot that has a formatted
 * volume to work with -- every disk in the verification rig starts blank, so a
 * test placed in the ordinary battery would report "skipped" on every machine
 * and prove nothing.
 *
 * What this adds over the descriptor test above is the part that touches a
 * disk: that a file written through a descriptor is a file, that closing is what
 * commits it, and that reading it back gives the bytes rather than a
 * plausible-looking prefix.
 */
bool vfs_file_self_test(void)
{
	static const char message[] = "written through a descriptor";
	struct file *f;
	i64 err = SYS_OK;
	char back[64];
	i64 n;
	bool ok = true;

	if (!rootfs()) {
		kputs("  vfs: no volume on this machine to open a file on\n");
		return true;
	}

	f = file_open_path("/descriptor-test", OPEN_WRITE | OPEN_CREATE, 0600,
			   &err);
	if (!f) {
		kprintf("  vfs: could not create a file through a descriptor "
			"(%ld)\n", (long)err);
		return false;
	}

	if (f->ops->write(f, message, sizeof(message)) != (i64)sizeof(message)) {
		kputs("  vfs: a write through a descriptor was short\n");
		ok = false;
	}

	/* NOT VISIBLE YET, and that is the contract rather than a defect.
	 *
	 * The bytes are held until the close, because the filesystem underneath
	 * writes whole files in one transaction. Asserting it here is what stops
	 * somebody later "fixing" the close to be a formality and losing every
	 * write that was not followed by one. */
	if (rootfs_read_file("/descriptor-test", back, sizeof(back), NULL,
			     NULL) == RECONFS_OK) {
		kputs("  vfs: the file existed before it was closed, so the "
		      "close is not what commits it\n");
		ok = false;
	}

	/* And the close is where a failure would appear. Thrown away, it would
	 * be a write whose result was thrown away. */
	err = file_release(f);
	if (err != SYS_OK) {
		kprintf("  vfs: closing the file reported %ld\n", (long)err);
		ok = false;
	}

	/* Opened again, as a different file object over the same name. */
	f = file_open_path("/descriptor-test", OPEN_READ, 0, &err);
	if (!f) {
		kprintf("  vfs: could not open back what was just written "
			"(%ld)\n", (long)err);
		return false;
	}

	n = f->ops->read(f, back, sizeof(back));
	if (n != (i64)sizeof(message) ||
	    kmemcmp(back, message, sizeof(message)) != 0) {
		kprintf("  vfs: read back %ld bytes, and not the ones "
			"written\n", (long)n);
		ok = false;
	}

	/* At the end. A second read must say the file has ended rather than
	 * repeating the last thing it had. */
	if (f->ops->read(f, back, sizeof(back)) != 0) {
		kputs("  vfs: reading past the end of a file did not report "
		      "the end\n");
		ok = false;
	}

	/* And back to a position, which is what makes it a descriptor rather
	 * than a whole-file read with extra steps. */
	if (f->ops->seek(f, 8, SEEK_START) != 8) {
		kputs("  vfs: seeking to a position failed\n");
		ok = false;
	}

	n = f->ops->read(f, back, 6);
	if (n != 6 || kmemcmp(back, message + 8, 6) != 0) {
		kputs("  vfs: reading after a seek gave the wrong bytes\n");
		ok = false;
	}

	/* Before the start is refused rather than clamped to zero. A seek that
	 * silently became zero would read the beginning of the file and report
	 * success. */
	if (f->ops->seek(f, -1, SEEK_START) != SYS_EINVAL) {
		kputs("  vfs: seeking before the start was allowed\n");
		ok = false;
	}

	file_release(f);

	/* Creating it a second time is refused, the same way rootfs_create_file
	 * refuses -- and the refusal arrives at the close, because that is when
	 * anything is attempted. */
	f = file_open_path("/descriptor-test", OPEN_WRITE | OPEN_CREATE, 0600,
			   &err);
	if (f) {
		if (f->ops->write(f, message, 4) != 4) {
			kputs("  vfs: a write into the second copy failed "
			      "early\n");
			ok = false;
		}

		if (file_release(f) != SYS_EEXIST) {
			kputs("  vfs: creating a file that already exists was "
			      "allowed\n");
			ok = false;
		}
	}

	return ok;
}

/* --- a mapping backed by a file -------------------------------------------
 *
 * The last row of the memory-management section, and it needed a VFS rather
 * than more memory management: the page tables have been able to fill a page on
 * demand since checkpoint 19, and what was missing was anything to fill it
 * *from* that was not zeroes.
 *
 * Three things are asserted, and the second and third are the ones a naive
 * implementation gets wrong:
 *
 *   - a page that is touched arrives holding the file's bytes;
 *   - a page past the end of what the file backs arrives holding zeroes, which
 *     is what an ELF segment with a .bss is made of;
 *   - a write to a mapped page changes the page and **not the file**, which is
 *     what "private" means and is the difference between this and a shared
 *     mapping that does not exist yet.
 */
bool vfs_mmap_self_test(void)
{
	static const char content[] = "mapped, not copied";
	struct addrspace *as;
	struct file *f;
	i64 err = SYS_OK;
	vaddr_t at = 0x30000000;
	const u8 *page;
	bool ok = true;

	if (!rootfs()) {
		kputs("  vfs: no volume to map a file from\n");
		return true;
	}

	f = file_open_path("/mapped-test", OPEN_WRITE | OPEN_CREATE, 0600,
			   &err);
	if (!f) {
		kprintf("  vfs: could not create the file to map (%ld)\n",
			(long)err);
		return false;
	}

	f->ops->write(f, content, sizeof(content));

	if (file_release(f) != SYS_OK) {
		kputs("  vfs: could not commit the file to map\n");
		return false;
	}

	f = file_open_path("/mapped-test", OPEN_READ, 0, &err);
	if (!f) {
		kprintf("  vfs: could not open the file to map (%ld)\n",
			(long)err);
		return false;
	}

	as = addrspace_create();
	if (!as) {
		file_release(f);
		kputs("  vfs: no address space to map into\n");
		return false;
	}

	/* Two pages of range, one page of file. The second page is the one that
	 * has to arrive as zeroes. */
	if (!addrspace_map_file(as, at, 2 * PAGE_SIZE,
				VM_READ | VM_WRITE | VM_USER, f, 0,
				sizeof(content))) {
		addrspace_release(as);
		file_release(f);
		kputs("  vfs: the mapping was refused\n");
		return false;
	}

	/* The mapping holds its own reference now. */
	file_release(f);

	addrspace_activate(as);

	/* Touched for the first time. Every byte of this comes from a page
	 * fault that had to go and read a file. */
	page = (const u8 *)(uintptr_t)at;

	if (kmemcmp(page, content, sizeof(content)) != 0) {
		kputs("  vfs: a mapped page did not hold the file's bytes\n");
		ok = false;
	}

	/* Past what the file backs. Zeroes, and checked at a byte the file
	 * never reached rather than at the very end -- an implementation that
	 * read one byte too many would still look right at the boundary. */
	{
		const u8 *beyond = (const u8 *)(uintptr_t)(at + PAGE_SIZE);
		unsigned i;

		for (i = 0; i < 64; i++)
			if (beyond[i] != 0) {
				kputs("  vfs: a page past the end of the file "
				      "was not zero\n");
				ok = false;
				break;
			}
	}

	/* Written, and then the file read again through a fresh descriptor.
	 * A shared mapping would change the file here; a private one must not,
	 * and a program that expected otherwise would find out by losing its
	 * changes. */
	{
		u8 *writable = (u8 *)(uintptr_t)at;
		char back[64];
		u32 got = 0;

		writable[0] = 'X';

		if (writable[0] != 'X') {
			kputs("  vfs: writing to a mapped page did not take\n");
			ok = false;
		}

		addrspace_activate(NULL);

		if (rootfs_read_file("/mapped-test", back, sizeof(back), &got,
				     NULL) == RECONFS_OK && back[0] == 'X') {
			kputs("  vfs: a write to a private mapping reached the "
			      "file\n");
			ok = false;
		}
	}

	addrspace_activate(NULL);
	addrspace_release(as);
	return ok;
}
