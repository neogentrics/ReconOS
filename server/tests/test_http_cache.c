/*
 * Validators, and reading an If-None-Match.
 *
 * Pure: no sockets, no files, no clock. Which is the point of having pulled
 * this out of `files.c` -- the parts that decide whether a client is told *you
 * already have this* can be checked exhaustively in milliseconds, and getting
 * them wrong is expensive in a way that is hard to notice.
 *
 * **Both failure directions are bad, and they are bad differently.**
 *
 * A validator that changes when it should not means the body is sent again:
 * wasteful, visible, and self-correcting.
 *
 * A validator that *stays the same when the content changed* means the client
 * goes on serving its stored copy, and keeps doing so until the cache expires.
 * Nothing at either end reports a fault. That is the direction the cases below
 * spend most of their effort on.
 *
 * **Watched failing first**, against a tag built from the length alone and
 * against a matcher that compared `If-None-Match` as a plain string -- the
 * same-length edit case and the list and weak-tag cases failed as they should.
 */

#include "../http/cache.h"

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

static unsigned long long hash_of(const char *s)
{
	return http_hash(HTTP_HASH_SEED, s, strlen(s));
}

static void tag_of(const char *s, char *into, size_t room)
{
	http_etag_format(into, room, (unsigned long)strlen(s), hash_of(s));
}

