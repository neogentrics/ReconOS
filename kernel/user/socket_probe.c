/* A program that opens a socket, from ring 3, and reports what happened.
 *
 * --- why this exists --------------------------------------------------------
 *
 * The five socket system calls landed with a self-test that could not test the
 * thing they are for. `socket_syscall_test` runs on a kernel thread, and a
 * kernel thread has no process -- so it has no descriptor table, `fd_install`
 * fails, and every assertion it can make is about a refusal. It says so in its
 * own comment:
 *
 *     What this does not cover [...] that a socket descriptor works with
 *     SYS_READ, SYS_WRITE and SYS_CLOSE, which is the entire design claim of
 *     this file. That needs a ring-3 program.
 *
 * This is the ring-3 program. The claim it settles is not that TCP works --
 * `net_self_test` covers the stack, and a connection needs a peer this has no
 * way to arrange. It is the claim underneath: **that a socket is a descriptor
 * like any other**, which is the reason there is no `SYS_SEND` and no
 * `SYS_RECV`, and which nothing has ever checked.
 *
 * --- how it reports ---------------------------------------------------------
 *
 * By exit code, like `hello.c` and the facts program, because a failure has to
 * name the step. A program that prints and exits zero tells you it ran; a
 * program that exits 4 tells you `listen` refused a socket it had just bound.
 *
 * --- and it defines no wrappers ---------------------------------------------
 *
 * Every call here goes through `RECON_CALL1`/`RECON_CALL3` directly rather
 * than through a `recon_socket()` of its own. The C library's socket wrappers
 * belong to the desktop session -- they said so, and they are writing them --
 * and a second set invented here to make one test program read nicely would be
 * two designs for the same call, settled by whichever merged last.
 *
 * `recon_close` and `recon_exit` already exist and are used as they are.
 *
 * Nothing here is allowed to loop forever. A ring-3 program that hangs takes
 * the boot's self-test with it, and the kernel's deadline would report "never
 * reached its exit call" -- true, unhelpful, and indistinguishable from a
 * program that faulted on its first instruction.
 */
#include <recon.h>

/* Every exit code this can produce, so that the kernel's table and this file
 * cannot drift apart silently: the numbers are here and the words are there,
 * and both are written from this list. */
enum {
	OK              = 55,	/* 0 would mean it never reached the exit */
	E_SOCKET        = 1,	/* SYS_SOCKET did not return a descriptor */
	E_DISTINCT      = 2,	/* it returned one that names something else */
	E_BIND          = 3,
	E_LISTEN        = 4,
	E_ACCEPT_BLOCK  = 5,	/* accept blocked instead of saying EAGAIN */
	E_CLOSE         = 6,
	E_CLOSE_TWICE   = 7,	/* closing it twice was allowed */
	E_BAD_TYPE      = 8,	/* a type nobody defined was accepted */
	E_AFTER_CLOSE   = 9,	/* a closed descriptor still worked */
	E_NOT_A_SOCKET  = 10,	/* a socket call took a pipe's descriptor */
	E_PIPE          = 11	/* SYS_PIPE would not make one to test with */
};

int main(void)
{
	long fd, second, rc;

	/* A type nobody defined, first, because a call that accepts anything
	 * is not a call that has checked anything. */
	if (RECON_CALL1(SYS_SOCKET, 99) >= 0)
		return E_BAD_TYPE;

	/* **A descriptor that is not a socket, handed to a socket call.**
	 *
	 * Descriptor 1 is the screen. `bind` must refuse it -- and refusing it
	 * means checking what the descriptor *is*, not merely that the process
	 * owns it. A version that trusted the number and read the file's
	 * `private` pointer as a `struct socket *` would follow whatever the
	 * screen keeps there, on a path a program chooses.
	 *
	 * This assertion exists because the test was deliberately run against
	 * a kernel with that check removed, and everything passed. Nothing
	 * else here or in `socket_syscall_test` ever hands a *live* non-socket
	 * descriptor to one of these calls: the kernel-side test cannot, for
	 * want of a descriptor table, and the rest of this program only ever
	 * uses a socket it made itself. */
	/* **EBADF specifically, not merely a failure.** That distinction is
	 * the whole assertion, and it was learned by breaking the kernel on
	 * purpose: with the type check removed, `bind` still refused -- it
	 * read the screen's `private` as a `struct socket *`, `socket_bind`
	 * found nonsense in it, and the call returned EINVAL. A test that only
	 * asked *did it fail* passed against a kernel following a wild
	 * pointer.
	 *
	 * EBADF means the descriptor was rejected for **what it names**, before
	 * anything was dereferenced. EINVAL means something looked at it.
	 *
	 * **A pipe, not the screen**, and that also had to be learned: the
	 * screen's file keeps nothing in `private`, so even the broken kernel
	 * returned NULL from its cast and its own null check produced EBADF.
	 * The test agreed with the fault. A pipe's `private` points at a real
	 * pipe, so a kernel that trusts the number reaches it and a kernel
	 * that checks the type does not. */
	{
		int pipe_fds[2];

		if (RECON_CALL1(SYS_PIPE, pipe_fds) != 0)
			return E_PIPE;

		if (RECON_CALL3(SYS_BIND, pipe_fds[0], 0, 51500) != -11)
			return E_NOT_A_SOCKET;

		recon_close(pipe_fds[0]);
		recon_close(pipe_fds[1]);
	}

	fd = RECON_CALL1(SYS_SOCKET, 1);	/* SOCK_STREAM */
	if (fd < 0)
		return E_SOCKET;

	/* **A socket is a descriptor, and this is the whole point.** It must
	 * come from the same numbering as every other open file -- not a
	 * separate space that happens to start at zero -- or `read`, `write`
	 * and `close` cannot serve it and the design is wrong. Descriptors 0,
	 * 1 and 2 are already taken by the screen, so a socket must not be
	 * one of them. */
	if (fd < 3)
		return E_DISTINCT;

	if (RECON_CALL3(SYS_BIND, fd, 0, 51423) != 0)
		return E_BIND;

	if (RECON_CALL2(SYS_LISTEN, fd, 4) != 0)
		return E_LISTEN;

	/* Nobody is connecting. This must say so rather than block -- the
	 * kernel's own comment promises EAGAIN and a blocking accept here
	 * would hang this program and be reported as a program that never
	 * exited. */
	rc = RECON_CALL1(SYS_ACCEPT, fd);
	if (rc >= 0)
		return E_ACCEPT_BLOCK;	/* a connection nobody made */

	/* And `close` takes it, which is the other half of being a
	 * descriptor. */
	if (recon_close(fd) != 0)
		return E_CLOSE;

	/* Closing it twice must be refused. A descriptor that can be closed
	 * repeatedly is one whose slot can be freed twice, and the second free
	 * lands on whatever took the number in between. */
	if (recon_close(fd) == 0)
		return E_CLOSE_TWICE;

	/* And it must be gone. Binding a closed descriptor reaching the socket
	 * layer would mean the slot still points at freed state. */
	if (RECON_CALL3(SYS_BIND, fd, 0, 51424) == 0)
		return E_AFTER_CLOSE;

	/* One more, to prove the number is reusable rather than leaked: after
	 * a close the next socket should be able to have it. Not required to
	 * be the *same* number -- nothing promises that -- only that asking
	 * again works. */
	second = RECON_CALL1(SYS_SOCKET, 1);
	if (second < 0)
		return E_SOCKET;
	recon_close(second);

	return OK;
}
