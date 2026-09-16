/*
 * Asking a name server for an address, and not believing the answer too
 * readily.
 *
 * --- Why this exists now, when the audit said DNS was blocked ---
 *
 * `docs/SERVER.md` listed DNS as **blocked** on "an unconnected datagram
 * socket", and that was true of the half it was written about: a DNS *server*
 * must reply to whoever asked, and DHCP must broadcast. Neither can be written
 * without `sendto` and `recvfrom`, which the kernel has internally and does not
 * expose.
 *
 * **A resolver is the other half, and it is a connected datagram.** One known
 * server, one question, one answer. `kernel/core/socket_file.c` says so in as
 * many words -- *a connected UDP socket works through `write` today* -- and
 * the machine agrees. Measured on 16 September 2026, before a line of this was
 * written:
 *
 *     udp probe: fd=5 connect=0 write=29 read=61 tries=2619
 *     udp probe: id=1234 flags=8180 qd=1 an=2 last=104.20.23.154
 *
 * A real query to 10.0.2.3:53 and a real answer for example.com. So the entry
 * was not wrong about DHCP and was too broad about DNS, which is the fourth
 * time this role has recorded a blocker wider than the truth -- VF-004,
 * VF-009, VF-014, and now this.
 *
 * --- Where the danger is ---
 *
 * **Not in asking. In believing.** A DNS response is a message from an
 * unauthenticated source over an unauthenticated transport, parsed by code
 * that then decides where traffic goes. Historically that combination has
 * produced two families of fault, and this file is arranged against both.
 *
 * **Parsing.** A name is a chain of length-prefixed labels, and a label may
 * instead be a *pointer* to somewhere earlier in the message -- compression.
 * A pointer that points at itself is an infinite loop; a pointer that points
 * forward can be made to loop with a second one. A server that hangs parsing a
 * reply is a server taken down by anyone who can answer a query first. Every
 * pointer here must point strictly backwards, and the chain is bounded anyway.
 *
 * **Accepting.** A response is only an answer to *this* question if it matches
 * on every field that identifies it. An attacker who cannot see the query can
 * still guess it and answer first, which is how a resolver is made to hand out
 * an address of somebody else's choosing. So the identifier, the question
 * count, the question name, its type and its class are all checked, and a
 * mismatch is discarded rather than repaired.
 *
 * This does not make the answer trustworthy. Nothing short of DNSSEC does, and
 * there is no crypto here. It makes it *this* answer to *this* question, which
 * is the part a parser can be responsible for.
 */

#ifndef RECON_SERVER_DNS_H
#define RECON_SERVER_DNS_H

#include <stddef.h>

/* A DNS message over UDP is 512 bytes unless both ends agreed otherwise, and
 * nothing here does. Anything larger is a message this cannot have asked for. */
#define DNS_MESSAGE_MAX 512

/* 253 characters of text, which is the most a name can hold once the length
 * bytes are counted. Plus a terminator. */
#define DNS_NAME_MAX 254

/* How many addresses one answer may yield. A name with more is not an error --
 * the rest are simply not reported, and `dns_result.count` says how many were
 * kept rather than how many arrived, because a caller acting on the first is
 * doing the ordinary thing. */
#define DNS_ADDRS_MAX 8

/*
 * How deep a compression pointer chain may go.
 *
 * Every pointer must already point strictly backwards, which makes a loop
 * impossible on its own. This bound is the second answer to the same question,
 * kept because the cost is one comparison and the failure it prevents is a
 * server that stops responding.
 */
#define DNS_JUMPS_MAX 16

/* What came back. Verdicts are negative; `DNS_OK` means the message was a
 * well-formed answer to the question asked, which is not the same as the name
 * existing. */
#define DNS_OK            0
#define DNS_EMALFORMED  (-1)	/* not a DNS message this can read */
#define DNS_ETRUNCATED  (-2)	/* the server set TC: the answer did not fit */
#define DNS_EMISMATCH   (-3)	/* not an answer to the question asked */
#define DNS_ENAME       (-4)	/* the name could not be encoded */
#define DNS_EROOM       (-5)	/* the caller's buffer is too small */
#define DNS_ELOOP       (-6)	/* a compression pointer that does not go back */
#define DNS_ESERVER     (-7)	/* the server answered with an error code */

