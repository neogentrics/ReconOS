/*
 * Reading one HTTP request, and refusing everything ambiguous.
 *
 * See `http.h` for why this file holds no sockets. The rule it follows, worth
 * repeating where the code is: **the largest class of HTTP vulnerability is
 * two implementations reading one request two different ways.** So this parser
 * never picks the more likely reading of an ambiguous message. It rejects it.
 *
 * --- What is deliberately not implemented, and why that is safer ---
 *
 * **`Transfer-Encoding` is accepted only as exactly `chunked`, and only when
 * nothing else frames the message.** It was refused outright until 0.26.0,
 * when `chunked.c` arrived to read one; what has not changed by a word is the
 * reason the refusals are there. A server that ignores a framing header while
 * something upstream honours it is the exact shape of request smuggling: the
 * proxy frames the body one way, the origin frames it another, and the tail of
 * one request becomes the head of the next.
 *
 * So: `chunked` alone, on HTTP/1.1, is read. `Transfer-Encoding` **with**
 * `Content-Length` is refused. Two `Transfer-Encoding` headers are refused. Any
 * coding this does not implement -- `gzip`, `deflate`, a list, `chunked`
 * appearing anywhere but alone -- is refused. And chunked on HTTP/1.0 is
 * refused, because the framing postdates it and a 1.0 client that sent it is
 * not a 1.0 client.
 *
 * **Line folding (`obs-fold`) is refused rather than unfolded.** A header
 * continued onto the next line was deprecated for the same reason -- two
 * parsers disagree about where the value ends.
 *
 * **Absolute-form targets are refused.** `GET http://elsewhere/ HTTP/1.1` is
 * legal to send to a proxy and this is not one. Accepting it would mean
 * deciding whether the authority names this machine, which is a decision with
 * a wrong answer.
 *
 * --- What was tried and rejected ---
 *
 * **Normalising a duplicated `Content-Length` by taking the first, or the
 * smallest.** Both are readings. RFC 7230 says such a message is invalid, and
 * every reading of an invalid message is somebody's desynchronisation. It is
 * refused.
 *
 * **Collapsing `//` and resolving `..` after opening the file.** The check has
 * to happen on the path the caller asked for, not on the one the filesystem
 * resolved, or a symlink decides the answer. `..` that would climb above the
 * root is refused here, before anything is opened.
 */

#include "http.h"

/* No <string.h> and no <ctype.h>: this file is built against the host's
 * library for its test and against ReconOS's on the machine, and `tolower` is
 * locale-dependent on a host. A field name that folds one way in the test and
 * another on the wire is a duplicate check that passes and a duplicate that
 * gets through. The helpers are three lines each. */

static int is_digit(char c)      { return c >= '0' && c <= '9'; }
static int is_upper(char c)      { return c >= 'A' && c <= 'Z'; }
static char lower(char c)        { return is_upper(c) ? (char)(c - 'A' + 'a') : c; }
static int is_ows(char c)        { return c == ' ' || c == '\t'; }

static int seq(const char *a, const char *b)
{
	size_t i = 0;
	while (a[i] && a[i] == b[i])
		i++;
	return a[i] == b[i];
}

/* Case-insensitive compare against an already-lower-case literal. */
static int seq_fold(const char *a, const char *lower_b)
{
	size_t i = 0;
	while (a[i] && lower_b[i] && lower(a[i]) == lower_b[i])
		i++;
	return lower(a[i]) == lower_b[i];
}

/*
 * The characters RFC 7230 allows in a field name or a method.
 *
 * Written out rather than expressed as "not a separator", because the
 * complement is the set that is easy to get wrong -- and a field name allowed
 * to carry a colon or a space is a field name that can forge a second header.
 */
static int is_token(char c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c))
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

/* A value may hold printable ASCII and horizontal tab. Nothing else.
 *
 * DEL and the C0 controls are excluded because a CR or LF smuggled into a
 * value is a forged header, and that is the whole attack. Bytes above 127 are
 * excluded because a field value is not text in any declared encoding, and
 * accepting them means deciding which one. */
static int is_value_char(unsigned char c)
{
	return (c >= 0x20 && c < 0x7F) || c == '\t';
}

/* --- percent-decoding -------------------------------------------------- */

