/*
 * Building a query, and reading an answer without trusting it.
 *
 * See `dns.h` for why a resolver is buildable when the audit said DNS was
 * blocked, and for the two families of fault this is arranged against.
 *
 * --- What was tried and rejected ---
 *
 * **Following compression pointers wherever they point.** That is what the
 * format appears to allow and it is how a parser is made to loop forever: a
 * pointer to its own offset, or two pointing at each other. Every pointer here
 * must target an offset strictly *before* the one it was read from, which
 * makes a cycle impossible rather than merely bounded -- each jump lands
 * nearer the start and the start is a floor.
 *
 * **Comparing the returned name to the question by decoding both.** Tempting,
 * and it means running the decoder over attacker-chosen bytes twice. The
 * question in a response is compared **byte for byte against the encoded name
 * this asked for**, which needs no decoding at all: this file already has the
 * encoded form, because it wrote it.
 *
 * **Reporting partial results on a mismatch.** A caller handed "the answers,
 * but the id was wrong" uses the answers. There is one way out of this file
 * with data in it and it passes every check.
 *
 * **Following CNAME chains.** A name that is an alias comes back as a CNAME
 * plus, usually, the A records for its target in the same message -- which is
 * why the scan below takes A records whose owner it has not verified, and says
 * so where it does. Chasing the chain properly means re-querying and a loop
 * bound of its own, and neither is written. What is here is honest about
 * which of the two it is doing.
 */

#include "dns.h"

/* --- small helpers, counted rather than terminated --------------------------
 *
 * Everything below runs over bytes a name server chose, which may hold a NUL
 * anywhere. The string functions are the wrong tools and none is used. */

