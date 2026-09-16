/*
 * <sys/types.h>.
 *
 * The handful of typedefs the file layer needs, and nothing else. There is no
 * `uid_t` beyond what the identity calls already use, and no `time_t` -- that
 * one belongs to <time.h> and having it in two places is how they come to
 * disagree.
 *
 * `pid_t` arrived on 15 September, when two files turned out to want it and
 * not to want anything else. It had been left out a few hours earlier on the
 * grounds that everything needing it was blocked on wayland anyway -- which
 * was measured, and stopped being true the same afternoon. **A typedef nobody
 * can use does not belong here; one two files need does.**
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

/*
 * A process id.
 *
 * Added when something needed it and not before: this header used to say
 * there was none "because nothing here creates a process", which was true of
 * *creating* and beside the point -- `include/recon_procinfo.h` names one to
 * report on a process, and the kernel has had `SYS_GETPID` since the
 * beginning.
 *
 * `int`, which is what POSIX has and what every caller here compares against
 * a literal. The kernel's own ids are small.
 */
typedef int pid_t;

#endif /* RECON_SYS_TYPES_H */
