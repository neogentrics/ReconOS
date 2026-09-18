/*
 * Escaping text for JSON, and the byte that ends a string early.
 *
 * Pure. As with the HTML escaper, the value is the list rather than the
 * algorithm -- every case below is a byte that an escaper handling only the
 * quote would let through, with what it then allows.
 *
 * **The case this exists for** is a value that closes its own string. A client
 * that can influence any text an endpoint reports -- a machine name, a request
 * target in a log line -- can then write fields of its own into the document a
 * reader is parsing. That was live in `/api/status` and in the reply from
 * `POST /api/name`, safe only because the name validator happened to forbid a
 * quote.
 *
 * **Watched failing first**, against an escaper handling only `"` and `\`:
 * 13 of 28 checks failed -- every control character, the refusal of bytes
 * above ASCII, and the buffer arithmetic that depends on an escape being six
 * bytes wide. The quote cases all passed, which is the point: the part
 * everybody writes is the part a suite for it would have covered.
 *
 * The control characters are the interesting half. They make a document two
 * parsers read differently rather than one they both reject, which is the same
 * shape as two HTTP parsers disagreeing about where a body ends.
 *
 * The mutant was written by hand. A scripted edit to `json.c` was tried first
 * and landed on two of its three sites, leaving the short forms in place --
 * a weaker mutant that would have been reported as a stronger one.
 */

#include "../http/json.h"

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
	long n = json_escape(in, OUT, sizeof(OUT));

	checks++;
	if (n < 0) {
		failures++;
		printf("  FAIL  %s: refused\n", what);
		return;
	}
	if (strcmp(OUT, want) != 0) {
		failures++;
		printf("  FAIL  %s: got [%s], wanted [%s]\n", what, OUT, want);
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
	printf("escaping text for JSON, and the byte that ends a string early\n");

	/* --- text that needs nothing ------------------------------------------- */
	gives("M17", "M17", "an ordinary name");
	gives("web-srv01", "web-srv01", "hyphens are not special");
	gives("", "", "an empty string");
	gives("GET /api/status -> 200", "GET /api/status -> 200",
	      "an ordinary log line");

	/* --- the one everybody handles ------------------------------------------
	 *
	 * Reconstructed as a machine name, because that is where it was live. */
	gives("M17\",\"role\":\"root",
	      "M17\\\",\\\"role\\\":\\\"root",
	      "a quote that would have written a field of its own");

	gives("a\\b", "a\\\\b", "a backslash");

	/* A backslash must be escaped or the escape it produces gets read as an
	 * escape of whatever follows. Checked on something already escaped. */
	gives("\\\"", "\\\\\\\"", "an escaped quote is escaped again, not passed");

	/* --- the ones an escaper handling two would let through ------------------
	 *
	 * A raw control character inside a JSON string is invalid. Parsers
	 * differ on whether they accept it, which means two readers of one
	 * document disagree about where the string ends -- the same shape as
	 * two HTTP parsers disagreeing about a body. */
	gives("a\nb", "a\\nb", "a newline");
	gives("a\rb", "a\\rb", "a carriage return");
	gives("a\tb", "a\\tb", "a tab");
	gives("a\bb", "a\\bb", "a backspace");
	gives("a\fb", "a\\fb", "a form feed");

	/* The rest below a space get the long form rather than being dropped. A
	 * dropped byte is a value that is not the value that arrived. */
	{
		const char raw[] = { 'a', 0x01, 'b', '\0' };

		gives(raw, "a\\u0001b", "an escape with no short form");
	}
	{
		const char raw[] = { 0x1F, '\0' };

		gives(raw, "\\u001f", "the last one below a space");
	}

	/* A NUL inside a counted run is a byte like any other here -- the
	 * terminated form cannot reach it, which is why both exist. */
	{
		const char raw[] = { 'a', '\0', 'b' };
		long n = json_escape_n(raw, 3, OUT, sizeof(OUT));

		ok(n == 8 && strcmp(OUT, "a\\u0000b") == 0,
		   "a NUL in a counted run is escaped, not an ending");
	}

	/* --- above ASCII is refused ----------------------------------------------
	 *
	 * JSON must be valid UTF-8 and this does not validate it, so it refuses
	 * rather than emitting a document a parser will reject -- or quietly
	 * repair differently from the next parser. */
	{
		const char high[] = { 'a', (char)0x80, 'b', '\0' };
		const char utf8[] = { (char)0xC3, (char)0xA9, '\0' };  /* é */

		ok(json_escape(high, OUT, sizeof(OUT)) == -1,
		   "a lone high byte is refused");
		ok(json_escape(utf8, OUT, sizeof(OUT)) == -1,
		   "and so is well-formed UTF-8 -- this writer emits ASCII only,"
		   " and says so rather than guessing");
		ok(json_escape("\x7f", OUT, sizeof(OUT)) == 1,
		   "DEL is ASCII and passes -- the boundary is 0x7F, not 0x7E");
	}

	/* --- refusing rather than truncating -------------------------------------
	 *
	 * A string cut mid-escape ends `\u00`, and what a parser reads next is
	 * whatever followed. */
	{
		char tiny[8];

		ok(json_escape("abc", tiny, sizeof(tiny)) == 3, "what fits, fits");

		/* `\"` is two bytes plus a terminator. */
		ok(json_escape("\"", tiny, 2) == -1,
		   "an escape that does not fit is refused");
		ok(json_escape("\"", tiny, 3) == 2,
		   "and fits in exactly the room it needs");

		/* `\x01` is six plus a terminator -- written as an escape, because
		 * the byte itself in a comment is the fault `check-c-literals.py`
		 * now refuses, and it was here. */
		{
			const char raw[] = { 0x01, '\0' };

			ok(json_escape(raw, tiny, 6) == -1,
			   "a long escape that does not fit is refused whole");
			ok(json_escape(raw, tiny, 7) == 6, "and fits at seven");
		}

		ok(json_escape("aaaaaaaaaa", tiny, sizeof(tiny)) == -1,
		   "plain text past the buffer is refused too");
		ok(json_escape(0, tiny, sizeof(tiny)) == -1, "no input");
		ok(json_escape("a", 0, 10) == -1, "no output buffer");
		ok(json_escape("a", tiny, 0) == -1, "no room at all");
	}

	/* --- what a caller must budget for ---------------------------------------- */
	{
		char big[64];
		const char raw[] = { 0x01, 0x02, 0x03, '\0' };

		ok(json_escape(raw, big, sizeof(big)) == 18,
		   "six bytes out for one in, worst case");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
