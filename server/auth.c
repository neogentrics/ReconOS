/*
 * The token, and comparing it without saying how much of it was right.
 *
 * See `auth.h` for why this exists and what it does not protect against.
 *
 * --- What was tried and rejected ---
 *
 * **`strcmp` for the token.** It stops at the first byte that differs, so the
 * time it takes says how many leading bytes were correct. That turns guessing
 * a 32-character secret from an impossible search into 32 small ones, each
 * decided by measuring a reply. The comparison here reads every byte every
 * time and folds the differences together.
 *
 * Whether the difference is measurable across a network on this kernel is
 * genuinely unclear -- the receive path alone adds milliseconds of noise, and
 * VF-013 measured a stall of seconds. It is written the careful way regardless,
 * because the cost is four lines and the alternative is a defence that depends
 * on a property nobody has measured and that a faster kernel would remove.
 *
 * **Accepting the token in a query string**, as `?token=...`. Convenient, and
 * it puts the secret in the one place that leaks by design: a target is
 * written to this server's own access log, and a browser sends it onward in
 * `Referer`. A header is not private, but it is not routinely copied
 * elsewhere. Only the header is read.
 *
 * **Reporting why a request was refused.** A guard that distinguishes "no
 * token" from "wrong token" tells an attacker which half of the problem they
 * have solved. One answer.
 */

#include "auth.h"

static char hex(unsigned v)
{
	static const char DIGITS[] = "0123456789abcdef";

	return DIGITS[v & 0xF];
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int auth_arm(struct auth *a, const unsigned char *bytes, size_t len)
{
	size_t i;

	if (!a)
		return 0;

	a->armed = 0;
	a->token[0] = '\0';

	/*
	 * Too few bytes is what a failed `SYS_RANDOM` looks like, and it is
	 * refused rather than padded. A token made of whatever was in the
	 * buffer is a token an attacker can often guess, and it would look
	 * exactly like a real one from every angle except the one that
	 * matters.
	 */
	if (!bytes || len < AUTH_TOKEN_BYTES)
		return 0;

	for (i = 0; i < AUTH_TOKEN_BYTES; i++) {
		a->token[i * 2]     = hex((unsigned)bytes[i] >> 4);
		a->token[i * 2 + 1] = hex((unsigned)bytes[i]);
	}
	a->token[AUTH_TOKEN_CHARS] = '\0';
	a->armed = 1;
	return 1;
}

/*
 * Compare `n` bytes without stopping early.
 *
 * Every byte of both is read every time and the differences are folded into
 * one value, so the work done does not depend on where -- or whether -- they
 * differ.
 */
static int same_always(const char *a, const char *b, size_t n)
{
	unsigned char diff = 0;
	size_t i;

	for (i = 0; i < n; i++)
		diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

int auth_ok(const struct auth *a, const char *value)
{
	static const char SCHEME[] = "bearer";
	size_t i, at = 0;

	/*
	 * No token, no entry. See `auth.h`: a guard whose secret was never set
	 * must refuse everything, because the alternative is a server that
	 * reports itself as guarded and is open.
	 */
	if (!a || !a->armed)
		return 0;
	if (!value)
		return 0;

	while (value[at] == ' ' || value[at] == '\t')
		at++;

	for (i = 0; i < sizeof(SCHEME) - 1; i++)
		if (lower(value[at + i]) != SCHEME[i])
			return 0;
	at += sizeof(SCHEME) - 1;

	/*
	 * At least one space after the scheme, and then the token.
	 *
	 * Required rather than optional: without it `Bearer<token>` would be
	 * accepted, and a value this server accepts that another reader would
	 * split differently is the shape of fault the whole HTTP path here is
	 * arranged against.
	 */
	if (value[at] != ' ' && value[at] != '\t')
		return 0;
	while (value[at] == ' ' || value[at] == '\t')
		at++;

	/*
	 * The length is checked first and separately.
	 *
	 * That does leak whether the length was right, which is not a secret:
	 * the length is a compile-time constant printed in this file's own
	 * header. What it avoids is reading past the end of a short value.
	 */
	for (i = 0; value[at + i]; i++)
		if (i > AUTH_TOKEN_CHARS)
			return 0;
	if (i != AUTH_TOKEN_CHARS)
		return 0;

	/* Trailing anything is a different value, not this one. The loop above
	 * already ended at the terminator, so reaching here means the rest of
	 * the header is exactly the token's length. */
	return same_always(value + at, a->token, AUTH_TOKEN_CHARS);
}
