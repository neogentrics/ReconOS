/*
 * Reading a `multipart/form-data` body.
 *
 * See `multipart.h` for what is refused and why. This file is the mechanics.
 *
 * --- What was tried and rejected ---
 *
 * **Searching for `--<boundary>` and taking everything before it.** It is the
 * obvious implementation and it is wrong by exactly two bytes: the CRLF before
 * the delimiter belongs to the delimiter, not to the data. Two extra bytes on
 * the end of a form field is invisible; two extra bytes on the end of a zip is
 * a file that will not open. The search here is for `CRLF--<boundary>` and the
 * only delimiter without a leading CRLF is the first.
 *
 * **Reusing the header parser from `request.c`.** A part's headers look like a
 * request's and are not the same thing: a part has no request line, a far
 * smaller legal set, and `Content-Disposition` -- which a request never
 * carries -- is the only one that matters. Sharing would mean a flag, and the
 * flag would eventually be passed wrong in the direction that accepts more.
 *
 * **Stripping a filename down to its last segment.** That is what almost every
 * implementation does and it is a repair, so it has a fixed point: `....//`
 * reduced once is `../`, and a name that was refused becomes a name that
 * escapes. `multipart.h` sets this out. Refused, not repaired.
 *
 * **Accepting LF-only line endings inside part headers.** Tolerant here and
 * strict in `request.c` would mean the two disagree about where a header ends,
 * which is the shape of every smuggling bug in this server's history. Same
 * rule in both places: CRLF, or nothing.
 */

#include "multipart.h"

/* --- small helpers, deliberately not from the libc ---------------------------
 *
 * These run against bytes that may hold a NUL, so the string functions are the
 * wrong tools -- `strstr` on a body stops at the first NUL a client sent, and
 * the parse then silently covers less than the body. Everything below is
 * counted. */

