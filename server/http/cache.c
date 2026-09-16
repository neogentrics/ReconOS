/*
 * Hashing bytes, and reading an If-None-Match.
 *
 * See `cache.h` for why the validator is a content hash rather than a
 * modification time, and for what that costs.
 *
 * --- What was tried and rejected ---
 *
 * **An ETag from the length alone.** It needs no second read of the file and
 * it is wrong in the worst possible direction: an edit that keeps a file the
 * same length -- a corrected digit, a flipped boolean, a swapped word of equal
 * width -- leaves the tag unchanged, so every client holding the old copy goes
 * on serving it from cache and nothing anywhere reports a fault. A validator
 * that misses the edits people actually make is worse than no validator, which
 * at least sends the bytes.
 *
 * **Comparing `If-None-Match` with a plain string compare.** It is one line
 * and it fails on the two shapes clients really send: a list, and a weak tag.
 * A browser revalidating several copies sends `"a", "b"`, and a proxy that
 * touched the response sends `W/"a"`. Either is a match that a string compare
 * calls a miss -- so the body is sent again, the cache never settles, and the
 * only symptom is that revalidation quietly never works.
 */

#include "cache.h"

unsigned long long http_hash(unsigned long long state, const void *data,
                             size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	size_t i;

	for (i = 0; i < len; i++) {
		state ^= (unsigned long long)p[i];
		state *= 0x100000001b3ULL;
	}
	return state;
}

/* Render `v` in hex, most significant digit first, into `digits`. Returns how
 * many. Backwards into scratch and reversed by the caller, which is the only
 * order that does not need to know the digit count in advance. */
static size_t hex_of(unsigned long long v, char *digits)
{
	static const char HEX[] = "0123456789abcdef";
	size_t n = 0;

	if (v == 0) {
		digits[n++] = '0';
		return n;
	}
	while (v) {
		digits[n++] = HEX[v & 0xF];
		v >>= 4;
	}
	return n;
}

void http_etag_format(char *into, size_t room, unsigned long length,
                      unsigned long long hash)
{
	char a[32], b[32];
	size_t na, nb, total, at = 0;

	if (!into || room == 0)
		return;
	into[0] = '\0';

	na = hex_of((unsigned long long)length, a);
	nb = hex_of(hash, b);

	/* Measured before a byte is written, so a buffer too small leaves the
	 * caller with an empty string rather than a short tag.
	 *
	 * **A truncated validator is the dangerous failure here**, not an
	 * absent one. Two different files cut to the same prefix share a tag,
	 * and a shared tag means a client serves one file's bytes for the
	 * other out of its own cache, for as long as the cache lives. Nothing
	 * at either end reports it. An empty tag merely means no caching.
	 *
	 * `HTTP_ETAG_MAX` is sized so a caller using it never reaches this. */
	total = 1 + na + 1 + nb + 1;		/* " len - hash " */
	if (total + 1 > room)
		return;

	into[at++] = '"';
	while (na)
		into[at++] = a[--na];
	into[at++] = '-';
	while (nb)
		into[at++] = b[--nb];
	into[at++] = '"';
	into[at] = '\0';
}

/* One tag from a list: skips whitespace and an optional `W/`, then reads a
 * quoted string. Returns where it stopped, or NULL at the end. */
static const char *next_tag(const char *at, const char **start, size_t *len)
{
	while (*at == ' ' || *at == '\t' || *at == ',')
		at++;
	if (!*at)
		return 0;

	/* A weak tag. `If-None-Match` compares weakly, so the marker is
	 * stepped over rather than being part of what is compared. */
	if (at[0] == 'W' && at[1] == '/')
		at += 2;

	if (*at != '"')
		return 0;	/* not a tag; stop rather than guess */
	at++;
	*start = at;

	while (*at && *at != '"')
		at++;
	if (*at != '"')
		return 0;	/* unterminated */
	*len = (size_t)(at - *start);
	return at + 1;
}

int http_if_none_match(const char *header, const char *etag)
{
	const char *at = header;
	const char *mine;
	size_t mine_len = 0;

	if (!header || !etag || !*etag)
		return 0;

	/* `*` means "any representation I might have". A client sends it to
	 * ask for the body only if the thing does not exist at all. */
	{
		const char *p = header;

		while (*p == ' ' || *p == '\t')
			p++;
		if (p[0] == '*')
			return 1;
	}

	/* The server's own tag, unquoted, so the two are compared as contents
	 * rather than as spellings. */
	if (etag[0] != '"')
		return 0;
	mine = etag + 1;
	while (mine[mine_len] && mine[mine_len] != '"')
		mine_len++;
	if (mine[mine_len] != '"')
		return 0;

	for (;;) {
		const char *start = 0;
		size_t len = 0;
		size_t i;

		at = next_tag(at, &start, &len);
		if (!at)
			return 0;

		if (len == mine_len) {
			for (i = 0; i < len; i++)
				if (start[i] != mine[i])
					break;
			if (i == len)
				return 1;
		}
	}
}