struct dns_result {
	/* Addresses in host byte order, most significant octet first, so
	 * `0x0A000203` is 10.0.2.3 -- the same convention `dial.c` takes. */
	unsigned int addrs[DNS_ADDRS_MAX];
	size_t       count;

	/* The server's RCODE, kept even when it is zero. A caller that wants
	 * to tell "no such name" (3) from "server failed" (2) needs it, and
	 * collapsing both into one verdict is how a temporary failure gets
	 * cached as a permanent one. */
	int rcode;

	/* The smallest TTL among the addresses kept, in seconds; 0 when none
	 * were. Not used yet -- there is no cache -- and carried because a
	 * cache that has to re-parse to find it would re-parse untrusted
	 * bytes a second time. */
	unsigned long ttl;
};

/*
 * Write a query for `name`'s A records into `out`.
 *
 * `id` is the identifier the answer must echo. **A caller must make it
 * unpredictable**: it is one of the few things an off-path attacker has to
 * guess, and a counter starting at one is not a guess anybody has to make.
 * `server_init.c` takes it from `SYS_RANDOM`.
 *
 * Returns the number of bytes written, or a negative verdict.
 */
long dns_build_query(const char *name, unsigned short id, unsigned char *out,
                     size_t room);

/*
 * Read a response to a query for `name` with `id`.
 *
 * **Both are required, and that is the point.** Handing this only the message
 * would make it a parser; handing it the question makes it a check. A message
 * that does not match is `DNS_EMISMATCH` and its contents are not reported at
 * all -- not even partially, because a caller shown "some" of a mismatched
 * answer will use it.
 */
int dns_parse_response(const unsigned char *msg, size_t len, const char *name,
                       unsigned short id, struct dns_result *into);

/*
 * Encode a name into wire format -- labels with length bytes, ending in a zero.
 *
 * Exposed because it is where every name-shaped refusal lives, and a suite that
 * can call it directly can drive shapes a query builder would never produce.
 *
 * Refuses: an empty name, an empty label (`a..b`), a label over 63 bytes, a
 * name over 253, and any byte below 0x21 or above 0x7E -- which covers the NUL
 * that would end a name early somewhere else and the space that would split it.
 */
long dns_encode_name(const char *name, unsigned char *out, size_t room);

/* --- the socket half ---------------------------------------------------------
 *
 * Separate from everything above, which is pure and tested exhaustively. This
 * part cannot be: it needs a network and a name server. It is kept small for
 * exactly that reason -- every decision lives above, and this only moves bytes.
 */

struct dns_client {
	/* The resolver to ask, most significant octet first. */
	unsigned int server;
	unsigned short port;

	/* Set by the caller to whatever this system uses to let other work
	 * run while waiting, or NULL. Nothing blocks on this kernel, so a
	 * caller without one spins. Same argument as `serve.h`'s `idle`. */
	void (*idle)(void);

	/* Milliseconds since some fixed point, or NULL. Used only for
	 * differences. Without one the wait is bounded by attempts instead,
	 * which is not a duration and says so. */
	unsigned long (*now_ms)(void);

	/* How long to wait for an answer. */
	unsigned long timeout_ms;

	/* Counters, for the console line and the service registry. */
	unsigned long asked;
	unsigned long answered;
	unsigned long refused;
};

/*
 * Ask, wait, and check. Returns `DNS_OK` or a verdict.
 *
 * `id` must be unpredictable -- see `dns_build_query`. This does not choose one
 * itself, because the only good source is `SYS_RANDOM` and reaching it from
 * here would tie this file to ReconOS; `serve.c` keeps the same discipline for
 * the same reason.
 *
 * One query, one wait, no retry. A caller that wants to try a second server
 * calls again with a different one, which is a decision about policy and does
 * not belong here.
 */
int dns_resolve(struct dns_client *c, const char *name, unsigned short id,
                struct dns_result *into);

#endif
