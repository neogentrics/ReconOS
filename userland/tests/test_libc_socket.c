/*
 * Sockets, held against the host's.
 *
 * The five system calls landed on 15 September with no caller at all, and the
 * kernel session said plainly what that meant: **the claim that a socket
 * descriptor works with read and write is argued, not measured.** Their
 * self-test cannot hold one -- a kernel thread has no process, so `fd_install`
 * fails there for sockets exactly as it does for pipes.
 *
 * This is the first thing to call them. Half of what it checks is ordinary
 * differential work; the other half exists because of three places this
 * deliberately differs from the system it replaces, and each of those is a
 * place where agreeing with the host would mean the library was wrong.
 *
 * --- Where it is differential, and where it cannot be ---
 *
 * `hostsys.c` answers the five primitives with real POSIX sockets, so
 * ReconOS's `connect` and the host's talk to the same listener over the
 * loopback and are required to behave the same way. That covers making a
 * socket, giving it an address, listening, connecting, and moving bytes.
 *
 * It cannot cover the *kernel's* behaviour, and this file says so rather than
 * implying otherwise: on the host these calls reach Linux. What is being
 * compared is the library's own arithmetic -- unpacking a `sockaddr_in`,
 * refusing a family it does not know, turning a status into an `errno`. The
 * kernel's half is settled on the machine, by `recon_init`.
 *
 * Run with: ./build/recon_libc_socket_tests
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Ours, renamed by prefix.h on the way past. */
int recon_libc_socket(int domain, int type, int protocol);
int recon_libc_bind(int fd, const void *address, unsigned int length);
int recon_libc_listen(int fd, int backlog);
int recon_libc_accept(int fd, void *address, unsigned int *length);
int recon_libc_connect(int fd, const void *address, unsigned int length);
long recon_libc_send(int fd, const void *buffer, size_t length, int flags);
long recon_libc_recv(int fd, void *buffer, size_t length, int flags);
int recon_libc_setsockopt(int fd, int level, int option, const void *value,
    unsigned int length);
int recon_libc_getsockopt(int fd, int level, int option, void *value,
    unsigned int *length);
int recon_libc_close(int fd);
long recon_libc_write(int fd, const void *from, unsigned long length);
long recon_libc_read(int fd, void *into, unsigned long length);

/*
 * **ReconOS's errno, not the host's.**
 *
 * This file is deliberately not renamed by `prefix.h` -- that is what lets it
 * call the host's sockets for comparison -- so a bare `errno` here is Linux's,
 * and the library sets `recon_errno`. The first run of this suite read the
 * host's and saw 9 and 11 left over from calls made on the way, which look
 * enough like plausible answers to be believed.
 */
extern int recon_errno;

static int g_failures;
static int g_checks;

static void check(int condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void same_int(int got, int wanted, const char *what) {
    g_checks++;
    if (got != wanted) {
        g_failures++;
        printf("  FAIL: %s -- got %d, wanted %d\n", what, got, wanted);
    }
}

/* An address on the loopback, built the way every caller builds one. */
static void loopback(struct sockaddr_in *into, unsigned short port) {
    memset(into, 0, sizeof(*into));
    into->sin_family = AF_INET;
    into->sin_port = htons(port);
    into->sin_addr.s_addr = htonl(0x7F000001);
}

/* --- Tests --- */

static void test_it_makes_a_socket(void) {
    printf("a socket is made, and is a descriptor like any other\n");

    int fd = recon_libc_socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "a stream socket opens");

    int second = recon_libc_socket(AF_INET, SOCK_STREAM, 0);
    check(second >= 0, "and a second one");
    check(second != fd, "with a descriptor of its own");

    check(recon_libc_close(fd) == 0, "it closes");
    check(recon_libc_close(second) == 0, "and so does the other");

    /*
     * Closing it twice is refused. This is the part that is about the
     * descriptor table rather than about sockets: a socket that was never
     * really installed as a file would close twice quite happily.
     */
    check(recon_libc_close(fd) != 0, "closing it again is refused");
    same_int(recon_errno, EBADF, "and says the descriptor names nothing");

    int datagram = recon_libc_socket(AF_INET, SOCK_DGRAM, 0);
    check(datagram >= 0, "a datagram socket opens too");
    if (datagram >= 0) {
        recon_libc_close(datagram);
    }
}

