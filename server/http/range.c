/*
 * Reading a Range header.
 *
 * See `range.h` for why only one range is honoured and why `If-Range` is not
 * optional.
 *
 * --- What was tried and rejected ---
 *
 * **Clamping a start past the end of the file to the last byte.** It makes
 * every range satisfiable and it answers a question the client did not ask: a
 * request for bytes 9000-9999 of a 500-byte file is a client working from a
 * length it got somewhere else, and handing it the last byte with a 206 tells
 * it that it now has bytes 9000-9999. 416 with the real length is the answer
 * that lets it recover.
 *
 * **Clamping an *end* past the file, on the other hand, is correct** and is
 * what happens below. `bytes=0-` and `bytes=0-99999` on a short file both mean
 * "to the end", and the standard says so.
 *
 * **Parsing with `strtoul`.** It accepts a leading `+`, a leading `-`, and
 * surrounding space, and it reports success while stopping at the first byte it
 * did not like. Every one of those turns a header that should have been ignored
 * into a range that was not asked for. The digits are read by hand, and
 * anything that is not a digit ends the number.
 */

#include "range.h"

/* The largest length this will do arithmetic on. A range is checked against a
 * file's length, which comes from `lseek`, and a number past this would be a
 * file larger than the arithmetic below is safe for -- so it is refused rather
 * than wrapped. */
#define RANGE_MAX 0xFFFFFFFFUL

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

/*
 * Read digits into `*out`, returning how many were read.
 *
 * Overflow answers -1 rather than wrapping. A wrapped range is a small number
 * where an enormous one was written, which is a request the client never made
 * being served without complaint.
 */
static int digits(const char *at, unsigned long *out)
{
	unsigned long v = 0;
	int n = 0;

	while (is_digit(at[n])) {
		unsigned d = (unsigned)(at[n] - '0');

		if (v > (RANGE_MAX - d) / 10UL)
			return -1;
		v = v * 10UL + d;
		n++;
	}

	*out = v;
	return n;
}

int http_range_parse(const char *header, unsigned long length,
                     struct http_range *into)
{
	const char *at = header;
	unsigned long first = 0, last = 0;
	int nf, nl;

	if (!header || !into)
		return HTTP_RANGE_NONE;
	if (length > RANGE_MAX)
		return HTTP_RANGE_NONE;

	while (*at == ' ' || *at == '\t')
		at++;

	/* `bytes=` and nothing else. Other units exist in the standard and
	 * none of them are implemented, so a unit this does not know is a
	 * header to ignore rather than to guess at. */
	if (at[0] != 'b' || at[1] != 'y' || at[2] != 't' || at[3] != 'e'
	    || at[4] != 's' || at[5] != '=')
		return HTTP_RANGE_NONE;
	at += 6;

	while (*at == ' ' || *at == '\t')
		at++;

	/* --- bytes=-N, the last N bytes ------------------------------------- */
	if (*at == '-') {
		at++;
		nl = digits(at, &last);
		if (nl <= 0)
			return HTTP_RANGE_NONE;	/* `bytes=-` is not a range */
		at += nl;

		/* A list. Refused by serving everything -- see `range.h`. */
		if (*at == ',')
			return HTTP_RANGE_NONE;
		while (*at == ' ' || *at == '\t')
			at++;
		if (*at != '\0')
			return HTTP_RANGE_NONE;

		/* `bytes=-0` asks for the last zero bytes. It is syntactically
		 * fine and cannot be satisfied, which is exactly what 416 is
		 * for. */
		if (last == 0)
			return HTTP_RANGE_UNSATISFIABLE;

		/* An empty file has no last byte either. */
		if (length == 0)
			return HTTP_RANGE_UNSATISFIABLE;

		if (last >= length) {
			/* More than there is: the whole file, which is a
			 * satisfiable answer to "the last N bytes". */
			into->first = 0;
			into->last = length - 1;
			return HTTP_RANGE_OK;
		}

		into->first = length - last;
		into->last = length - 1;
		return HTTP_RANGE_OK;
	}

