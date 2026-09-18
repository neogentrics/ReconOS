/*
 * The JSON reader.
 *
 * See `jsonread.h` for the rule and for the two narrowings that come from this
 * machine rather than from taste. This file is the parser and the refusals.
 *
 * --- What was tried and rejected ---
 *
 * **Recursive descent.** It is the natural shape for this grammar and it puts
 * the nesting bound on the C stack, where exceeding it is a fault of a kind
 * this system cannot report -- the init program has one stack and no handler
 * for running off it. The parser below keeps its own stack of open containers,
 * `JSON_DEPTH_MAX` entries, and a document deeper than that is refused with a
 * verdict rather than by falling over.
 *
 * **Keeping strings as offsets into the input and decoding on demand.** It
 * saves the pool, and it means every caller of `json_string` decodes again,
 * with the escape rules living in the accessor where two callers can disagree
 * about them. Decoded once, at parse time, into a pool the document owns.
 *
 * **Sorting an object's members so a duplicate key is cheap to find.** The
 * document is small and the members are few; the check below is a walk over
 * what has already been read, which is O(n^2) on a handful of fields and
 * exactly as correct on the first day as on the last.
 */

#include "jsonread.h"

/* --- bytes ----------------------------------------------------------------- */

static int is_space(char c)
{
	/* The four JSON allows, and no others. A vertical tab or a form feed
	 * between values is something some parsers permit; permitting it here
	 * would be this parser reading a document another one refuses. */
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int hex_of(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

/* --- the parser's own state ------------------------------------------------ */

struct reader {
	const char *text;
	size_t      len;
	size_t      at;
	struct json *doc;

	/* Open containers, innermost last. An index into `doc->nodes`. */
	int         stack[JSON_DEPTH_MAX];
	size_t      depth;
};

/*
 * Refuse, and leave nothing behind but the position.
 *
 * **The clear is the point**, and it is the same one `config.c` earns in its
 * own suite: everything parsed before the fault parsed perfectly, and a caller
 * that can see it is a caller that can use it. A handler reading the first two
 * fields of a document this server rejected is acting on half a message chosen
 * by whoever truncated it.
 *
 * `count` alone would do -- `json_root` answers NULL on zero -- but the pool
 * goes too, so there is no decoded text left for an accessor reached some
 * other way to hand back.
 */
static int fail(struct reader *r, int verdict)
{
	r->doc->count = 0;
	r->doc->pool_used = 0;
	r->doc->where = r->at;
	return verdict;
}

static void skip_space(struct reader *r)
{
	while (r->at < r->len && is_space(r->text[r->at]))
		r->at++;
}

/* Take a node. Returns its index, or -1 when there are no more. */
static int node_new(struct reader *r, int kind)
{
	struct json_node *n;
	int which;

	if (r->doc->count >= JSON_NODES_MAX)
		return -1;

	which = (int)r->doc->count++;
	n = &r->doc->nodes[which];
	n->kind = kind;
	n->at = 0;
	n->len = 0;
	n->name_at = 0;
	n->name_len = 0;
	n->first = -1;
	n->next = -1;
	return which;
}

/* Put a decoded byte in the pool. */
static int pool_put(struct reader *r, char c)
{
	if (r->doc->pool_used + 1 >= JSON_TEXT_MAX)
		return 0;
	r->doc->pool[r->doc->pool_used++] = c;
	return 1;
}

/*
 * Read a string, decoding it into the pool.
 *
 * `*at` and `*len` receive where it landed. The opening quote has already been
 * consumed by the caller, which is the only state this shares with it.
 */
static int read_string(struct reader *r, size_t *at, size_t *len)
{
	size_t start = r->doc->pool_used;

	for (;;) {
		char c;

		if (r->at >= r->len)
			return fail(r, JSON_EMALFORMED);	/* unterminated */
		c = r->text[r->at];

		if (c == '"') {
			r->at++;
			break;
		}

		/*
		 * A control byte, unescaped.
		 *
		 * RFC 8259 forbids anything below 0x20 inside a string, and a
		 * parser that allowed a raw newline or a raw NUL would accept
		 * a document that half the world refuses -- and one whose
		 * bytes end up in a log line this server writes.
		 */
		if ((unsigned char)c < 0x20)
			return fail(r, JSON_EMALFORMED);

		/* See `jsonread.h`: the writer cannot emit these, so the
		 * reader does not accept them. The two move together. */
		if ((unsigned char)c > 0x7F)
			return fail(r, JSON_ENONASCII);

		if (c != '\\') {
			if (!pool_put(r, c))
				return fail(r, JSON_EROOM);
			r->at++;
			continue;
		}

		/* An escape. */
		r->at++;
		if (r->at >= r->len)
			return fail(r, JSON_EMALFORMED);
		c = r->text[r->at];

		switch (c) {
		case '"':  case '\\': case '/':
			if (!pool_put(r, c))
				return fail(r, JSON_EROOM);
			r->at++;
			continue;
		case 'b': case 'f': case 'n': case 'r': case 't': {
			static const char FROM[] = "bfnrt";
			static const char TO[]   = "\b\f\n\r\t";
			size_t i;

			for (i = 0; FROM[i]; i++) {
				if (FROM[i] != c)
					continue;
				if (!pool_put(r, TO[i]))
					return fail(r, JSON_EROOM);
				break;
			}
			r->at++;
			continue;
		}
		case 'u': {
			int value = 0;
			int i;

			r->at++;
			for (i = 0; i < 4; i++) {
				int d;

				if (r->at >= r->len)
					return fail(r, JSON_EMALFORMED);
				d = hex_of(r->text[r->at]);
				if (d < 0)
					return fail(r, JSON_EMALFORMED);
				value = (value << 4) | d;
				r->at++;
			}
			/*
			 * Above ASCII, refused -- including a surrogate half.
			 *
			 * A lone surrogate is not a character and is the
			 * classic way to get two parsers to disagree: some
			 * pass it through, some substitute U+FFFD, some
			 * combine it with whatever follows. All three are
			 * readings. See `jsonread.h` for why even a valid
			 * character above 0x7F is refused here.
			 *
			 * `\u0000` is deliberately *not* refused: it is valid
			 * JSON, it decodes to a NUL, and `json_string` hands
			 * back a length as well as a terminator precisely so
			 * that a caller sees the whole string.
			 */
			if (value > 0x7F)
				return fail(r, JSON_ENONASCII);
			if (!pool_put(r, (char)value))
				return fail(r, JSON_EROOM);
			continue;
		}
		default:
			/* An escape this does not know. Not passed through as
			 * the character itself, which is what several parsers
			 * do and which makes `\z` mean `z` to them and a
			 * refusal here. */
			return fail(r, JSON_EMALFORMED);
		}
	}

	if (!pool_put(r, '\0'))
		return fail(r, JSON_EROOM);

	*at = start;
	*len = r->doc->pool_used - start - 1;	/* not the terminator */
	return JSON_OK;
}

/*
 * A number, checked against the grammar and kept as text.
 *
 * The grammar is narrow and every parser that widens it widens it differently:
 * `+1`, `01`, `.5`, `1.`, `0x10`, `NaN`, `Infinity`. Each of those is accepted
 * somewhere, and a document containing one means different things to the
 * sender and to this. None is accepted.
 */
static int read_number(struct reader *r, size_t *at, size_t *len)
{
	size_t start = r->at;

	if (r->at < r->len && r->text[r->at] == '-')
		r->at++;

	/* An integer part is required, and a leading zero may not be followed
	 * by another digit -- `01` is two tokens to some readers and one to
	 * others. */
	if (r->at >= r->len || !is_digit(r->text[r->at]))
		return fail(r, JSON_EMALFORMED);
	if (r->text[r->at] == '0') {
		r->at++;
		if (r->at < r->len && is_digit(r->text[r->at]))
			return fail(r, JSON_EMALFORMED);
	} else {
		while (r->at < r->len && is_digit(r->text[r->at]))
			r->at++;
	}

	/* A fraction, if there is one, must have a digit after the point. */
	if (r->at < r->len && r->text[r->at] == '.') {
		r->at++;
		if (r->at >= r->len || !is_digit(r->text[r->at]))
			return fail(r, JSON_EMALFORMED);
		while (r->at < r->len && is_digit(r->text[r->at]))
			r->at++;
	}

	/* And an exponent, likewise. */
	if (r->at < r->len && (r->text[r->at] == 'e' || r->text[r->at] == 'E')) {
		r->at++;
		if (r->at < r->len
		    && (r->text[r->at] == '+' || r->text[r->at] == '-'))
			r->at++;
		if (r->at >= r->len || !is_digit(r->text[r->at]))
			return fail(r, JSON_EMALFORMED);
		while (r->at < r->len && is_digit(r->text[r->at]))
			r->at++;
	}

	*at = start;
	*len = r->at - start;
	return JSON_OK;
}

/*
 * Is the parser sitting where a value has just ended?
 *
 * After a number or a bare word there must be whitespace, a comma, a closing
 * bracket, or the end of the input -- **nothing else**. Without this, `truex`
 * parses as `true` followed by trailing text and `1x` as `1` followed by
 * trailing text, and at the top level the verdict comes out as *more than one
 * document* for what is plainly one malformed value. Worse, inside an array a
 * lenient reader would take `[1x]` as `[1]`.
 */
static int at_delimiter(const struct reader *r)
{
	char c;

	if (r->at >= r->len)
		return 1;
	c = r->text[r->at];
	return is_space(c) || c == ',' || c == '}' || c == ']';
}

/* A bare word: `true`, `false` or `null`, and nothing else. */
static int word_is(struct reader *r, const char *word)
{
	size_t i;

	for (i = 0; word[i]; i++) {
		if (r->at + i >= r->len || r->text[r->at + i] != word[i])
			return 0;
	}
	r->at += i;
	return 1;
}

/* Attach `which` to the container on top of the stack, or make it the root. */
static void attach(struct reader *r, int which)
{
	struct json_node *parent;
	int last;

	if (r->depth == 0)
		return;			/* the root; nothing to attach to */

	parent = &r->doc->nodes[r->stack[r->depth - 1]];
	if (parent->first < 0) {
		parent->first = which;
		return;
	}
	last = parent->first;
	while (r->doc->nodes[last].next >= 0)
		last = r->doc->nodes[last].next;
	r->doc->nodes[last].next = which;
}

/*
 * Has this object already got this key?
 *
 * Compared over the decoded text, because `{"a":1,"a":2}` is the same key
 * twice and a check on the raw bytes would not see it. That is not a
 * hypothetical shape -- it is how a duplicate is hidden from a parser that
 * checks before decoding.
 */
static int already_has(struct reader *r, int object, size_t at, size_t len)
{
	int child = r->doc->nodes[object].first;

	while (child >= 0) {
		struct json_node *n = &r->doc->nodes[child];

		if (n->name_len == len) {
			size_t i;
			int same = 1;

			for (i = 0; i < len; i++) {
				if (r->doc->pool[n->name_at + i]
				    != r->doc->pool[at + i]) {
					same = 0;
					break;
				}
			}
			if (same)
				return 1;
		}
		child = n->next;
	}
	return 0;
}

int json_parse(const char *text, size_t len, struct json *into)
{
	struct reader r;
	size_t i;
	int expect_value = 1;	/* the document starts wanting one */
	int done = 0;

	if (!text || !into)
		return JSON_EMALFORMED;

	{
		char *raw = (char *)into;

		for (i = 0; i < sizeof(*into); i++)
			raw[i] = 0;
	}
	into->text = text;

	r.text = text;
	r.len = len;
	r.at = 0;
	r.doc = into;
	r.depth = 0;

	if (len > JSON_INPUT_MAX)
		return fail(&r, JSON_EINPUT);

	/*
	 * A NUL anywhere in the input.
	 *
	 * Checked up front rather than encountered, because the interesting
	 * case is a NUL in a place the parser would never look -- after the
	 * document, inside whitespace -- where it is invisible to this and
	 * meaningful to something else that reads the same bytes.
	 */
	for (i = 0; i < len; i++) {
		if (text[i] == '\0') {
			r.at = i;
			return fail(&r, JSON_EMALFORMED);
		}
	}

	skip_space(&r);
	if (r.at >= r.len)
		return fail(&r, JSON_EEMPTY);

	while (!done) {
		char c;
		int which;

		skip_space(&r);
		if (r.at >= r.len)
			return fail(&r, JSON_EMALFORMED);	/* unclosed */
		c = r.text[r.at];

		/* --- closing a container ----------------------------------- */
		if (!expect_value && (c == '}' || c == ']')) {
			struct json_node *open;

			if (r.depth == 0)
				return fail(&r, JSON_EMALFORMED);
			open = &r.doc->nodes[r.stack[r.depth - 1]];
			if ((c == '}') != (open->kind == JSON_OBJECT))
				return fail(&r, JSON_EMALFORMED);
			r.at++;
			r.depth--;
			if (r.depth == 0)
				done = 1;
			continue;
		}

		/* --- a comma, or the end of a container -------------------- */
		if (!expect_value) {
			if (c != ',')
				return fail(&r, JSON_EMALFORMED);
			if (r.depth == 0)
				return fail(&r, JSON_EMALFORMED);
			r.at++;
			expect_value = 1;
			skip_space(&r);
			/* A trailing comma. Legal in several languages, in no
			 * version of JSON, and accepted by enough parsers that
			 * a document containing one means two things. */
			if (r.at < r.len
			    && (r.text[r.at] == '}' || r.text[r.at] == ']'))
				return fail(&r, JSON_EMALFORMED);
			continue;
		}

		/* --- an empty container ------------------------------------ */
		if (r.depth > 0) {
			struct json_node *open =
				&r.doc->nodes[r.stack[r.depth - 1]];

			if (open->first < 0 && (c == '}' || c == ']')) {
				if ((c == '}') != (open->kind == JSON_OBJECT))
					return fail(&r, JSON_EMALFORMED);
				r.at++;
				r.depth--;
				expect_value = 0;
				if (r.depth == 0)
					done = 1;
				continue;
			}
		}

		/* --- a member's name, if this is an object ------------------ */
		{
			size_t name_at = 0, name_len = 0;
			int named = 0;

			if (r.depth > 0
			    && r.doc->nodes[r.stack[r.depth - 1]].kind
			       == JSON_OBJECT) {
				int rc;

				if (c != '"')
					return fail(&r, JSON_EMALFORMED);
				r.at++;
				rc = read_string(&r, &name_at, &name_len);
				if (rc != JSON_OK)
					return rc;

				if (already_has(&r, r.stack[r.depth - 1],
				                name_at, name_len))
					return fail(&r, JSON_EDUPLICATE);

				skip_space(&r);
				if (r.at >= r.len || r.text[r.at] != ':')
					return fail(&r, JSON_EMALFORMED);
				r.at++;
				skip_space(&r);
				if (r.at >= r.len)
					return fail(&r, JSON_EMALFORMED);
				c = r.text[r.at];
				named = 1;
			}

			/* --- the value itself ------------------------------ */
			if (c == '{' || c == '[') {
				which = node_new(&r, c == '{' ? JSON_OBJECT
				                              : JSON_ARRAY);
				if (which < 0)
					return fail(&r, JSON_ENODES);
				if (named) {
					r.doc->nodes[which].name_at = name_at;
					r.doc->nodes[which].name_len = name_len;
				}
				attach(&r, which);
				if (r.depth >= JSON_DEPTH_MAX)
					return fail(&r, JSON_EDEPTH);
				r.stack[r.depth++] = which;
				r.at++;
				expect_value = 1;
				continue;
			}

			if (c == '"') {
				size_t at = 0, vlen = 0;
				int rc;

				r.at++;
				rc = read_string(&r, &at, &vlen);
				if (rc != JSON_OK)
					return rc;
				which = node_new(&r, JSON_STRING);
				if (which < 0)
					return fail(&r, JSON_ENODES);
				r.doc->nodes[which].at = at;
				r.doc->nodes[which].len = vlen;
			} else if (c == '-' || is_digit(c)) {
				size_t at = 0, vlen = 0;
				int rc = read_number(&r, &at, &vlen);

				if (rc != JSON_OK)
					return rc;
				if (!at_delimiter(&r))
					return fail(&r, JSON_EMALFORMED);
				which = node_new(&r, JSON_NUMBER);
				if (which < 0)
					return fail(&r, JSON_ENODES);
				r.doc->nodes[which].at = at;
				r.doc->nodes[which].len = vlen;
			} else if (word_is(&r, "true")) {
				if (!at_delimiter(&r))
					return fail(&r, JSON_EMALFORMED);
				which = node_new(&r, JSON_TRUE);
				if (which < 0)
					return fail(&r, JSON_ENODES);
			} else if (word_is(&r, "false")) {
				if (!at_delimiter(&r))
					return fail(&r, JSON_EMALFORMED);
				which = node_new(&r, JSON_FALSE);
				if (which < 0)
					return fail(&r, JSON_ENODES);
			} else if (word_is(&r, "null")) {
				if (!at_delimiter(&r))
					return fail(&r, JSON_EMALFORMED);
				which = node_new(&r, JSON_NULL);
				if (which < 0)
					return fail(&r, JSON_ENODES);
			} else {
				return fail(&r, JSON_EMALFORMED);
			}

			if (named) {
				r.doc->nodes[which].name_at = name_at;
				r.doc->nodes[which].name_len = name_len;
			}
			attach(&r, which);
			expect_value = 0;
			if (r.depth == 0)
				done = 1;
		}
	}

	/*
	 * Nothing after the document but whitespace.
	 *
	 * A second value after the first is where a signed or logged request
	 * and the request that was acted on come apart: one reader takes the
	 * first document, one takes the last, and both think they read the
	 * message.
	 */
	skip_space(&r);
	if (r.at < r.len)
		return fail(&r, JSON_ETRAILING);

	into->where = 0;
	return JSON_OK;
}

/* --- reading what was parsed ----------------------------------------------- */

const char *json_reason(int verdict)
{
	switch (verdict) {
	case JSON_OK:         return "no fault";
	case JSON_EMALFORMED: return "not JSON";
	case JSON_EDEPTH:     return "nested deeper than this reads";
	case JSON_ENODES:     return "more values than this holds";
	case JSON_EROOM:      return "more text than this holds";
	case JSON_EDUPLICATE: return "the same key twice in one object";
	case JSON_ENONASCII:  return "a character this server cannot report back";
	case JSON_ETRAILING:  return "more than one document";
	case JSON_EEMPTY:     return "an empty document";
	case JSON_EINPUT:     return "a document too large to read";
	default:              return "unknown";
	}
}

const struct json_node *json_root(const struct json *doc)
{
	if (!doc || doc->count == 0)
		return 0;
	return &doc->nodes[0];
}

size_t json_count(const struct json *doc, const struct json_node *value)
{
	int child;
	size_t n = 0;

	if (!doc || !value)
		return 0;
	if (value->kind != JSON_OBJECT && value->kind != JSON_ARRAY)
		return 0;

	child = value->first;
	while (child >= 0) {
		n++;
		child = doc->nodes[child].next;
	}
	return n;
}

const struct json_node *json_member(const struct json *doc,
                                    const struct json_node *object,
                                    const char *name)
{
	int child;

	if (!doc || !object || !name || object->kind != JSON_OBJECT)
		return 0;

	child = object->first;
	while (child >= 0) {
		const struct json_node *n = &doc->nodes[child];
		size_t i;
		int same = 1;

		for (i = 0; i < n->name_len; i++) {
			if (name[i] == '\0'
			    || doc->pool[n->name_at + i] != name[i]) {
				same = 0;
				break;
			}
		}
		if (same && name[n->name_len] == '\0')
			return n;
		child = n->next;
	}
	return 0;
}

const struct json_node *json_at(const struct json *doc,
                                const struct json_node *array, size_t index)
{
	int child;

	if (!doc || !array || array->kind != JSON_ARRAY)
		return 0;

	child = array->first;
	while (child >= 0) {
		if (index == 0)
			return &doc->nodes[child];
		index--;
		child = doc->nodes[child].next;
	}
	return 0;
}

const char *json_string(const struct json *doc, const struct json_node *value,
                        size_t *len)
{
	if (!doc || !value || value->kind != JSON_STRING)
		return 0;
	if (len)
		*len = value->len;
	return doc->pool + value->at;
}

int json_int(const struct json *doc, const struct json_node *value, long *out)
{
	const char *t;
	size_t i;
	int negative = 0;
	unsigned long n = 0;

	if (!doc || !value || value->kind != JSON_NUMBER || !out)
		return 0;

	t = doc->text + value->at;
	i = 0;
	if (value->len && t[0] == '-') {
		negative = 1;
		i = 1;
	}

	for (; i < value->len; i++) {
		if (!is_digit(t[i]))
			return 0;	/* a fraction or an exponent: not this */

		/*
		 * Bounded before it wraps, not after.
		 *
		 * A number past the range is refused rather than truncated:
		 * this machine has no floating point to fall back on and a
		 * wrapped value is a number the document did not contain.
		 */
		if (n > (2147483647UL * 4UL))
			return 0;
		n = n * 10UL + (unsigned long)(t[i] - '0');
		if (n > 2147483647UL * 4UL + 3UL)
			return 0;
	}

	*out = negative ? -(long)n : (long)n;
	return 1;
}

int json_bool(const struct json *doc, const struct json_node *value, int *out)
{
	(void)doc;
	if (!value || !out)
		return 0;
	if (value->kind == JSON_TRUE) {
		*out = 1;
		return 1;
	}
	if (value->kind == JSON_FALSE) {
		*out = 0;
		return 1;
	}
	return 0;
}
