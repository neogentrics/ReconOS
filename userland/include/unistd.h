/*
 * <unistd.h>.
 *
 * The descriptor half of the file interface. `open` is next door in
 * <fcntl.h>, where POSIX puts it.
 *
 * --- What is declared here and cannot be linked ---
 *
 * Some of these have a system call underneath them and some do not, and the
 * ones that do not are **declared and not defined**. A caller then fails to
 * link, naming the symbol, which is the same stance `<stdlib.h>` took about
 * `malloc` for as long as there was nothing to build an allocator on.
 *
 * The alternative is a stub that answers -1 with a plausible errno, and that
 * is worse in a specific way: a program asking whether a file exists would be
 * told no, and would then create it, and would do that every time. A link
 * error is a question somebody answers once.
 *
 * | here                        | underneath it                      |
 * |-----------------------------|------------------------------------|
 * | `close` `read` `write` `lseek` | SYS_CLOSE, SYS_READ, SYS_WRITE, SYS_SEEK |
 * | `sysconf`                   | SYS_MACHINE, for the page size     |
 * | `unlink` `rmdir` `access`   | **nothing yet** -- docs/KERNEL-WANTS.md |
 * | `getcwd` `chdir`            | **nothing, and nothing planned**   |
 *
 * `getcwd` and `chdir` are the interesting absence. ReconOS has no working
 * directory: every path a program uses is absolute, resolved against the one
 * root the system can see. That is a deliberate property -- see
 * `include/recon_fs.h` -- and not a gap, so these two are not declared at all
 * rather than declared and left unimplemented. A program that wants them is a
 * program making an assumption this system does not hold.
 */

#ifndef RECON_UNISTD_H
#define RECON_UNISTD_H

/* Quoted, so it finds the one beside it rather than the host's.
 * See the note in userland/libc/posix.c. */
#include "sys/types.h"

/* --- built on calls that exist ------------------------------------------- */

int close(int fd);
ssize_t read(int fd, void *into, size_t length);
ssize_t write(int fd, const void *from, size_t length);
off_t lseek(int fd, off_t offset, int whence);

#define SEEK_SET	0
#define SEEK_CUR	1
#define SEEK_END	2

/* Only the two questions this system can answer: how big a page is, and how
 * many descriptors a process may hold. Both come from the kernel rather than
 * from a constant here, because both are the kernel's to decide. */
long sysconf(int name);

#define _SC_PAGESIZE	30
#define _SC_OPEN_MAX	4

/* How many files one process may hold open.
 *
 * The kernel's `PROCESS_FDS_MAX`, written here as well because there is no
 * call that asks -- and the two are compared by `recon_libc_posix_tests`,
 * which reads the kernel's own header while it runs. A program that believes
 * it may hold more descriptors than it may is a program that fails on the last
 * one, far from here. */
#define RECON_OPEN_MAX	32

/*
 * End this process now, running nothing on the way out.
 *
 * The one a signal handler may call. What a handler is allowed to do is a
 * short list and `exit` is not on it -- it may run cleanup belonging to code
 * the handler interrupted, from inside the interruption.
 *
 * On this library the difference is smaller than on a hosted one, because
 * `exit` here runs no atexit handlers and flushes no streams (there are none
 * to run and the FILE layer flushes on close). It is offered under its own
 * name anyway, because a caller reaching for `_exit` is reaching for the
 * guarantee rather than for the behaviour it happens to have today.
 */
void _exit(int code) __attribute__((noreturn));

/* --- declared, and not yet linkable -------------------------------------- */

/*
 * Which session a process belongs to.
 *
 * ReconOS has no sessions to belong to -- there is one login and no notion of
 * a process group leader -- so this is **declared and not defined**, which is
 * this header's own rule for exactly this case: a caller fails to link,
 * naming `getsid`, rather than receiving a plausible wrong number.
 *
 * The plausible wrong number matters here. `src/recon_taskmgr.c` asks
 * `getsid(0)` to find out which session is its own, and then shows every
 * process in *other* sessions differently. A stub answering 0 would make every
 * process on the machine look like somebody else's.
 */
pid_t getsid(pid_t pid);

int unlink(const char *path);
int rmdir(const char *path);
int access(const char *path, int how);

#define F_OK	0
#define X_OK	1
#define W_OK	2
#define R_OK	4

#endif /* RECON_UNISTD_H */
