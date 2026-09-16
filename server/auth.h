/*
 * A secret printed on the console, and the endpoints that will not act without
 * it.
 *
 * --- The hole this closes ---
 *
 * `POST /api/name` renames the machine and `POST /api/upload` writes a file to
 * its volume, and until this existed **neither asked anything of the caller**.
 * Anyone who could reach port 80 could rename the server and fill its disk.
 * The upload endpoint made it worse and is what forced the question.
 *
 * --- Why not the two obvious answers ---
 *
 * **Restrict by source address.** Impossible here, and not as a matter of
 * effort: `SYS_ACCEPT` takes only a descriptor, so the kernel has no way to
 * report who connected. The C library zeroes the address and says so in a
 * comment. Filed in `docs/KERNEL-WANTS.md`, because the access log cannot name
 * a client either.
 *
 * **Basic authentication.** A user and a password in clear on every request,
 * which `docs/WEB.md` §5 already defers until TLS exists. Nothing here changes
 * that: this is not a login, and it does not want a password store, a hash, or
 * a user model -- none of which this system has.
 *
 * --- What this is instead ---
 *
 * A random token, made once at boot and printed on the serial console. A
 * caller sends it back as `Authorization: Bearer <token>`.
 *
 * It is a **capability, not a credential**: it says the holder can read this
 * machine's console, which is the same thing physical access to it would say.
 * It is not a person, it does not expire, and it is gone when the machine
 * restarts -- a new boot means a new token, which is the right behaviour for a
 * secret that only ever existed to gate a console.
 *
 * --- What it does not protect against, stated plainly ---
 *
 * **Anyone who can read the traffic.** Over plain HTTP the token is in a header
 * in clear, on every guarded request. Someone on the path takes it once and
 * has it until reboot.
 *
 * So this stops a passer-by on the network and does not stop an attacker on
 * the wire. That is a real improvement over nothing and it is not security
 * against a capable adversary, and it must not be described as the latter.
 * When TLS exists this becomes worth something; until then it is a lock on a
 * door with a window beside it.
 *
 * It is still worth fitting. The alternative on offer was no lock.
 *
 * --- What is guarded and what is not ---
 *
 * Writes are. Reads are not: the dashboard, the status, the service list and
 * the log stay open, so that a person or a monitor can look at a machine
 * without holding its secret.
 *
 * That is a decision with a cost and it is written down rather than assumed:
 * `GET /api/log` reports the target of every recent request, so anyone may
 * learn what paths have been asked for. Nothing in it names a client -- see
 * above, the kernel could not tell us -- and nothing in it carries a token,
 * which is why a token must never travel in a query string.
 */

#ifndef RECON_SERVER_AUTH_H
#define RECON_SERVER_AUTH_H

#include <stddef.h>

/*
 * 16 bytes, rendered as 32 hex characters.
 *
 * Enough that guessing is not a strategy, and short enough to be read off a
 * console and typed. Hex rather than anything denser for the same reason: a
 * secret somebody has to transcribe from a screen should not contain
 * characters that look like each other.
 */
#define AUTH_TOKEN_BYTES 16
#define AUTH_TOKEN_CHARS (AUTH_TOKEN_BYTES * 2)

struct auth {
	char token[AUTH_TOKEN_CHARS + 1];

	/*
	 * Whether a token was ever set.
	 *
	 * **Nothing is allowed while this is 0.** A server whose randomness
	 * failed has no secret, and a guard with no secret that lets everyone
	 * through is worse than no guard at all -- it reads as protection in
	 * every log and document while providing none.
	 */
	int armed;
};

/*
 * Set the token from `len` random bytes.
 *
 * Refuses, leaving the guard unarmed, if there are too few bytes -- which is
 * what a failed `SYS_RANDOM` looks like. Returns 1 when armed, 0 when not.
 */
int auth_arm(struct auth *a, const unsigned char *bytes, size_t len);

/*
 * Does this `Authorization` header value carry the right token?
 *
 * Takes the header value rather than the request, so the whole decision is
 * pure and every shape a client can send is driven directly by the suite.
 * `value` may be NULL, which is what an absent header gives.
 *
 * The scheme is matched without regard to case, because RFC 7235 says schemes
 * are case-insensitive and clients really do send `bearer`. **The token is
 * matched exactly**, and compared in constant time -- see `auth.c`.
 *
 * Returns 1 to allow and 0 to refuse. There is no third answer on purpose: a
 * caller that has to distinguish "wrong token" from "no token" is a caller
 * that will report the difference, and telling an attacker which half they got
 * right is the whole of what a guard must not do.
 */
int auth_ok(const struct auth *a, const char *value);

#endif
