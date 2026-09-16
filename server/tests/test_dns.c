/*
 * Asking for an address, and the answers that must not be believed.
 *
 * Pure: bytes in, a verdict out, no sockets. So every message a name server --
 * or anyone who answers faster than one -- can send is driven here directly.
 *
 * **The two cases this exists for.**
 *
 * A compression pointer that points at itself. The format lets a name say "the
 * rest is at offset N", and a parser that follows wherever it is sent loops for
 * ever on a message of five bytes. Nothing reports an error, nothing crashes,
 * and the server simply stops answering anybody. That is a denial of service
 * costing one datagram, available to whoever replies first.
 *
 * A response that does not answer the question asked. An attacker who cannot
 * see the query can still guess it and reply before the real server does; every
 * field that identifies a question is therefore checked, and a mismatch throws
 * the whole message away rather than keeping the parts that looked fine.
 *
 * **Watched failing first, against two parsers**, because the two families
 * fail in different ways and one run could not show both.
 *
 * Against a parser that follows a compression pointer wherever it points --
 * which is what the format appears to allow -- the suite **does not fail. It
 * hangs.** `timeout 20` killed it with nothing printed. That is the whole
 * argument for the backwards-only rule in one measurement: the fault does not
 * produce a wrong answer, it produces no answer, for ever, to anybody.
 *
 * Against a parser that skips the identifier and question checks and uses a
 * truncated reply, **4 of 65 checks failed** -- and each failure is a `DNS_OK`
 * where a refusal belonged. It accepted a response carrying a different
 * identifier and reported the address in it, which is off-path cache poisoning
 * working exactly as designed by the attacker.
 */

#include "../dns.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL  %s\n", what);
	}
}

static void verdict(int got, int want, const char *what)
{
	checks++;
	if (got != want) {
		failures++;
		printf("  FAIL  %s: got %d, wanted %d\n", what, got, want);
	}
}

/* --- building a message ------------------------------------------------------
 *
 * Assembled by hand rather than by a helper that also builds the real thing.
 * A helper shared with the code under test would agree with it about where a
 * field goes, which is one of the things being checked. */

static unsigned char MSG[DNS_MESSAGE_MAX];
static size_t MSG_LEN;

static void msg_start(unsigned short id, unsigned flags, unsigned qd,
                      unsigned an)
{
	memset(MSG, 0, sizeof(MSG));
	MSG[0] = (unsigned char)(id >> 8);
	MSG[1] = (unsigned char)id;
	MSG[2] = (unsigned char)(flags >> 8);
	MSG[3] = (unsigned char)flags;
	MSG[4] = (unsigned char)(qd >> 8); MSG[5] = (unsigned char)qd;
	MSG[6] = (unsigned char)(an >> 8); MSG[7] = (unsigned char)an;
	MSG_LEN = 12;
}

static void msg_bytes(const void *p, size_t n)
{
	memcpy(MSG + MSG_LEN, p, n);
	MSG_LEN += n;
}

static void msg_u8(unsigned char v)  { MSG[MSG_LEN++] = v; }
static void msg_u16(unsigned v)
{
	MSG[MSG_LEN++] = (unsigned char)(v >> 8);
	MSG[MSG_LEN++] = (unsigned char)v;
}
static void msg_u32(unsigned long v)
{
	MSG[MSG_LEN++] = (unsigned char)(v >> 24);
	MSG[MSG_LEN++] = (unsigned char)(v >> 16);
	MSG[MSG_LEN++] = (unsigned char)(v >> 8);
	MSG[MSG_LEN++] = (unsigned char)v;
}

/* The question, as this file encodes it independently of `dns.c`. */
static void msg_question(const char *encoded, size_t n)
{
	msg_bytes(encoded, n);
	msg_u16(1);		/* A */
	msg_u16(1);		/* IN */
}

/* `example.com` on the wire. Written out so the suite does not depend on the
 * encoder it is also testing. */
static const char Q_EXAMPLE[] =
	"\x07" "example" "\x03" "com" "\x00";