int main(void)
{
	char a[HTTP_ETAG_MAX], b[HTTP_ETAG_MAX];

	printf("validators, and reading an If-None-Match\n");

	/* --- the hash ---------------------------------------------------------- */
	ok(hash_of("hello") == hash_of("hello"),
	   "the same bytes hash the same");
	ok(hash_of("hello") != hash_of("hellp"),
	   "one changed byte changes the hash");
	ok(hash_of("ab") != hash_of("ba"),
	   "order matters, so a transposition is seen");

	/* Fed in pieces or whole, the answer must be the same -- the file is
	 * hashed in 8 KiB blocks and a block boundary must not change it. */
	{
		unsigned long long piecewise = HTTP_HASH_SEED;

		piecewise = http_hash(piecewise, "hel", 3);
		piecewise = http_hash(piecewise, "lo", 2);
		ok(piecewise == hash_of("hello"),
		   "hashing in pieces matches hashing whole");
	}

	ok(http_hash(HTTP_HASH_SEED, "", 0) == HTTP_HASH_SEED,
	   "hashing nothing changes nothing");

	/* --- the tag ----------------------------------------------------------- */
	{
		http_etag_format(a, sizeof(a), 0, 0);
		ok(strcmp(a, "\"0-0\"") == 0, "a zero length and zero hash");

		http_etag_format(a, sizeof(a), 255, 4095);
		ok(strcmp(a, "\"ff-fff\"") == 0, "hex, without padding");

		http_etag_format(a, sizeof(a), 1194, 0x0123456789abcdefULL);
		ok(strcmp(a, "\"4aa-123456789abcdef\"") == 0,
		   "a realistic tag");

		ok(a[0] == '"' && a[strlen(a) - 1] == '"',
		   "a tag carries its quotes -- one without them is not a tag");
	}

	/* --- the case the whole file exists for ---------------------------------
	 *
	 * Two files of the same length with different content. A validator
	 * built from the length alone gives these the same tag, so a client
	 * holding the first is never told about the second -- and goes on
	 * serving stale bytes out of its own store with nothing reporting it. */
	{
		tag_of("the value is 1", a, sizeof(a));
		tag_of("the value is 2", b, sizeof(b));

		ok(strlen("the value is 1") == strlen("the value is 2"),
		   "the two differ only in content, not in length");
		ok(strcmp(a, b) != 0,
		   "a same-length edit still changes the tag");
	}

	/* And two genuinely identical files must agree, or revalidation never
	 * settles and the body is sent every time. */
	{
		tag_of("same", a, sizeof(a));
		tag_of("same", b, sizeof(b));
		ok(strcmp(a, b) == 0, "identical content gives an identical tag");
	}

	/* --- a buffer too small refuses rather than truncates --------------------
	 *
	 * A cut tag is the dangerous one: two different files cut to the same
	 * prefix share it, which is the stale-cache failure again by another
	 * route. An empty tag merely means no caching. */
	{
		char tiny[6];

		http_etag_format(tiny, sizeof(tiny), 0xFFFFFFFFUL,
		                 0xFFFFFFFFFFFFFFFFULL);
		ok(tiny[0] == '\0',
		   "a tag that does not fit is empty, never shortened");
	}

	/* --- If-None-Match ------------------------------------------------------ */
	{
		const char *mine = "\"4aa-abc\"";

		ok(http_if_none_match("\"4aa-abc\"", mine) == 1,
		   "an exact match");
		ok(http_if_none_match("\"other\"", mine) == 0,
		   "a different tag does not match");
		ok(http_if_none_match("*", mine) == 1,
		   "* matches anything the server has");
		ok(http_if_none_match("  *  ", mine) == 1,
		   "* with surrounding space");

		/* A browser revalidating several stored copies sends a list.
		 * A matcher that compares the whole header as one string calls
		 * every one of these a miss. */
		ok(http_if_none_match("\"x\", \"4aa-abc\"", mine) == 1,
		   "a list, matching the second");
		ok(http_if_none_match("\"4aa-abc\", \"x\"", mine) == 1,
		   "a list, matching the first");
		ok(http_if_none_match("\"x\", \"y\", \"z\"", mine) == 0,
		   "a list matching none");
		ok(http_if_none_match("\"x\",\"4aa-abc\"", mine) == 1,
		   "a list with no space after the comma");

		/* A proxy that altered the response marks the tag weak.
		 * If-None-Match compares weakly, so this is a match. */
		ok(http_if_none_match("W/\"4aa-abc\"", mine) == 1,
		   "a weak tag matches its strong form");
		ok(http_if_none_match("\"x\", W/\"4aa-abc\"", mine) == 1,
		   "a weak tag inside a list");

		/* Malformed input is a miss, never an error: the client gets
		 * the body it would have got anyway. */
		ok(http_if_none_match("", mine) == 0, "an empty header");
		ok(http_if_none_match("garbage", mine) == 0, "an unquoted tag");
		ok(http_if_none_match("\"unterminated", mine) == 0,
		   "a tag with no closing quote");
		ok(http_if_none_match(0, mine) == 0, "no header at all");
		ok(http_if_none_match("\"x\"", "") == 0,
		   "a server with no tag matches nothing");
		ok(http_if_none_match("\"x\"", 0) == 0, "no server tag at all");

		/* A near miss must not match. The prefix is shared, and a
		 * comparison that stopped at the shorter length would say yes
		 * -- and serve a stale body forever. */
		ok(http_if_none_match("\"4aa-ab\"", mine) == 0,
		   "a tag that is a prefix of ours does not match");
		ok(http_if_none_match("\"4aa-abcd\"", mine) == 0,
		   "nor one that extends it");
	}

	/* --- If-Match, which guards a write ---------------------------------------
	 *
	 * Everything here differs from `If-None-Match` in the direction of
	 * refusing, because a failed condition costs a client one more round
	 * trip and a wrongly-allowed one costs somebody their change.
	 */
	{
		static const char TAG[] = "\"12-abc\"";

		ok(http_if_match("\"12-abc\"", TAG) == 1,
		   "the tag a client read allows the write");
		ok(http_if_match("*", TAG) == 1,
		   "and so does a star, which means whatever is there now");
		ok(http_if_match("\"12-abd\"", TAG) == 0,
		   "a tag from an older read does not");
		ok(http_if_match("\"12-ab\"", TAG) == 0,
		   "nor does a prefix of the right one");
		ok(http_if_match("\"12-abcd\"", TAG) == 0,
		   "nor a longer one that begins with it");

		/*
		 * The strong comparison, which is the whole difference between
		 * the two headers. `If-None-Match` takes `W/"x"` as `"x"`
		 * because it is asking *have I already got this*. `If-Match`
		 * guards an overwrite, and a weak tag says only that two
		 * bodies are equivalent -- not that they are the same bytes.
		 */
		ok(http_if_none_match("W/\"12-abc\"", TAG) == 1,
		   "a weak tag matches for If-None-Match");
		ok(http_if_match("W/\"12-abc\"", TAG) == 0,
		   "and does not for If-Match, which is the whole difference");

		/* A list, and a weak one inside it, which must be stepped over
		 * rather than ending the walk. */
		ok(http_if_match("\"other\", \"12-abc\"", TAG) == 1,
		   "a tag later in a list is found");
		ok(http_if_match("W/\"12-abc\", \"12-abc\"", TAG) == 1,
		   "and a weak one before it does not stop the search");
		ok(http_if_match("W/\"12-abc\", \"nope\"", TAG) == 0,
		   "while a list of only weak and wrong tags refuses");

		/* Refusing rather than allowing, on everything malformed. */
		ok(http_if_match("", TAG) == 0, "an empty header refuses");
		ok(http_if_match("12-abc", TAG) == 0,
		   "so does a tag with no quotes, which is not a tag");
		ok(http_if_match("\"unterminated", TAG) == 0,
		   "so does one that never closes");
		ok(http_if_match(0, TAG) == 0, "and so does no header at all");
		ok(http_if_match("*", "") == 0,
		   "and a star against nothing is not a match");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