	/* --- bytes=N- and bytes=N-M ----------------------------------------- */
	nf = digits(at, &first);
	if (nf <= 0)
		return HTTP_RANGE_NONE;
	at += nf;

	if (*at != '-')
		return HTTP_RANGE_NONE;
	at++;

	if (length == 0)
		return HTTP_RANGE_UNSATISFIABLE;

	/*
	 * Whether an end was written matters, and getting this wrong is how the
	 * suite caught a real fault here.
	 *
	 * `bytes=N-` has no end, so one is synthesised as the last byte of the
	 * file. An earlier version synthesised it *before* checking the start,
	 * and then compared the two: for `bytes=5000-` on a 1000-byte file the
	 * synthesised end (999) came out below the start (5000), so it was
	 * reported as a **backwards range and ignored** -- and the client got
	 * the whole file where it should have got 416 and the real length.
	 *
	 * The start is checked against the file first now, and the backwards
	 * test applies only to an end the client actually wrote.
	 */
	{
		int explicit_end = 0;

		if (is_digit(*at)) {
			nl = digits(at, &last);
			if (nl < 0)
				return HTTP_RANGE_NONE;
			at += nl;
			explicit_end = 1;
		}

		if (*at == ',')
			return HTTP_RANGE_NONE;		/* a list */
		while (*at == ' ' || *at == '\t')
			at++;
		if (*at != '\0')
			return HTTP_RANGE_NONE;

		/* Past the end. See the file header for why this one is 416
		 * and an *end* past the file is not. */
		if (first >= length)
			return HTTP_RANGE_UNSATISFIABLE;

		if (!explicit_end) {
			last = length - 1;
		} else if (last < first) {
			/* Backwards, and not silently swapped: a client that
			 * sent `bytes=500-100` has a bug, and serving it bytes
			 * 100-500 would hide the bug and hand over something it
			 * did not ask for. */
			return HTTP_RANGE_NONE;
		}
	}

	if (last >= length)
		last = length - 1;

	into->first = first;
	into->last = last;
	return HTTP_RANGE_OK;
}

int http_if_range(const char *if_range, const char *etag)
{
	const char *at = if_range;
	const char *mine;
	size_t i = 0, mine_len = 0;

	/* No `If-Range` is not a mismatch. It means the client did not ask for
	 * the range to be conditional, so the range stands. */
	if (!if_range || !*if_range)
		return 1;

	if (!etag || !*etag)
		return 0;	/* nothing to compare against; send it all */

	while (*at == ' ' || *at == '\t')
		at++;

	/*
	 * A weak tag never satisfies `If-Range`, and this is the one place the
	 * comparison is strong rather than weak.
	 *
	 * The reason is what the header is for. A weak validator says "the same
	 * representation, near enough" -- fine for deciding whether to re-send
	 * a whole body, and not fine for stitching one half of a file onto
	 * another. Two representations that are weakly equal may differ byte
	 * for byte, which is precisely what a range depends on.
	 */
	if (at[0] == 'W' && at[1] == '/')
		return 0;

	/* Anything that is not a quoted tag -- a date, most likely. There is no
	 * clock here to compare a date against, so it is not a match, and the
	 * client gets the whole file. A wasted body rather than a corrupt one. */
	if (at[0] != '"')
		return 0;

	if (etag[0] != '"')
		return 0;
	mine = etag + 1;
	while (mine[mine_len] && mine[mine_len] != '"')
		mine_len++;
	if (mine[mine_len] != '"')
		return 0;

	at++;
	while (at[i] && at[i] != '"')
		i++;
	if (at[i] != '"')
		return 0;	/* unterminated */
	if (i != mine_len)
		return 0;

	{
		size_t k;

		for (k = 0; k < i; k++)
			if (at[k] != mine[k])
				return 0;
	}
	return 1;
}