static int hex_of(char c)
{
	if (is_digit(c))               return c - '0';
	if (c >= 'a' && c <= 'f')      return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')      return c - 'A' + 10;
	return -1;
}

/*
 * Decode `%XX` in place into `out`.
 *
 * Refuses `%` that is not followed by two hex digits rather than passing it
 * through -- "%" passed through is a different byte string from "%" rejected,
 * and something downstream will decode it later.
 *
 * Refuses `%00` specifically and loudly. A NUL inside a path truncates it in
 * any C interface it is later handed to, so `/safe.html%00.txt` passes an
 * extension check and opens something else. It is the oldest path trick there
 * is and it is still worth naming.
 */
static int decode(const char *in, size_t len, char *out, size_t room)
{
	size_t i = 0, o = 0;

	while (i < len) {
		char c = in[i];

		if (c == '%') {
			int hi, lo;

			/* Two hex digits must both be inside the target. The
			 * comparison is written as an addition on the left so
			 * it cannot underflow when `len` is 0 or 1. */
			if (i + 2 >= len)
				return HTTP_EMALFORMED;
			hi = hex_of(in[i + 1]);
			lo = hex_of(in[i + 2]);
			if (hi < 0 || lo < 0)
				return HTTP_EMALFORMED;
			c = (char)((hi << 4) | lo);
			if (c == '\0')
				return HTTP_EMALFORMED;
			i += 3;
		} else {
			/* A raw control character in a target is not a target.
			 * Space included: the request line is space-delimited,
			 * so a space here means the line was mis-split. */
			if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F
			    || c == ' ')
				return HTTP_EMALFORMED;
			i++;
		}

		if (o + 1 >= room)
			return HTTP_ELINE_LONG;
		out[o++] = c;
	}

	out[o] = '\0';
	return HTTP_OK;
}

/*
 * Resolve `.` and `..` against the root, refusing anything that climbs out.
 *
 * Operates on the already-decoded path, which is the point: `%2e%2e%2f` has
 * become `../` by the time it arrives here, and is refused as such. A
 * normaliser that ran before decoding would never see it.
 */
static int normalise(const char *path, char *out, size_t room)
{
	size_t i = 0, o = 0;

	if (path[0] != '/')
		return HTTP_EMALFORMED;

	while (path[i]) {
		size_t start, seg_len;

		while (path[i] == '/')
			i++;
		if (!path[i])
			break;

		start = i;
		while (path[i] && path[i] != '/')
			i++;
		seg_len = i - start;

		/* A backslash is refused rather than translated. This system's
		 * separator is '/', and a caller that sent '\' meant something
		 * by it -- most often that some other layer will translate it
		 * after this check has passed. */
		{
			size_t k;
			for (k = start; k < i; k++)
				if (path[k] == '\\')
					return HTTP_ETRAVERSAL;
		}

		if (seg_len == 1 && path[start] == '.')
			continue;

		if (seg_len == 2 && path[start] == '.' && path[start + 1] == '.') {
			/* Climb, refusing to climb past the root. Not clamped
			 * to the root: a request that tried to leave is a
			 * request to refuse, not one to quietly satisfy with a
			 * different file. */
			if (o == 0)
				return HTTP_ETRAVERSAL;
			while (o > 0 && out[o - 1] != '/')
				o--;
			if (o > 0)
				o--;	/* drop the '/' itself */
			continue;
		}

		if (o + 1 + seg_len + 1 > room)
			return HTTP_ELINE_LONG;
		out[o++] = '/';
		{
			size_t k;
			for (k = 0; k < seg_len; k++)
				out[o++] = path[start + k];
		}
	}

	if (o == 0) {
		if (room < 2)
			return HTTP_ELINE_LONG;
		out[o++] = '/';
	}
	out[o] = '\0';
	return HTTP_OK;
}

/* --- lines -------------------------------------------------------------- */

/*
 * Find the end of the line starting at `*at`.
 *
 * Accepts CRLF and a bare LF, and refuses a bare CR -- a CR that is not part
 * of a line ending is the byte used to forge one. `*len_out` is the line
 * without its terminator; `*at` moves past it.
 *
 * Returns HTTP_PARTIAL when no terminator has arrived yet.
 */
