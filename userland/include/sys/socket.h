/*
 * <sys/socket.h>.
 *
 * **Most of this is declared and deliberately not defined**, which is the same
 * stance `stdlib.h` took about `malloc` until there was something to build it
 * on: a caller that needs one of them fails to *link*, naming the symbol, at
 * the moment somebody can still decide what to do about it. A stub that
 * returned zero would let a program run and be wrong later, somewhere else.
 *
 * What is defined here is what the kernel actually offers, which as of kernel
 * 0.2.41 is five calls and the file layer underneath them.
 *
 * --- A socket is a file ---
 *
 * That is the whole design, and it is why `send` and `recv` are one line each.
 * `read`, `write` and `close` serve a connection, so only the things a file
 * cannot do -- make a socket, give it an address, listen, accept, connect --
 * needed numbers of their own.
 *
 * --- What is different from the system this replaces ---
 *
 * Said here rather than discovered, because each one is a loop somebody would
 * otherwise write wrong:
 *
 *   - **`accept` does not block.** It answers -1 with `EAGAIN` when nobody is
 *     waiting, because a listening socket has no wait queue on this kernel
 *     yet. A server polls. On Linux the same code blocks, so a program written
 *     against one and run on the other either spins or stalls.
 *   - **`sockaddr` is not how an address is passed.** The kernel takes an
 *     address as a number and a port as a number. The `bind` and `connect`
 *     here take a `struct sockaddr_in` because 35 call sites in the desktop
 *     already say so, and unpack it -- so anything that is not
 *     `AF_INET` is refused rather than reinterpreted.
 *   - **`setsockopt` refuses every option.** There is no timeout, no
 *     `SO_REUSEADDR` and no keepalive underneath it. Returning success and
 *     doing nothing would be the more comfortable lie: a caller that sets a
 *     receive timeout and is told it worked waits forever on a read that was
 *     supposed to give up.
 */
#ifndef RECON_SYS_SOCKET_H
#define RECON_SYS_SOCKET_H

#include "types.h"

/*
 * **`netinet/in.h` owns the address**, and has since v0.4.32 -- `AF_INET`,
 * `SOCK_STREAM`, `SOCK_DGRAM`, `struct in_addr` and `struct sockaddr_in` are
 * all there. This header defines none of them again.
 *
 * Defining them twice is what this file did first, and it is the fault
 * `libc/time.c`'s exception exists to prevent: two definitions of one struct,
 * kept identical by hand, in a tree where `arpa/inet.h` reads one and
 * `sys/socket.h` the other.
 */
#include "../netinet/in.h"

#define PF_INET AF_INET

/* For `setsockopt`, which refuses all of them. They are here so that a caller
 * naming one compiles, and finds out at run time with a reason. */
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21
#define SO_ERROR     4
#define SO_KEEPALIVE 9

/* For `shutdown`, which is also refused. */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

/*
 * The shape every caller passes, and which this library takes apart.
 *
 * `struct sockaddr` exists to be cast to, which is a way of saying the type
 * system was given up on. It is kept because the desktop's call sites are
 * written against it; what is *not* kept is any pretence that another family
 * might turn up, and `bind` and `connect` say so by refusing one.
 */
struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

/* --- What the kernel has --- */

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *address, socklen_t length);
int listen(int fd, int backlog);

/*
 * The next connection, or -1 with `errno` set to `EAGAIN` when there is none.
 *
 * `address` may be NULL, and is filled in with nothing useful when it is not:
 * the kernel does not report who connected yet. Left in the signature because
 * the call sites pass it, and filled with zeroes rather than with something
 * invented.
 */
int accept(int fd, struct sockaddr *address, socklen_t *length);
int connect(int fd, const struct sockaddr *address, socklen_t length);

/* A socket is a file, so these are `write` and `read` with a flags argument
 * that must be zero -- there is no `MSG_` anything underneath. */
long send(int fd, const void *buffer, size_t length, int flags);
long recv(int fd, void *buffer, size_t length, int flags);

/* --- What it does not have --- */

/*
 * Defined, and every option refused with `ENOPROTOOPT`.
 *
 * Not declared-and-absent like the rest below, because the desktop's five call
 * sites all *ignore the result* -- so leaving these unlinkable would stop the
 * whole of `recon_control.c` linking over four lines that do not check what
 * they are told. Refusing lets it link and keeps the refusal true.
 */
int setsockopt(int fd, int level, int option, const void *value,
    socklen_t length);
int getsockopt(int fd, int level, int option, void *value, socklen_t *length);

/*
 * **Declared and not defined.** A caller fails to link, naming the symbol.
 *
 * `shutdown` needs a half-close the kernel does not have. `sendto` and
 * `recvfrom` need an unconnected datagram to carry an address per message, and
 * the kernel refuses such a socket rather than half-serving it.
 */
int shutdown(int fd, int how);
long sendto(int fd, const void *buffer, size_t length, int flags,
    const struct sockaddr *to, socklen_t to_length);
long recvfrom(int fd, void *buffer, size_t length, int flags,
    struct sockaddr *from, socklen_t *from_length);

#endif
