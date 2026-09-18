/*
 * Reading `Accept`, and choosing from it.
 *
 * See `accept.h` for what this decides and for the one place it is deliberately
 * quiet -- parameters that are not `q`, whose shape is checked and whose
 * meaning is ignored, because refusing them would answer 400 to every request a
 * browser makes.
 *
 * --- What was tried and rejected ---
 *
 * **Looking for the offered type as a substring of the header.** It is how this
 * is usually done and it is wrong in both directions: `application/json` is a
 * substring of `application/jsonrequest`, so a client that will not take JSON
 * gets it; and a client sending a full wildcard gets nothing, because the string is
 * not there. Both failures are silent.
 *
 * **Taking the first range that matches.** Ranges are not in preference order
 * and never have been -- `q` is what orders them, and Chrome's own header is
 * deliberately not sorted. A first-match parser gives `text/html` to a client
 * that said `text/html;q=0.1, application/json`.
 *
 * **Treating `q=0` as merely low.** It means *not acceptable*, and it is the
 * only way a client can say "anything but this". A parser that reads it as a
 * small preference sends exactly the thing that was excluded.
 *
 * --- The quality value, without floating point ---
 *
 * This machine has no floating-point unit; see `jsonread.h`. A `q` is
 * therefore read as **thousandths, as an integer**: `q=1` is 1000, `q=0.8` is
 * 800, `q=0.123` is 123. The grammar allows at most three digits after the
 * point, so this is exact rather than an approximation of the real thing --
 * which is the same reason the specification wrote it that way.
 */

#include "accept.h"

/* --- bytes ----------------------------------------------------------------- */

static int is_space(char c)
{
	return c == ' ' || c == '\t';
}

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/*
 * The token characters of RFC 9110, which is what a media type and a parameter
 * name are made of. Written out rather than approximated by "not a separator":
 * the two differ on bytes like `\` and `"`, and a parser that admits those in
 * a type is one that can be handed a type containing a quote.
 */
static int is_token(char c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
	    || (c >= '0' && c <= '9'))
		return 1;
	switch (c) {
	case '!': case '#': case '$': case '%': case '&': case '\'':
	case '*': case '+': case '-': case '.': case '^': case '_':
	case '`': case '|': case '~':
		return 1;
	default:
		return 0;
	}
}

/* One media range, as read. */
struct range {
	char type[ACCEPT_TOKEN_MAX];
	char sub[ACCEPT_TOKEN_MAX];
	int  weight;		/* thousandths; 1000 unless `q` said otherwise */
};

/* Copy a token into a bounded buffer. Returns 0 if it does not fit or is
 * empty -- never a truncation, because a truncated media type is a different
 * media type. */
static int take_token(const char *from, size_t len, char *out, size_t room)
{
	size_t i;

	if (len == 0 || len + 1 > room)
		return 0;
	for (i = 0; i < len; i++)
		out[i] = lower(from[i]);
	out[len] = '\0';
	return 1;
}

int http_accept_matches(const char *range_type, const char *range_sub,
                        const char *type)
{
	size_t i = 0;
	const char *slash;
	int star_type = range_type[0] == '*' && range_type[1] == '\0';
	int star_sub = range_sub[0] == '*' && range_sub[1] == '\0';

	/* Split the offered type at its slash. An offer with no slash is the
	 * server's own mistake rather than a client's, and matching nothing is
	 * the safe answer to it. */
	for (slash = type; *slash && *slash != '/'; slash++)
		;
	if (*slash != '/')
		return 0;

	if (star_type)
		return star_sub ? 1 : 0;	/* a full wildcard; a star type
						 * with a real subtype is not a
						 * thing and matches nothing */

	/* The type must be equal. */
	for (i = 0; range_type[i]; i++) {
		if (type + i >= slash || lower(type[i]) != range_type[i])
			return 0;
	}
	if (type + i != slash)
		return 0;

	if (star_sub)
		return 2;

	/* And the subtype. */
	{
		const char *sub = slash + 1;

		for (i = 0; range_sub[i]; i++) {
			if (sub[i] == '\0' || lower(sub[i]) != range_sub[i])
				return 0;
		}
		return sub[i] == '\0' ? 3 : 0;
	}
}

/*
 * A quality value, as thousandths.
 *
 * `0`, `1`, `0.5`, `0.25`, `1.000`. Refuses `2`, `1.5`, `.5`, `1.0000`, an
 * empty value and anything with a sign -- each of which some parser accepts,
 * and each of which would give a different ordering to two servers reading one
 * header.
 */
