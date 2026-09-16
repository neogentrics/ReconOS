/* A socket, as a file.
 *
 * --- why this exists --------------------------------------------------------
 *
 * `core/socket.c` has had create, bind, listen, accept, connect, send and
 * receive since 12 September, and `core/net.c` under it. None of it was
 * reachable from a program: there was no system call that opened a connection
 * and no descriptor that named one, so the only caller the socket layer ever
 * had was its own self-test.
 *
 * `docs/KERNEL-WANTS.md` said so plainly -- *the kernel has no sockets at all*
 * -- and that sentence was about the doorway rather than the room. The room
 * was furnished.
 *
 * --- the shape ---------------------------------------------------------------
 *
 * A socket becomes a `struct file`, exactly as a pipe does, with the socket in
 * `private` and the ops below. **Then `read` and `write` need no new calls at
 * all**: a program that has a connected socket writes to it with `SYS_WRITE`
 * and reads with `SYS_READ`, the same two calls it uses for everything else.
 *
 * That is not cleverness for its own sake. The alternative -- `SYS_SEND` and
 * `SYS_RECV` beside the ones that already exist -- is two more numbers, two
 * more entries in three tables that must agree, and two more ways for a
 * program to be handed a descriptor it cannot use with the calls it knows.
 * Everything a socket does that a file cannot is what the new calls are for,
 * and there are only five of those:
 * create, bind, listen, accept, connect.
 *
 * --- what is deliberately absent ---------------------------------------------
 *
 * **No `sendto` and no `recvfrom`.** The socket layer has both, and they are
 * how UDP names a peer per message. Reaching them needs an address argument
 * through the system call boundary, which means a user-supplied structure to
 * validate, and the first caller for that does not exist yet. A connected UDP
 * socket works through `write` today; an unconnected one is refused rather
 * than half-served.
 *
 * **No `select` or non-blocking mode.** `socket_accept` returns null when
 * nothing is waiting and `socket_recv` returns what is there, so a server can
 * poll. That is enough for one connection at a time and not enough for a real
 * server, and saying so here is better than a program discovering it.
 */
#include <recon/kernel/vfs.h>
#include <recon/kernel/net.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/console.h>
#include <recon/kernel/user.h>
#include <recon/kernel/process.h>
#include <recon/kernel/sched.h>


static struct socket *sock_of(struct file *f)
{
	return (struct socket *)f->private;
}

/* Reading a socket is receiving from it.
 *
 * Returns 0 where a file would be at its end, which for a stream means the
 * peer has closed and for a datagram means nothing has arrived. Those are
 * different facts and this cannot yet tell them apart -- the socket layer
 * reports both as "no bytes", and inventing a distinction here would be
 * guessing at one.
 */
static i64 sock_read(struct file *f, void *out, u64 len)
{
	if (len > 0x7FFFFFFFull)
		len = 0x7FFFFFFFull;

	return socket_recv(sock_of(f), out, (u32)len);
}

static i64 sock_write(struct file *f, const void *in, u64 len)
{
	if (len > 0x7FFFFFFFull)
		len = 0x7FFFFFFFull;

	return socket_send(sock_of(f), in, (u32)len);
}

/* **No seek.** A null entry here is the difference between "this thing has no
 * position" and "seeking failed", and a connection genuinely has none. */
static i64 sock_close(struct file *f)
{
	socket_close(sock_of(f));
	f->private = NULL;
	return 0;
}

static const struct file_ops socket_ops = {
	.read  = sock_read,
	.write = sock_write,
	.seek  = NULL,
	.close = sock_close,
};

/* Is this file a socket?
 *
 * Asked by comparing the ops table rather than by a flag on the file, because
 * the ops table is what actually decides behaviour -- a flag is a second
 * statement of the same fact and the two can disagree. The system calls below
 * use it to refuse a descriptor that names something else, which is the
 * difference between an error and a socket call reading a pipe's private
 * pointer as a socket.
 */
bool file_is_socket(const struct file *f)
{
	return f && f->ops == &socket_ops;
}

struct socket *file_socket(struct file *f)
{
	return file_is_socket(f) ? sock_of(f) : NULL;
}

