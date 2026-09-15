/*
 * <errno.h>.
 *
 * --- Why these are Linux's numbers ---
 *
 * The values below are the ones glibc uses, and that is a deliberate choice
 * rather than laziness. ReconOS's kernel has error numbers of its own --
 * `SYS_ENOENT` is -7 -- and the obvious thing would be to expose those.
 *
 * Three reasons not to.
 *
 * **The desktop is compiled against both at once, today.**
 * `scripts/check-userland.sh` builds individual sources of `src/` with these
 * headers while the rest of the program is still built against glibc's. If
 * `ENOENT` were 7 in one translation unit and 2 in another, a comparison
 * between an errno set in one and tested in the other would be wrong -- and
 * would look like a filesystem fault.
 *
 * **`strerror` becomes testable.** Every other function in this library is
 * held against the host's by calling both and comparing. That only means
 * something if both are asked the same question, and the question `strerror`
 * is asked is a number.
 *
 * **And the numbers are not ours to invent.** They are the ones every C
 * program that has ever been written has seen. A system that chose its own
 * would be a system where a hardcoded `if (errno == 2)` -- which exists in the
 * wild, however much it should not -- silently means something else.
 *
 * So the kernel's numbering stays the kernel's, and `libc/errno.c` translates.
 * That translation is the one place the two can disagree, and it is checked.
 */

#ifndef RECON_ERRNO_H
#define RECON_ERRNO_H

/*
 * A variable, not a macro over a per-thread lookup.
 *
 * glibc defines `errno` as `(*__errno_location())` because it has threads.
 * ReconOS has none in user mode, so this is a plain int -- and the day there
 * are threads, this line is where it changes, and the change is visible to
 * everything that includes this header rather than hidden in the library.
 */
extern int errno;

/* The ones the kernel can produce today, which is what `libc/errno.c` maps. */
#define EPERM		 1	/* not permitted */
#define ENOENT		 2	/* no such file or directory */
#define ESRCH		 3
#define EINTR		 4
#define EIO		 5	/* the device failed */
#define ENXIO		 6
#define E2BIG		 7
#define ENOEXEC		 8
#define EBADF		 9	/* that descriptor names nothing */
#define ECHILD		10
#define EAGAIN		11	/* try again */
#define ENOMEM		12
#define EACCES		13
#define EFAULT		14	/* an address the caller does not own */
#define EBUSY		16
#define EEXIST		17	/* the name is taken */
#define EXDEV		18
#define ENODEV		19	/* no such device, or nothing mounted */
#define ENOTDIR		20
#define EISDIR		21
#define EINVAL		22
#define ENFILE		23
#define EMFILE		24	/* this process holds as many files as it may */
#define ENOTTY		25
#define EFBIG		27
#define ENOSPC		28	/* the volume is full */
#define ESPIPE		29
#define EROFS		30
#define EMLINK		31
#define EPIPE		32	/* the other end is gone */
#define EDOM		33
#define ERANGE		34
#define ENAMETOOLONG	36
#define ENOSYS		38	/* no such call in this personality */
#define ENOTEMPTY	39
#define ELOOP		40
#define EOVERFLOW	75

/* Named by the desktop and reachable once there are sockets. Defined now so
 * that a source which handles them compiles against these headers today --
 * `check-userland.sh` is what would otherwise stop at the first one. */
#define ENOTSOCK	88
#define EDESTADDRREQ	89
#define EMSGSIZE	90
#define EOPNOTSUPP	95
#define EAFNOSUPPORT	97
#define EADDRINUSE	98
#define ENETUNREACH	101
#define ECONNRESET	104
#define EISCONN		106
#define ENOTCONN	107
#define ETIMEDOUT	110
#define ECONNREFUSED	111
#define EHOSTUNREACH	113
#define EALREADY	114
#define EINPROGRESS	115

/* Linux makes these two the same number, and a program that tests for one
 * after checking the other is relying on that. Said here rather than left for
 * somebody to discover. */
#define EWOULDBLOCK	EAGAIN

/*
 * --- Not here, and why ---
 *
 * There is no `EDEADLK`, `ENOLCK` or anything else about locking, because
 * there is nothing in this system that locks a file. There is no `EDQUOT` or
 * `ESTALE` because there are no quotas and no network filesystem. A constant
 * defined for a condition that cannot arise is a constant somebody writes a
 * branch for, and that branch is never taken and never tested.
 */

#endif /* RECON_ERRNO_H */
