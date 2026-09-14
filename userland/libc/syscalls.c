/*
 * The seven calls the C library is built on, as real functions.
 *
 * `recon.h` defines the system calls as `static inline`, which is right for a
 * program: there is nothing to link against and the trap is one instruction
 * where it is used. The library needs them as *symbols* instead, for one
 * reason that is worth the file:
 *
 * **So that the whole library can be compiled for the host and compared
 * against the system's.** `userland/tests/hostsys.c` defines these same seven
 * over POSIX, and with it the `FILE` layer can be run on Linux and checked
 * against Linux's own stdio -- reading the same files, seeking to the same
 * places, and being required to answer the same things. Without the
 * indirection the library would only ever be testable by booting it.
 *
 * There is no second implementation of anything here. This file is four lines
 * per call and exists purely to give the inline ones a name.
 */

#include <recon.h>

#include "internal.h"

long recon_sys_open(const char *path, unsigned long flags)
{
	return (long)recon_open(path, recon_strlen(path), flags, 0);
}

long recon_sys_read(int fd, void *into, unsigned long length)
{
	return (long)recon_read(fd, into, length);
}

long recon_sys_write(int fd, const void *from, unsigned long length)
{
	return (long)recon_write(fd, from, length);
}

long recon_sys_seek(int fd, long long offset, int from)
{
	return (long)RECON_CALL3(SYS_SEEK, fd, offset, from);
}

long recon_sys_close(int fd)
{
	return (long)recon_close(fd);
}

void recon_sys_exit(int code)
{
	recon_exit(code);
}

/*
 * Two clocks, in nanoseconds, and they are two calls because they answer two
 * questions -- see the comment on `sys_walltime` in `kernel/core/user.c`.
 */
long long recon_sys_time(void)
{
	return (long long)recon_time();
}

long long recon_sys_walltime(void)
{
	return (long long)recon_walltime();
}
