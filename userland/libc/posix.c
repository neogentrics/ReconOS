/*
 * The descriptor layer: files by name, by number, and directories.
 *
 * Everything here is a translation. The system calls underneath already do the
 * work; what this file does is turn ReconOS's answers into the ones C has
 * always given -- a count or -1, with `errno` set -- and ReconOS's flag values
 * into POSIX's.
 *
 * **That translation is the whole risk.** `RECON_O_READ` is 1 and `O_RDONLY`
 * is 0, so a wrapper that passed flags through unchanged would open every
 * read-only file for writing and every write-only one for reading. It builds,
 * it passes the first test, and it destroys a file on the fourth.
 * `userland/tests/hostsys.c` carries the same warning because it does the same
 * translation in the other direction, and this file is checked against the
 * host's `open` by opening the same files the same ways and comparing.
 *
 * --- What is here and what is not ---
 *
 * The eleven that have a system call today: `open`, `close`, `read`, `write`,
 * `lseek`, `mkdir`, `opendir`, `readdir`, `closedir`, `realpath` and
 * `sysconf`. The rest -- `stat` and its two relatives, `unlink`, `rmdir`,
 * `access`, `chmod`, `umask` -- are declared in the headers and deliberately
 * not defined, so a caller fails to link rather than getting a plausible wrong
 * answer. The headers say what each of them is waiting for.
 */

#include "internal.h"

/*
 * The public headers, by relative path, which is the same exception `time.c`
 * and `errno.c` take and for the same reason: the rule against including them
 * exists so the differential build cannot compare a function against itself,
 * and that cannot happen when nothing puts the host's copies on this library's
 * include path.
 *
 * What it buys here is that the flag values, the struct layouts and the
 * constants a caller will see are the ones this file is written against --
 * one definition rather than two kept identical by hand, for a file whose
 * whole subject is two sets of numbers that must not be confused.
 *
 * They include their own siblings with quoted paths so that this works from
 * here as well as from the desktop's include path.
 */
#include "../include/errno.h"
#include "../include/fcntl.h"
#include "../include/unistd.h"
#include "../include/dirent.h"
#include "../include/sys/stat.h"

/* --- flags ---------------------------------------------------------------- */

/*
 * POSIX's idea of how a file is being opened, into ReconOS's.
 *
 * Written as a decision on the access mode rather than as a bitwise map,
 * because they are not bits on the POSIX side: `O_RDONLY` is 0, `O_WRONLY` is
 * 1 and `O_RDWR` is 2, so `flags & O_WRONLY` is true for a read-write file and
 * false for a read-only one, and any code that tests them as flags is wrong in
 * a way that looks right.
 */
static unsigned long recon_flags_from_posix(int flags)
{
	unsigned long out = 0;

	switch (flags & O_ACCMODE) {
	case O_WRONLY:
		out = RECON_O_WRITE;
		break;
	case O_RDWR:
		out = RECON_O_READ | RECON_O_WRITE;
		break;
	default:
		out = RECON_O_READ;
		break;
	}

	return out;
}

/* Every failing call ends the same way, so it is written once. Returns -1 so
 * that a caller can `return fail(status);` and read as one thought. */
static long fail(long status)
{
	errno = recon_errno_from_status(status);
	return -1;
}

/* --- files by name and by number ------------------------------------------ */

int open(const char *path, int flags, ...)
{
	long fd;

	if (!path) {
		errno = EFAULT;
		return -1;
	}

	/*
	 * The mode a variadic `open` may carry is **not passed on**, and that
	 * is worth saying rather than leaving as an omission.
	 *
	 * ReconOS creates a file with its permissions already set, in one
	 * transaction, through `SYS_CREATE` -- see `rootfs.h`, which explains
	 * at length why that is separate from opening: there is no moment when
	 * a file exists with the wrong mode, not even across a power cut.
	 * `SYS_OPEN` therefore has nothing to do with modes.
	 *
	 * A caller that passes `O_CREAT` and a mode gets a file opened for
	 * writing, which creates it -- with the volume's default mode rather
	 * than the one asked for. That is a real difference from POSIX and it
	 * is why `recon_create` exists next to this.
	 */
	fd = recon_sys_open(path, recon_flags_from_posix(flags));

	if (fd < 0)
		return (int)fail(fd);

	return (int)fd;
}