static void test_what_it_refuses_to_make(void) {
    printf("a socket it cannot make is refused, and says which part\n");

    check(recon_libc_socket(AF_UNIX, SOCK_STREAM, 0) < 0,
        "a family it does not have is refused");
    same_int(recon_errno, EAFNOSUPPORT, "naming the family as the problem");

    check(recon_libc_socket(AF_INET, 99, 0) < 0,
        "a type it does not have is refused");
    same_int(recon_errno, ESOCKTNOSUPPORT, "naming the type");

    check(recon_libc_socket(AF_INET, SOCK_STREAM, 132) < 0,
        "and a protocol that is not the one for that type");
    same_int(recon_errno, EPROTONOSUPPORT, "naming the protocol");

    /*
     * The two that are allowed: no protocol at all, and the one that type
     * would have got anyway. Refusing IPPROTO_TCP on a stream would turn
     * perfectly ordinary code away for naming what it was going to be given.
     */
    int fd = recon_libc_socket(AF_INET, SOCK_STREAM, 6);
    check(fd >= 0, "IPPROTO_TCP on a stream is what it would have been");
    if (fd >= 0) {
        recon_libc_close(fd);
    }
}

static void test_an_address_it_does_not_understand(void) {
    printf("an address whose family is not AF_INET is refused, not read\n");

    int fd = recon_libc_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        check(0, "no socket to bind");
        return;
    }

    struct sockaddr_in where;
    loopback(&where, 0);

    /*
     * The family field is the only thing that says what the other fourteen
     * bytes mean. A version that skipped this check would read the first four
     * bytes of a `sockaddr_un`'s path as an IPv4 address and bind to it --
     * which is a perfectly plausible address.
     */
    where.sin_family = AF_UNIX;
    check(recon_libc_bind(fd, &where, sizeof(where)) != 0,
        "a family it does not know is refused");
    same_int(recon_errno, EAFNOSUPPORT, "and says so");

    /* And something far too short to be an address at all. */
    loopback(&where, 0);
    check(recon_libc_bind(fd, &where, 4) != 0,
        "an address shorter than one is refused");
    same_int(recon_errno, EINVAL, "as invalid rather than as a family problem");

    check(recon_libc_bind(fd, NULL, sizeof(where)) != 0,
        "and no address at all is refused");

    recon_libc_close(fd);
}

/*
 * The whole path, against the host's own sockets: bind, listen, connect,
 * accept, and bytes both ways.
 *
 * Both ends are ReconOS's calls here -- the point is not that Linux can talk
 * to itself, it is that this library's unpacking and status handling produce a
 * working conversation over the same primitives the kernel will answer.
 */