static int next_line(const char *buf, size_t len, size_t *at,
                     const char **line, size_t *line_len)
{
	size_t i = *at, start = *at;

	while (i < len) {
		char c = buf[i];

		if (c == '\0')
			return HTTP_EMALFORMED;
		if (c == '\n') {
			size_t end = i;

			if (end > start && buf[end - 1] == '\r')
				end--;
			*line = buf + start;
			*line_len = end - start;
			*at = i + 1;
			return HTTP_OK;
		}
		if (c == '\r') {
			/* Legal only immediately before the LF handled above. */
			if (i + 1 >= len)
				return HTTP_PARTIAL;
			if (buf[i + 1] != '\n')
				return HTTP_EMALFORMED;
		}
		i++;
	}
	return HTTP_PARTIAL;
}

/* --- the request line --------------------------------------------------- */

static int parse_request_line(const char *line, size_t len,
                              struct http_request *r)
{
	size_t i = 0, start;
	size_t mlen, tlen;
	const char *target;
	char decoded[HTTP_TARGET_MAX];
	size_t qat;
	int rc;

	/* method */
	start = 0;
	while (i < len && line[i] != ' ')
		i++;
	mlen = i - start;
	if (mlen == 0 || i >= len)
		return HTTP_EMALFORMED;
	if (mlen >= HTTP_METHOD_MAX)
		return HTTP_EMETHOD;
	{
		size_t k;
		for (k = 0; k < mlen; k++) {
			if (!is_token(line[k]))
				return HTTP_EMALFORMED;
			r->method[k] = line[k];
		}
		r->method[mlen] = '\0';
	}
	i++;	/* the single space */

	/* target */
	start = i;
	while (i < len && line[i] != ' ')
		i++;
	tlen = i - start;
	if (tlen == 0 || i >= len)
		return HTTP_EMALFORMED;
	target = line + start;
	i++;

	/* version -- exactly "HTTP/1.0" or "HTTP/1.1", and the rest of the
	 * line must be the version and nothing else. A trailing field would
	 * mean the line was split somewhere other than where it looks. */
	{
		size_t vlen = len - i;
		const char *v = line + i;

		if (vlen != 8)
			return HTTP_EMALFORMED;
		if (!(v[0] == 'H' && v[1] == 'T' && v[2] == 'T' && v[3] == 'P'
		      && v[4] == '/' && is_digit(v[5]) && v[6] == '.'
		      && is_digit(v[7])))
			return HTTP_EMALFORMED;
		if (v[5] != '1')
			return HTTP_EVERSION;
		if (v[7] != '0' && v[7] != '1')
			return HTTP_EVERSION;
		r->minor = v[7] - '0';
	}

	/* Origin-form only. See the file header for why absolute-form is
	 * refused rather than accepted and checked. */
	if (target[0] != '/')
		return HTTP_EMALFORMED;

	/* Split the query off before decoding. The query is kept raw: '&' and
	 * '=' inside a decoded value are indistinguishable from the separators,
	 * so decoding here would destroy the structure a later reader needs. */
	qat = 0;
	while (qat < tlen && target[qat] != '?')
		qat++;

	if (qat < tlen) {
		size_t qlen = tlen - qat - 1;
		size_t k;

		if (qlen >= HTTP_QUERY_MAX)
			return HTTP_ELINE_LONG;
		for (k = 0; k < qlen; k++) {
			unsigned char c = (unsigned char)target[qat + 1 + k];

			if (c < 0x20 || c == 0x7F || c == ' ')
				return HTTP_EMALFORMED;
			r->query[k] = (char)c;
		}
		r->query[qlen] = '\0';
	} else {
		r->query[0] = '\0';
	}

	rc = decode(target, qat, decoded, sizeof(decoded));
	if (rc != HTTP_OK)
		return rc;

	return normalise(decoded, r->target, HTTP_TARGET_MAX);
}

/* --- headers ------------------------------------------------------------ */

