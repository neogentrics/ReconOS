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

/* Not <arpa/inet.h>: it declares inet_ntoa, internal.h declares its own,
 * and this file is deliberately not renamed -- so the two would be the
 * same name disagreeing about a struct. htons and htonl are in
 * <netinet/in.h>, which is included below. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

#include "../libc/internal.h"

/*
 * The host's errno, as the number a ReconOS system call would have answered.
 *
 * Every primitive here goes through it. Returning POSIX's -1 instead -- which
 * this file did until the descriptor layer was written -- is a stand-in with a
 * *looser contract than the real call*, and that is the one way a stand-in can
 * be worse than not having one: it lets a wrapper pass here while reporting
 * the wrong reason on a machine.
 *
 * Found by `recon_libc_posix_tests` on its first run: a file that was not
 * there came back as ENOSYS rather than ENOENT.
 *
 * The values are the kernel's, from `userland/include/recon.h`. Written as
 * numbers because this file must not include that header -- it is the one
 * file compiled against the *host's* headers, unrenamed, on purpose.
 */
static long as_recon_status(void)
{
	switch (errno) {
	case ENOENT:		return -7;	/* SYS_ENOENT */
	case ENOTDIR:		return -7;
	case EEXIST:		return -6;	/* SYS_EEXIST */
	case EBADF:		return -11;	/* SYS_EBADF  */
	case EACCES:		return -12;	/* SYS_EPERM  */
	case EPERM:		return -12;
	case EROFS:		return -12;
	case EINVAL:		return -3;	/* SYS_EINVAL */
	case EFAULT:		return -2;	/* SYS_EFAULT */
	case ENOSPC:		return -8;	/* SYS_ENOSPC */
	case EMFILE:		return -10;	/* SYS_EMFILE */
	case ENFILE:		return -10;
	case ENOMEM:		return -14;	/* SYS_ENOMEM */
	case EPIPE:		return -13;	/* SYS_EPIPE  */
	case EAGAIN:		return -4;	/* SYS_EAGAIN */
	case ESPIPE:		return -3;
	case ENODEV:		return -5;	/* SYS_ENODEV */
	default:		return -9;	/* SYS_EIO    */
	}
}

long recon_sys_open(const char *path, unsigned long flags)
{
	int posix;
	int fd;

	if ((flags & RECON_O_WRITE) && (flags & RECON_O_READ)) {
		posix = O_RDWR | O_CREAT;
	} else if (flags & RECON_O_WRITE) {
		posix = O_WRONLY | O_CREAT | O_TRUNC;
	} else {
		posix = O_RDONLY;
	}

	fd = open(path, posix, 0644);

	return fd < 0 ? as_recon_status() : (long)fd;
}

long recon_sys_read(int fd, void *into, unsigned long length)
{
	ssize_t n = read(fd, into, (size_t)length);

	return n < 0 ? as_recon_status() : (long)n;
}

long recon_sys_write(int fd, const void *from, unsigned long length)
{
	ssize_t n = write(fd, from, (size_t)length);

	return n < 0 ? as_recon_status() : (long)n;
}

long recon_sys_seek(int fd, long long offset, int from)
{
	int whence = SEEK_SET;

	if (from == RECON_SEEK_CUR) {
		whence = SEEK_CUR;
	} else if (from == RECON_SEEK_END) {
		whence = SEEK_END;
	}
	off_t where = lseek(fd, (off_t)offset, whence);

	return where < 0 ? as_recon_status() : (long)where;
}

long recon_sys_close(int fd)
{
	return close(fd) < 0 ? as_recon_status() : 0;
}

/*
 * The host's two clocks, answering the same two questions.
 *
 * `CLOCK_MONOTONIC` and `CLOCK_REALTIME` are the host's names for exactly the
 * split ReconOS makes with two system calls, which is the whole reason this
 * file can stand in for the kernel at all: the distinction is not ReconOS's
 * invention, it is the one every system with a clock has had to make.
 *
 * A failure answers zero rather than a negative number. Nothing above this
 * checks, because on the kernel these cannot fail -- and a negative
 * nanosecond count would be read as a date in 1969, which is the wrong kind
 * of wrong to introduce here in order to report an impossibility.
 */
long long recon_sys_time(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0;
	}
	return (long long)now.tv_sec * 1000000000LL + (long long)now.tv_nsec;
}

long long recon_sys_walltime(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
		return 0;
	}
	return (long long)now.tv_sec * 1000000000LL + (long long)now.tv_nsec;
}

