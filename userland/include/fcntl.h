/*
 * <fcntl.h> -- opening a file by name.
 *
 * --- The flag values are POSIX's, and the kernel's are not ---
 *
 * `RECON_O_READ` is 1 and `O_RDONLY` is 0. That is not a detail: a wrapper
 * that passed these through unchanged would open every read-only file for
 * writing and every write-only one for reading, which is the sort of thing
 * that works on the first test and destroys a file on the fourth.
 *
 * `userland/tests/hostsys.c` has the same paragraph at the top, because it
 * translates the same two numberings in the other direction. The translation
 * lives in `libc/posix.c` and is checked against the host's `open` by opening
 * the same files the same ways and comparing what comes back.
 */

#ifndef RECON_FCNTL_H
#define RECON_FCNTL_H

/* Quoted, so it finds the one beside it rather than the host's.
 * See the note in userland/libc/posix.c. */
#include "sys/types.h"

#define O_RDONLY	0
#define O_WRONLY	1
#define O_RDWR		2
#define O_ACCMODE	3

#define O_CREAT		0100
#define O_EXCL		0200
#define O_TRUNC		01000
#define O_APPEND	02000

/*
 * Refuse to open a symbolic link.
 *
 * POSIX's value, and on ReconOS it currently changes nothing, because there
 * are no symbolic links: no command makes one, there is no `SYS_LINK`, and
 * `S_IFLNK` appears nowhere in either tree.
 *
 * **It is defined as the real bit rather than as 0**, and that is the whole
 * point of this comment. Defining it as 0 would compile identically today and
 * would be a trap the day links arrive: every caller asking for this
 * protection would silently stop asking for it, with nothing in the source
 * changed to notice. As the real value it is a flag the kernel does not
 * implement yet -- which is a thing that can be found, and fixed, once.
 *
 * `src/recon_fs.c` passes it when writing a file it means to be private, on
 * the argument that a link put there by somebody else must not redirect the
 * write.
 */
#define O_NOFOLLOW	0400000

/*
 * `open` takes a mode only when it is creating. Declared variadic, as POSIX
 * does, rather than always taking one -- a caller that passes a mode to a
 * plain open is saying something it does not mean, and the compiler should be
 * able to say so.
 */
int open(const char *path, int flags, ...);

#endif /* RECON_FCNTL_H */
