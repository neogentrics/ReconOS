/*
 * Addresses and byte order.
 *
 * Five functions from the socket group that need **nothing from the kernel**:
 * `htons`, `htonl`, `inet_pton`, `inet_ntoa` and `gai_strerror`. They are
 * arithmetic on numbers and text, and the fourteen that do need a kernel --
 * `socket`, `connect`, `bind` and the rest -- are declared in the headers and
 * deliberately not defined, so a caller fails to link naming the symbol.
 *
 * Splitting them out is worth doing rather than waiting: a program that parses
 * an address does not need a network, and `src/recon_net.c` reads
 * `/proc/net/route` with `%lx` and turns the result into dotted quads long
 * before it opens anything.
 *
 * --- Byte order is not a swap ---
 *
 * `htons` is written as arithmetic on the *value*, not as a byte swap of its
 * storage. The two are the same thing only on a little-endian machine, and
 * writing the second means a function that is correct here, correct on
 * aarch64 because aarch64 is also little-endian, and wrong on the first
 * big-endian machine anybody tries -- with no test able to say so, because
 * every machine in the rig agrees.
 *
 * Shifting and masking is endian-independent by construction: it says "the
 * high byte goes first" in a way the compiler implements correctly on
 * whatever it is targeting.
 */

#include "internal.h"

#include "../include/errno.h"

/* --- byte order ------------------------------------------------------------ */

/*
 * Network order is big-endian: the most significant byte first. These build
 * that arrangement out of the value rather than reordering its bytes, so they
 * are right on a machine of either endianness -- see the note above, which is
 * the whole reason they are not two lines of `__builtin_bswap`.
 */
unsigned short htons(unsigned short value)
{
	unsigned char out[2];

	out[0] = (unsigned char)((value >> 8) & 0xFF);
	out[1] = (unsigned char)(value & 0xFF);

	{
		unsigned short result;

		memcpy(&result, out, sizeof(result));
		return result;
	}
}

unsigned int htonl(unsigned int value)
{
	unsigned char out[4];

	out[0] = (unsigned char)((value >> 24) & 0xFF);
	out[1] = (unsigned char)((value >> 16) & 0xFF);
	out[2] = (unsigned char)((value >> 8) & 0xFF);
	out[3] = (unsigned char)(value & 0xFF);

	{
		unsigned int result;

		memcpy(&result, out, sizeof(result));
		return result;
	}
}

/* The same operation in both directions, and named twice because a caller
 * reading `ntohs` at the point it converts an incoming number is a caller
 * whose intent is legible. */
unsigned short ntohs(unsigned short value)
{
	return htons(value);
}

unsigned int ntohl(unsigned int value)
{
	return htonl(value);
}

/* --- text to an address ---------------------------------------------------- */

/*
 * `inet_pton` for AF_INET, and it is strict on purpose.
 *
 * The strictness is the feature. `inet_aton` -- which this is not -- accepts
 * `010.0.0.1` as octal, `1.2.3` as a three-part form, and `16777217` as a bare
 * number, and those readings are how a check that a string is "the same
 * address" can be defeated. `inet_pton` accepts exactly four decimal parts,
 * each 0 to 255, with no leading zeroes and nothing else in the string, and
 * answers 0 for anything else.
 *
 * Returns 1 for a good address, 0 for text that is not one, and -1 with
 * `errno` set for a family this does not know -- which is three answers a
 * caller acts on differently, and is why it does not simply return a bool.
 */
int inet_pton(int family, const char *text, void *into)
{
	unsigned char out[4];
	unsigned part = 0;
	unsigned digits = 0;
	unsigned filled = 0;
	const char *p;

	if (family != RECON_AF_INET) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	if (!text || !into)
		return 0;

	for (p = text; ; p++) {
		if (*p >= '0' && *p <= '9') {
			/* A leading zero on a multi-digit part is refused
			 * rather than read as octal, which is the difference
			 * between this and `inet_aton` and the reason two
			 * spellings of one address cannot both parse. */
			if (digits == 1 && out[filled] == 0)
				return 0;

			part = part * 10 + (unsigned)(*p - '0');
			if (part > 255)
				return 0;

			out[filled] = (unsigned char)part;
			digits++;

			if (digits > 3)
				return 0;

			continue;
		}

		if (*p == '.' || *p == '\0') {
			if (!digits)
				return 0;

			filled++;
			digits = 0;
			part = 0;

			if (*p == '\0')
				break;

			if (filled == 4)
				return 0;	/* a fifth part */

			continue;
		}

		return 0;			/* anything else at all */
	}

	if (filled != 4)
		return 0;

	memcpy(into, out, sizeof(out));
	return 1;
}

/* --- an address to text ---------------------------------------------------- */

/*
 * A static buffer, which is what `inet_ntoa` has always been and is why
 * `inet_ntop` exists. Said here because it is a real hazard: two calls in one
 * `printf` argument list give the same pointer twice, and the second address
 * is printed for both.
 *
 * Kept anyway, because the desktop calls it and changing the interface is not
 * this library's decision to make.
 */
static char address_text[16];		/* 255.255.255.255 and a terminator */

char *inet_ntoa(struct recon_in_addr address)
{
	unsigned char octet[4];

	memcpy(octet, &address.s_addr, sizeof(octet));

	snprintf(address_text, sizeof(address_text), "%u.%u.%u.%u",
		 (unsigned)octet[0], (unsigned)octet[1],
		 (unsigned)octet[2], (unsigned)octet[3]);

	return address_text;
}

/* --- what a name lookup failed with ---------------------------------------- */

/*
 * The messages are glibc's word for word, for the reason `errno.c` gives about
 * its own: a message this library invents differs from the one the same
 * program prints today on Linux for the same fault, and somebody will compare
 * two logs. It also makes the whole thing testable by equality.
 *
 * `getaddrinfo` itself is not implemented -- it needs a resolver, which needs
 * sockets -- so these describe failures nothing here can yet produce. That is
 * deliberate: the one call site in the desktop is a *message* for an error it
 * is handed, and the message is this side's work whether or not the lookup is.
 */
const char *gai_strerror(int code)
{
	switch (code) {
	case RECON_EAI_BADFLAGS:	return "Bad value for ai_flags";
	case RECON_EAI_NONAME:		return "Name or service not known";
	case RECON_EAI_AGAIN:		return "Temporary failure in name resolution";
	case RECON_EAI_FAIL:		return "Non-recoverable failure in name resolution";
	case RECON_EAI_FAMILY:		return "ai_family not supported";
	case RECON_EAI_SOCKTYPE:	return "ai_socktype not supported";
	case RECON_EAI_SERVICE:		return "Servname not supported for ai_socktype";
	case RECON_EAI_MEMORY:		return "Memory allocation failure";
	case RECON_EAI_SYSTEM:		return "System error";
	case RECON_EAI_OVERFLOW:	return "Result too large for supplied buffer";
	case 0:				return "Success";
	default:			return "Unknown error";
	}
}
