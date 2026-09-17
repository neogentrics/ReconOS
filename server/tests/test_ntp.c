/*
 * Asking the time, and the replies that must not set a clock.
 *
 * Pure: bytes in, a verdict and two numbers out, no sockets.
 *
 * **The case this exists for** is a reply that did not answer this query. What
 * an NTP reply sets is *the clock*, and a clock is what every expiry in a
 * system is read against -- a certificate's, a token's, a cache entry's. An
 * attacker who can move it backwards can un-expire things that were meant to
 * be dead.
 *
 * The only thing separating an answer from an assertion is that the server
 * echoes, exactly, the 64-bit transmit timestamp this machine sent. So the
 * checks below spend most of their effort on that one field and on the
 * statements a server makes about its own trustworthiness -- the stratum, the
 * leap indicator, and the kiss-of-death that asks a client to stop.
 *
 * **Watched failing first**, against a parser that takes the server's word --
 * no echoed-timestamp check, no mode check, and a stratum read as a number
 * rather than as a statement. **9 of 41 checks failed, and every one of them
 * returned `NTP_OK` where a refusal belonged.**
 *
 * That is the shape worth noticing. None of the nine is a crash or a wrong
 * number: each is a forged or useless reply accepted as the truth, handed back
 * with a confident offset in milliseconds. A clock set from any of them is
 * wrong by exactly as much as whoever sent it chose.
 */

#include "../ntp.h"

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

/* --- building a reply by hand -----------------------------------------------
 *
 * Assembled here rather than by a helper shared with the code under test,
 * which would agree with it about where a field goes. */

static unsigned char R[NTP_PACKET_BYTES];

