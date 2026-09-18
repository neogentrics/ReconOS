/*
 * Choosing what to send, and the headers that are usually read wrong.
 *
 * `Accept` is the header most often handled by looking for a substring, and
 * that fails in both directions at once: `application/json` is a substring of
 * `application/jsonrequest`, so a client that will not take JSON is sent it;
 * and a client sending nothing but a wildcard is sent nothing, because the
 * string is not there. Both failures are silent, which is why most of this file
 * is headers whose correct answer is not the obvious one.
 *
 * Three properties carry the weight here, and each has a section:
 *
 *   * `q` orders the ranges, and the header is **not** in preference order --
 *     Chrome's own is deliberately not sorted;
 *   * `q=0` means *not acceptable*, which is the only way a client can say
 *     "anything but this";
 *   * the **most specific** matching range decides an offer, so
 *     a `text` wildcard at q=1 beside `text/html;q=0` excludes HTML
 *     rather than preferring it.
 */

#include "../http/accept.h"

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

/* The two this server actually offers, in its own order of preference. */
static const char *const TEXT_FIRST[] = { "text/plain", "application/json" };
static const char *const JSON_FIRST[] = { "application/json", "text/plain" };

static void picks(const char *header, const char *const *offers, size_t count,
                  int wanted, const char *what)
{
	int got = http_accept_pick(header, offers, count);

	checks++;
	if (got != wanted) {
		failures++;
		printf("  FAIL  %s -- chose %d, wanted %d\n", what, got,
		       wanted);
	}
}

