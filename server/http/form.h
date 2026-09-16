/*
 * Reading a form, and a query string, which are the same format.
 *
 * `application/x-www-form-urlencoded`: pairs joined by `&`, a name and a value
 * joined by `=`, percent-encoding throughout, and `+` meaning a space.
 *
 * --- Why this is not the same decoder as the one in `request.c` ---
 *
 * **`+` is a space here and is a plus sign in a path.** That one difference has
 * produced a long history of bugs in both directions: a path decoder used on a
 * form turns `1+2` into `1 2`, and a form decoder used on a path turns a file
 * named `a+b` into `a b`. They look like the same encoding and they are not,
 * so they are two functions rather than one with a flag -- a flag is a thing
 * somebody passes wrong.
 *
 * --- The decision that matters: a name given twice ---
 *
 * `a=1&a=2` is legal to send and there is no agreement about what it means.
 * Different stacks take the first, the last, or both; some join them with a
 * comma. **That disagreement is the vulnerability**: where a filter and the
 * thing behind it read the same request differently, a value can be smuggled
 * past the filter -- the same shape as request smuggling, one layer up. It has
 * a name, HTTP parameter pollution, because it is common enough to need one.
 *
 * So `http_form_get` **refuses** a name that appears more than once. It answers
 * NULL, exactly as it does for a name that is absent, and
 * `http_form_count` tells the two apart for a caller who wants to know. A
 * caller that genuinely wants every value asks for them by index.
 *
 * This is the same rule the request parser follows for two `Content-Length`
 * headers, and for the same reason: agreement is not the property that makes a
 * message safe, being unambiguous is.
 */

#ifndef RECON_HTTP_FORM_H
#define RECON_HTTP_FORM_H

#include "http.h"

/* Bounds. Each refuses rather than truncating: a truncated value is a
 * different value, and a form with fields silently dropped is a form somebody
 * filled in and the server did not read. */
#define HTTP_FORM_FIELDS_MAX  32
#define HTTP_FORM_NAME_MAX    64
#define HTTP_FORM_VALUE_MAX  512

struct http_form_field {
	char name[HTTP_FORM_NAME_MAX];
	char value[HTTP_FORM_VALUE_MAX];
	size_t value_len;	/* a value may hold any byte except NUL */
};

struct http_form {
	struct http_form_field fields[HTTP_FORM_FIELDS_MAX];
	size_t count;
};

/*
 * Decode `len` bytes of form-encoded input into `into`.
 *
 * `body` need not be NUL-terminated and is never read past `len`. An empty
 * input is a valid form with no fields, which is what a POST of length zero
 * is.
 *
 * Returns HTTP_OK, or a verdict from `http.h`:
 *   HTTP_EMALFORMED  bad encoding, an empty name, a pair with no `=`, or a
 *                    NUL arriving through `%00`
 *   HTTP_EFIELD_LONG a name or value past its bound
 *   HTTP_ETOOMANY    more fields than HTTP_FORM_FIELDS_MAX
 */
int http_form_parse(const char *body, size_t len, struct http_form *into);

/*
 * The value for `name`, or NULL.
 *
 * NULL means "no single answer": either the name is absent, or it was given
 * more than once. See the header above for why those are deliberately the same
 * answer. Use `http_form_count` to tell them apart.
 *
 * The value is NUL-terminated for convenience *and* carries its length in the
 * field, because a form value may legally contain any byte. A caller handling
 * arbitrary bytes should use `http_form_value_len`.
 */
const char *http_form_get(const struct http_form *form, const char *name);

/* How many times `name` appears: 0, 1, or more. */
size_t http_form_count(const struct http_form *form, const char *name);

/* The length of the value `http_form_get` would return, or 0. */
size_t http_form_value_len(const struct http_form *form, const char *name);

#endif
