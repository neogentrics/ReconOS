/*
 * The five primitives, over POSIX, so the C library can be run on Linux.
 *
 * This is what makes the `FILE` layer testable at all. Without it the only way
 * to find out whether `fgets` keeps its newline, or whether `fseek` from the
 * current position accounts for what is sitting in the read buffer, would be
 * to boot a kernel and look -- and those are exactly the faults that do not
 * announce themselves.
 *
 * With it, ReconOS's stdio and the host's stdio open the same file, read it
 * the same way, and are required to produce the same bytes and the same
 * positions. Any difference is a difference the desktop would have hit.
 *
 * **The flag values are ReconOS's, not POSIX's.** `RECON_O_READ` is 1 and
 * `O_RDONLY` is 0, so a shim that passed them through unchanged would open
 * every file for writing and every write-mode file for reading -- which is the
 * sort of thing that works on the first test and destroys a file on the
 * fourth. They are translated explicitly below.
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include "../libc/internal.h"

long recon_sys_open(const char *path, unsigned long flags)
{
	int posix;

	if ((flags & RECON_O_WRITE) && (flags & RECON_O_READ)) {
		posix = O_RDWR | O_CREAT;
	} else if (flags & RECON_O_WRITE) {
		posix = O_WRONLY | O_CREAT | O_TRUNC;
	} else {
		posix = O_RDONLY;
	}

	return (long)open(path, posix, 0644);
}

long recon_sys_read(int fd, void *into, unsigned long length)
{
	return (long)read(fd, into, (size_t)length);
}

long recon_sys_write(int fd, const void *from, unsigned long length)
{
	return (long)write(fd, from, (size_t)length);
}

long recon_sys_seek(int fd, long long offset, int from)
{
	int whence = SEEK_SET;

	if (from == RECON_SEEK_CUR) {
		whence = SEEK_CUR;
	} else if (from == RECON_SEEK_END) {
		whence = SEEK_END;
	}
	return (long)lseek(fd, (off_t)offset, whence);
}

long recon_sys_close(int fd)
{
	return (long)close(fd);
}

void recon_sys_exit(int code)
{
	_exit(code);
}