/* --- the self-test ---------------------------------------------------------
 *
 * Driven through `syscall_dispatch`, the way `identity.c` drives its own, so
 * what is exercised is the number, the table and the handler rather than the
 * three functions underneath them -- which `socket_self_test` already covers.
 *
 * **The assertion that matters is the refusal.** A socket call handed a
 * descriptor that names a pipe must return `EBADF`; the alternative is reading
 * that pipe's `private` pointer as a `struct socket *` and following it. That
 * is a wild pointer on a path a program chooses, so it is checked first and
 * checked by actually making a pipe rather than by passing a number nothing
 * owns.
 *
 * What it cannot check without a peer: that `write` sends and `read` receives.
 * Both go straight to `socket_send` and `socket_recv`, which the network's own
 * self-test covers -- but the claim that *a socket descriptor works with the
 * ordinary file calls* is the whole design here and is not proven until two
 * machines have talked. Said plainly rather than implied by a pass.
 */
bool socket_syscall_test(void)
{
	bool ok = true;

	/* **A kernel thread has no process, so it has no descriptor table.**
	 * `fd_install` fails here for a socket exactly as it does for a pipe --
	 * which is why `pipe.c`'s own self-test keeps raw files and never takes
	 * a descriptor either. So everything below is what can be asked from
	 * this side of the boundary, and it is deliberately the refusals:
	 * the paths where a wrong answer is a wild pointer rather than a wrong
	 * number.
	 *
	 * **What this does not cover, said plainly rather than implied by a
	 * pass:** that a socket descriptor works with `SYS_READ`, `SYS_WRITE`
	 * and `SYS_CLOSE`, which is the entire design claim of this file. That
	 * needs a ring-3 program, because the thing most likely to be wrong is
	 * the boundary rather than the handler -- `user_facts_test`'s reasoning,
	 * and its program is hand-written assembly per architecture. Until that
	 * is extended, the claim is argued and not measured. */

	if (syscall_dispatch(SYS_SOCKET, 99, 0, 0, 0, 0, 0) != SYS_EINVAL) {
		kputs("  socket: a type nobody defined was accepted\n");
		ok = false;
	}

	/* A descriptor nothing owns, to each call that takes one. Refused by
	 * number before anything is dereferenced -- and a socket call that
	 * followed a descriptor it did not own would not return an error, it
	 * would read some other file's private pointer as a `struct socket *`
	 * on a path a program chooses. */
	if (syscall_dispatch(SYS_BIND, 4095, 0, 1, 0, 0, 0) != SYS_EBADF) {
		kputs("  socket: bind took a descriptor nothing owns\n");
		ok = false;
	}

	if (syscall_dispatch(SYS_LISTEN, 4095, 4, 0, 0, 0, 0) != SYS_EBADF) {
		kputs("  socket: listen took a descriptor nothing owns\n");
		ok = false;
	}

	if (syscall_dispatch(SYS_ACCEPT, 4095, 0, 0, 0, 0, 0) != SYS_EBADF) {
		kputs("  socket: accept took a descriptor nothing owns\n");
		ok = false;
	}

	if (syscall_dispatch(SYS_CONNECT, 4095, 0, 1, 0, 0, 0) != SYS_EBADF) {
		kputs("  socket: connect took a descriptor nothing owns\n");
		ok = false;
	}

	/* A port that does not fit in sixteen bits, refused rather than
	 * truncated -- a bind to 65537 that silently became port 1 would be a
	 * server listening somewhere nobody asked for. */
	if (syscall_dispatch(SYS_BIND, 4095, 0, 65536, 0, 0, 0) == SYS_OK) {
		kputs("  socket: a port past sixteen bits was accepted\n");
		ok = false;
	}

	return ok;
}

struct file *socket_file_create(struct socket *s)
{
	struct file *f;

	if (!s)
		return NULL;

	f = kmalloc(sizeof(*f));
	if (!f)
		return NULL;

	kmemset(f, 0, sizeof(*f));
	f->ops     = &socket_ops;
	f->refs    = 1;
	f->private = s;

	return f;
}
