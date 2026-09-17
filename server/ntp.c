/*
 * Building a query, and reading a reply that might not be an answer.
 *
 * See `ntp.h` for what this can and cannot do, and for why the echoed
 * timestamp is the only thing separating an answer from an assertion.
 *
 * --- What was tried and rejected ---
 *
 * **Trusting the server's transmit timestamp on its own.** That is the field
 * everybody wants -- it is *the time* -- and using it alone throws away the
 * two facts that make it meaningful: how long the round trip took, and whether
 * the reply was to this query at all. A clock set from an unchecked datagram
 * is a clock anybody on the path can choose.
 *
 * **Working in the protocol's 64-bit fixed point all the way through.** It is
 * the exact representation and it makes every subtraction a place where a
 * sign error hides. The four timestamps are converted to signed milliseconds
 * once, at the edge, and the arithmetic after that is ordinary. The precision
 * given up is nanoseconds against a round trip measured in milliseconds.
 *
 * **Reporting an offset without the delay beside it.** The true offset is
 * within half the round trip of the measured one, so a number reported alone
 * is a number without its error bar. Both are returned and the console prints
 * both.
 *
 * **Accepting a reply longer than 48 bytes.** Extensions and authentication
 * make a longer packet, and neither is understood here. Reading the first 48
 * and ignoring the rest would mean accepting a message this cannot fully
 * parse, which is how a parser ends up agreeing with a sender about something
 * it never read.
 */

#include "ntp.h"

/* --- the wire ------------------------------------------------------------- */

static unsigned long rd32(const unsigned char *p)
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
	     | ((unsigned long)p[2] << 8) | p[3];
}

static unsigned long long rd64(const unsigned char *p)
{
	return ((unsigned long long)rd32(p) << 32) | rd32(p + 4);
}