static int read_q(const char **at, const char *end, int *out)
{
	const char *p = *at;
	int whole;
	int value;
	int digits = 0;

	if (p >= end || (*p != '0' && *p != '1'))
		return 0;
	whole = *p - '0';
	p++;

	value = whole * 1000;
	if (p < end && *p == '.') {
		int scale = 100;

		p++;
		while (p < end && is_digit(*p)) {
			if (digits >= 3)
				return 0;
			value += (*p - '0') * scale;
			scale /= 10;
			digits++;
			p++;
		}
		/* A point with no digits after it. */
		if (digits == 0)
			return 0;
	}
	if (value > 1000)
		return 0;		/* `1.5`, and anything past the top */

	*out = value;
	*at = p;
	return 1;
}

/* Skip a parameter's value: a token, or a quoted string. The shape is checked;
 * see `accept.h` for why the meaning is not. */
static int skip_value(const char **at, const char *end)
{
	const char *p = *at;

	if (p < end && *p == '"') {
		p++;
		while (p < end && *p != '"') {
			if (*p == '\\') {
				p++;		/* the escaped byte */
				if (p >= end)
					return 0;
			}
			p++;
		}
		if (p >= end)
			return 0;		/* unterminated */
		p++;
		*at = p;
		return 1;
	}

	while (p < end && is_token(*p))
		p++;
	if (p == *at)
		return 0;			/* a parameter with no value */
	*at = p;
	return 1;
}

/* Parse the header into `into`. Returns how many ranges, or negative. */
static int read_header(const char *header, struct range *into, size_t room)
{
	const char *p = header;
	const char *end = header;
	size_t count = 0;

	while (*end)
		end++;

	for (;;) {
		const char *start;
		size_t len;
		struct range *r;

		while (p < end && (is_space(*p) || *p == ','))
			p++;
		if (p >= end)
			break;

		if (count >= room)
			return ACCEPT_EMALFORMED;
		r = &into[count];
		r->weight = 1000;

		/* type */
		start = p;
		while (p < end && (is_token(*p) || *p == '*'))
			p++;
		len = (size_t)(p - start);
		if (!take_token(start, len, r->type, sizeof(r->type)))
			return ACCEPT_EMALFORMED;
		if (p >= end || *p != '/')
			return ACCEPT_EMALFORMED;
		p++;

		/* subtype */
		start = p;
		while (p < end && (is_token(*p) || *p == '*'))
			p++;
		len = (size_t)(p - start);
		if (!take_token(start, len, r->sub, sizeof(r->sub)))
			return ACCEPT_EMALFORMED;

		/*
		 * `*`/something.
		 *
		 * Refused rather than treated as `*` /`*`. A client cannot
		 * mean *any type, as long as the subtype is json*, so a header
		 * containing it is one the client did not intend -- and the
		 * two obvious repairs, treating it as everything or as
		 * nothing, are opposite readings of the same bytes.
		 */
		if (r->type[0] == '*' && r->type[1] == '\0'
		    && !(r->sub[0] == '*' && r->sub[1] == '\0'))
			return ACCEPT_EMALFORMED;

		/* parameters */
		for (;;) {
			const char *name;
			size_t name_len;

			while (p < end && is_space(*p))
				p++;
			if (p >= end || *p != ';')
				break;
			p++;
			while (p < end && is_space(*p))
				p++;

			name = p;
			while (p < end && is_token(*p))
				p++;
			name_len = (size_t)(p - name);
			if (name_len == 0)
				return ACCEPT_EMALFORMED;
			while (p < end && is_space(*p))
				p++;
			if (p >= end || *p != '=')
				return ACCEPT_EMALFORMED;
			p++;
			while (p < end && is_space(*p))
				p++;

			if (name_len == 1 && lower(*name) == 'q') {
				if (!read_q(&p, end, &r->weight))
					return ACCEPT_EMALFORMED;
				continue;
			}
			if (!skip_value(&p, end))
				return ACCEPT_EMALFORMED;
		}

		count++;

		while (p < end && is_space(*p))
			p++;
		if (p >= end)
			break;
		if (*p != ',')
			return ACCEPT_EMALFORMED;
		p++;
	}

	if (count == 0)
		return ACCEPT_EMALFORMED;	/* a header with nothing in it */
	return (int)count;
}