static int same_n(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static size_t len_of(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

/* Case-insensitive for ASCII only. Header field names are ASCII by
 * definition, and a locale-aware fold is how `I` stops matching `i`. */
static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int same_ci(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (lower(a[i]) != lower(b[i]))
			return 0;
	return 1;
}

static int is_space(char c)
{
	return c == ' ' || c == '\t';
}

/*
 * Find `needle` (of `nlen` bytes) in `hay` (of `hlen`), from `from`.
 * Returns the offset, or `hlen` for not found -- a value a caller cannot
 * mistake for a position, since no position equals the length.
 */
static size_t find(const char *hay, size_t hlen, size_t from,
                   const char *needle, size_t nlen)
{
	size_t i;

	if (nlen == 0 || nlen > hlen)
		return hlen;
	for (i = from; i + nlen <= hlen; i++)
		if (same_n(hay + i, needle, nlen))
			return i;
	return hlen;
}

/* Copy `n` bytes and terminate, or refuse. Never truncates: see the header. */
static int take(char *dst, size_t room, const char *src, size_t n)
{
	size_t i;

	if (n + 1 > room)
		return HTTP_EFIELD_LONG;
	for (i = 0; i < n; i++)
		dst[i] = src[i];
	dst[n] = '\0';
	return HTTP_OK;
}

/* --- the boundary ---------------------------------------------------------- */

/*
 * RFC 2046's `bcharsnospace`, plus the space, which is legal inside a boundary
 * but not at its end. Listed rather than described, because "printable ASCII"
 * would admit the quote and the semicolon and those end the parameter.
 */
static int boundary_char(char c)
{
	if (c >= 'A' && c <= 'Z')
		return 1;
	if (c >= 'a' && c <= 'z')
		return 1;
	if (c >= '0' && c <= '9')
		return 1;
	switch (c) {
	case '\'': case '(': case ')': case '+': case '_': case ',':
	case '-':  case '.': case '/': case ':': case '=': case '?':
	case ' ':
		return 1;
	default:
		return 0;
	}
}

int http_multipart_boundary(const char *content_type, char *out, size_t room)
{
	static const char WANT[] = "multipart/form-data";
	const char *s = content_type;
	size_t n, at;
	int found = 0;
	size_t b_at = 0, b_len = 0;

	if (!content_type || !out || room == 0)
		return HTTP_EMALFORMED;

	n = len_of(s);

	/* The type itself. Case-insensitive, and anything else is not this
	 * format -- including `multipart/mixed`, which has the same shape and
	 * different rules about what the parts mean. */
	at = sizeof(WANT) - 1;
	if (n < at || !same_ci(s, WANT, at))
		return HTTP_EMALFORMED;

	/* Parameters. */
	while (at < n) {
		size_t name_at, name_len, val_at, val_len;
		int quoted = 0;

		while (at < n && (is_space(s[at]) || s[at] == ';'))
			at++;
		if (at >= n)
			break;

		name_at = at;
		while (at < n && s[at] != '=' && s[at] != ';')
			at++;
		name_len = at - name_at;
		while (name_len > 0 && is_space(s[name_at + name_len - 1]))
			name_len--;

		if (at >= n || s[at] != '=') {
			/* A parameter with no value. Skipped rather than
			 * refused: it is not this parameter, and refusing a
			 * whole request over a stray token in a header the
			 * caller does not otherwise care about would reject
			 * messages that are merely untidy. */
			continue;
		}
		at++;	/* past '=' */

		while (at < n && is_space(s[at]))
			at++;

		if (at < n && s[at] == '"') {
			quoted = 1;
			at++;
		}
		val_at = at;
		if (quoted) {
			while (at < n && s[at] != '"')
				at++;
			if (at >= n)
				return HTTP_EMALFORMED;	/* never closed */
			val_len = at - val_at;
			at++;	/* past the closing quote */
		} else {
			while (at < n && s[at] != ';' && !is_space(s[at]))
				at++;
			val_len = at - val_at;
		}

		if (name_len == sizeof("boundary") - 1
		    && same_ci(s + name_at, "boundary", name_len)) {
			/*
			 * Twice is a refusal even when the two agree.
			 *
			 * The request parser refuses two `Content-Length`
			 * headers on identical values for the same reason: the
			 * fault is not that readers disagree about which is
			 * right, it is that there is something to disagree
			 * about. A proxy taking the first and this taking the
			 * last would split the body differently, which is
			 * request smuggling with extra steps.
			 */
			if (found)
				return HTTP_EMALFORMED;
			found = 1;
			b_at = val_at;
			b_len = val_len;
		}
	}

	if (!found || b_len == 0)
		return HTTP_EMALFORMED;
	if (b_len > HTTP_BOUNDARY_MAX)
		return HTTP_EFIELD_LONG;

	/* A boundary ending in a space cannot be matched: the spec allows a
	 * space inside one and not at the end, and a sender that put one there
	 * has produced a delimiter nothing will ever equal. */
	if (s[b_at + b_len - 1] == ' ')
		return HTTP_EMALFORMED;

	for (at = 0; at < b_len; at++)
		if (!boundary_char(s[b_at + at]))
			return HTTP_EMALFORMED;

	return take(out, room, s + b_at, b_len);
}

/* --- a part's headers -------------------------------------------------------- */

/*
 * A filename is a label, not a path.
 *
 * Refused rather than repaired -- see `multipart.h`. The NUL matters as much as
 * the separators: a name checked as `a.txt\0.exe` and later used as a C string
 * is two different names, and every check above this line would have passed.
 */
static int filename_ok(const char *v, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (v[i] == '/' || v[i] == '\\' || v[i] == '\0')
			return 0;
		if (v[i] == '.' && i + 1 < len && v[i + 1] == '.')
			return 0;
	}
	return 1;
}

/*
 * One `name="value"` parameter out of a `Content-Disposition` value.
 *
 * Returns 1 and sets `*at`/`*vlen` when `want` is present; 0 when absent; -1
 * when it is present more than once, which is the same refusal the boundary
 * gets and for the same reason.
 */
static int disposition_param(const char *s, size_t n, const char *want,
                             size_t *at_out, size_t *len_out)
{
	size_t at = 0;
	size_t wlen = len_of(want);
	int found = 0;

	while (at < n) {
		size_t name_at, name_len, val_at, val_len;
		int quoted = 0;

		while (at < n && (is_space(s[at]) || s[at] == ';'))
			at++;
		if (at >= n)
			break;

		name_at = at;
		while (at < n && s[at] != '=' && s[at] != ';')
			at++;
		name_len = at - name_at;
		while (name_len > 0 && is_space(s[name_at + name_len - 1]))
			name_len--;

		if (at >= n || s[at] != '=')
			continue;
		at++;

		while (at < n && is_space(s[at]))
			at++;

		if (at < n && s[at] == '"') {
			quoted = 1;
			at++;
		}
		val_at = at;
		if (quoted) {
			/*
			 * No backslash unescaping.
			 *
			 * RFC 2616 allowed `\"` inside a quoted string and
			 * browsers do not send it -- they send the quote
			 * percent-encoded, or they send a name with no quote in
			 * it. Implementing the escape means this parser and the
			 * sender disagree about where the name ends whenever a
			 * name legitimately contains a backslash, which on
			 * Windows is most of a path. Not unescaping is the
			 * behaviour that matches what is actually sent.
			 */
			while (at < n && s[at] != '"')
				at++;
			if (at >= n)
				return -1;	/* never closed */
			val_len = at - val_at;
			at++;
		} else {
			while (at < n && s[at] != ';' && !is_space(s[at]))
				at++;
			val_len = at - val_at;
		}

		if (name_len == wlen && same_ci(s + name_at, want, wlen)) {
			if (found)
				return -1;
			found = 1;
			*at_out = val_at;
			*len_out = val_len;
		}
	}
	return found;
}

/* Is there a `filename*` here? Refused rather than half-read: see the header. */
static int has_extended_filename(const char *s, size_t n)
{
	size_t at = 0;

	while (at < n) {
		size_t name_at, name_len;

		while (at < n && (is_space(s[at]) || s[at] == ';'))
			at++;
		if (at >= n)
			break;

		name_at = at;
		while (at < n && s[at] != '=' && s[at] != ';')
			at++;
		name_len = at - name_at;
		while (name_len > 0 && is_space(s[name_at + name_len - 1]))
			name_len--;

		if (name_len == sizeof("filename*") - 1
		    && same_ci(s + name_at, "filename*", name_len))
			return 1;

		if (at < n && s[at] == '=') {
			/* Step over the value so a value containing `;` does
			 * not get read as another parameter name. */
			at++;
			while (at < n && is_space(s[at]))
				at++;
			if (at < n && s[at] == '"') {
				at++;
				while (at < n && s[at] != '"')
					at++;
				if (at < n)
					at++;
			} else {
				while (at < n && s[at] != ';')
					at++;
			}
		}
	}
	return 0;
}

/*
 * Read the headers at the top of one part.
 *
 * `head` is the bytes before the blank line. Fills the part's name, filename
 * and content type.
 */
static int part_headers(const char *head, size_t len, struct http_part *p)
{
	size_t at = 0;
	int saw_disposition = 0;
	int rc;

	p->name[0] = '\0';
	p->filename[0] = '\0';
	p->has_filename = 0;
	p->content_type[0] = '\0';

	while (at < len) {
		size_t line_at = at, line_len;
		size_t colon, vat, vlen;

		/*
		 * One line, CRLF-terminated. A bare CR or a bare LF is a
		 * refusal, exactly as in `request.c`: where two parsers
		 * disagree about what ends a line, one of them can be made to
		 * see a header the other does not.
		 */
		while (at < len && head[at] != '\r' && head[at] != '\n')
			at++;
		line_len = at - line_at;

		if (at >= len)
			return HTTP_EMALFORMED;	/* no terminator */
		if (head[at] == '\n')
			return HTTP_EMALFORMED;	/* bare LF */
		if (at + 1 >= len || head[at + 1] != '\n')
			return HTTP_EMALFORMED;	/* bare CR */
		at += 2;

		if (line_len == 0)
			continue;

		/*
		 * A line beginning with space is `obs-fold`, a continuation of
		 * the header above. Deprecated, and accepted by enough things
		 * that a folded header is a header two parsers read
		 * differently. Refused, as in `request.c`.
		 */
		if (is_space(head[line_at]))
			return HTTP_EMALFORMED;

		colon = line_at;
		while (colon < line_at + line_len && head[colon] != ':')
			colon++;
		if (colon >= line_at + line_len)
			return HTTP_EMALFORMED;	/* no colon */

		/* A space before the colon makes the field name ambiguous and
		 * is how a header gets smuggled past something that trims. */
		if (colon > line_at && is_space(head[colon - 1]))
			return HTTP_EMALFORMED;

		vat = colon + 1;
		while (vat < line_at + line_len && is_space(head[vat]))
			vat++;
		vlen = (line_at + line_len) - vat;
		while (vlen > 0 && is_space(head[vat + vlen - 1]))
			vlen--;

		if (colon - line_at == sizeof("content-disposition") - 1
		    && same_ci(head + line_at, "content-disposition",
		               colon - line_at)) {
			size_t pat, plen;
			int got;

			/* Twice, again. One part, one disposition. */
			if (saw_disposition)
				return HTTP_EMALFORMED;
			saw_disposition = 1;

			/* It must be `form-data`. `attachment` and `inline` are
			 * legal dispositions elsewhere and mean nothing here;
			 * accepting them would be accepting a part whose
			 * purpose this server has not established. */
			if (vlen < sizeof("form-data") - 1
			    || !same_ci(head + vat, "form-data",
			                sizeof("form-data") - 1))
				return HTTP_EMALFORMED;

			if (has_extended_filename(head + vat, vlen))
				return HTTP_EMALFORMED;

			got = disposition_param(head + vat, vlen, "name",
			                        &pat, &plen);
			if (got != 1)
				return HTTP_EMALFORMED;	/* absent, or twice */
			rc = take(p->name, sizeof(p->name),
			          head + vat + pat, plen);
			if (rc != HTTP_OK)
				return rc;
			if (plen == 0)
				return HTTP_EMALFORMED;	/* an empty name */

			got = disposition_param(head + vat, vlen, "filename",
			                        &pat, &plen);
			if (got < 0)
				return HTTP_EMALFORMED;	/* twice */
			if (got == 1) {
				if (!filename_ok(head + vat + pat, plen))
					return HTTP_EMALFORMED;
				rc = take(p->filename, sizeof(p->filename),
				          head + vat + pat, plen);
				if (rc != HTTP_OK)
					return rc;
				p->has_filename = 1;
			}
		} else if (colon - line_at == sizeof("content-type") - 1
		           && same_ci(head + line_at, "content-type",
		                      colon - line_at)) {
			rc = take(p->content_type, sizeof(p->content_type),
			          head + vat, vlen);
			if (rc != HTTP_OK)
				return rc;
		}
		/* Anything else in a part is ignored rather than refused. A
		 * part may legally carry headers this server has no use for,
		 * and none of them changes where the part ends -- which is the
		 * property that would make an unknown header dangerous. */
	}

	if (!saw_disposition)
		return HTTP_EMALFORMED;
	return HTTP_OK;
}

/* --- the body ----------------------------------------------------------------- */

int http_multipart_parse(const char *body, size_t len, const char *boundary,
                         struct http_multipart *into)
{
	char dash[HTTP_BOUNDARY_MAX + 8];	/* "\r\n--" + boundary */
	size_t blen, dlen;
	size_t at;
	int rc;

	if (!body || !boundary || !into)
		return HTTP_EMALFORMED;

	into->count = 0;

	blen = len_of(boundary);
	if (blen == 0)
		return HTTP_EMALFORMED;
	if (blen > HTTP_BOUNDARY_MAX)
		return HTTP_EFIELD_LONG;

	/*
	 * The delimiter, with its leading CRLF. See the top of this file: the
	 * CRLF ends the preceding part, and a search that leaves it on the data
	 * hands back a file two bytes too long.
	 */
	dash[0] = '\r';
	dash[1] = '\n';
	dash[2] = '-';
	dash[3] = '-';
	for (at = 0; at < blen; at++)
		dash[4 + at] = boundary[at];
	dlen = 4 + blen;

	/*
	 * The first delimiter has no part before it, so it has no leading CRLF
	 * either -- unless the sender wrote a preamble, which is legal and
	 * which nothing sends. Both shapes are accepted here: the opening
	 * delimiter is found by looking for the one at offset zero without the
	 * CRLF, and failing that by the ordinary search.
	 */
	if (len >= dlen - 2 && same_n(body, dash + 2, dlen - 2)) {
		at = dlen - 2;
	} else {
		at = find(body, len, 0, dash, dlen);
		if (at == len)
			return HTTP_EMALFORMED;	/* no delimiter at all */
		at += dlen;
	}

	for (;;) {
		size_t head_at, blank, next, part_len;

		/*
		 * What follows a delimiter is either `--` (the close) or CRLF
		 * (another part). Anything else -- including the end of the
		 * body -- is malformed, which is what catches a truncated
		 * upload: the bytes stop where a delimiter was expected.
		 */
		if (at + 2 <= len && body[at] == '-' && body[at + 1] == '-')
			return HTTP_OK;		/* closed properly */

		/* Transport padding: spaces and tabs are legal between the
		 * delimiter and its line ending. */
		while (at < len && is_space(body[at]))
			at++;

		if (at + 2 > len || body[at] != '\r' || body[at + 1] != '\n')
			return HTTP_EMALFORMED;
		at += 2;

		if (into->count >= HTTP_PARTS_MAX)
			return HTTP_ETOOMANY;

		/*
		 * The part's headers end at a blank line. Searched from `at`
		 * so a CRLFCRLF inside a previous part's data cannot be found
		 * again, and bounded by the next delimiter so a part with no
		 * blank line cannot borrow the one belonging to the part after
		 * it.
		 */
		head_at = at;
		next = find(body, len, at, dash, dlen);
		if (next == len)
			return HTTP_EMALFORMED;	/* never closes */

		blank = find(body, next, head_at, "\r\n\r\n", 4);
		if (blank == next)
			return HTTP_EMALFORMED;	/* no end of headers */

		rc = part_headers(body + head_at, (blank + 2) - head_at,
		                  &into->parts[into->count]);
		if (rc != HTTP_OK)
			return rc;

		part_len = next - (blank + 4);
		into->parts[into->count].data = body + blank + 4;
		into->parts[into->count].data_len = part_len;
		into->count++;

		at = next + dlen;
	}
}

/* --- looking a part up --------------------------------------------------------
 *
 * Deliberately the same shape and the same silence as `form.c`. See `form.h`
 * for the argument; repeating the rule in a second place with a second
 * behaviour is how a caller learns to trust one and get the other. */

size_t http_multipart_count(const struct http_multipart *m, const char *name)
{
	size_t i, n = 0, wlen;

	if (!m || !name)
		return 0;
	wlen = len_of(name);
	for (i = 0; i < m->count; i++)
		if (len_of(m->parts[i].name) == wlen
		    && same_n(m->parts[i].name, name, wlen))
			n++;
	return n;
}

const struct http_part *http_multipart_get(const struct http_multipart *m,
                                           const char *name)
{
	size_t i, wlen;

	if (http_multipart_count(m, name) != 1)
		return 0;
	wlen = len_of(name);
	for (i = 0; i < m->count; i++)
		if (len_of(m->parts[i].name) == wlen
		    && same_n(m->parts[i].name, name, wlen))
			return &m->parts[i];
	return 0;
}
