/*
 * <sys/types.h>.
 *
 * The handful of typedefs the file layer needs, and nothing else. There is no
 * `pid_t` because nothing here creates a process, no `uid_t` beyond what the
 * identity calls already use, and no `time_t` -- that one belongs to
 * <time.h> and having it in two places is how they come to disagree.
 *
 * The widths are the ones LP64 gives, which is what both architectures this
 * system runs on are. A 32-bit `off_t` is the bug that made files stop at two
 * gigabytes for a decade, and it is not worth inheriting.
 */

#ifndef RECON_SYS_TYPES_H
#define RECON_SYS_TYPES_H

#include <stddef.h>

typedef long ssize_t;		/* a count, or -1 */
typedef long long off_t;	/* a position in a file, signed, 64-bit */
typedef unsigned int mode_t;	/* permission bits */
typedef unsigned long long ino_t;

#endif /* RECON_SYS_TYPES_H */