#define Q_EXAMPLE_LEN 13

int main(void)
{
	struct dns_result r;
	unsigned char q[DNS_MESSAGE_MAX];
	long n;

	printf("asking for an address, and the answers that must not be believed\n");

	/* --- encoding a name -------------------------------------------------- */
	{
		unsigned char out[DNS_NAME_MAX + 2];

		n = dns_encode_name("example.com", out, sizeof(out));
		ok(n == Q_EXAMPLE_LEN, "example.com encodes to thirteen bytes");
		ok(n == Q_EXAMPLE_LEN
		   && memcmp(out, Q_EXAMPLE, Q_EXAMPLE_LEN) == 0,
		   "as a length byte before each label, ending in a zero");

		n = dns_encode_name("example.com.", out, sizeof(out));
		ok(n == Q_EXAMPLE_LEN
		   && memcmp(out, Q_EXAMPLE, Q_EXAMPLE_LEN) == 0,
		   "a single trailing dot is the root, and is the same name");

		n = dns_encode_name("a", out, sizeof(out));
		ok(n == 3, "a single label: a length, a byte, and the root");

		/* Refusals. Each is a text that would otherwise encode to a
		 * name somebody did not type. */
		verdict((int)dns_encode_name("", out, sizeof(out)), DNS_ENAME,
		        "an empty name");
		verdict((int)dns_encode_name(".", out, sizeof(out)), DNS_ENAME,
		        "the root alone");
		verdict((int)dns_encode_name("a..b", out, sizeof(out)),
		        DNS_ENAME, "an empty label between two dots");
		verdict((int)dns_encode_name(".a", out, sizeof(out)), DNS_ENAME,
		        "a leading dot");
		verdict((int)dns_encode_name("a..", out, sizeof(out)),
		        DNS_ENAME, "two trailing dots");
		verdict((int)dns_encode_name("a b.com", out, sizeof(out)),
		        DNS_ENAME, "a space, which splits a name elsewhere");

		{
			/* A NUL inside the text cannot be reached through a C
			 * string, which is exactly why the byte rule refuses
			 * everything below a space rather than the NUL alone. */
			char tab[] = "a\tb.com";

			verdict((int)dns_encode_name(tab, out, sizeof(out)),
			        DNS_ENAME, "a tab");
		}

		{
			char big[80];
			size_t i;

			for (i = 0; i < 64; i++)
				big[i] = 'a';
			big[64] = '\0';
			verdict((int)dns_encode_name(big, out, sizeof(out)),
			        DNS_ENAME, "a label of sixty-four");

			big[63] = '\0';
			ok(dns_encode_name(big, out, sizeof(out)) == 65,
			   "and sixty-three fits");
		}

		{
			/* 254 characters of text: one past what a name holds. */
			char big[300];
			size_t i, at = 0;

			for (i = 0; i < 127; i++) {
				big[at++] = 'a';
				big[at++] = '.';
			}
			big[at - 1] = 'a';
			big[at] = '\0';
			verdict((int)dns_encode_name(big, out, sizeof(out)),
			        DNS_ENAME, "a name past two hundred and fifty-three");
		}

		verdict((int)dns_encode_name("example.com", out, 4), DNS_EROOM,
		        "a buffer too small to hold it");
		verdict((int)dns_encode_name(0, out, sizeof(out)), DNS_EROOM,
		        "no name");
	}

	/* --- building a query -------------------------------------------------- */
	{
		n = dns_build_query("example.com", 0x1234, q, sizeof(q));
		ok(n == 12 + Q_EXAMPLE_LEN + 4, "a query is header, name, type, class");
		ok(n > 0 && q[0] == 0x12 && q[1] == 0x34, "the id is where it belongs");
		ok(n > 0 && q[2] == 0x01, "recursion is asked for");
		ok(n > 0 && q[4] == 0 && q[5] == 1, "exactly one question");
		ok(n > 0 && q[6] == 0 && q[7] == 0, "and no answers in a question");
		ok(n > 0 && memcmp(q + 12, Q_EXAMPLE, Q_EXAMPLE_LEN) == 0,
		   "the name, encoded");
		verdict((int)dns_build_query("a", 1, q, 8), DNS_EROOM,
		        "a buffer too small for even a header");
		verdict((int)dns_build_query("a..b", 1, q, sizeof(q)), DNS_ENAME,
		        "a name the encoder refuses is a query never built");
	}

	/* --- an answer that is real --------------------------------------------- */
	{
		msg_start(0x1234, 0x8180, 1, 1);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		msg_u16(0xC00C);	/* a pointer back to the question */
		msg_u16(1); msg_u16(1);
		msg_u32(300);
		msg_u16(4);
		msg_u8(93); msg_u8(184); msg_u8(216); msg_u8(34);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_OK, "an ordinary answer");
		ok(r.count == 1, "one address");
		ok(r.count == 1 && r.addrs[0] == 0x5DB8D822u,
		   "93.184.216.34, most significant octet first");
		ok(r.ttl == 300, "and its time to live");
		ok(r.rcode == 0, "with no error");
	}

	/* --- two answers, and the smallest TTL ---------------------------------- */
	{
		msg_start(0x1234, 0x8180, 1, 2);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		msg_u16(0xC00C); msg_u16(1); msg_u16(1); msg_u32(300);
		msg_u16(4); msg_u8(1); msg_u8(1); msg_u8(1); msg_u8(1);
		msg_u16(0xC00C); msg_u16(1); msg_u16(1); msg_u32(60);
		msg_u16(4); msg_u8(2); msg_u8(2); msg_u8(2); msg_u8(2);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_OK, "two answers");
		ok(r.count == 2, "both kept");
		ok(r.count == 2 && r.addrs[0] == 0x01010101u
		   && r.addrs[1] == 0x02020202u, "in the order they arrived");
		ok(r.ttl == 60, "and the shortest life among them");
	}

	/* --- the case the file exists for: a pointer that does not go back -------
	 *
	 * Against a parser that follows a pointer wherever it points, this does
	 * not fail. It runs for ever. */
	{
		msg_start(0x1234, 0x8180, 1, 1);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		/* An answer whose owner name points at itself. */
		{
			unsigned at = (unsigned)MSG_LEN;

			msg_u16(0xC000 | at);
		}
		msg_u16(1); msg_u16(1); msg_u32(300);
		msg_u16(4); msg_u8(9); msg_u8(9); msg_u8(9); msg_u8(9);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_ELOOP,
		        "a compression pointer to its own offset");
		ok(r.count == 0, "and nothing is reported from it");
	}

	{
		/* Forward, to a byte after itself. The same loop with a step
		 * in it, and refused by the same rule. */
		msg_start(0x1234, 0x8180, 1, 1);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		{
			unsigned at = (unsigned)MSG_LEN;

			msg_u16(0xC000 | (at + 2));
		}
		msg_u16(1); msg_u16(1); msg_u32(300);
		msg_u16(4); msg_u8(9); msg_u8(9); msg_u8(9); msg_u8(9);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_ELOOP,
		        "a compression pointer that points forward");
	}

	/* --- responses that do not answer this question -------------------------- */
	{
		msg_start(0x9999, 0x8180, 1, 1);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		msg_u16(0xC00C); msg_u16(1); msg_u16(1); msg_u32(300);
		msg_u16(4); msg_u8(6); msg_u8(6); msg_u8(6); msg_u8(6);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_EMISMATCH,
		        "a different identifier -- somebody answering first");
		ok(r.count == 0, "and not one address is reported from it");
	}

	{
		msg_start(0x1234, 0x0100, 1, 1);	/* QR clear: a question */
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		msg_u16(0xC00C); msg_u16(1); msg_u16(1); msg_u32(300);
		msg_u16(4); msg_u8(6); msg_u8(6); msg_u8(6); msg_u8(6);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_EMISMATCH,
		        "a message that is a question, not an answer");
	}

	{
		static const char Q_OTHER[] = "\x07" "notthis" "\x03" "com" "\x00";

		msg_start(0x1234, 0x8180, 1, 1);
		msg_question(Q_OTHER, 13);
		msg_u16(0xC00C); msg_u16(1); msg_u16(1); msg_u32(300);
		msg_u16(4); msg_u8(6); msg_u8(6); msg_u8(6); msg_u8(6);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_EMISMATCH,
		        "the right id echoing a different question");
	}

	{
		/* The question echoed in a different case, which is legal and
		 * which servers really do. */
		static const char Q_LOUD[] = "\x07" "EXAMPLE" "\x03" "COM" "\x00";

		msg_start(0x1234, 0x8180, 1, 1);
		msg_question(Q_LOUD, 13);
		msg_u16(0xC00C); msg_u16(1); msg_u16(1); msg_u32(300);
		msg_u16(4); msg_u8(5); msg_u8(5); msg_u8(5); msg_u8(5);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_OK,
		        "names match without regard to case");
	}

	{
		msg_start(0x1234, 0x8180, 0, 1);	/* no question at all */
		msg_u16(0xC00C); msg_u16(1); msg_u16(1); msg_u32(300);
		msg_u16(4); msg_u8(6); msg_u8(6); msg_u8(6); msg_u8(6);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_EMISMATCH,
		        "no question to match against");
	}

	{
		msg_start(0x1234, 0x8180, 2, 1);	/* two questions */
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_EMISMATCH,
		        "two questions -- this never asked two");
	}

	{
		/* The question asked, but for a different type. */
		msg_start(0x1234, 0x8180, 1, 0);
		msg_bytes(Q_EXAMPLE, Q_EXAMPLE_LEN);
		msg_u16(15);		/* MX */
		msg_u16(1);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_EMISMATCH,
		        "an answer to a question about a different type");
	}

	{
		msg_start(0x1234, 0x8180, 1, 0);
		msg_bytes(Q_EXAMPLE, Q_EXAMPLE_LEN);
		msg_u16(1);
		msg_u16(3);		/* class CH */

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_EMISMATCH,
		        "or a different class");
	}

	/* --- what the server says went wrong -------------------------------------- */
	{
		msg_start(0x1234, 0x8183, 1, 0);	/* RCODE 3: no such name */
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_ESERVER, "an error code");
		ok(r.rcode == 3,
		   "reported as itself -- no such name is not server failure");
		ok(r.count == 0, "with no addresses");
	}

	{
		msg_start(0x1234, 0x8182, 1, 0);	/* RCODE 2 */
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_ESERVER, "a server failure");
		ok(r.rcode == 2, "told apart from the one above, so a cache can");
	}

	{
		msg_start(0x1234, 0x8380, 1, 0);	/* TC set */
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_ETRUNCATED,
		        "a truncated answer is refused, not half-used");
	}

	{
		msg_start(0x1234, 0x8180, 1, 0);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_OK,
		        "no answers is not an error");
		ok(r.count == 0, "and yields no address");
	}

	/* --- messages that are malformed -------------------------------------------- */
	{
		msg_start(0x1234, 0x8180, 1, 1);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		msg_u16(0xC00C); msg_u16(1); msg_u16(1); msg_u32(300);
		msg_u16(5);		/* an A record claiming five bytes */
		msg_u8(1); msg_u8(2); msg_u8(3); msg_u8(4); msg_u8(5);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_EMALFORMED,
		        "an address record that is not four bytes long");
	}

	{
		msg_start(0x1234, 0x8180, 1, 1);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		msg_u16(0xC00C); msg_u16(1); msg_u16(1); msg_u32(300);
		msg_u16(400);		/* rdlength past the end of the message */
		msg_u8(1); msg_u8(2); msg_u8(3); msg_u8(4);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_EMALFORMED,
		        "a record longer than the message holding it");
	}

	{
		msg_start(0x1234, 0x8180, 1, 1);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		/* An answer that stops in the middle of its own header. */
		msg_u16(0xC00C);
		msg_u16(1);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_EMALFORMED,
		        "an answer cut off inside its header");
	}

	{
		msg_start(0x1234, 0x8180, 1, 1);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		msg_u8(0x80);		/* reserved label type */
		msg_u8(0);

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_ELOOP,
		        "a label with a reserved top bit");
	}

	{
		msg_start(0x1234, 0x8180, 1, 1);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		/* A name that runs off the end without a terminator. */
		msg_u8(60);
		msg_u8('a');

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_ELOOP,
		        "a label longer than what is left of the message");
	}

	verdict(dns_parse_response(MSG, 8, "example.com", 0x1234, &r),
	        DNS_EMALFORMED, "a message too short to be a header");
	verdict(dns_parse_response(0, 40, "example.com", 0x1234, &r),
	        DNS_EMALFORMED, "no message");
	verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234, 0),
	        DNS_EMALFORMED, "nowhere to put the answer");
	/* Asked to check a response against a name that cannot be encoded.
	 * DNS_ENAME rather than DNS_EMISMATCH, which is the more precise of the
	 * two: the fault is in the question, not in the answer. */
	verdict(dns_parse_response(MSG, MSG_LEN, "a..b", 0x1234, &r),
	        DNS_ENAME, "a question this could never have asked");

	{
		/* Larger than a datagram this could have received. */
		msg_start(0x1234, 0x8180, 1, 0);
		verdict(dns_parse_response(MSG, DNS_MESSAGE_MAX + 1,
		                           "example.com", 0x1234, &r),
		        DNS_EMALFORMED, "a message larger than 512 bytes");
	}

	/* --- more answers than there is room for ------------------------------------ */
	{
		int i;

		msg_start(0x1234, 0x8180, 1, DNS_ADDRS_MAX + 4);
		msg_question(Q_EXAMPLE, Q_EXAMPLE_LEN);
		for (i = 0; i < DNS_ADDRS_MAX + 4; i++) {
			msg_u16(0xC00C); msg_u16(1); msg_u16(1); msg_u32(300);
			msg_u16(4);
			msg_u8(10); msg_u8(0); msg_u8(0); msg_u8((unsigned char)i);
		}

		verdict(dns_parse_response(MSG, MSG_LEN, "example.com", 0x1234,
		                          &r), DNS_OK,
		        "more addresses than fit is not an error");
		ok(r.count == DNS_ADDRS_MAX, "as many as there was room for");
	}

	/* --- a result is never left holding the last one's answer ---------------
	 *
	 * Found on the machine, not here. A name the encoder refuses returns
	 * before any query is sent, and the endpoint reporting that failure
	 * printed `"rcode":3` -- a plausible "no such name" that no server had
	 * said, read from whatever the previous request left on the stack.
	 *
	 * **The suite as written could not have caught it.** Every check above
	 * passes a fresh `r` and reads only fields the call fills in. This one
	 * dirties the structure first, which is what a real caller does by
	 * reusing it.
	 */
	{
		struct dns_client c;
		struct dns_result dirty;

		memset(&c, 0, sizeof(c));
		dirty.count = 7;
		dirty.rcode = 3;
		dirty.ttl = 999;

		verdict(dns_resolve(&c, "a..b", 0x1234, &dirty), DNS_ENAME,
		        "a name that cannot be encoded is refused before asking");
		ok(dirty.rcode == 0,
		   "and the result holds no rcode a server never gave");
		ok(dirty.count == 0, "nor an address");
		ok(dirty.ttl == 0, "nor a time to live");

		dirty.rcode = 3;
		verdict(dns_resolve(0, "a.com", 1, &dirty), DNS_EMALFORMED,
		        "no client");
		verdict(dns_resolve(&c, 0, 1, &dirty), DNS_EMALFORMED,
		        "no name");
		verdict(dns_resolve(&c, "a.com", 1, 0), DNS_EMALFORMED,
		        "nowhere to put the answer");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
