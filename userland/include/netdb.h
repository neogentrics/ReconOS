/*
 * <netdb.h> -- turning a name into an address.
 *
 * **Only `gai_strerror` is implemented.** `getaddrinfo` needs a resolver,
 * which needs sockets, which need the kernel -- so it and its relatives are
 * declared here and deliberately not defined, and a caller fails to link
 * naming the symbol.
 *
 * The message function is separate from the lookup and arrives first, which
 * looks odd until you see the call site: the desktop's one use of it is
 * reporting a failure it was handed. Turning a number into a sentence is this
 * side's work whether or not the lookup is.
 */

#ifndef RECON_NETDB_H
#define RECON_NETDB_H

/* Quoted, so it finds the one beside it rather than the host's. */
#include "sys/types.h"
#include "netinet/in.h"

/*
 * The failure codes, with glibc's values.
 *
 * Negative, and irregularly spaced, because that is what they are everywhere
 * -- see `userland/include/errno.h` on why this library keeps the numbers it
 * did not choose.
 */
#define EAI_BADFLAGS	(-1)
#define EAI_NONAME	(-2)
#define EAI_AGAIN	(-3)
#define EAI_FAIL	(-4)
#define EAI_FAMILY	(-6)
#define EAI_SOCKTYPE	(-7)
#define EAI_SERVICE	(-8)
#define EAI_MEMORY	(-10)
#define EAI_SYSTEM	(-11)
#define EAI_OVERFLOW	(-12)

#define AI_PASSIVE	0x0001
#define AI_CANONNAME	0x0002
#define AI_NUMERICHOST	0x0004
#define AI_NUMERICSERV	0x0400

#define NI_NUMERICHOST	0x0001
#define NI_NUMERICSERV	0x0002

#define NI_MAXHOST	1025
#define NI_MAXSERV	32

struct addrinfo {
	int ai_flags;
	int ai_family;
	int ai_socktype;
	int ai_protocol;
	unsigned int ai_addrlen;
	struct sockaddr_in *ai_addr;
	char *ai_canonname;
	struct addrinfo *ai_next;
};

/* --- implemented ----------------------------------------------------------- */

const char *gai_strerror(int code);

/* --- declared, and not yet linkable ---------------------------------------- */

int getaddrinfo(const char *host, const char *service,
		const struct addrinfo *hints, struct addrinfo **result);
void freeaddrinfo(struct addrinfo *list);
int getnameinfo(const struct sockaddr_in *address, unsigned int length,
		char *host, unsigned int host_len,
		char *service, unsigned int service_len, int flags);

#endif /* RECON_NETDB_H */
