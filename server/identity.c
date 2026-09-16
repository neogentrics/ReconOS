/*
 * Naming a parallel.
 *
 * See `server/include/recon_server.h` for why this is a module. The short
 * version: it runs on a machine with no screen, and the name it picks is the
 * name that machine answers to afterwards.
 *
 * --- The rule, in one sentence ---
 *
 * A name ends in a run of digits or it names no family, and a name that names
 * no family gets no parallel.
 *
 * `M16` is `M` and 16. `srv007` is `srv` and 7 written three wide. `gateway`
 * is refused, because there is no next `gateway` and inventing `gateway2`
 * would be this code deciding what the operator meant.
 *
 * --- What was tried and rejected ---
 *
 * **Splitting on the last non-digit and taking the rest as a number with
 * `atoi`** reads the same for `M16` and is wrong twice over. `atoi` has no way
 * to report overflow, so `M99999999999999999999` would have become some number
 * rather than a refusal; and the digit count is lost, so `srv007` numbers on
 * to `srv8` and silently leaves the family it was cloning.
 *
 * **Counting from 1 and taking the first free number** was the first shape,
 * and it is wrong for the case this exists to serve. A wire holding `M16`
 * alone would hand a parallel `M1` -- a lower name than the machine it is
 * backing, which reads as the original to anyone looking at the two. Parallels
 * count upward from their peer.
 */

#include "include/recon_server.h"

/* No <string.h> and no <ctype.h>.
 *
 * This file is built twice: once against the host's library for the test
 * below, and once against ReconOS's own. `tolower` in particular is locale-
 * dependent on a host and is not here, and a name that compares equal under
 * one library and not the other is a collision that appears on the machine and
 * not in the test. The three helpers are four lines each and remove the
 * question. */

static size_t len_of(const char *s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static char fold(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive equality, because a host name is. */
static int same_name(const char *a, const char *b)
{
	size_t i = 0;

	while (a[i] && b[i]) {
		if (fold(a[i]) != fold(b[i]))
			return 0;
		i++;
	}
	return a[i] == b[i];
}

/* What a host name may carry: letters, digits and the hyphen.
 *
 * RFC 1123's set, and deliberately not the underscore -- it appears in service
 * records and in a good deal of Windows naming, and accepting it here would
 * produce names that this machine answers to and DNS will not publish. Refused
 * at the door rather than at the zone file. */
static int legal_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
	    || is_digit(c) || c == '-';
}

int server_name_split(const char *name, struct name_family *into)
{
	size_t n, stem_len, i;
	unsigned value = 0, digits = 0;

	if (!name || !into)
		return SERVER_EEMPTY;

	n = len_of(name);
	if (n == 0)
		return SERVER_EEMPTY;
	if (n >= RECON_NAME_MAX)
		return SERVER_ETOOLONG;

	for (i = 0; i < n; i++)
		if (!legal_char(name[i]))
			return SERVER_ECHAR;

	/* A hyphen may not open or close a host name. Checked here rather than
	 * in `legal_char`, which sees one character and cannot know where it
	 * sat. */
	if (name[0] == '-' || name[n - 1] == '-')
		return SERVER_ECHAR;

	/* Walk back over the trailing digits. */
	stem_len = n;
	while (stem_len > 0 && is_digit(name[stem_len - 1]))
		stem_len--;

	digits = (unsigned)(n - stem_len);
	if (digits == 0)
		return SERVER_EUNNUMBERED;
	if (stem_len == 0)
		return SERVER_EALLDIGITS;

	/* Parse the run, refusing an overflow rather than wrapping.
	 *
	 * The check is done before the multiply, not after: `value * 10`
	 * having already wrapped cannot be detected by looking at the result,
	 * which is the bug this shape exists to avoid. */
	for (i = stem_len; i < n; i++) {
		unsigned d = (unsigned)(name[i] - '0');

		if (value > (0xFFFFFFFFu - d) / 10u)
			return SERVER_ERANGE;
		value = value * 10u + d;
	}

	for (i = 0; i < stem_len; i++)
		into->stem[i] = name[i];
	into->stem[stem_len] = '\0';
	into->number = value;
	into->digits = digits;

	return SERVER_OK;
}

int server_name_format(const struct name_family *family, unsigned number,
                       char *into, size_t room)
{
	char digits[11];		/* 4294967295 is ten digits */
	size_t ndigits = 0, stem_len, pad, total, i, at = 0;

	if (!family || !into || room == 0)
		return SERVER_EEMPTY;

	/* Render the number backwards, then measure before writing anything.
	 * Nothing reaches `into` until the whole name is known to fit, so a
	 * refusal leaves the caller's buffer as it found it rather than
	 * half-written. */
	if (number == 0) {
		digits[ndigits++] = '0';
	} else {
		unsigned v = number;

		while (v > 0) {
			digits[ndigits++] = (char)('0' + (v % 10u));
			v /= 10u;
		}
	}

	stem_len = len_of(family->stem);

	/* The family's width is a floor, never a ceiling: M99 goes to M100. */
	pad = (family->digits > ndigits) ? family->digits - ndigits : 0;

	total = stem_len + pad + ndigits;
	if (total >= RECON_NAME_MAX)
		return SERVER_ETOOLONG;
	if (total + 1 > room)
		return SERVER_ETOOLONG;

	for (i = 0; i < stem_len; i++)
		into[at++] = family->stem[i];
	for (i = 0; i < pad; i++)
		into[at++] = '0';
	while (ndigits > 0)
		into[at++] = digits[--ndigits];
	into[at] = '\0';

	return SERVER_OK;
}

int server_name_next_parallel(const char *peer,
                              const char *const *taken, size_t count,
                              char *into, size_t room)
{
	struct name_family family;
	char candidate[RECON_NAME_MAX];
	unsigned step;
	int rc;

	if (!peer || !into || room == 0)
		return SERVER_EEMPTY;
	if (count > 0 && !taken)
		return SERVER_EEMPTY;

	rc = server_name_split(peer, &family);
	if (rc != SERVER_OK)
		return rc;

	/* Count upward from the peer, not from one. See the file header. */
	for (step = 1; step <= RECON_PARALLEL_SEARCH_MAX; step++) {
		size_t i;
		int free_name = 1;

		if (family.number > 0xFFFFFFFFu - step)
			return SERVER_ERANGE;

		rc = server_name_format(&family, family.number + step,
		                        candidate, sizeof(candidate));
		if (rc != SERVER_OK)
			return rc;

		for (i = 0; i < count; i++) {
			if (taken[i] && same_name(taken[i], candidate)) {
				free_name = 0;
				break;
			}
		}

		if (free_name)
			return server_name_format(&family,
			                          family.number + step,
			                          into, room);
	}

	return SERVER_EEXHAUSTED;
}