static void put32(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void put64(unsigned char *p, unsigned long long v)
{
	put32(p, (unsigned long)(v >> 32));
	put32(p + 4, (unsigned long)(v & 0xFFFFFFFFULL));
}

/* One second, as the protocol's 32.32 fixed point. */
#define SEC (1ULL << 32)

static void reply(unsigned leap, unsigned version, unsigned mode,
                  unsigned stratum, unsigned long long orig,
                  unsigned long long rx, unsigned long long tx)
{
	memset(R, 0, sizeof(R));
	R[0] = (unsigned char)((leap << 6) | (version << 3) | mode);
	R[1] = (unsigned char)stratum;
	put32(R + 12, 0x47505300);	/* "GPS\0", a plausible reference id */
	put64(R + 24, orig);
	put64(R + 32, rx);
	put64(R + 40, tx);
}

int main(void)
{
	struct ntp_sample s;
	unsigned char q[NTP_PACKET_BYTES];
	/* A transmit timestamp with bits set all the way down, so a parser
	 * that compares only the seconds passes here and fails below. */
	const unsigned long long SENT = (3900000000ULL << 32) | 0x12345678ULL;

	printf("asking the time, and the replies that must not set a clock\n");

	/* --- the query -------------------------------------------------------- */
	{
		long n = ntp_build_query(SENT, q, sizeof(q));

		ok(n == NTP_PACKET_BYTES, "a query is forty-eight bytes");
		ok(n > 0 && q[0] == 0x23,
		   "leap 0, version 4, mode 3 -- a client asking");
		ok(n > 0 && q[1] == 0,
		   "stratum unspecified: a client is not claiming to serve time");
		{
			/* Read back rather than compared against a hex literal.
			 * The first version of this check carried eight bytes
			 * worked out by hand and one of them was wrong, so it
			 * failed against a correct encoder -- a check that is
			 * harder to get right than the code it checks is a
			 * check that reports its own mistakes as faults. */
			unsigned char want[8];
			int i;

			for (i = 0; i < 8; i++)
				want[i] = (unsigned char)(SENT >> (56 - 8 * i));

			ok(n > 0 && memcmp(q + 40, want, 8) == 0,
			   "and the transmit timestamp is where the reply must"
			   " echo it, most significant byte first");
		}
		verdict((int)ntp_build_query(SENT, q, 47), NTP_EROOM,
		        "a buffer one byte short");
		verdict((int)ntp_build_query(SENT, 0, 48), NTP_EROOM,
		        "no buffer");
	}

	/* --- an answer that is real -------------------------------------------
	 *
	 * Sent at T1, the server saw it at T1+2s, replied at T1+2s, and it came
	 * back at T1+4s. A symmetric two-second trip each way, and a clock that
	 * agrees: offset zero, delay four seconds. */
	{
		reply(0, 4, 4, 2, SENT, SENT + 2 * SEC, SENT + 2 * SEC);

		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_OK, "an ordinary reply");
		ok(s.offset_ms == 0,
		   "a symmetric trip to a clock that agrees is no offset");
		ok(s.delay_ms == 4000, "and a four-second round trip");
		ok(s.stratum == 2, "the stratum is reported");
		ok(s.ref_id == 0x47505300UL, "and what the server syncs from");
	}

	/* --- a clock that is wrong ---------------------------------------------
	 *
	 * The same trip, but the server's clock reads ten seconds later than
	 * this machine's. This machine is behind, so the offset is positive. */
	{
		reply(0, 4, 4, 2, SENT, SENT + 12 * SEC, SENT + 12 * SEC);

		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_OK, "a clock that disagrees");
		ok(s.offset_ms == 10000,
		   "ten seconds behind, reported as positive ten thousand");
		ok(s.delay_ms == 4000, "with the round trip unchanged");
	}

	{
		/* And the other way: this machine ahead of the server. */
		reply(0, 4, 4, 2, SENT, SENT - 8 * SEC, SENT - 8 * SEC);

		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_OK, "a clock that is ahead");
		ok(s.offset_ms == -10000, "reported as negative");
	}

	{
		/* Sub-second precision has to survive the conversion. A
		 * fraction shifted before it is scaled is a fraction thrown
		 * away, which produces offsets that are always whole seconds
		 * and look entirely plausible. */
		unsigned long long half = SEC / 2;

		reply(0, 4, 4, 2, SENT, SENT + half, SENT + half);
		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + SEC, &s),
		        NTP_OK, "a reply inside one second");
		ok(s.delay_ms == 1000, "the round trip in milliseconds");
		ok(s.offset_ms == 0, "and no offset from a symmetric trip");
	}

	/* --- the case the file exists for: a reply to a different query --------
	 *
	 * Everything about this message is well formed. The only thing wrong is
	 * that this machine never sent what it claims to answer. */
	{
		reply(0, 4, 4, 1, SENT + 1, SENT + 2 * SEC, SENT + 2 * SEC);

		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EMISMATCH,
		        "an originate timestamp off by one bit");
		ok(s.offset_ms == 0 && s.delay_ms == 0,
		   "and not one number is reported from it");
	}

	{
		/* The seconds match and the fraction does not -- which is what
		 * an attacker who knows roughly when the query went out can
		 * produce. A parser comparing only the top half accepts it. */
		reply(0, 4, 4, 1, (SENT & 0xFFFFFFFF00000000ULL),
		      SENT + 2 * SEC, SENT + 2 * SEC);

		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EMISMATCH,
		        "the right second with the wrong fraction");
	}

	{
		reply(0, 4, 4, 1, 0, SENT + 2 * SEC, SENT + 2 * SEC);

		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EMISMATCH,
		        "an originate of zero, which is what a forger sends"
		        " when it did not see the query");
	}

	/* --- what the server says about itself --------------------------------- */
	{
		reply(0, 4, 4, 0, SENT, SENT + 2 * SEC, SENT + 2 * SEC);
		put32(R + 12, 0x52415445);	/* "RATE" */

		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EKISS,
		        "stratum zero is a kiss-of-death, not a time");
		ok(s.ref_id == 0x52415445UL,
		   "and the four letters saying why are reported");
	}

	{
		reply(0, 4, 4, 16, SENT, SENT + 2 * SEC, SENT + 2 * SEC);
		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EUNSYNCED,
		        "stratum sixteen: the server is not synchronised");
	}

	{
		reply(3, 4, 4, 2, SENT, SENT + 2 * SEC, SENT + 2 * SEC);
		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EUNSYNCED,
		        "leap indicator three says the same in another field");
	}

	{
		reply(0, 4, 4, 2, SENT, SENT + 2 * SEC, 0);
		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EMALFORMED,
		        "a transmit timestamp of zero would set a clock to 1900");
	}

	/* --- messages that are not replies -------------------------------------- */
	{
		reply(0, 4, 3, 2, SENT, SENT + 2 * SEC, SENT + 2 * SEC);
		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EMISMATCH,
		        "mode three is another client's question");

		reply(0, 4, 5, 2, SENT, SENT + 2 * SEC, SENT + 2 * SEC);
		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EMISMATCH,
		        "mode five is a broadcast nobody asked for");

		reply(0, 2, 4, 2, SENT, SENT + 2 * SEC, SENT + 2 * SEC);
		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EMALFORMED,
		        "version two is older than this reads");

		reply(0, 7, 4, 2, SENT, SENT + 2 * SEC, SENT + 2 * SEC);
		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 4 * SEC,
		                           &s), NTP_EMALFORMED,
		        "and version seven is newer");
	}

	{
		reply(0, 4, 4, 2, SENT, SENT + 2 * SEC, SENT + 2 * SEC);
		verdict(ntp_parse_response(R, 47, SENT, SENT + 4 * SEC, &s),
		        NTP_EMALFORMED, "forty-seven bytes is not a message");
		verdict(ntp_parse_response(R, 56, SENT, SENT + 4 * SEC, &s),
		        NTP_EMALFORMED,
		        "and fifty-six is one this cannot fully read");
		verdict(ntp_parse_response(0, 48, SENT, SENT + 4 * SEC, &s),
		        NTP_EMALFORMED, "no message");
		verdict(ntp_parse_response(R, 48, SENT, SENT + 4 * SEC, 0),
		        NTP_EMALFORMED, "nowhere to put the answer");
	}

	/* --- a round trip that cannot have happened ----------------------------
	 *
	 * The server claims to have held the query for six seconds inside a
	 * round trip of two, which means one of the two clocks moved during the
	 * exchange. The delay computes negative, and a negative round trip is
	 * not a small number -- it is a measurement that means nothing.
	 *
	 * The first version of this case set the server's receive and transmit
	 * to the same instant, which makes the held time zero and the delay
	 * positive. It was checking the clamp against a case that never
	 * reaches it. */
	{
		reply(0, 4, 4, 2, SENT, SENT + 1 * SEC, SENT + 7 * SEC);
		verdict(ntp_parse_response(R, sizeof(R), SENT, SENT + 2 * SEC,
		                           &s), NTP_OK,
		        "a trip that appears to take negative time still parses");
		ok(s.delay_ms == 0,
		   "and its delay is reported as zero, not as a negative"
		   " somebody would read as precision");
	}

	/* --- nothing at all ------------------------------------------------------- */
	{
		struct ntp_client c;

		memset(&c, 0, sizeof(c));
		verdict(ntp_query(0, &s), NTP_EMALFORMED, "no client");
		verdict(ntp_query(&c, 0), NTP_EMALFORMED, "nowhere to put it");
		verdict(ntp_query(&c, &s), NTP_EMALFORMED,
		        "a client with no clock cannot ask -- the query carries"
		        " the timestamp the reply must echo");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