static void wr32(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

static void wr64(unsigned char *p, unsigned long long v)
{
	wr32(p, (unsigned long)(v >> 32));
	wr32(p + 4, (unsigned long)(v & 0xFFFFFFFFULL));
}

/*
 * A 64-bit NTP timestamp as signed milliseconds.
 *
 * The whole seconds are the top half and the fraction the bottom. Multiplying
 * the fraction before shifting keeps the milliseconds; shifting first would
 * throw the fraction away entirely, which is a class of bug that produces
 * offsets that are always a whole number of seconds and look plausible.
 */
static long long to_ms(unsigned long long ts)
{
	unsigned long long secs = ts >> 32;
	unsigned long long frac = ts & 0xFFFFFFFFULL;

	return (long long)(secs * 1000ULL) + (long long)((frac * 1000ULL) >> 32);
}

long ntp_build_query(unsigned long long sent, unsigned char *out, size_t room)
{
	size_t i;

	if (!out || room < NTP_PACKET_BYTES)
		return NTP_EROOM;

	for (i = 0; i < NTP_PACKET_BYTES; i++)
		out[i] = 0;

	/*
	 * Leap 0, version 4, mode 3 (client): 00 100 011.
	 *
	 * Version 4 rather than 3 because a version-4 server answers a
	 * version-3 query and the reverse is not guaranteed, and this only
	 * reads fields the two share.
	 */
	out[0] = 0x23;
	out[1] = 0;		/* stratum: unspecified, which is right for a
				 * client -- it is not claiming to serve time */
	out[2] = 6;		/* poll interval, as a power of two seconds */
	out[3] = (unsigned char)0xEC;	/* precision: about a microsecond */

	/* The transmit timestamp. This is the field the reply must echo. */
	wr64(out + 40, sent);

	return NTP_PACKET_BYTES;
}

int ntp_parse_response(const unsigned char *msg, size_t len,
                       unsigned long long sent, unsigned long long arrived,
                       struct ntp_sample *into)
{
	unsigned leap, version, mode;
	unsigned long long originate, server_rx, server_tx;
	long long t1, t2, t3, t4;

	if (!msg || !into)
		return NTP_EMALFORMED;

	into->offset_ms = 0;
	into->delay_ms = 0;
	into->stratum = 0;
	into->ref_id = 0;

	/* Exactly 48. See the header of this file for why longer is refused. */
	if (len != NTP_PACKET_BYTES)
		return NTP_EMALFORMED;

	leap    = (unsigned)(msg[0] >> 6) & 0x3;
	version = (unsigned)(msg[0] >> 3) & 0x7;
	mode    = (unsigned)msg[0] & 0x7;

	/* Mode 4 is a server answering. Mode 3 is another client's query, and
	 * mode 5 is a broadcast nobody asked for -- neither is a reply to this. */
	if (mode != 4)
		return NTP_EMISMATCH;

	if (version < 3 || version > 4)
		return NTP_EMALFORMED;

	into->stratum = (int)msg[1];
	into->ref_id = rd32(msg + 12);

	/*
	 * Stratum 0 is a kiss-of-death: the reference identifier carries four
	 * letters saying why, and `RATE` means this client is asking too often.
	 * It is not an error in the message -- it is the server asking to be
	 * left alone, and a client that retries through it is the reason the
	 * packet exists.
	 */
	if (into->stratum == 0)
		return NTP_EKISS;

	/* 16 and above is a server that is not synchronised to anything, and
	 * leap 3 is the same statement in another field. Either way its time
	 * is not worth taking. */
	if (into->stratum >= 16 || leap == 3)
		return NTP_EUNSYNCED;

	originate = rd64(msg + 24);
	server_rx = rd64(msg + 32);
	server_tx = rd64(msg + 40);

	/*
	 * **The check this protocol turns on.**
	 *
	 * The originate field must be, exactly, the transmit timestamp this
	 * machine put in the query. It is 64 bits an off-path attacker has to
	 * guess to be believed, and it is the only thing distinguishing a
	 * reply from an assertion by whoever answered first.
	 *
	 * Discarded whole on a mismatch, with nothing reported -- a caller
	 * shown part of a reply it should not have believed will use it.
	 */
	if (originate != sent)
		return NTP_EMISMATCH;

	/* A server with nothing to say puts zero here. Taking it would set a
	 * clock to 1900. */
	if (server_tx == 0)
		return NTP_EMALFORMED;

	t1 = to_ms(sent);		/* this machine sent */
	t2 = to_ms(server_rx);		/* the server received */
	t3 = to_ms(server_tx);		/* the server replied */
	t4 = to_ms(arrived);		/* this machine received */

	/*
	 * The two figures, from the four times.
	 *
	 * The offset averages the two one-way differences, which cancels the
	 * travel time when it is symmetric. The delay is the round trip less
	 * the time the server spent holding the query, and it is what says how
	 * far the offset can be trusted.
	 */
	into->offset_ms = ((t2 - t1) + (t3 - t4)) / 2;
	into->delay_ms = (t4 - t1) - (t3 - t2);

	/* A negative round trip is impossible and means one of the two clocks
	 * moved during the exchange, so the offset computed from it means
	 * nothing. Reported as zero rather than as a small negative number
	 * somebody would read as precision. */
	if (into->delay_ms < 0)
		into->delay_ms = 0;

	return NTP_OK;
}

/* --- the socket half -------------------------------------------------------
 *
 * The same shape as `dns.c`'s, and for the same reason: everything that can be
 * got wrong is above, driven by the suite, and this only moves bytes.
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define NTP_TRIES_MAX 400000

int ntp_query(struct ntp_client *c, struct ntp_sample *into)
{
	unsigned char query[NTP_PACKET_BYTES];
	unsigned char reply[NTP_PACKET_BYTES + 8];
	struct sockaddr_in to;
	unsigned long long sent, arrived;
	unsigned long started = 0, tries = 0;
	long n, got = 0;
	int fd, rc, verdict;

	if (!c || !into || !c->now_ntp)
		return NTP_EMALFORMED;

	into->offset_ms = 0;
	into->delay_ms = 0;
	into->stratum = 0;
	into->ref_id = 0;

	sent = c->now_ntp();
	n = ntp_build_query(sent, query, sizeof(query));
	if (n < 0)
		return (int)n;

	c->asked++;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return NTP_EMALFORMED;

	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = (unsigned short)((c->port >> 8) | (c->port << 8));
	to.sin_addr.s_addr = (unsigned int)((c->server >> 24)
	                                    | ((c->server >> 8) & 0x0000FF00u)
	                                    | ((c->server << 8) & 0x00FF0000u)
	                                    | (c->server << 24));

	rc = connect(fd, (struct sockaddr *)&to, sizeof(to));
	if (rc != 0 || write(fd, query, (size_t)n) != n) {
		close(fd);
		c->refused++;
		return NTP_EMALFORMED;
	}

	if (c->now_ms)
		started = c->now_ms();

	/* Nothing blocks here. Ask again, yielding between attempts -- the
	 * same loop and the same reason as `dns.c` and `serve.c`. */
	for (;;) {
		got = read(fd, reply, sizeof(reply));
		if (got > 0)
			break;

		if (c->now_ms) {
			if (c->now_ms() - started >= c->timeout_ms)
				break;
		} else if (++tries >= NTP_TRIES_MAX) {
			break;
		}

		if (c->idle)
			c->idle();
	}

	/*
	 * The arrival time is taken **after the read and before anything else**,
	 * so the round trip this measures is the network's rather than the
	 * network's plus however long the parsing took.
	 */
	arrived = c->now_ntp();
	close(fd);

	if (got <= 0) {
		c->refused++;
		return NTP_EMISMATCH;	/* nothing arrived in time */
	}

	verdict = ntp_parse_response(reply, (size_t)got, sent, arrived, into);
	if (verdict == NTP_OK) {
		c->answered++;
		c->last = *into;
		c->have_last = 1;
	} else {
		c->refused++;
	}
	return verdict;
}
