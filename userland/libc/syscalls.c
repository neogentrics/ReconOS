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
#include <recon_machine.h>

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

long recon_sys_list(const char *path, char *names, unsigned long names_len)
{
	return (long)RECON_CALL4(SYS_LIST, path, recon_strlen(path),
				 names, names_len);
}

long recon_sys_mkdir(const char *path, unsigned long mode)
{
	return (long)recon_mkdir(path, recon_strlen(path), mode);
}

long recon_sys_socket(int type)
{
	return (long)recon_socket((u64)type);
}

long recon_sys_bind(int fd, unsigned int addr, int port)
{
	return (long)recon_bind(fd, (u64)addr, (u64)port);
}

long recon_sys_listen(int fd, int backlog)
{
	return (long)recon_listen(fd, (u64)backlog);
}

long recon_sys_accept(int fd)
{
	return (long)recon_accept(fd);
}

long recon_sys_connect(int fd, unsigned int addr, int port)
{
	return (long)recon_connect(fd, (u64)addr, (u64)port);
}

long recon_sys_page_size(void)
{
	struct recon_machine machine;

	if (recon_machine_facts(&machine, sizeof(machine)) < 0)
		return -1;

	return (long)machine.page_size;
}

void recon_sys_exit(int code)
{
	recon_exit(code);
}

long recon_sys_getpid(void)
{
	return (long)recon_getpid();
}

long recon_sys_kill(long pid, unsigned long signal_number)
{
	return (long)recon_kill((i64)pid, signal_number);
}

long recon_sys_sigaction(unsigned long signal_number, unsigned long what,
			 unsigned long handler, unsigned long restorer)
{
	return (long)recon_sigaction(signal_number, what, handler, restorer);
}

/*
 * Where a signal handler returns to.
 *
 * `SYS_SIGACTION` refuses a handler without a **restorer** -- an address the
 * handler returns to, holding a few instructions that invoke SYS_SIGRETURN.
 * The kernel's own comment says why: *"a handler with nowhere to return to
 * runs once and then executes whatever follows it in memory."*
 *
 * It has to be machine code: there is no C for "return through a system
 * call". A top-level `__asm__` block rather than a `naked` function, because
 * `naked` is not available for every architecture on every compiler and a
 * function that merely looks empty still gets a prologue that would run first.
 *
 * **The number in the assembly is checked by the compiler.** A literal in an
 * `__asm__` block is exactly the kind of hand-written syscall number that
 * `scripts/check-syscall-numbers.py` exists to police and cannot see, so the
 * assertion below holds it against the enum. If the numbers ever shift, this
 * file stops building rather than returning into the wrong call.
 */
_Static_assert(SYS_SIGRETURN == 22,
	"the restorer's syscall number no longer matches SYS_SIGRETURN");

#if defined(__x86_64__)
__asm__(
	".text\n"
	".globl recon_sig_restorer\n"
	".hidden recon_sig_restorer\n"
	".type recon_sig_restorer, @function\n"
	"recon_sig_restorer:\n"
	"    movq $22, %rax\n"
	"    syscall\n"
	".size recon_sig_restorer, .-recon_sig_restorer\n");
#elif defined(__aarch64__)
__asm__(
	".text\n"
	".globl recon_sig_restorer\n"
	".hidden recon_sig_restorer\n"
	".type recon_sig_restorer, %function\n"
	"recon_sig_restorer:\n"
	"    mov x8, #22\n"
	"    svc #0\n"
	".size recon_sig_restorer, .-recon_sig_restorer\n");
#else
/*
 * Refused rather than left out. A build with no restorer would compile, link,
 * and fail the first time a program installed a handler -- at a moment chosen
 * by whatever went wrong, which is the worst time to find a missing piece.
 */
#error "no signal restorer for this architecture"
#endif

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