int close(int fd)
{
	long r = recon_sys_close(fd);

	return r < 0 ? (int)fail(r) : 0;
}

ssize_t read(int fd, void *into, size_t length)
{
	long r;

	if (length && !into) {
		errno = EFAULT;
		return -1;
	}

	r = recon_sys_read(fd, into, (unsigned long)length);

	return r < 0 ? fail(r) : r;
}

ssize_t write(int fd, const void *from, size_t length)
{
	long r;

	if (length && !from) {
		errno = EFAULT;
		return -1;
	}

	r = recon_sys_write(fd, from, (unsigned long)length);

	return r < 0 ? fail(r) : r;
}

off_t lseek(int fd, off_t offset, int whence)
{
	long r;

	/* SEEK_SET, SEEK_CUR and SEEK_END are 0, 1 and 2 on both sides, and
	 * that is checked rather than assumed -- a constant that happens to
	 * agree today is a constant that can stop agreeing. */
	if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
		errno = EINVAL;
		return -1;
	}

	r = recon_sys_seek(fd, (long long)offset, whence);

	return r < 0 ? fail(r) : (off_t)r;
}

/* --- directories ---------------------------------------------------------- */

int mkdir(const char *path, mode_t mode)
{
	long r;

	if (!path) {
		errno = EFAULT;
		return -1;
	}

	r = recon_sys_mkdir(path, (unsigned long)mode);

	return r < 0 ? (int)fail(r) : 0;
}

/*
 * A directory being read.
 *
 * The names are fetched once, in full -- see <dirent.h> for why the kernel's
 * interface is that shape and why it is the better one. What is kept here is
 * the buffer and a cursor into it.
 */
struct recon_dir {
	char *names;		/* NUL-terminated, back to back */
	size_t bytes;
	size_t at;
	struct dirent entry;	/* handed back by readdir, reused each call */
};

DIR *opendir(const char *path)
{
	struct recon_dir *dir;
	long needed;

	if (!path) {
		errno = EFAULT;
		return 0;
	}

	/* How much, before anything is allocated. Asking with no room at all
	 * is the documented way to find out the size, and doing it first means
	 * a directory that grew between the two calls is a refusal rather than
	 * a short answer -- the second call is whole-or-nothing. */
	needed = recon_sys_list(path, 0, 0);

	if (needed < 0) {
		errno = recon_errno_from_status(needed);
		return 0;
	}

	dir = malloc(sizeof(*dir));
	if (!dir) {
		errno = ENOMEM;
		return 0;
	}

	/* One byte more than needed, so that an empty directory still has a
	 * buffer and the walk below has something to find its end in. */
	dir->names = malloc((size_t)needed + 1);
	if (!dir->names) {
		free(dir);
		errno = ENOMEM;
		return 0;
	}

	if (needed > 0) {
		long got = recon_sys_list(path, dir->names,
					  (unsigned long)needed);

		if (got < 0 || got > needed) {
			/* `got > needed` is the directory having grown between
			 * the two calls: the kernel wrote nothing and told us
			 * the new size. Refused rather than retried, because a
			 * retry loop against a directory being written to is a
			 * loop with no end. */
			free(dir->names);
			free(dir);
			errno = got < 0 ? recon_errno_from_status(got) : EAGAIN;
			return 0;
		}

		needed = got;
	}

	dir->names[needed] = '\0';
	dir->bytes = (size_t)needed;
	dir->at = 0;

	return dir;
}

struct dirent *readdir(DIR *dir)
{
	size_t length;

	if (!dir) {
		errno = EFAULT;
		return 0;
	}

