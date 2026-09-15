/*
 * <netinet/in.h> -- internet addresses, and the byte order they are kept in.
 *
 * `htons` and its three relatives are here, and they are the whole of what
 * this header can do today. Everything that opens a connection is next door in
 * <sys/socket.h>, declared and not defined.
 *
 * --- Only AF_INET ---
 *
 * There is no `AF_INET6` and no `struct sockaddr_in6`, because there is no
 * IPv6 anywhere in this system: `docs/KERNEL.md` lists it as named, absent and
 * deliberately outside the gate. A constant defined for a family nothing can
 * carry is a constant somebody writes a branch for, and that branch is never
 * taken and never tested.
 */

#ifndef RECON_NETINET_IN_H
#define RECON_NETINET_IN_H

/* Quoted, so it finds the one beside it rather than the host's.
 * See the note in userland/libc/posix.c. */
#include "../sys/types.h"

#define AF_UNSPEC	0
#define AF_UNIX		1
#define AF_INET		2

#define SOCK_STREAM	1
#define SOCK_DGRAM	2

#define IPPROTO_IP	0
#define IPPROTO_TCP	6
#define IPPROTO_UDP	17

typedef unsigned short in_port_t;
typedef unsigned int in_addr_t;

struct in_addr {
	in_addr_t s_addr;	/* already in network order */
};

struct sockaddr_in {
	unsigned short sin_family;
	in_port_t sin_port;	/* network order -- use htons */
	struct in_addr sin_addr;
	unsigned char sin_zero[8];
};

#define INADDR_ANY		((in_addr_t)0x00000000)
#define INADDR_LOOPBACK		((in_addr_t)0x7F000001)
#define INADDR_BROADCAST	((in_addr_t)0xFFFFFFFF)

/*
 * Network order is big-endian.
 *
 * `userland/libc/inet.c` builds the arrangement out of the *value* rather than
 * reordering the bytes of its storage, which is the same thing only on a
 * little-endian machine. Both architectures this system runs on are
 * little-endian, so a byte-swap version would pass every test in the rig and
 * be wrong on the first machine that is not.
 */
unsigned short htons(unsigned short value);
unsigned int htonl(unsigned int value);
unsigned short ntohs(unsigned short value);
unsigned int ntohl(unsigned int value);

#endif /* RECON_NETINET_IN_H */
