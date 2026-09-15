/*
 * <sys/stat.h>.
 *
 * `mkdir` works. `stat` does not, and the shape of this file says which is
 * which: the struct is defined so that code handling a file's size and mode
 * compiles, and the functions that would fill it are declared and not linked.
 *
 * --- What `stat` needs, which is one call ---
 *
 * The kernel knows all of it. `rootfs_owner_of` already answers a path's mode,
 * owner and group, and the inode carries its size and three timestamps. What
 * is missing is a system call that hands them out -- see
 * `docs/KERNEL-WANTS.md`.
 *
 * Until then a caller fails to link, naming `stat`, rather than receiving a
 * struct of zeroes. A zeroed `st_mode` says "not a directory, not a file, no
 * permissions", which a file manager would draw as an empty list and a
 * permission check would read as "forbidden" -- both plausible, both wrong,
 * and neither traceable back to here.
 *
 * The field names and the bit values are POSIX's, for the reason
 * `userland/include/errno.h` gives about its numbers: the desktop is compiled
 * against both libraries at once today, and a `S_IFDIR` that differs between
 * two translation units is a fault that reads as a filesystem fault.
 */

#ifndef RECON_SYS_STAT_H
#define RECON_SYS_STAT_H

/* The sibling in this directory, not the host's. */
#include "types.h"

struct stat {
	mode_t st_mode;
	off_t st_size;
	ino_t st_ino;
	unsigned int st_uid;
	unsigned int st_gid;
	long long st_atime;	/* seconds, as <time.h> counts them */
	long long st_mtime;
	long long st_ctime;
};

#define S_IFMT		0170000
#define S_IFREG		0100000
#define S_IFDIR		0040000
#define S_IFCHR		0020000

#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)

#define S_IRWXU		0700
#define S_IRUSR		0400
#define S_IWUSR		0200
#define S_IXUSR		0100
#define S_IRWXG		0070
#define S_IRGRP		0040
#define S_IWGRP		0020
#define S_IXGRP		0010
#define S_IRWXO		0007
#define S_IROTH		0004
#define S_IWOTH		0002
#define S_IXOTH		0001

/* --- built on SYS_MKDIR --------------------------------------------------- */

int mkdir(const char *path, mode_t mode);

/* --- declared, and not yet linkable --------------------------------------- */

int stat(const char *path, struct stat *into);
int lstat(const char *path, struct stat *into);
int fstat(int fd, struct stat *into);
int chmod(const char *path, mode_t mode);
mode_t umask(mode_t mask);

#endif /* RECON_SYS_STAT_H */