/*
 * Every name in a directory, in one call, the way SYS_LIST answers.
 *
 * Deliberately not "POSIX readdir with a loop around it and whatever falls
 * out". The kernel's contract is specific and the layer above is written
 * against it, so this reproduces it exactly:
 *
 *   - names NUL-terminated and back to back,
 *   - the size of the *whole* listing returned whether or not it fitted,
 *   - and **nothing written at all** unless all of it fits.
 *
 * A stand-in that got the last point wrong would let `opendir` pass here while
 * reading half a directory on a machine.
 *
 * `.` and `..` are dropped, because a ReconFS listing has no such entries --
 * `reconfs_list` walks the names a directory holds and those two are not
 * among them.
 */
long recon_sys_list(const char *path, char *names, unsigned long names_len)
{
	DIR *dir = opendir(path);
	struct dirent *e;
	unsigned long needed = 0;

	if (!dir) {
		switch (errno) {
		case ENOENT:	return -7;	/* SYS_ENOENT */
		case ENOTDIR:	return -7;
		case EACCES:	return -12;	/* SYS_EPERM  */
		default:	return -9;	/* SYS_EIO    */
		}
	}

	while ((e = readdir(dir)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		needed += strlen(e->d_name) + 1;
	}

	if (!names || names_len < needed) {
		closedir(dir);
		return (long)needed;
	}

	rewinddir(dir);
	needed = 0;

	while ((e = readdir(dir)) != NULL) {
		size_t n;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;

		n = strlen(e->d_name) + 1;
		memcpy(names + needed, e->d_name, n);
		needed += n;
	}

	closedir(dir);
	return (long)needed;
}

long recon_sys_mkdir(const char *path, unsigned long mode)
{
	if (mkdir(path, (mode_t)mode) != 0) {
		switch (errno) {
		case EEXIST:	return -6;	/* SYS_EEXIST */
		case ENOENT:	return -7;	/* SYS_ENOENT */
		case EACCES:	return -12;	/* SYS_EPERM  */
		case ENOSPC:	return -8;	/* SYS_ENOSPC */
		default:	return -9;	/* SYS_EIO    */
		}
	}

	return 0;
}

/* --- The five socket primitives ---------------------------------------
 *
 * Real sockets on the host, because a stand-in that only pretended would let a
 * suite pass against behaviour the kernel does not have. What is translated is
 * the *shape*: the kernel takes an address as a number in host order and a
 * port as a number, where POSIX takes a `sockaddr_in` with both in network
 * order, so this puts them back the way POSIX wants them.
 */

static int as_family(unsigned int addr, int port, struct sockaddr_in *into)
{
	if (port < 0 || port > 0xFFFF) {
		/* The same refusal the kernel makes: a port that will not fit
		 * sixteen bits is refused rather than truncated. */
		errno = EINVAL;
		return -1;
	}

	memset(into, 0, sizeof(*into));
	into->sin_family = AF_INET;
	into->sin_port = htons((unsigned short)port);
	into->sin_addr.s_addr = htonl(addr);
	return 0;
}

long recon_sys_socket(int type)
{
	int kind = (type == 2) ? SOCK_DGRAM : SOCK_STREAM;
	int fd = socket(AF_INET, kind, 0);

	return fd < 0 ? as_recon_status() : (long)fd;
}

long recon_sys_bind(int fd, unsigned int addr, int port)
{
	struct sockaddr_in where;

	if (as_family(addr, port, &where) != 0) {
		return as_recon_status();
	}
	if (bind(fd, (struct sockaddr *)&where, sizeof(where)) != 0) {
		return as_recon_status();
	}
	return 0;
}

long recon_sys_listen(int fd, int backlog)
{
	/*
	 * **Made non-blocking here**, because the kernel's `accept` does not
	 * block and a stand-in that did would be looser than the real call --
	 * which this file's own header calls the one way a stand-in can be
	 * worse than none. A test written against a blocking accept would pass
	 * here and hang on the machine.
	 */
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}

	if (listen(fd, backlog) != 0) {
		return as_recon_status();
	}
	return 0;
}

long recon_sys_accept(int fd)
{
	int got = accept(fd, NULL, NULL);

	/* EAGAIN already answers SYS_EAGAIN in the table above, which is what
	 * "nobody is waiting" is on the kernel. EWOULDBLOCK is the same number
	 * on Linux and is not required to be anywhere. */
	if (got < 0) {
		if (errno == EWOULDBLOCK) {
			errno = EAGAIN;
		}
		return as_recon_status();
	}
	return (long)got;
}

long recon_sys_connect(int fd, unsigned int addr, int port)
{
	struct sockaddr_in where;

	if (as_family(addr, port, &where) != 0) {
		return as_recon_status();
	}
	if (connect(fd, (struct sockaddr *)&where, sizeof(where)) != 0) {
		return as_recon_status();
	}
	return 0;
}

long recon_sys_page_size(void)
{
	return sysconf(_SC_PAGESIZE);
}

void recon_sys_exit(int code)
{
	_exit(code);
}
