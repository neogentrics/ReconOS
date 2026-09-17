/*
 * Sockets, over the five calls the kernel took numbers for.
 *
 * See `userland/include/sys/socket.h` for the three places this differs from
 * the system it replaces, each of which is a loop somebody would otherwise
 * write wrong. The short version: `accept` does not block, an address is a
 * number rather than a `sockaddr`, and `setsockopt` refuses everything.
 *
 * --- Why there is so little here ---
 *
 * Because a socket is a `struct file` on the other side, so `read`, `write`
 * and `close` already serve a connection -- they are `posix.c`'s and they took
 * no changes at all. `send` and `recv` are one line each on top of them, and
 * the rest of this file is unpacking a `sockaddr_in` into the two numbers the
 * kernel wants and turning a kernel status into an `errno`.
 *
 * --- What this file is the first thing to test ---
 *
 * The kernel session flagged it themselves, and it is worth repeating where
 * somebody will find it: **that a socket descriptor works with read and write
 * is argued, not measured.** Their self-test cannot hold a descriptor, because
 * a kernel thread has no process and `fd_install` fails there for sockets
 * exactly as it does for pipes. Proving it needs a program in ring 3, and
 * `userland/init/recon_init.c` is one.
 */

#include "internal.h"

/*
 * **The public header, included directly**, which the library otherwise never
 * does -- see the top of `internal.h`.
 *
 * The same exception `libc/time.c` takes, for the same reason: there is one
 * definition of `struct sockaddr_in` rather than two kept identical by hand.
 * It is safe for the same reason too -- `prefix.h` arrives by `-include` and
 * renames what the header declares on the way past, so the host's copy never
 * wins the include path.
 */
#include "../include/errno.h"
#include "../include/sys/socket.h"
/* `read` and `write` are posix.c's, and a socket is a file. */
#include "../include/unistd.h"
/*
 * A `sockaddr_in` taken apart into the two numbers the kernel wants.
 *
 * **Refuses anything that is not AF_INET**, rather than reading the first four
 * bytes of whatever it was given as an address. A `sockaddr` is a shape
 * callers cast *to*, so the family field is the only thing standing between
 * this and interpreting a `sockaddr_un`'s path as an IPv4 address -- and it
 * would be a perfectly plausible one.
 *
 * The address and port arrive in network order, because `htons` and
 * `inet_pton` put them there and every call site in the desktop uses them. The
 * kernel wants host order. Converting here means the conversion happens once,
 * at the boundary, rather than at each of the call sites that would each have
 * to remember.
 */
static int unpack(const struct sockaddr *address, size_t length,
        unsigned int *out_addr, unsigned int *out_port)
{
    const struct sockaddr_in *in;

    if (address == NULL || length < sizeof(*in)) {
        errno = EINVAL;
        return -1;
    }

    in = (const struct sockaddr_in *)address;

    if (in->sin_family != AF_INET) {
        /* Not "unsupported address" -- the family is the one field that says
         * what the other fourteen bytes mean, and a wrong one means this does
         * not know what it is holding. */
        errno = EAFNOSUPPORT;
        return -1;
    }

    *out_addr = (unsigned int)ntohl(in->sin_addr.s_addr);
    *out_port = (unsigned int)ntohs(in->sin_port);
    return 0;
}

/* Kernel status to errno, and -1, which is what every call below returns. */
static int refused(long status)
{
    errno = recon_errno_from_status((int)status);
    return -1;
}

int socket(int domain, int type, int protocol)
{
    long fd;

    if (domain != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    /*
     * A protocol of zero means "the usual one for this type", which is the
     * only thing there is. A caller naming IPPROTO_TCP on a stream is asking
     * for what it would get anyway and is allowed; anything else is refused
     * rather than quietly given TCP.
     */
    int kind = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (protocol != 0 &&
            !(kind == SOCK_STREAM && protocol == 6) &&
            !(kind == SOCK_DGRAM && protocol == 17)) {
        errno = EPROTONOSUPPORT;
        return -1;
    }

    /*
     * --- The two flags, and what each means on this kernel ---
     *
     * Stripped before the type is checked, because they are part of the same
     * argument and a type with a flag on it is not an unknown type.
     *
     * **`SOCK_NONBLOCK` is granted, and it is also the only thing on offer.**
     * Nothing on this kernel blocks: `accept` answers EAGAIN when nobody is
     * waiting, and so does a read with nothing to read. A caller asking for
     * non-blocking gets what it asked for; a caller *not* asking for it also
     * gets non-blocking, which is the difference from Linux that the header
     * above spells out. Refusing the flag would be worse than granting it --
     * the program that asks is the one written for this behaviour.
     *
     * **`SOCK_CLOEXEC` is accepted and does nothing**, because there is no
     * `exec` for a descriptor to survive into. It is taken rather than refused
     * so that a program written the careful way still runs; the day this
     * kernel can replace a program, the flag is already being asked for and
     * this comment is where to start.
     */
    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        errno = ESOCKTNOSUPPORT;
        return -1;
    }

    fd = recon_sys_socket((int)type);
    if (fd < 0) {
        return refused(fd);
    }
    return (int)fd;
}

