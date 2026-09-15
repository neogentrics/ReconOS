/*
 * <arpa/inet.h> -- addresses as text.
 *
 * `inet_pton` is here and `inet_aton` is not, deliberately.
 *
 * `inet_aton` accepts `010.0.0.1` as octal, `1.2.3` as a three-part form, and
 * a bare `16777217`. Those readings are how a check that two strings name the
 * same address is defeated, and the function exists only because it predates
 * anybody thinking about that. `inet_pton` takes exactly four decimal parts of
 * 0 to 255 with no leading zeroes and nothing else in the string.
 *
 * That strictness is the feature, so the loose one is not offered next to it.
 */

#ifndef RECON_ARPA_INET_H
#define RECON_ARPA_INET_H

/* Quoted, so it finds the one beside it rather than the host's. */
#include "../netinet/in.h"

/* 1 for an address, 0 for text that is not one, -1 with errno set for a family
 * this does not know. Three answers, because a caller acts on them
 * differently. */
int inet_pton(int family, const char *text, void *into);

/* **A static buffer**, which is what this function has always been and why
 * `inet_ntop` exists. Two calls in one argument list give the same pointer
 * twice and print the second address for both. Kept because the desktop calls
 * it; changing the interface is not this library's decision. */
char *inet_ntoa(struct in_addr address);

#endif /* RECON_ARPA_INET_H */
