/*
 * Addresses, byte order, and the three stdio readers.
 *
 * Nine functions that need nothing from the kernel, held against the host's.
 *
 * --- The one that cannot be compared, and why it is here anyway ---
 *
 * `htons` is checked against the host's, and that comparison is **weaker than
 * it looks**: both machines this runs on are little-endian, so a version
 * written as a byte swap of the value's storage agrees with glibc on every
 * machine in the rig and is wrong on the first big-endian one.
 *
 * So the byte order is also checked *directly* -- the bytes of the result are
 * required to be the value's own bytes, most significant first, which is what
 * "network order" means and is true on a machine of either endianness. That
 * assertion would fail on a byte-swap implementation even here.
 *
 * --- inet_pton is a refusal, mostly ---
 *
 * Its value is what it turns down. `inet_aton` reads `010.0.0.1` as octal and
 * `1.2.3` as a three-part address, and those readings are how a check that two
 * strings name the same host is defeated. Most of the corpus below is text
 * that must **not** parse, and every case is put to the host as well, because
 * "both refuse" is a stronger statement than "we refuse".
 *
 * Run with: ./build/recon_libc_inet_tests
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ReconOS's, renamed by prefix.h when libc/ was compiled. */
unsigned short recon_htons(unsigned short value);
unsigned int recon_htonl(unsigned int value);
unsigned short recon_ntohs(unsigned short value);
unsigned int recon_ntohl(unsigned int value);
int recon_inet_pton(int family, const char *text, void *into);
const char *recon_gai_strerror(int code);

struct recon_in_addr {
	unsigned int s_addr;
};
char *recon_inet_ntoa(struct recon_in_addr address);

static unsigned long checks;
static unsigned long failures;

static void ok(int condition, const char *what)
{
	checks++;

	if (!condition) {
		failures++;
		if (failures <= 25)
			printf("  FAIL  %s\n", what);
	}
}

/* --- byte order ------------------------------------------------------------ */

static void byte_order(void)
{
	static const unsigned short shorts[] = {
		0, 1, 0x00FF, 0xFF00, 0x1234, 0x0102, 0xABCD, 0xFFFF, 80, 443
	};
	static const unsigned int longs[] = {
		0u, 1u, 0x000000FFu, 0xFF000000u, 0x01020304u,
		0x7F000001u, 0xC0A80101u, 0xFFFFFFFFu
	};
	size_t i;

	for (i = 0; i < sizeof(shorts) / sizeof(shorts[0]); i++) {
		unsigned short mine = recon_htons(shorts[i]);
		unsigned char bytes[2];
		char what[120];

		snprintf(what, sizeof(what), "htons(0x%04X) is 0x%04X, the "
			 "host says 0x%04X", shorts[i], mine,
			 htons(shorts[i]));
		ok(mine == htons(shorts[i]), what);

		/*
		 * And the part the comparison cannot make: the bytes in
		 * memory, most significant first. A byte-swap implementation
		 * agrees with glibc on this machine and fails here.
		 */
		memcpy(bytes, &mine, sizeof(bytes));
		snprintf(what, sizeof(what),
			 "htons(0x%04X) stores %02X %02X, and network order is "
			 "%02X %02X", shorts[i], bytes[0], bytes[1],
			 (shorts[i] >> 8) & 0xFF, shorts[i] & 0xFF);
		ok(bytes[0] == ((shorts[i] >> 8) & 0xFF) &&
		   bytes[1] == (shorts[i] & 0xFF), what);

		/* And back again is where it started. */
		snprintf(what, sizeof(what), "ntohs(htons(0x%04X)) is itself",
			 shorts[i]);
		ok(recon_ntohs(mine) == shorts[i], what);
	}

	for (i = 0; i < sizeof(longs) / sizeof(longs[0]); i++) {
		unsigned int mine = recon_htonl(longs[i]);
		unsigned char bytes[4];
		char what[140];

		snprintf(what, sizeof(what), "htonl(0x%08X) is 0x%08X, the "
			 "host says 0x%08X", longs[i], mine, htonl(longs[i]));
		ok(mine == htonl(longs[i]), what);

		memcpy(bytes, &mine, sizeof(bytes));
		snprintf(what, sizeof(what),
			 "htonl(0x%08X) stores %02X %02X %02X %02X",
			 longs[i], bytes[0], bytes[1], bytes[2], bytes[3]);
		ok(bytes[0] == ((longs[i] >> 24) & 0xFF) &&
		   bytes[1] == ((longs[i] >> 16) & 0xFF) &&
		   bytes[2] == ((longs[i] >> 8) & 0xFF) &&
		   bytes[3] == (longs[i] & 0xFF), what);

		snprintf(what, sizeof(what), "ntohl(htonl(0x%08X)) is itself",
			 longs[i]);
		ok(recon_ntohl(mine) == longs[i], what);
	}
}

/* --- text to an address ---------------------------------------------------- */