	if (dir->at >= dir->bytes)
		return 0;		/* the end, and not an error */

	length = recon_strlen(dir->names + dir->at);

	/* A name longer than `struct dirent` can hold. Skipped rather than
	 * truncated: a truncated name is a name that does not open, and a
	 * caller cannot tell it from one that does. RECONFS_NAME_MAX is 255
	 * and d_name is 256, so this cannot happen on a ReconFS volume -- it
	 * is here for the filesystems that are not. */
	if (length >= sizeof(dir->entry.d_name)) {
		dir->at += length + 1;
		return readdir(dir);
	}

	memcpy(dir->entry.d_name, dir->names + dir->at, length + 1);

	/* SYS_LIST answers with names and nothing else. POSIX allows exactly
	 * this; <dirent.h> records that the desktop reads d_name at every one
	 * of its sites and d_type at none. */
	dir->entry.d_type = DT_UNKNOWN;
	dir->entry.d_ino = 0;

	dir->at += length + 1;
	return &dir->entry;
}

int closedir(DIR *dir)
{
	if (!dir) {
		errno = EBADF;
		return -1;
	}

	free(dir->names);
	free(dir);
	return 0;
}

/* --- paths ---------------------------------------------------------------- */

/*
 * The path a name really means, with `.`, `..` and repeated slashes resolved.
 *
 * Entirely arithmetic on the string: no system call, nothing asked of the
 * filesystem, and **no check that the path exists**. That is a difference from
 * POSIX worth stating -- the host's `realpath` resolves symbolic links and
 * fails with ENOENT for a path that is not there, and this cannot do either,
 * because ReconOS has no symbolic links and no call that asks whether a path
 * exists.
 *
 * What it is for is the thing the desktop uses it for: making two spellings of
 * the same place compare equal, and making sure a path cannot climb out of the
 * root. `..` at the root stays at the root rather than going above it, which
 * is the containment `include/recon_fs.h` describes and is the one behaviour
 * here that is a refusal rather than a convenience.
 */
char *realpath(const char *path, char *into)
{
	char *out;
	size_t at = 0;
	size_t i = 0;

	if (!path || !into) {
		errno = EFAULT;
		return 0;
	}

	if (path[0] != '/') {
		/* Every path in this system is absolute; there is no working
		 * directory to resolve a relative one against. See
		 * <unistd.h> on why `getcwd` is not declared at all. */
		errno = EINVAL;
		return 0;
	}

	out = into;
	out[at++] = '/';

	while (path[i]) {
		size_t start;
		size_t length;

		while (path[i] == '/')
			i++;

		if (!path[i])
			break;

		start = i;
		while (path[i] && path[i] != '/')
			i++;
		length = i - start;

		if (length == 1 && path[start] == '.')
			continue;

		if (length == 2 && path[start] == '.' &&
		    path[start + 1] == '.') {
			/* Back one component, and never past the root. */
			while (at > 1 && out[at - 1] != '/')
				at--;
			if (at > 1)
				at--;		/* the slash itself */
			continue;
		}

		if (at > 1)
			out[at++] = '/';

		memcpy(out + at, path + start, length);
		at += length;
	}

	out[at] = '\0';
	return out;
}

/* --- what the machine says about itself ----------------------------------- */

long sysconf(int name)
{
	switch (name) {
	case _SC_PAGESIZE:
	{
		long size = recon_sys_page_size();

		if (size < 0) {
			errno = EIO;
			return -1;
		}

		return size;
	}

	case _SC_OPEN_MAX:
		/* A constant, because there is no call that asks -- and the
		 * suite reads the kernel's own header while it runs and
		 * compares, since a limit copied by hand is a limit that goes
		 * stale. See <unistd.h>. */
		return RECON_OPEN_MAX;

	default:
		/* -1 with errno untouched is POSIX's way of saying "no limit",
		 * and -1 with EINVAL is "no such question". They are different
		 * answers and a caller acts on them differently. */
		errno = EINVAL;
		return -1;
	}
}