int main(void)
{
	printf("choosing what to send, and the headers that are read wrong\n");

	/* --- the ordinary cases ------------------------------------------------ */
	picks(0, TEXT_FIRST, 2, 0,
	      "no header at all takes the server's first preference");
	picks("*/*", TEXT_FIRST, 2, 0, "a bare wildcard does the same");
	picks("text/plain", TEXT_FIRST, 2, 0, "an exact type is chosen");
	picks("application/json", TEXT_FIRST, 2, 1,
	      "including the one the server likes less");
	picks("application/json", JSON_FIRST, 2, 0,
	      "and the server's order is its own");
	picks("text/*", TEXT_FIRST, 2, 0, "a subtype wildcard matches");
	picks("application/*", TEXT_FIRST, 2, 1, "and matches the other one");
	picks("  text/plain  ", TEXT_FIRST, 2, 0, "whitespace around a range");
	picks("TEXT/PLAIN", TEXT_FIRST, 2, 0,
	      "a shouted media type is the same media type");

	/* --- what the client will not take -------------------------------------- */
	picks("image/png", TEXT_FIRST, 2, ACCEPT_NONE,
	      "a type this cannot produce is 406");
	picks("image/*", TEXT_FIRST, 2, ACCEPT_NONE, "and a wildcard for one");
	picks("text/html", TEXT_FIRST, 2, ACCEPT_NONE,
	      "a subtype this cannot produce, under a type it can");

	/*
	 * The substring trap, in both directions.
	 *
	 * `application/json` is a substring of `application/jsonrequest`. A
	 * server matching on substrings sends JSON to a client that asked for
	 * something else entirely, and the client's parser then fails on a
	 * document it never requested.
	 */
	picks("application/jsonrequest", TEXT_FIRST, 2, ACCEPT_NONE,
	      "a longer type that contains an offered one is not that type");
	picks("text/plaintext", TEXT_FIRST, 2, ACCEPT_NONE,
	      "and a longer subtype is not that subtype");
	picks("xtext/plain", TEXT_FIRST, 2, ACCEPT_NONE,
	      "nor is a longer type in front");

	/* --- q orders the ranges, and the header is not in order ---------------- */
	picks("text/plain;q=0.1, application/json", TEXT_FIRST, 2, 1,
	      "the better-weighted range wins, not the first one");
	picks("application/json;q=0.2, text/plain;q=0.8", TEXT_FIRST, 2, 0,
	      "and again the other way round");
	picks("text/plain;q=1, application/json;q=1", TEXT_FIRST, 2, 0,
	      "a tie goes to the server's preference");
	picks("text/plain;q=1, application/json;q=1", JSON_FIRST, 2, 0,
	      "which is the server's, not the client's order");
	picks("text/plain;q=0.5", TEXT_FIRST, 2, 0,
	      "a weight below one is still acceptable");
	picks("text/plain; q=0.5", TEXT_FIRST, 2, 0,
	      "with a space before the parameter");
	picks("text/plain ;q=0.5", TEXT_FIRST, 2, 0, "and before the semicolon");
	picks("text/plain;Q=0.5", TEXT_FIRST, 2, 0, "and a shouted q");

	/*
	 * `q=0` means **not acceptable**, not *slightly worse*. It is the only
	 * way a client can say "anything except this", and a parser that reads
	 * it as a small preference sends exactly what was excluded.
	 */
	picks("text/plain;q=0", TEXT_FIRST, 2, ACCEPT_NONE,
	      "q=0 on the only match refuses the request");
	picks("text/plain;q=0, application/json", TEXT_FIRST, 2, 1,
	      "q=0 excludes one and leaves the other");
	picks("*/*, text/plain;q=0", TEXT_FIRST, 2, 1,
	      "and excludes it even under a wildcard that takes everything");
	picks("*/*;q=0", TEXT_FIRST, 2, ACCEPT_NONE,
	      "a wildcard at zero refuses everything");

	/*
	 * The most specific range decides an offer -- not the best-weighted
	 * one that happens to match. This is the case that separates a real
	 * implementation from one that works on the common headers.
	 */
	/*
	 * The naive reading of this header is *anything textual is perfect*,
	 * which sends `text/plain`. The correct reading is *anything textual
	 * except plain*, and since this route has nothing else textual to
	 * offer, the answer is 406. A parser that took the highest matching
	 * weight rather than the most specific match would send the one thing
	 * the client excluded.
	 *
	 * The first draft of this check expected `application/json` and was
	 * wrong for a duller reason -- it excluded `text/html`, which is not
	 * on offer, so it excluded nothing. Kept as a note because a check
	 * whose header does not do what its name says is a check that passes
	 * for the wrong reason.
	 */
	picks("text/*;q=1, text/plain;q=0", TEXT_FIRST, 2, ACCEPT_NONE,
	      "an exact exclusion beats a wildcard that accepts");
	picks("text/*;q=1, text/plain;q=0", JSON_FIRST, 2, ACCEPT_NONE,
	      "whichever way round the offers are");
	picks("*/*;q=0.1, application/json;q=0.9", TEXT_FIRST, 2, 1,
	      "an exact preference beats a wildcard");
	picks("*/*;q=0.9, application/json;q=0.1", TEXT_FIRST, 2, 0,
	      "and the wildcard still decides the offer it is the only match for");

	/* --- what a browser really sends ----------------------------------------
	 *
	 * Chrome's header, verbatim. It carries a parameter that is not `q`
	 * (`v=b3`), which is the reason parameters are accepted and ignored
	 * rather than refused: a parser that refused them would answer 400 to
	 * every request a browser makes. See `accept.h`.
	 */
	picks("text/html,application/xhtml+xml,application/xml;q=0.9,"
	      "image/avif,image/webp,image/apng,*/*;q=0.8,"
	      "application/signed-exchange;v=b3;q=0.7",
	      TEXT_FIRST, 2, 0,
	      "Chrome's own Accept header is read, and takes the wildcard");
	picks("text/html,application/xhtml+xml,application/xml;q=0.9,"
	      "image/avif,image/webp,image/apng,*/*;q=0.8,"
	      "application/signed-exchange;v=b3;q=0.7",
	      JSON_FIRST, 2, 0,
	      "and the server's preference still decides which");

	/* curl's, which is the other one that matters. */
	picks("*/*", JSON_FIRST, 2, 0, "curl's header takes the first offer");

	/* A quoted parameter value, which the grammar allows. */
	picks("text/plain;note=\"a; b\";q=0.9, application/json;q=0.5",
	      TEXT_FIRST, 2, 0,
	      "a quoted parameter value, with a semicolon inside it");
	picks("text/plain;note=\"esc\\\"aped\";q=0.9", TEXT_FIRST, 2, 0,
	      "and one with an escaped quote in it");

	/* --- headers that do not parse ------------------------------------------
	 *
	 * Refused rather than repaired. This header decides whether a request
	 * is answered at all, so a server that guesses at a broken one is
	 * guessing at the answer.
	 */
	picks("", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "an empty header, which is not the same as no header");
	picks("   ", TEXT_FIRST, 2, ACCEPT_EMALFORMED, "nor is whitespace");
	picks("text", TEXT_FIRST, 2, ACCEPT_EMALFORMED, "a type with no slash");
	picks("text/", TEXT_FIRST, 2, ACCEPT_EMALFORMED, "a type with no subtype");
	picks("/plain", TEXT_FIRST, 2, ACCEPT_EMALFORMED, "a subtype with no type");
	picks("text/plain,", TEXT_FIRST, 2, 0,
	      "a trailing comma is allowed -- the grammar's own list rule");
	picks("text/plain,,application/json", TEXT_FIRST, 2, 0,
	      "and so is an empty element, for the same reason");
	picks("text/plain application/json", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "two ranges with no comma between them");
	picks("text/plain;", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "a semicolon with no parameter");
	picks("text/plain;q", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "a parameter with no value");
	picks("text/plain;=0.5", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "a value with no name");
	picks("text/plain;note=\"unterminated", TEXT_FIRST, 2,
	      ACCEPT_EMALFORMED, "a quoted value that never closes");
	picks("*/json", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "a wildcard type with a real subtype, which cannot be meant");

	/* Quality values some parser somewhere accepts. Each would give two
	 * servers a different ordering for one header. */
	picks("text/plain;q=2", TEXT_FIRST, 2, ACCEPT_EMALFORMED, "q above one");
	picks("text/plain;q=1.5", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "q above one with a fraction");
	picks("text/plain;q=.5", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "q with no integer part");
	picks("text/plain;q=0.", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "q with nothing after the point");
	picks("text/plain;q=0.1234", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "q with four decimals, which the grammar does not have");
	picks("text/plain;q=-1", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "a negative q");
	picks("text/plain;q=high", TEXT_FIRST, 2, ACCEPT_EMALFORMED,
	      "a q that is not a number");
	picks("text/plain;q=", TEXT_FIRST, 2, ACCEPT_EMALFORMED, "an empty q");

	/* And `q` read exactly, which matters because two weights three
	 * thousandths apart still order the offers. */
	picks("text/plain;q=0.501, application/json;q=0.5", TEXT_FIRST, 2, 0,
	      "a thousandth decides an ordering");
	picks("text/plain;q=0.5, application/json;q=0.501", TEXT_FIRST, 2, 1,
	      "and decides it the other way when it is the other way");

	/* --- bounds --------------------------------------------------------------- */
	{
		static char MANY[2048];
		size_t at = 0;
		int i;

		for (i = 0; i < ACCEPT_RANGES_MAX + 1; i++)
			at += (size_t)sprintf(MANY + at, "%stype%d/sub",
			                      i ? "," : "", i);
		picks(MANY, TEXT_FIRST, 2, ACCEPT_EMALFORMED,
		      "more ranges than this reads is a refusal, not a "
		      "truncation -- the ones at the end are the low-weight "
		      "ones that decide whether the answer is 406");

		at = (size_t)sprintf(MANY, "text/");
		for (i = 0; i < ACCEPT_TOKEN_MAX + 8; i++)
			MANY[at++] = 'y';
		MANY[at] = '\0';
		picks(MANY, TEXT_FIRST, 2, ACCEPT_EMALFORMED,
		      "a subtype past the bound");
	}

	/* --- the matching rule, directly ------------------------------------------ */
	ok(http_accept_matches("text", "plain", "text/plain") == 3,
	   "an exact match is the most specific");
	ok(http_accept_matches("text", "*", "text/plain") == 2,
	   "a subtype wildcard is less specific");
	ok(http_accept_matches("*", "*", "text/plain") == 1,
	   "and a full wildcard least of all");
	ok(http_accept_matches("text", "plain", "text/html") == 0,
	   "a different subtype does not match");
	ok(http_accept_matches("image", "*", "text/plain") == 0,
	   "nor a different type");
	ok(http_accept_matches("text", "plain", "textplain") == 0,
	   "nor an offer with no slash in it");

	/*
	 * The offer's type longer than the range's, and the range's longer
	 * than the offer's.
	 *
	 * **Added because a mutant survived.** Every substring check above
	 * uses a hostile *range* against this server's two real offers, and
	 * none of them reaches the line that requires the offered type to end
	 * where the range's does. Deleting that line left all sixty-nine
	 * checks passing while `text` matched `textual/plain` -- which is a
	 * client that asked for one media type being sent another, silently,
	 * which is the exact failure this file opens by describing.
	 */
	ok(http_accept_matches("text", "plain", "textual/plain") == 0,
	   "an offered type that merely begins with the range's is not it");
	ok(http_accept_matches("textual", "plain", "text/plain") == 0,
	   "and a range that merely begins with the offer's is not it either");
	ok(http_accept_matches("text", "plain", "text/plainer") == 0,
	   "the same rule on the subtype");
	ok(http_accept_matches("text", "plainer", "text/plain") == 0,
	   "and the same rule the other way");
	ok(http_accept_matches("text", "*", "textual/plain") == 0,
	   "a subtype wildcard does not loosen the type");
	ok(http_accept_matches("*", "plain", "text/plain") == 0,
	   "and a wildcard type with a real subtype matches nothing");

	/* --- Accept-Encoding, which is the same grammar with no slash -------------
	 *
	 * The case that separates this header from `Accept` is the absent one.
	 * A client that did not mention encodings gets identity, deliberately:
	 * RFC 9110 permits sending any coding when the field is missing, and
	 * doing so is how a hand-written client that never heard of gzip is
	 * handed bytes it cannot read. See `accept.h`.
	 */
	ok(http_accept_gzip(0) == 0, "no Accept-Encoding at all means no gzip");
	ok(http_accept_gzip("gzip") == 1, "asking for gzip gets it");
	ok(http_accept_gzip("GZIP") == 1, "and a shouted coding is the same coding");
	ok(http_accept_gzip("gzip, deflate") == 1, "in a list");
	ok(http_accept_gzip("deflate, gzip, br") == 1, "anywhere in a list");
	ok(http_accept_gzip("gzip;q=0.5") == 1, "with a weight below one");
	ok(http_accept_gzip("deflate") == 0, "a coding this cannot send is not gzip");
	ok(http_accept_gzip("br, deflate, zstd") == 0, "nor are three of them");
	ok(http_accept_gzip("identity") == 0, "nor is identity");
	ok(http_accept_gzip("") == 0, "an empty header is not an invitation");

	/* `q=0` is a refusal here too, and a named refusal beats a wildcard --
	 * `*, gzip;q=0` means anything except gzip. */
	ok(http_accept_gzip("gzip;q=0") == 0, "gzip at zero is a refusal");
	ok(http_accept_gzip("*") == 1, "a wildcard accepts it");
	ok(http_accept_gzip("*;q=0") == 0, "and a wildcard at zero refuses it");
	ok(http_accept_gzip("*, gzip;q=0") == 0,
	   "a named refusal beats a wildcard that accepts");
	ok(http_accept_gzip("*;q=0, gzip") == 1,
	   "and a named acceptance beats a wildcard that refuses");

	/* What a browser sends. */
	ok(http_accept_gzip("gzip, deflate, br, zstd") == 1,
	   "Chrome's own Accept-Encoding");

	/* A header that does not parse answers no rather than refusing the
	 * request: this one is a preference, not a decision about whether the
	 * request can be answered, so the safe reading is the bytes everybody
	 * can read. */
	ok(http_accept_gzip("gzip;q=") == 0, "a broken weight sends identity");
	ok(http_accept_gzip("gzip;;q=1") == 0, "and so does a broken parameter");
	ok(http_accept_gzip("gzip q=1") == 0, "and a missing comma");

	/* --- asked for nothing ---------------------------------------------------- */
	ok(http_accept_pick("*/*", 0, 0) == ACCEPT_NONE,
	   "a route that offers nothing can satisfy nobody");

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