static void test_a_conversation(void) {
    printf("a listener, a connection, and bytes in both directions\n");

    int server = recon_libc_socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        check(0, "no socket to listen on");
        return;
    }

    struct sockaddr_in where;
    loopback(&where, 0);          /* port 0: the host picks a free one */

    if (recon_libc_bind(server, &where, sizeof(where)) != 0) {
        check(0, "the listener would not bind");
        recon_libc_close(server);
        return;
    }
    check(1, "it binds");

    check(recon_libc_listen(server, 4) == 0, "and listens");

    /*
     * Which port it got. Asked of the host directly, because there is no
     * `getsockname` in this library and inventing one for a test would be
     * testing the test.
     */
    struct sockaddr_in got;
    socklen_t got_length = sizeof(got);
    if (getsockname(server, (struct sockaddr *)&got, &got_length) != 0) {
        check(0, "could not find out which port it bound to");
        recon_libc_close(server);
        return;
    }

    /* Nobody has connected yet, and accept says so without waiting. */
    check(recon_libc_accept(server, NULL, NULL) < 0,
        "accept with nobody waiting refuses");
    same_int(recon_errno, EAGAIN, "with EAGAIN, which means try again, not stop");

    int client = recon_libc_socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in to;
    loopback(&to, ntohs(got.sin_port));

    check(recon_libc_connect(client, &to, sizeof(to)) == 0, "the client connects");

    int served = recon_libc_accept(server, NULL, NULL);
    check(served >= 0, "and the listener accepts it");

    if (served >= 0) {
        /*
         * **The claim this file exists for**, on the host side of it: a socket
         * descriptor is a file, so `write` and `read` serve a connection. The
         * kernel's half of the same claim is settled by `recon_init`.
         */
        static const char HELLO[] = "hello from the client";
        check(recon_libc_write(client, HELLO, sizeof(HELLO) - 1) ==
                (long)(sizeof(HELLO) - 1),
            "write puts bytes into the connection");

        char back[64];
        long had = recon_libc_read(served, back, sizeof(back) - 1);
        check(had == (long)(sizeof(HELLO) - 1), "read takes the same number out");
        if (had > 0) {
            back[had] = '\0';
            check(strcmp(back, HELLO) == 0, "and they are the same bytes");
        }

        /* And the other way, through send and recv, which are those two with
         * a flags argument. */
        static const char REPLY[] = "and hello back";
        check(recon_libc_send(served, REPLY, sizeof(REPLY) - 1, 0) ==
                (long)(sizeof(REPLY) - 1),
            "send is write");

        char answer[64];
        long said = recon_libc_recv(client, answer, sizeof(answer) - 1, 0);
        check(said == (long)(sizeof(REPLY) - 1), "recv is read");
        if (said > 0) {
            answer[said] = '\0';
            check(strcmp(answer, REPLY) == 0, "and the reply arrives whole");
        }

        recon_libc_close(served);
    }

    recon_libc_close(client);
    recon_libc_close(server);
}

static void test_a_flag_it_cannot_honour(void) {
    printf("a flag it cannot honour is refused, not ignored\n");

    int fd = recon_libc_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        check(0, "no socket");
        return;
    }

    char buffer[8];

    /*
     * MSG_PEEK is the one that matters: a caller asking to look without
     * consuming, given a consuming read instead, loses the bytes it was
     * looking at and has no way to know.
     */
    check(recon_libc_recv(fd, buffer, sizeof(buffer), MSG_PEEK) < 0,
        "recv with MSG_PEEK is refused");
    same_int(recon_errno, EOPNOTSUPP, "because there is no peek underneath");

    check(recon_libc_send(fd, "x", 1, MSG_OOB) < 0,
        "send with MSG_OOB is refused");
    same_int(recon_errno, EOPNOTSUPP, "because there is no out-of-band data");

    recon_libc_close(fd);
}

/*
 * Every option refused, and that being the honest answer.
 *
 * `recon_control.c` asks for a receive timeout, a send timeout and
 * SO_REUSEADDR at five call sites, and ignores what it is told at all five. So
 * refusing costs those callers nothing -- and reporting success would mean a
 * program that set a receive timeout waiting for ever on a read that was
 * supposed to give up, with nothing anywhere saying why.
 */
static void test_every_option_is_refused(void) {
    printf("setsockopt refuses rather than accepting and doing nothing\n");

    int fd = recon_libc_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        check(0, "no socket");
        return;
    }

    int one = 1;
    check(recon_libc_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one,
            sizeof(one)) != 0,
        "SO_REUSEADDR is refused");
    same_int(recon_errno, ENOPROTOOPT, "as an option that is not there");

    struct timeval limit = { 5, 0 };
    check(recon_libc_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit,
            sizeof(limit)) != 0,
        "and so is a receive timeout -- there is none underneath");

    int held = 0;
    unsigned int held_length = sizeof(held);
    check(recon_libc_getsockopt(fd, SOL_SOCKET, SO_ERROR, &held,
            &held_length) != 0,
        "and reading one back is refused too");

    recon_libc_close(fd);
}

int main(void) {
    printf("ReconOS C library: sockets\n\n");

    test_it_makes_a_socket();
    test_what_it_refuses_to_make();
    test_an_address_it_does_not_understand();
    test_a_conversation();
    test_a_flag_it_cannot_honour();
    test_every_option_is_refused();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