static size_t len_of(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

static unsigned rd16(const unsigned char *p)
{
	return ((unsigned)p[0] << 8) | p[1];
}

static unsigned long rd32(const unsigned char *p)
{
	return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
	     | ((unsigned long)p[2] << 8) | p[3];
}

/* --- encoding a name -------------------------------------------------------- */

/*
 * The bytes a label may hold.
 *
 * Printable ASCII only. The real rule for host names is narrower and the real
 * rule for DNS is wider -- a label may hold any byte at all -- and this sits
 * between them deliberately.
 *
 * Wider would admit a NUL, which ends the name early in every C caller that
 * later handles it, and a space or a dot, which split it somewhere else.
 * Narrower would refuse the underscore that `_dmarc` and every SRV record
 * need. What is refused is what changes meaning when the name is passed on.
 */
static int label_byte(unsigned char c)
{
	return c > 0x20 && c < 0x7F && c != '.';
}

long dns_encode_name(const char *name, unsigned char *out, size_t room)
{
	size_t at = 0, i = 0, n;
	size_t label_start;

	if (!name || !out || room == 0)
		return DNS_EROOM;

	n = len_of(name);

	/* A single trailing dot is the root and is allowed: `example.com.` and
	 * `example.com` are the same name. More than one is not. */
	if (n > 0 && name[n - 1] == '.')
		n--;

	if (n == 0)
		return DNS_ENAME;
	if (n > DNS_NAME_MAX - 1)
		return DNS_ENAME;

	while (i < n) {
		size_t label_len = 0;

		label_start = at;
		if (at + 1 > room)
			return DNS_EROOM;
		out[at++] = 0;			/* filled in below */

		while (i < n && name[i] != '.') {
			if (!label_byte((unsigned char)name[i]))
				return DNS_ENAME;
			if (at + 1 > room)
				return DNS_EROOM;
			out[at++] = (unsigned char)name[i];
			label_len++;
			i++;
		}

		/* An empty label is `a..b`, or a name beginning with a dot.
		 * Refused rather than skipped: skipping it would make two
		 * different texts encode to one name, and a check that
		 * compares texts would then disagree with one that compares
		 * names. */
		if (label_len == 0)
			return DNS_ENAME;
		if (label_len > 63)
			return DNS_ENAME;

		out[label_start] = (unsigned char)label_len;

		if (i < n && name[i] == '.') {
			i++;
			/*
			 * A dot with nothing after it.
			 *
			 * The one root dot was already taken off above, so a
			 * remaining trailing dot is an empty final label --
			 * `a..` reaching here rather than being refused. Found
			 * by the suite: without this the text encoded happily
			 * to the same name as `a.`, which means two different
			 * names a person could type became one, and a check
			 * comparing texts would then disagree with one
			 * comparing names.
			 */
			if (i >= n)
				return DNS_ENAME;
		}
	}

	if (at + 1 > room)
		return DNS_EROOM;
	out[at++] = 0;				/* the root label */

	return (long)at;
}

long dns_build_query(const char *name, unsigned short id, unsigned char *out,
                     size_t room)
{
	long n;
	size_t at = 12;

	if (!out || room < 12)
		return DNS_EROOM;

	out[0] = (unsigned char)(id >> 8);
	out[1] = (unsigned char)id;
	out[2] = 0x01;		/* recursion desired */
	out[3] = 0x00;
	out[4] = 0x00; out[5] = 0x01;	/* one question */
	out[6] = 0x00; out[7] = 0x00;	/* no answers */
	out[8] = 0x00; out[9] = 0x00;	/* no authority */
	out[10] = 0x00; out[11] = 0x00;	/* no additional */

	n = dns_encode_name(name, out + at, room - at);
	if (n < 0)
		return n;
	at += (size_t)n;

	if (at + 4 > room)
		return DNS_EROOM;
	out[at++] = 0x00; out[at++] = 0x01;	/* type A */
	out[at++] = 0x00; out[at++] = 0x01;	/* class IN */

	return (long)at;
}

/* --- walking a name in a message -------------------------------------------- */

/*
 * Step over the name at `at`, returning the offset just past it.
 *
 * Does **not** decode it. Nothing here needs the text: the question is
 * compared against the encoded form this file wrote, and an answer's owner
 * name only has to be stepped over to reach the record behind it. A decoder
 * would be more code operating on more attacker-chosen bytes for no caller.
 *
 * Returns `len` on any fault, which no valid offset can equal.
 */
static size_t skip_name(const unsigned char *msg, size_t len, size_t at)
{
	int jumps = 0;
	size_t here = at;

	for (;;) {
		unsigned char c;

		if (here >= len)
			return len;
		c = msg[here];

		if (c == 0)
			return here + 1;

		if ((c & 0xC0) == 0xC0) {
			size_t target;

			if (here + 1 >= len)
				return len;
			target = (size_t)(((c & 0x3F) << 8) | msg[here + 1]);

			/*
			 * **Strictly backwards, always.**
			 *
			 * A pointer to its own offset is an immediate loop; a
			 * pointer forward can be paired with another to make
			 * one. Requiring each jump to land before the byte it
			 * was read from makes a cycle impossible rather than
			 * merely unlikely, because every jump moves nearer a
			 * floor.
			 *
			 * A server that hangs here is a server taken down by
			 * whoever answers a query first.
			 */
			if (target >= here)
				return len;
			if (++jumps > DNS_JUMPS_MAX)
				return len;

			/* The name ends here as far as the *caller* is
			 * concerned: a pointer is the last thing in a name, so
			 * whatever follows in the record starts after it. The
			 * jump matters only if somebody wanted the text, and
			 * nothing here does. */
			return here + 2;
		}

		/* A length byte with either top bit set is neither a length
		 * nor a pointer. Reserved, and refused. */
		if (c & 0xC0)
			return len;
		if (c > 63)
			return len;

		here += 1 + c;
	}
}

/* --- reading a response ------------------------------------------------------ */

int dns_parse_response(const unsigned char *msg, size_t len, const char *name,
                       unsigned short id, struct dns_result *into)
{
	unsigned char wanted[DNS_NAME_MAX + 2];
	long wanted_len;
	unsigned flags, qd, an;
	size_t at, i;

	if (!msg || !name || !into)
		return DNS_EMALFORMED;

	into->count = 0;
	into->rcode = 0;
	into->ttl = 0;

	if (len < 12 || len > DNS_MESSAGE_MAX)
		return DNS_EMALFORMED;

	/* The identifier, first, because everything after it is only worth
	 * reading if this message is a reply to what was asked. */
	if (rd16(msg) != id)
		return DNS_EMISMATCH;

	flags = rd16(msg + 2);
	if ((flags & 0x8000) == 0)
		return DNS_EMISMATCH;	/* a question, not an answer */

	/*
	 * Truncated. The server is saying the real answer did not fit in a
	 * datagram and the right move is to ask again over TCP, which is not
	 * written. Refused rather than used: a partial answer is a subset
	 * somebody else chose, and using it is how a resolver is steered by an
	 * attacker who only has to make the real answer large.
	 */
	if (flags & 0x0200)
		return DNS_ETRUNCATED;

	into->rcode = (int)(flags & 0x000F);

	qd = rd16(msg + 4);
	an = rd16(msg + 6);

	/* Exactly the one question that was asked. Zero means there is nothing
	 * to match against; more than one means this cannot say which answer
	 * belongs to which, and neither is a message this sent. */
	if (qd != 1)
		return DNS_EMISMATCH;

	wanted_len = dns_encode_name(name, wanted, sizeof(wanted));
	if (wanted_len < 0)
		return DNS_ENAME;

	/*
	 * The question, compared byte for byte against what this asked.
	 *
	 * No decoding: this file wrote the encoded form and can compare it
	 * directly. A question containing a compression pointer therefore
	 * fails to match, which is correct -- there is nothing earlier in the
	 * message for it to point at, so a server sending one is not sending
	 * the question this asked.
	 */
	at = 12;
	if (at + (size_t)wanted_len + 4 > len)
		return DNS_EMISMATCH;
	for (i = 0; i < (size_t)wanted_len; i++) {
		unsigned char a = msg[at + i];
		unsigned char b = wanted[i];

		/* Names are compared without regard to case, which is the
		 * rule, and is also what lets a server echo `EXAMPLE.com`. */
		if (a >= 'A' && a <= 'Z')
			a = (unsigned char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = (unsigned char)(b - 'A' + 'a');
		if (a != b)
			return DNS_EMISMATCH;
	}
	at += (size_t)wanted_len;

	if (rd16(msg + at) != 1 || rd16(msg + at + 2) != 1)
		return DNS_EMISMATCH;	/* not A, or not IN */
	at += 4;

	/* An error from the server is reported as one. The RCODE is in
	 * `into->rcode` either way, because "no such name" and "try later" are
	 * different facts and a caller that caches needs to tell them apart. */
	if (into->rcode != 0)
		return DNS_ESERVER;

	/* --- the answers ---------------------------------------------------- */
	for (i = 0; i < an && into->count < DNS_ADDRS_MAX; i++) {
		size_t after_name;
		unsigned type, klass, rdlen;
		unsigned long ttl;

		after_name = skip_name(msg, len, at);
		if (after_name >= len)
			return DNS_ELOOP;
		at = after_name;

		/* type, class, ttl, rdlength */
		if (at + 10 > len)
			return DNS_EMALFORMED;
		type  = rd16(msg + at);
		klass = rd16(msg + at + 2);
		ttl   = rd32(msg + at + 4);
		rdlen = rd16(msg + at + 8);
		at += 10;

		if (at + rdlen > len)
			return DNS_EMALFORMED;

		if (type == 1 && klass == 1) {
			/*
			 * An A record. Its length is four and nothing else --
			 * a record claiming to be an address and carrying
			 * three bytes or five is not one, and reading four
			 * from it anyway is how a parser is made to read past
			 * what arrived.
			 */
			if (rdlen != 4)
				return DNS_EMALFORMED;

			into->addrs[into->count++] = (unsigned int)rd32(msg + at);
			if (into->ttl == 0 || ttl < into->ttl)
				into->ttl = ttl;
		}
		/*
		 * Everything else is stepped over, CNAME included.
		 *
		 * **The owner name of each answer is not checked**, which is a
		 * deliberate limitation rather than an oversight. A name that
		 * is an alias comes back as a CNAME plus the A records of its
		 * target, and those A records are owned by the target rather
		 * than by the name asked for -- so requiring the owner to
		 * match would refuse every aliased name, which is most of the
		 * web.
		 *
		 * What that costs: a server that answers with the question
		 * asked and then attaches an A record for an unrelated name
		 * gets that address believed. The protection against it is the
		 * checks above -- this is an answer to this question from the
		 * server that was asked -- and, properly, DNSSEC, which does
		 * not exist here. It is written down rather than glossed
		 * because a reader deciding whether to trust this needs to
		 * know which of the two it is doing.
		 */
		at += rdlen;
	}

	return DNS_OK;
}

/* --- the socket half ---------------------------------------------------------
 *
 * Everything above is pure and is driven by 65 checks. This is the part that
 * needs a network, and it is deliberately the smallest thing that could work:
 * open, connect, write, poll, parse. Every decision that could be got wrong
 * lives above it.
 *
 * **POSIX names, one source, two systems** -- the same discipline `serve.c`
 * keeps. `socket`, `connect`, `read`, `write` and `close` are all this uses,
 * and on ReconOS the C library answers them over the five calls the kernel
 * took numbers for.
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

/*
 * How many times an answer is asked for before giving up, when the caller
 * supplied no clock.
 *
 * An attempt count is not a duration, and this says so rather than pretending
 * otherwise -- the same note `serve.c` carries about its own fallback. A
 * caller that cares passes `now_ms`.
 */
#define DNS_TRIES_MAX 400000

int dns_resolve(struct dns_client *c, const char *name, unsigned short id,
                struct dns_result *into)
{
	unsigned char query[DNS_MESSAGE_MAX];
	unsigned char reply[DNS_MESSAGE_MAX];
	struct sockaddr_in to;
	long qlen, got = 0;
	unsigned long started = 0, tries = 0;
	int fd, rc, verdict;

	if (!c || !name || !into)
		return DNS_EMALFORMED;

	/*
	 * Cleared before anything can fail.
	 *
	 * **Found on the machine.** A name the encoder refuses returns below
	 * without a query ever being sent, and the endpoint reporting the
	 * failure printed `"rcode":3` -- a real-looking "no such name" that was
	 * uninitialised stack left by the previous request. It would have been
	 * read as an answer from a server that was never asked.
	 *
	 * `dns_parse_response` already clears it on the path through; this
	 * covers every path that never reaches it.
	 */
	into->count = 0;
	into->rcode = 0;
	into->ttl = 0;

	qlen = dns_build_query(name, id, query, sizeof(query));
	if (qlen < 0)
		return (int)qlen;

	c->asked++;

	/* SOCK_DGRAM. A connected datagram socket, which is the one shape of
	 * UDP this kernel offers -- see `dns.h`. */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return DNS_EMALFORMED;

	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = (unsigned short)((c->port >> 8) | (c->port << 8));
	to.sin_addr.s_addr = (unsigned int)((c->server >> 24)
	                                    | ((c->server >> 8) & 0x0000FF00u)
	                                    | ((c->server << 8) & 0x00FF0000u)
	                                    | (c->server << 24));

	rc = connect(fd, (struct sockaddr *)&to, sizeof(to));
	if (rc != 0) {
		close(fd);
		c->refused++;
		return DNS_EMALFORMED;
	}

	if (write(fd, query, (size_t)qlen) != qlen) {
		close(fd);
		c->refused++;
		return DNS_EMALFORMED;
	}

	if (c->now_ms)
		started = c->now_ms();

	/*
	 * Wait for the answer.
	 *
	 * Nothing blocks on this kernel: a read with nothing buffered answers
	 * 0 and returns. So this asks again, yielding between attempts -- the
	 * same shape and the same reason as `read_more` in `serve.c`, which is
	 * where that behaviour was first measured and first misread.
	 */
	for (;;) {
		got = read(fd, reply, sizeof(reply));
		if (got > 0)
			break;

		if (c->now_ms) {
			if (c->now_ms() - started >= c->timeout_ms)
				break;
		} else if (++tries >= DNS_TRIES_MAX) {
			break;
		}

		if (c->idle)
			c->idle();
	}

	close(fd);

	if (got <= 0) {
		c->refused++;
		return DNS_EMISMATCH;	/* nothing arrived in time */
	}

	verdict = dns_parse_response(reply, (size_t)got, name, id, into);
	if (verdict == DNS_OK)
		c->answered++;
	else
		c->refused++;
	return verdict;
}
