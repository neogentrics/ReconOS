/*
 * Escaping text for HTML, and the characters people forget.
 *
 * Pure, and short. The value is not in the algorithm -- it is in the list.
 *
 * **Every case below is a character an escaper that handled only `<` and `&`
 * would let through**, together with what it would then let somebody do. That
 * is the whole reason this file is longer than the function it checks: the
 * function is fifteen lines and the argument for each entry in its table is
 * the part worth keeping.
 *
 * **Watched failing first**, against an escaper handling only `<` and `&`: the
 * attribute cases failed, including the one reconstructed from the dashboard's
 * own input field.
 */

#include "../http/escape.h"

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

static char OUT[512];

static void gives(const char *in, const char *want, const char *what)
{
	long n = http_escape(in, OUT, sizeof(OUT));

	checks++;
	if (n < 0) {
		failures++;
		printf("  FAIL  %s: refused\n", what);
		return;
	}
	if (strcmp(OUT, want) != 0) {
		failures++;
		printf("  FAIL  %s: got \"%s\", wanted \"%s\"\n", what, OUT,
		       want);
		return;
	}
	if ((size_t)n != strlen(want)) {
		failures++;
		printf("  FAIL  %s: length %ld, wanted %lu\n", what, n,
		       (unsigned long)strlen(want));
	}
}

int main(void)
{
	printf("escaping text for HTML, and the characters people forget\n");

	/* --- text that needs nothing done to it -------------------------------- */
	gives("M17", "M17", "an ordinary name passes through");
	gives("web-srv01", "web-srv01", "hyphens are not special");
	gives("", "", "an empty string");

	/* --- the two everybody handles ------------------------------------------ */
	gives("<script>", "&lt;script&gt;", "a tag");
	gives("a & b", "a &amp; b", "an ampersand");

	/* An ampersand must be escaped *first* in any implementation that does
	 * them in passes, or the escapes it writes get escaped again. Checked
	 * by escaping something that is already an entity. */
	gives("&lt;", "&amp;lt;", "an entity is escaped, not passed through");
	gives("&amp;", "&amp;amp;", "and so is an escaped ampersand");

	/* --- the three that an escaper handling two would let through ------------ */
	gives(">", "&gt;", "a closing angle bracket");

	/*
	 * The one the dashboard's input field would have fallen to.
	 *
	 * `<input name="name" value="NAME">` with a name containing a quote
	 * closes the value early and everything after it becomes attributes.
	 * This is a name, so it is reconstructed as one.
	 */
	gives("M17\" onmouseover=\"alert(1)",
	      "M17&quot; onmouseover=&quot;alert(1)",
	      "a quote that would have escaped an attribute's value");

	gives("it's", "it&#39;s",
	      "an apostrophe, for a page that quotes attributes with them");

	/* All five at once, in the order they appear in the table. */
	gives("&<>\"'", "&amp;&lt;&gt;&quot;&#39;", "all five together");

	/* --- bytes rather than a string ------------------------------------------
	 *
	 * A form value may hold any byte, so a caller measuring with `strlen`
	 * is measuring something shorter than what arrived. The length-taking
	 * form is what such a caller must use. */
	{
		const char raw[] = "a<b";
		long n = http_escape_n(raw, 3, OUT, sizeof(OUT));

		/* "a" + "&lt;" + "b" is six, not seven -- which this line got
		 * wrong first time round and the suite caught. */
		ok(n == 6 && strcmp(OUT, "a&lt;b") == 0,
		   "escaping a counted run of bytes");

		/* Only the bytes it was given, with a `<` sitting just past
		 * the end that must not be read. */
		n = http_escape_n("safe<script>", 4, OUT, sizeof(OUT));
		ok(n == 4 && strcmp(OUT, "safe") == 0,
		   "nothing past the given length is read");
	}

	/* --- refusing rather than truncating -------------------------------------
	 *
	 * Text cut mid-entity ends `&qu`, and a browser reading what follows as
	 * the rest of an entity is the confusion escaping exists to prevent. */
	{
		char tiny[8];

		ok(http_escape("abc", tiny, sizeof(tiny)) == 3,
		   "what fits, fits");

		/* `&quot;` is six bytes plus a terminator: seven. A buffer of
		 * six cannot hold it, and must not hold part of it. */
		ok(http_escape("\"", tiny, 6) == -1,
		   "an entity that does not fit is refused");
		ok(http_escape("\"", tiny, 7) == 6,
		   "and fits in exactly the room it needs");

		ok(http_escape("aaaaaaaaaaaa", tiny, sizeof(tiny)) == -1,
		   "plain text past the buffer is refused too");
		ok(http_escape(0, tiny, sizeof(tiny)) == -1, "no input");
		ok(http_escape("a", 0, 10) == -1, "no output buffer");
		ok(http_escape("a", tiny, 0) == -1, "no room at all");
	}

	/* --- the growth a caller has to budget for --------------------------------
	 *
	 * Worst case is six bytes out for one in. A caller sizing a buffer at
	 * the input length has a buffer that refuses on the first quote, which
	 * is safe and useless. */
	{
		char big[64];
		long n = http_escape("\"\"\"\"\"", big, sizeof(big));

		ok(n == 30, "five quotes become thirty bytes");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
