/*
 * Asking what the time is, and knowing how wrong this machine's clock is.
 *
 * --- What this can and cannot do, said first ---
 *
 * **It measures. It does not correct.** There is no call that sets the clock:
 * `SYS_TIME` and `SYS_WALLTIME` both read, and nothing writes. So this answers
 * *by how much is this machine wrong*, and a person or a later kernel does
 * something about it.
 *
 * That is worth building rather than waiting for. A server whose clock is
 * eleven seconds fast and knows it is a different machine from one that does
 * not know -- every log timestamp, every TTL, and every certificate this
 * system will eventually check is read against that clock. The number is the
 * useful half; applying it is one syscall away and not this role's to add.
 *
 * `docs/SERVER.md` listed NTP as blocked on *"same [datagram]; and no
 * user-mode timer"*. The datagram half was wrong in the same way DNS's was --
 * see `dns.h` and VF-017 -- because this, too, is a **connected** datagram to
 * a known server. The clock half is true and is what limits this to measuring.
 *
 * --- Where the danger is ---
 *
 * The same place as DNS: in believing the answer.
 *
 * A reply arrives over an unauthenticated datagram from whoever answers first,
 * and what it sets is *the clock* -- which is the thing every expiry check in a
 * system is read against. A clock an attacker can move is a certificate they
 * can un-expire and a token they can revive.
 *
 * So the check that matters is the same shape as the DNS identifier: **the
 * server must echo, exactly, the transmit timestamp this machine sent.** That
 * is a 64-bit value an off-path attacker has to guess, and it is the only
 * thing separating an answer from an assertion. A reply that does not echo it
 * is discarded whole, and nothing in it is reported.
 *
 * Everything else here is refusal: a mode that is not a server's, a stratum
 * that says the server itself is not synchronised, a leap indicator that says
 * the same, and the kiss-of-death packet a server sends when it wants to be
 * left alone.
 */

#ifndef RECON_SERVER_NTP_H
#define RECON_SERVER_NTP_H

#include <stddef.h>

/* An SNTP message is exactly this, and a reply of any other size is not one.
 * Extensions and authentication make a longer packet; neither is understood
 * here, so a longer one is refused rather than read as far as it makes sense. */
#define NTP_PACKET_BYTES 48

/* Seconds between 1900-01-01 and 1970-01-01. NTP counts from the former and
 * every clock in this system from the latter. Written as the number rather
 * than computed, because it is a constant of the protocol and not arithmetic
 * anybody should have to check. */
#define NTP_EPOCH_OFFSET 2208988800ULL

#define NTP_OK            0
#define NTP_EMALFORMED  (-1)	/* not an SNTP message this can read */
#define NTP_EMISMATCH   (-2)	/* it did not echo what was sent -- see above */
#define NTP_EUNSYNCED   (-3)	/* the server says its own time is no good */
#define NTP_EKISS       (-4)	/* stratum 0: a kiss-of-death, meaning stop */
#define NTP_EROOM       (-5)	/* the caller's buffer is too small */

struct ntp_sample {
	/*
	 * How far this machine's clock is from the server's, in milliseconds.
	 * Positive means **this machine is behind** and should move forward.
	 *
	 * Signed and in milliseconds rather than the protocol's 64-bit fixed
	 * point, because every consumer of this is a person reading a console
	 * or a line in a log. The precision thrown away is far below what a
	 * datagram's round trip can resolve anyway.
	 */
	long long offset_ms;

	/*
	 * The round trip, which is what bounds how much the offset can be
	 * trusted: the true offset is within half the delay of the measured
	 * one. Reported so nobody has to take a number without its error.
	 *
	 * **Only as good as the caller's clock, which on ReconOS today is not
	 * good at all.** `SYS_WALLTIME` is declared in nanoseconds and counts
	 * whole seconds -- measured, five reads giving the same value with
	 * nine zeroes on the end. Both of this machine's timestamps therefore
	 * land in the same second and this computes to zero, which is
	 * quantisation rather than a measurement and must not be printed as
	 * one.
	 *
	 * The offset is less affected because two of its four terms are the
	 * server's, which are fine-grained; what it loses is precision, to
	 * roughly a second. `server_init.c` says so on the console instead of
	 * implying an accuracy it cannot have.
	 */
	long long delay_ms;

	int stratum;		/* 1 is a reference clock, 2 is one step away */
	unsigned long ref_id;	/* four bytes naming the server's source */
};

/*
 * Write a client query into `out`.
 *
 * `sent` is this machine's current time as a 64-bit NTP timestamp, and it goes
 * into the transmit field. **The reply must echo it exactly**, which is the
 * whole of this protocol's protection against an answer from somebody who was
 * not asked -- so a caller must pass the real clock rather than a constant, and
 * the more of its low bits that vary the better.
 *
 * Returns the bytes written, or a negative verdict.
 */
long ntp_build_query(unsigned long long sent, unsigned char *out, size_t room);

/*
 * Read a reply.
 *
 * `sent` is what was put in the query and `arrived` is this machine's clock
 * when the reply came back -- both as NTP timestamps. Both are required
 * because an offset is arithmetic over four times, and two of them are this
 * machine's.
 *
 * **`sent` is also the check.** Handing this only the message would make it a
 * parser; handing it what was asked makes it a check, exactly as
 * `dns_parse_response` takes the question.
 */
int ntp_parse_response(const unsigned char *msg, size_t len,
                       unsigned long long sent, unsigned long long arrived,
                       struct ntp_sample *into);

/* --- the socket half ------------------------------------------------------ */

struct ntp_client {
	unsigned int server;	/* most significant octet first, as `dial.c` */
	unsigned short port;

	void (*idle)(void);
	unsigned long (*now_ms)(void);
	unsigned long long (*now_ntp)(void);	/* this machine's clock */
	unsigned long timeout_ms;

	unsigned long asked;
	unsigned long answered;
	unsigned long refused;

	/* The last good sample, kept so a caller that polls on a schedule can
	 * report the most recent answer rather than nothing between rounds. */
	struct ntp_sample last;
	int have_last;
};

int ntp_query(struct ntp_client *c, struct ntp_sample *into);

#endif