int bind(int fd, const struct sockaddr *address, socklen_t length)
{
    unsigned int addr = 0;
    unsigned int port = 0;
    long status;

    if (unpack(address, length, &addr, &port) != 0) {
        return -1;
    }

    status = recon_sys_bind(fd, (unsigned int)addr, (int)port);
    return status < 0 ? refused(status) : 0;
}

int listen(int fd, int backlog)
{
    long status;

    if (backlog < 0) {
        errno = EINVAL;
        return -1;
    }

    status = recon_sys_listen(fd, backlog);
    return status < 0 ? refused(status) : 0;
}

/*
 * The next connection, or -1 with EAGAIN.
 *
 * **It does not block**, and the errno says which of the two "no" answers this
 * is: `EAGAIN` means try again and anything else means stop. A caller that
 * reads every -1 as an error closes a listener that is working perfectly.
 */
int accept(int fd, struct sockaddr *address, socklen_t *length)
{
    long got = recon_sys_accept(fd);

    if (got < 0) {
        return refused(got);
    }

    /*
     * Zeroed rather than filled in, because the kernel does not report who
     * connected. A caller that reads it gets an address of 0.0.0.0 port 0,
     * which is at least a value nobody will mistake for a real peer -- and
     * leaving the caller's buffer untouched would be worse, since it would
     * hold whatever was on the stack.
     */
    if (address != NULL && length != NULL && *length >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)address;
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        *length = sizeof(*in);
    }

    return (int)got;
}

int connect(int fd, const struct sockaddr *address, socklen_t length)
{
    unsigned int addr = 0;
    unsigned int port = 0;
    long status;

    if (unpack(address, length, &addr, &port) != 0) {
        return -1;
    }

    status = recon_sys_connect(fd, (unsigned int)addr, (int)port);
    return status < 0 ? refused(status) : 0;
}

/*
 * `send` and `recv` are `write` and `read`.
 *
 * That is not a shortcut taken here, it is the kernel's design: a socket is a
 * `struct file`, so the file calls serve a connection. The only thing this
 * adds is refusing a flag, because there is no `MSG_PEEK`, no `MSG_DONTWAIT`
 * and no out-of-band data underneath -- and a caller that asked to peek and
 * was given a consuming read would lose the bytes it was looking at.
 */
long send(int fd, const void *buffer, size_t length, int flags)
{
    if (flags != 0) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return (long)write(fd, buffer, length);
}

long recv(int fd, void *buffer, size_t length, int flags)
{
    if (flags != 0) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return (long)read(fd, buffer, length);
}

/*
 * Every option refused.
 *
 * The five call sites in `recon_control.c` want a receive timeout, a send
 * timeout and `SO_REUSEADDR`, and none of the three exists below this. They
 * all ignore what they are told, so refusing costs them nothing -- and it is
 * the answer that stays true. Reporting success would mean a caller that set a
 * receive timeout waiting for ever on a read that was supposed to give up,
 * which is a fault with no message attached to it.
 *
 * Defined at all, rather than left unlinkable like `shutdown`, precisely
 * because those call sites ignore the result: making them fail to link would
 * stop a 3,000-line file building over four lines that do not check.
 */
int setsockopt(int fd, int level, int option, const void *value,
        socklen_t length)
{
    (void)fd; (void)value; (void)length;

    if (level != SOL_SOCKET) {
        errno = ENOPROTOOPT;
        return -1;
    }

    (void)option;
    errno = ENOPROTOOPT;
    return -1;
}

int getsockopt(int fd, int level, int option, void *value,
        socklen_t *length)
{
    (void)fd; (void)level; (void)option; (void)value; (void)length;
    errno = ENOPROTOOPT;
    return -1;
}