static int parse_header(const char *line, size_t len, struct http_request *r)
{
	size_t i = 0, nlen, vstart, vend;
	struct http_header *h;

	/* obs-fold: a header continued onto this line. Refused, not unfolded. */
	if (len > 0 && is_ows(line[0]))
		return HTTP_EMALFORMED;

	while (i < len && line[i] != ':')
		i++;
	if (i >= len)
		return HTTP_EMALFORMED;	/* no colon: not a header */
	nlen = i;
	if (nlen == 0)
		return HTTP_EMALFORMED;

	/* No space is permitted between the name and the colon. RFC 7230 is
	 * explicit, and the reason is concrete: something that strips the
	 * space sees a header that something else does not. */
	{
		size_t k;
		for (k = 0; k < nlen; k++)
			if (!is_token(line[k]))
				return HTTP_EMALFORMED;
	}
	if (nlen >= HTTP_NAME_MAX)
		return HTTP_EFIELD_LONG;

	i++;	/* the colon */

	vstart = i;
	while (vstart < len && is_ows(line[vstart]))
		vstart++;
	vend = len;
	while (vend > vstart && is_ows(line[vend - 1]))
		vend--;

	if (vend - vstart >= HTTP_VALUE_MAX)
		return HTTP_EFIELD_LONG;

	{
		size_t k;
		for (k = vstart; k < vend; k++)
			if (!is_value_char((unsigned char)line[k]))
				return HTTP_EMALFORMED;
	}

	if (r->header_count >= HTTP_HEADERS_MAX)
		return HTTP_ETOOMANY;

	h = &r->headers[r->header_count];
	{
		size_t k;
		for (k = 0; k < nlen; k++)
			h->name[k] = lower(line[k]);
		h->name[nlen] = '\0';
		for (k = vstart; k < vend; k++)
			h->value[k - vstart] = line[k];
		h->value[vend - vstart] = '\0';
	}
	r->header_count++;
	return HTTP_OK;
}

/* --- framing ------------------------------------------------------------ */

static int parse_length(const char *v, unsigned long *into)
{
	size_t i = 0;
	unsigned long n = 0;

	if (!v[0])
		return HTTP_EMALFORMED;

	/* Digits only. No sign, no whitespace, no `0x`, and no trailing
	 * anything -- `strtoul` would accept a leading '+' and stop at the
	 * first non-digit, reporting success on "5abc" and on " 5". Both of
	 * those are messages that something else may frame differently. */
	while (v[i]) {
		unsigned d;

		if (!is_digit(v[i]))
			return HTTP_EMALFORMED;
		d = (unsigned)(v[i] - '0');
		if (n > (0xFFFFFFFFUL - d) / 10UL)
			return HTTP_EBODY_LONG;
		n = n * 10UL + d;
		i++;
	}

	*into = n;
	return HTTP_OK;
}

/*
 * Is this value exactly the token `chunked`, and nothing else?
 *
 * Deliberately not a list parser. `gzip, chunked` is legal and this server
 * does not implement `gzip`, so the only honest answer to it is a refusal --
 * and a function that walked the list would be a function that could be made
 * to accept `chunked` in a position that means something different to the
 * proxy in front of it. One token, compared whole.
 *
 * Case-insensitive, because a transfer coding is a token and tokens are.
 * Surrounding whitespace has already been trimmed by the header parser.
 */
static int is_chunked(const char *value)
{
	static const char WORD[] = "chunked";
	size_t i;

	for (i = 0; WORD[i]; i++) {
		char c = value[i];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c + 32);
		if (c != WORD[i])
			return 0;
	}
	return value[i] == '\0';
}