static void addresses_from_text(void)
{
	static const char *good[] = {
		"0.0.0.0", "1.2.3.4", "127.0.0.1", "255.255.255.255",
		"192.168.1.1", "8.8.8.8", "10.0.0.255", "0.0.0.1",
	};

	/*
	 * Text that must not parse. Most of this list is the point of using
	 * `inet_pton` rather than `inet_aton`: every entry is a spelling some
	 * older function accepts, and accepting it is how two strings that
	 * look different come to mean the same host.
	 */
	static const char *bad[] = {
		"", ".", "1", "1.2", "1.2.3", "1.2.3.4.5",
		"256.0.0.1", "0.0.0.256", "999.1.1.1",
		"01.2.3.4",		/* a leading zero, read as octal by inet_aton */
		"1.2.3.04",
		"0x7f.0.0.1",		/* hexadecimal, likewise */
		"1.2.3.-4", "-1.2.3.4",
		"1.2.3.4 ", " 1.2.3.4",	/* space either end */
		"1.2.3.4\n",
		"1..3.4", "1.2..4", "...",
		"1.2.3.a", "a.b.c.d",
		"1.2.3.4.", "1.2.3.4:80",
	};
	size_t i;

	for (i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
		unsigned char mine[4], theirs[4];
		int a, b;
		char what[160];

		memset(mine, 0xA5, sizeof(mine));
		memset(theirs, 0xA5, sizeof(theirs));

		a = recon_inet_pton(AF_INET, good[i], mine);
		b = inet_pton(AF_INET, good[i], theirs);

		snprintf(what, sizeof(what),
			 "%s parses: ours %d %u.%u.%u.%u, host %d %u.%u.%u.%u",
			 good[i], a, mine[0], mine[1], mine[2], mine[3],
			 b, theirs[0], theirs[1], theirs[2], theirs[3]);

		ok(a == 1 && b == 1 &&
		   memcmp(mine, theirs, sizeof(mine)) == 0, what);
	}

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		unsigned char mine[4];
		int a, b;
		char what[160];

		memset(mine, 0xA5, sizeof(mine));

		a = recon_inet_pton(AF_INET, bad[i], mine);
		b = inet_pton(AF_INET, bad[i], mine);

		snprintf(what, sizeof(what),
			 "\"%s\" must not parse: ours %d, the host %d",
			 bad[i], a, b);

		ok(a == 0 && b == 0, what);
	}

	/* A family neither knows about is a third answer, not a refusal. */
	{
		unsigned char into[16];

		ok(recon_inet_pton(1234, "1.2.3.4", into) == -1,
		   "an unknown family answers -1, not 0");
	}

	ok(recon_inet_pton(AF_INET, NULL, NULL) == 0,
	   "and NULL is refused rather than followed");
}

/* --- an address to text ---------------------------------------------------- */

static void text_from_addresses(void)
{
	static const char *cases[] = {
		"0.0.0.0", "1.2.3.4", "127.0.0.1", "255.255.255.255",
		"192.168.1.1", "10.0.0.255",
	};
	size_t i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct recon_in_addr mine;
		struct in_addr theirs;
		char kept[32];
		char what[160];

		inet_pton(AF_INET, cases[i], &theirs.s_addr);
		mine.s_addr = theirs.s_addr;

		snprintf(kept, sizeof(kept), "%s", recon_inet_ntoa(mine));

		snprintf(what, sizeof(what), "%s comes back as %s, the host "
			 "says %s", cases[i], kept, inet_ntoa(theirs));

		ok(strcmp(kept, cases[i]) == 0 &&
		   strcmp(kept, inet_ntoa(theirs)) == 0, what);
	}

	/* Round trip, which catches an endianness mistake the other way. */
	{
		unsigned char raw[4] = { 203, 0, 113, 7 };
		struct recon_in_addr a;

		memcpy(&a.s_addr, raw, sizeof(raw));
		ok(strcmp(recon_inet_ntoa(a), "203.0.113.7") == 0,
		   "the bytes come out in the order they went in");
	}
}

/* --- what a lookup failed with --------------------------------------------- */

static void lookup_messages(void)
{
	static const int codes[] = {
		EAI_BADFLAGS, EAI_NONAME, EAI_AGAIN, EAI_FAIL, EAI_FAMILY,
		EAI_SOCKTYPE, EAI_SERVICE, EAI_MEMORY, EAI_SYSTEM,
		EAI_OVERFLOW, 0,
	};
	size_t i;

	for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		const char *mine = recon_gai_strerror(codes[i]);
		const char *theirs = gai_strerror(codes[i]);
		char what[200];

		snprintf(what, sizeof(what), "gai_strerror(%d): ours \"%s\", "
			 "the host's \"%s\"", codes[i], mine, theirs);

		ok(mine && theirs && strcmp(mine, theirs) == 0, what);
	}

	ok(recon_gai_strerror(9999)[0] != '\0',
	   "a code neither knows still answers something");
}

int main(void)
{
	printf("Addresses and byte order\n\n");

	byte_order();
	addresses_from_text();
	text_from_addresses();
	lookup_messages();

	printf("\n%lu checks, %lu failures\n", checks, failures);
	return failures ? 1 : 0;
}
