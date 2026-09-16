/*
 * Decoding a form, and the field that has two values.
 *
 * Pure: every case is a string literal, no sockets, no files. That is the
 * whole argument for keeping the decoder in its own file -- the part that
 * faces a filled-in form from a stranger is the part that should be cheap to
 * run exhaustively.
 *
 * **The case this suite is really about is `a=1&a=2`.** It is legal to send and
 * there is no agreement about what it means: stacks variously take the first,
 * the last, both, or a comma-joined pair. Where a filter and the thing behind
 * it choose differently, a value walks past the filter -- HTTP parameter
 * pollution, which is request smuggling's shape one layer up. The answer here
 * is to have no answer, and the checks below pin that down.
 *
 * **Watched failing first.** Run against a decoder that returned the first
 * match instead of refusing a duplicate, and against one that shared
 * `request.c`'s percent-decoder so `+` stayed a plus, the pollution and the
 * space cases failed as they should before the real one was believed.
 */

#include "../http/form.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL  %s\n", what);
	}
}

static struct http_form F;

static int parse(const char *s)
{
	return http_form_parse(s, strlen(s), &F);
}

/* The field `name` holds exactly `value`. */
static void holds(const char *input, const char *name, const char *value,
                  const char *what)
{
	const char *got;

	checks++;
	if (parse(input) != HTTP_OK) {
		failures++;
		printf("  FAIL  %s: refused\n", what);
		return;
	}
	got = http_form_get(&F, name);
	if (!got || strcmp(got, value) != 0) {
		failures++;
		printf("  FAIL  %s: got \"%s\", wanted \"%s\"\n", what,
		       got ? got : "(none)", value);
	}
}

static void refuses(const char *input, int why, const char *what)
{
	int rc = parse(input);

	checks++;
	if (rc != why) {
		failures++;
		printf("  FAIL  %s: got %d, wanted %d\n", what, rc, why);
	}
}

int main(void)
{
	printf("decoding a form, and the field that has two values\n");

	/* --- ordinary --------------------------------------------------------- */
	holds("name=M17", "name", "M17", "one field");
	holds("a=1&b=2&c=3", "b", "2", "one of several");
	holds("a=1&b=2&c=3", "c", "3", "the last of several");
	holds("empty=", "empty", "", "an empty value is a value");

	ok(parse("") == HTTP_OK && F.count == 0,
	   "an empty body is a form with no fields");
	ok(parse("a=1&") == HTTP_OK && F.count == 1,
	   "a trailing & is skipped, not refused");
	ok(parse("a=1&&b=2") == HTTP_OK && F.count == 2,
	   "a doubled & is skipped too");

	/* --- the difference from a path --------------------------------------
	 *
	 * `+` is a space in a form and a plus sign in a path. Sharing one
	 * decoder between the two is the bug this pair of cases exists to
	 * prevent, in both directions. */
	holds("q=hello+world", "q", "hello world", "+ is a space in a form");
	holds("q=1%2B2", "q", "1+2", "an encoded plus stays a plus");
	holds("q=a%20b", "q", "a b", "%20 is a space as well");

	/* --- percent-decoding -------------------------------------------------- */
	holds("k=%41%42%43", "k", "ABC", "uppercase hex");
	holds("k=%61%62%63", "k", "abc", "lowercase hex");
	holds("name=M%31%37", "name", "M17", "a name built from escapes");
	holds("k=a%26b", "k", "a&b", "an encoded & is not a separator");
	holds("k=a%3Db", "k", "a=b", "an encoded = is not a separator");
	holds("%6Eame=x", "name", "x", "the field name is decoded too");

	refuses("k=%zz", HTTP_EMALFORMED, "% not followed by hex");
	refuses("k=%4", HTTP_EMALFORMED, "% with one digit at the end");
	refuses("k=%", HTTP_EMALFORMED, "% at the very end");
	refuses("k=a%00b", HTTP_EMALFORMED,
	        "%00 -- a NUL truncates the value in any C interface after this");

	/* --- shapes that are not fields ---------------------------------------- */
	refuses("novalue", HTTP_EMALFORMED, "a pair with no =");
	refuses("a=1&novalue", HTTP_EMALFORMED, "a pair with no = among good ones");
	refuses("=1", HTTP_EMALFORMED, "a value with no name");
	refuses("a=1&=2", HTTP_EMALFORMED, "an empty name among good ones");

	/* --- the field given twice ---------------------------------------------
	 *
	 * The point of the whole file. Two values is not an answer, so there is
	 * no answer -- and a caller who needs to know the difference between
	 * absent and ambiguous can ask. */
	{
		ok(parse("a=1&a=2") == HTTP_OK,
		   "a duplicated field parses -- it is legal to send");
		ok(F.count == 2, "and both values are kept");
		ok(http_form_get(&F, "a") == 0,
		   "but asking for one value gets none, rather than a guess");
		ok(http_form_count(&F, "a") == 2,
		   "and the count says why: it was given twice");
		ok(http_form_count(&F, "absent") == 0,
		   "which is how absent and ambiguous are told apart");

		ok(parse("a=1&a=1") == HTTP_OK && http_form_get(&F, "a") == 0,
		   "two identical values are still two -- agreement is not the"
		   " property that makes it unambiguous");

		ok(parse("role=user&role=admin") == HTTP_OK
		   && http_form_get(&F, "role") == 0,
		   "the pollution case: neither value is handed out");
	}

	/* --- a value may hold any byte but NUL --------------------------------- */
	{
		ok(parse("k=%01%02%FF") == HTTP_OK,
		   "control and high bytes are legal in a value");
		ok(http_form_value_len(&F, "k") == 3,
		   "and the length is the real one");
		ok(parse("k=a%00") != HTTP_OK,
		   "NUL is the exception, because it ends strings");
	}

	/* A value carrying a NUL would measure short with strlen; the length is
	 * carried separately for exactly that reason. Checked here on a value
	 * with no NUL so the two agree, which is the ordinary case. */
	ok(parse("k=abcd") == HTTP_OK && http_form_value_len(&F, "k") == 4
	   && strlen(http_form_get(&F, "k")) == 4,
	   "length and strlen agree when there is nothing hiding in the value");

	/* --- bounds, which refuse rather than truncate -------------------------- */
	{
		static char many[HTTP_FORM_FIELDS_MAX * 8 + 64];
		static char huge[HTTP_FORM_VALUE_MAX + 64];
		size_t at = 0;
		int i;

		for (i = 0; i < HTTP_FORM_FIELDS_MAX + 4; i++)
			at += (size_t)snprintf(many + at, sizeof(many) - at,
			                       "f%d=v&", i);
		refuses(many, HTTP_ETOOMANY, "more fields than there is room for");

		at = (size_t)snprintf(huge, sizeof(huge), "k=");
		for (i = 0; i < HTTP_FORM_VALUE_MAX + 8; i++)
			huge[at++] = 'x';
		huge[at] = '\0';
		refuses(huge, HTTP_EFIELD_LONG,
		        "a value past its bound is refused, never cut");
	}

	/* --- a body that is not NUL-terminated ---------------------------------
	 *
	 * The server hands the body as a pointer and a length, out of the
	 * middle of a connection buffer with the next request sitting after it.
	 * A decoder that read to a terminator would swallow that. */
	{
		const char raw[] = "a=1&b=2GET / HTTP/1.1";

		ok(http_form_parse(raw, 7, &F) == HTTP_OK && F.count == 2,
		   "only the bytes it was given are read");
		ok(http_form_get(&F, "b") != 0
		   && strcmp(http_form_get(&F, "b"), "2") == 0,
		   "and the field at the boundary stops where it should");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
