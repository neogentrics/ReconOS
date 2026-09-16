/*
 * Decoding a form.
 *
 * See `form.h` for why `+` makes this a different decoder from the one in
 * `request.c`, and for why a name given twice has no answer.
 *
 * --- What was tried and rejected ---
 *
 * **Reusing `request.c`'s percent-decoder with a "plus means space" flag.**
 * It is four lines shorter and it is the version that produces the bug the
 * header describes, because the flag is a thing a caller passes and a caller
 * passes it wrong exactly once. Two functions cannot be confused at a call
 * site; a boolean can.
 *
 * **Treating a pair with no `=` as a name with an empty value.** PHP does
 * this, and it means `?admin` and `?admin=` arrive identically at a check that
 * asks whether the field is present. It is refused here: a form field has a
 * name and a value, and a caller that meant an empty value can write one.
 *
 * **Accepting `%00`.** A NUL inside a value truncates it in any C interface it
 * later reaches, so a name that passed a check is not the name that gets used.
 * The request parser refuses it in a path for the same reason and this refuses
 * it in a value; the fact that a form value may hold *other* arbitrary bytes
 * does not extend to the one byte that ends strings.
 */

#include "form.h"

/* No <string.h>: built against the host's library for the suite and ReconOS's
 * on the machine, and a comparison that folds differently between them is a
 * field lookup that answers differently on the two. */

static int same(const char *a, const char *b)
{
	size_t i = 0;

	while (a[i] && a[i] == b[i])
		i++;
	return a[i] == b[i];
}

static int hex_of(char c)
{
	if (c >= '0' && c <= '9')	return c - '0';
	if (c >= 'a' && c <= 'f')	return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')	return c - 'A' + 10;
	return -1;
}

/*
 * Decode one name or value out of `in[0..len)` into `out`.
 *
 * `out` is NUL-terminated and `*out_len` is the real length, which is not the
 * same thing: a form value may hold any byte, so a caller that measures with
 * `strlen` is measuring something shorter than what arrived. The one byte
 * refused is NUL itself, for the reason in the file header.
 */
static int decode(const char *in, size_t len, char *out, size_t room,
                  size_t *out_len)
{
	size_t i = 0, o = 0;

	while (i < len) {
		char c = in[i];

		if (c == '+') {
			/* The whole reason this is not `request.c`'s decoder. */
			c = ' ';
			i++;
		} else if (c == '%') {
			int hi, lo;

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
			i++;
		}

		if (o + 1 >= room)
			return HTTP_EFIELD_LONG;
		out[o++] = c;
	}

	out[o] = '\0';
	*out_len = o;
	return HTTP_OK;
}

int http_form_parse(const char *body, size_t len, struct http_form *into)
{
	size_t at = 0;

	if (!into)
		return HTTP_EMALFORMED;
	into->count = 0;
	if (!body || len == 0)
		return HTTP_OK;		/* an empty form, which is legal */

	while (at < len) {
		size_t start = at, eq = 0;
		int have_eq = 0;
		struct http_form_field *f;
		size_t nlen;
		int rc;

		while (at < len && body[at] != '&') {
			if (body[at] == '=' && !have_eq) {
				eq = at;
				have_eq = 1;
			}
			at++;
		}

		/* A trailing or doubled `&` produces an empty pair. Skipped
		 * rather than refused: `a=1&` is what a great many forms send,
		 * and refusing it would reject correct clients over a byte that
		 * carries no meaning. */
		if (at == start) {
			at++;
			continue;
		}

		if (!have_eq)
			return HTTP_EMALFORMED;	/* see the file header */

		nlen = eq - start;
		if (nlen == 0)
			return HTTP_EMALFORMED;	/* a value with no name */

		if (into->count >= HTTP_FORM_FIELDS_MAX)
			return HTTP_ETOOMANY;

		f = &into->fields[into->count];
		{
			size_t got;

			rc = decode(body + start, nlen, f->name,
			            sizeof(f->name), &got);
			if (rc != HTTP_OK)
				return rc;
			if (got == 0)
				return HTTP_EMALFORMED;
		}

		rc = decode(body + eq + 1, at - eq - 1, f->value,
		            sizeof(f->value), &f->value_len);
		if (rc != HTTP_OK)
			return rc;

		into->count++;
		at++;			/* past the '&' */
	}

	return HTTP_OK;
}

size_t http_form_count(const struct http_form *form, const char *name)
{
	size_t i, n = 0;

	if (!form || !name)
		return 0;
	for (i = 0; i < form->count; i++)
		if (same(form->fields[i].name, name))
			n++;
	return n;
}

const char *http_form_get(const struct http_form *form, const char *name)
{
	size_t i;

	/* Two values is not an answer. See `form.h`. */
	if (http_form_count(form, name) != 1)
		return 0;

	for (i = 0; i < form->count; i++)
		if (same(form->fields[i].name, name))
			return form->fields[i].value;
	return 0;
}

size_t http_form_value_len(const struct http_form *form, const char *name)
{
	size_t i;

	if (http_form_count(form, name) != 1)
		return 0;

	for (i = 0; i < form->count; i++)
		if (same(form->fields[i].name, name))
			return form->fields[i].value_len;
	return 0;
}