static int frame(struct http_request *r)
{
	size_t i;
	int seen_length = 0;
	int seen_host = 0;
	int seen_coding = 0;

	r->has_length = 0;
	r->content_length = 0;
	r->chunked = 0;

	for (i = 0; i < r->header_count; i++) {
		const char *n = r->headers[i].name;

		/*
		 * `chunked`, alone, or nothing. See the file header.
		 *
		 * A second one is refused for the same reason a second
		 * `Content-Length` is, and more sharply: `Transfer-Encoding:
		 * chunked` twice is a published smuggling vector, because a
		 * reader that takes the last sees chunked framing and one that
		 * refuses the duplicate does not.
		 */
		if (seq(n, "transfer-encoding")) {
			if (seen_coding)
				return HTTP_ESMUGGLE;
			seen_coding = 1;
			if (!is_chunked(r->headers[i].value))
				return HTTP_ESMUGGLE;
			r->chunked = 1;
		}

		if (seq(n, "content-length")) {
			unsigned long v;
			int rc;

			/* A second one, whatever it says. Two that agree are
			 * still two, and the agreement is not the property
			 * that makes the message safe -- being unambiguous
			 * is, and it already is not. */
			if (seen_length)
				return HTTP_ESMUGGLE;
			seen_length = 1;

			rc = parse_length(r->headers[i].value, &v);
			if (rc != HTTP_OK)
				return rc;
			if (v > HTTP_BODY_MAX)
				return HTTP_EBODY_LONG;
			r->content_length = v;
			r->has_length = 1;
		}

		/*
		 * `Host`, and the same rule for the same reason.
		 *
		 * **Two of these is the fault above wearing a different
		 * name.** A proxy taking the first and this server taking the
		 * last are two readers disagreeing about which machine a
		 * request was for -- which is how a cache is poisoned and how
		 * a password-reset link is made to point somewhere else. The
		 * parser refused two `Content-Length` headers from the day it
		 * was written and accepted two `Host` headers until 0.24.0,
		 * with the argument for refusing them sitting six lines above.
		 */
		if (seq(n, "host")) {
			if (seen_host)
				return HTTP_ESMUGGLE;
			seen_host = 1;
		}
	}

	/*
	 * HTTP/1.1 requires one. RFC 7230 puts it as plainly as that
	 * specification ever puts anything: a server **must** answer 400 to a
	 * 1.1 request without it.
	 *
	 * Not required of HTTP/1.0, which predates the field -- and a 1.0
	 * client is not asking to be routed by name, so nothing downstream
	 * needs it.
	 */
	if (r->minor >= 1 && !seen_host)
		return HTTP_EMALFORMED;

	/*
	 * Framed twice, which is the fault this whole function exists for.
	 *
	 * Checked after the loop rather than inside it, because the two
	 * headers may arrive in either order and a check that fired on the
	 * second would let the pair through whenever the client chose the
	 * other order. That is not a hypothetical: sending them in an
	 * unexpected order is the first thing a smuggling tool tries.
	 */
	if (r->chunked && r->has_length)
		return HTTP_ESMUGGLE;

	/*
	 * Chunked postdates HTTP/1.0.
	 *
	 * A 1.0 message carrying it is either a client that does not know what
	 * version it is speaking or something deliberately shaped so that a
	 * 1.1 reader frames it one way and a 1.0 reader frames it another.
	 * There is no reading of it that both would agree on.
	 */
	if (r->chunked && r->minor < 1)
		return HTTP_ESMUGGLE;

	return HTTP_OK;
}

static void decide_keep_alive(struct http_request *r)
{
	const char *c = http_header_get(r, "connection");

	/* 1.1 keeps the connection by default and 1.0 does not, which is the
	 * one place this parser reads a default rather than refusing -- both
	 * versions define it, so there is no ambiguity to refuse. */
	r->keep_alive = (r->minor >= 1);

	if (!c)
		return;
	if (seq_fold(c, "close"))
		r->keep_alive = 0;
	else if (seq_fold(c, "keep-alive"))
		r->keep_alive = 1;
}

/* --- the whole thing ---------------------------------------------------- */

int http_request_parse(const char *buf, size_t len, struct http_request *into)
{
	size_t at = 0;
	const char *line;
	size_t line_len;
	int rc;

	if (!buf || !into)
		return HTTP_EMALFORMED;

	into->method[0] = '\0';
	into->target[0] = '\0';
	into->query[0] = '\0';
	into->header_count = 0;
	into->content_length = 0;
	into->has_length = 0;
	into->head_length = 0;
	into->minor = 1;
	into->keep_alive = 0;

	if (len > HTTP_REQUEST_MAX)
		return HTTP_ELINE_LONG;

	rc = next_line(buf, len, &at, &line, &line_len);
	if (rc != HTTP_OK)
		return rc;

	/* An empty first line is permitted once, because 1.0 clients were
	 * allowed to leave a CRLF behind. A second one is not a request. */
	if (line_len == 0) {
		rc = next_line(buf, len, &at, &line, &line_len);
		if (rc != HTTP_OK)
			return rc;
	}

	rc = parse_request_line(line, line_len, into);
	if (rc != HTTP_OK)
		return rc;

	for (;;) {
		rc = next_line(buf, len, &at, &line, &line_len);
		if (rc != HTTP_OK)
			return rc;
		if (line_len == 0)
			break;		/* the blank line ends the head */

		rc = parse_header(line, line_len, into);
		if (rc != HTTP_OK)
			return rc;
	}

	rc = frame(into);
	if (rc != HTTP_OK)
		return rc;

	/*
	 * A request with a body and **no framing at all**.
	 *
	 * Not the same as no `Content-Length`, which is what this said until
	 * 0.26.0 and was correct while chunked was refused: a chunked POST has
	 * no length and is framed perfectly well. The condition is *neither*
	 * framing, and the reason it is refused has not changed -- this server
	 * does not read a body to end-of-connection, because that is how a
	 * POST becomes half of the next request.
	 */
	if (!into->has_length && !into->chunked
	    && (seq(into->method, "POST") || seq(into->method, "PUT")))
		return HTTP_EMALFORMED;

	/* The methods this server implements. Everything else is 501, which is
	 * the honest answer -- not 400, which would claim the client erred. */
	if (!seq(into->method, "GET") && !seq(into->method, "HEAD")
	    && !seq(into->method, "POST"))
		return HTTP_EMETHOD;

	decide_keep_alive(into);
	into->head_length = at;
	return HTTP_OK;
}