int http_accept_pick(const char *header, const char *const *offers,
                     size_t count)
{
	struct range ranges[ACCEPT_RANGES_MAX];
	int n;
	size_t i;
	int best = ACCEPT_NONE;
	int best_weight = 0;

	if (!offers || count == 0)
		return ACCEPT_NONE;

	/*
	 * No header at all is not the same as an empty one.
	 *
	 * Absent means the client did not say, and RFC 9110 is explicit that
	 * this means anything is acceptable -- so the server's own first
	 * preference is sent. An empty header is a client that said something
	 * and said nothing, which is refused above.
	 */
	if (!header)
		return 0;

	n = read_header(header, ranges, ACCEPT_RANGES_MAX);
	if (n < 0)
		return ACCEPT_EMALFORMED;

	for (i = 0; i < count; i++) {
		int specificity = 0;
		int weight = 0;
		int k;

		/*
		 * The **most specific** matching range decides this offer, not
		 * the best-weighted one. `Accept:` a `text` wildcard at q=1 beside `text/html;q=0`
		 * says *anything textual except HTML*, and a parser that took
		 * the highest weight would send exactly the thing that was
		 * excluded.
		 */
		for (k = 0; k < n; k++) {
			int s = http_accept_matches(ranges[k].type,
			                            ranges[k].sub, offers[i]);

			if (s > specificity) {
				specificity = s;
				weight = ranges[k].weight;
			}
		}

		if (specificity == 0 || weight == 0)
			continue;	/* not matched, or refused outright */

		/* Strictly greater, so a tie is broken by the server's order:
		 * a client that says two things are equally good has said it
		 * does not mind, and the server does. */
		if (weight > best_weight) {
			best_weight = weight;
			best = (int)i;
		}
	}

	return best;
}

/*
 * `Accept-Encoding`, which is the same grammar with the slash taken out.
 *
 * Written as its own walk rather than by making `read_header` optional about
 * the slash. That function's whole value is that it refuses a media range that
 * is not one, and a flag telling it not to would be a flag somebody passes by
 * mistake -- the shape this project keeps naming, where one reader is asked to
 * behave as two.
 *
 * It is also much smaller, because the only question is about one coding.
 */
int http_accept_gzip(const char *header)
{
	const char *p = header;
	const char *end;
	int gzip_weight = -1;		/* -1: not mentioned */
	int star_weight = -1;

	/* See `accept.h`: absent means no, deliberately. */
	if (!header)
		return 0;

	for (end = header; *end; end++)
		;

	for (;;) {
		const char *start;
		size_t len;
		int weight = 1000;
		int is_gzip = 0, is_star = 0;

		while (p < end && (is_space(*p) || *p == ','))
			p++;
		if (p >= end)
			break;

		start = p;
		while (p < end && (is_token(*p) || *p == '*'))
			p++;
		len = (size_t)(p - start);
		if (len == 0)
			return 0;	/* not a coding: read nothing further */

		{
			static const char WORD[] = "gzip";
			size_t i;

			is_star = (len == 1 && start[0] == '*');
			if (len == sizeof(WORD) - 1) {
				is_gzip = 1;
				for (i = 0; i < len; i++) {
					if (lower(start[i]) != WORD[i])
						is_gzip = 0;
				}
			}
		}

		for (;;) {
			const char *name;
			size_t name_len;

			while (p < end && is_space(*p))
				p++;
			if (p >= end || *p != ';')
				break;
			p++;
			while (p < end && is_space(*p))
				p++;
			name = p;
			while (p < end && is_token(*p))
				p++;
			name_len = (size_t)(p - name);
			if (name_len == 0)
				return 0;
			while (p < end && is_space(*p))
				p++;
			if (p >= end || *p != '=')
				return 0;
			p++;
			while (p < end && is_space(*p))
				p++;
			if (name_len == 1 && lower(*name) == 'q') {
				if (!read_q(&p, end, &weight))
					return 0;
				continue;
			}
			if (!skip_value(&p, end))
				return 0;
		}

		if (is_gzip)
			gzip_weight = weight;
		else if (is_star)
			star_weight = weight;

		while (p < end && is_space(*p))
			p++;
		if (p >= end)
			break;
		if (*p != ',')
			return 0;
		p++;
	}

	/*
	 * Named beats the wildcard, the same rule the media ranges follow: a
	 * client writing `*, gzip;q=0` means *anything except gzip*, and a
	 * reader that took the wildcard would send the one thing excluded.
	 */
	if (gzip_weight >= 0)
		return gzip_weight > 0;
	return star_weight > 0;
}