const char *http_header_get(const struct http_request *r, const char *name)
{
	size_t i;

	if (!r || !name)
		return 0;
	for (i = 0; i < r->header_count; i++)
		if (seq(r->headers[i].name, name))
			return r->headers[i].value;
	return 0;
}

int http_status_for(int verdict)
{
	switch (verdict) {
	case HTTP_OK:
	case HTTP_PARTIAL:      return 0;
	case HTTP_ELINE_LONG:   return 414;
	case HTTP_EFIELD_LONG:
	case HTTP_ETOOMANY:     return 431;
	case HTTP_EMETHOD:      return 501;
	case HTTP_EVERSION:     return 505;
	case HTTP_EBODY_LONG:   return 413;
	/* Named rather than left to the default, which is 400. See `http.h`:
	 * this is the one verdict that is not the client's fault, and the
	 * default would have said it was. */
	case HTTP_EINTERNAL:    return 500;
	case HTTP_EMALFORMED:
	case HTTP_ESMUGGLE:
	case HTTP_ETRAVERSAL:
	default:                return 400;
	}
}

const char *http_reason(int status)
{
	/* Built from the one table in `http.h`. See the comment there: this
	 * function and the suite each kept their own list once, and the two
	 * drifted twice -- `304 Unknown`, then `206 Unknown` and `416 Unknown`
	 * on the same day a case had been added for every status then known. */
#define RECON_STATUS_CASE(code, phrase) case code: return phrase;
	switch (status) {
	HTTP_STATUSES(RECON_STATUS_CASE)
	default:  return "Unknown";
	}
#undef RECON_STATUS_CASE
}

/* --- which machine a request was for ---------------------------------------
 *
 * See `http.h` for the rule. The work here is deciding where a name ends,
 * which is the whole of it: a port, a trailing dot, and the brackets that
 * protect an address made of colons.
 */

/* How much of `host` is the name, ignoring any `:port` after it. */
static size_t host_name_length(const char *host)
{
	size_t i = 0;

	/*
	 * A bracketed literal. Everything up to and including `]` is the name,
	 * colons and all -- `[::1]:80` is one address and a port, and cutting
	 * at the first colon would leave `[`.
	 */
	if (host[0] == '[') {
		while (host[i] && host[i] != ']')
			i++;
		if (host[i] == ']')
			i++;
		return i;
	}

	while (host[i] && host[i] != ':')
		i++;
	return i;
}

int http_host_matches(const char *pattern, const char *header)
{
	size_t plen, hlen, i;

	if (!pattern || !header || !pattern[0] || !header[0])
		return 0;

	plen = host_name_length(pattern);
	hlen = host_name_length(header);

	/*
	 * A single trailing dot is the root form of the same name:
	 * `example.com.` and `example.com` are one host. Taken off both sides
	 * so a configured name and an arriving one need not agree about it.
	 */
	if (plen > 1 && pattern[plen - 1] == '.')
		plen--;
	if (hlen > 1 && header[hlen - 1] == '.')
		hlen--;

	if (plen == 0 || plen != hlen)
		return 0;

	for (i = 0; i < plen; i++)
		if (lower(pattern[i]) != lower(header[i]))
			return 0;

	return 1;
}
