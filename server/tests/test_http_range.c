/*
 * Asking for part of a file, and the ways that hands over the wrong part.
 *
 * Pure: no sockets, no files. Every case is a header and a length.
 *
 * **Three failures matter here and they are not equally visible.**
 *
 * A range refused that should have been honoured costs a whole body. Wasteful,
 * obvious, self-correcting.
 *
 * A range *honoured wrongly* -- an off-by-one at either end, a backwards range
 * quietly swapped, a start past the end clamped to the last byte -- hands the
 * client bytes it did not ask for, with a 206 beside them saying all is well.
 * A resumed download then has a gap or an overlap in the middle of a file that
 * looks complete.
 *
 * And a range honoured across a *changed* file stitches two different files
 * together. That is what `If-Range` is for, and it is the case with no symptom
 * at all until somebody opens the result.
 *
 * **Watched failing first**, against a parser that clamped a start past the end
 * instead of refusing it, and one that accepted a weak tag for `If-Range`. The
 * 416 cases and the weak-tag case failed as they should.
 */

#include "../http/range.h"

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

static struct http_range R;

/* The header, against a file of `len` bytes, gives first..last. */
static void gives(const char *header, unsigned long len, unsigned long first,
                  unsigned long last, const char *what)
{
	int rc = http_range_parse(header, len, &R);

	checks++;
	if (rc != HTTP_RANGE_OK) {
		failures++;
		printf("  FAIL  %s: verdict %d, wanted a range\n", what, rc);
		return;
	}
	if (R.first != first || R.last != last) {
		failures++;
		printf("  FAIL  %s: got %lu-%lu, wanted %lu-%lu\n", what,
		       R.first, R.last, first, last);
	}
}

static void verdict(const char *header, unsigned long len, int want,
                    const char *what)
{
	int rc = http_range_parse(header, len, &R);

	checks++;
	if (rc != want) {
		failures++;
		printf("  FAIL  %s: got %d, wanted %d\n", what, rc, want);
	}
}

int main(void)
{
	printf("asking for part of a file, and the ways that hands over the wrong part\n");

	/* --- the three shapes --------------------------------------------------- */
	gives("bytes=0-499", 1000, 0, 499, "the first five hundred bytes");
	gives("bytes=500-999", 1000, 500, 999, "a middle range");
	gives("bytes=500-", 1000, 500, 999, "from a point to the end");
	gives("bytes=-500", 1000, 500, 999, "the last five hundred bytes");
	gives("bytes=0-0", 1000, 0, 0, "one byte -- the range is inclusive");
	gives("bytes=999-999", 1000, 999, 999, "the last byte by number");
	gives("bytes=0-", 1000, 0, 999, "everything, said as a range");
	gives("bytes=-1000", 1000, 0, 999, "the last N where N is the whole file");
	gives("  bytes=0-9  ", 1000, 0, 9, "surrounding space is allowed");

	/* An end past the file means "to the end", and clamping it is correct.
	 * A start past the file does not, and clamping *that* is the bug the
	 * suite is really watching for -- see the 416 cases below. */
	gives("bytes=990-2000", 1000, 990, 999,
	      "an end past the file is clamped, which is what it means");
	gives("bytes=-2000", 1000, 0, 999,
	      "asking for more last-bytes than exist gives the whole file");

	/* --- what cannot be satisfied -------------------------------------------- */
	verdict("bytes=1000-1200", 1000, HTTP_RANGE_UNSATISFIABLE,
	        "a start exactly past the end");
	verdict("bytes=5000-", 1000, HTTP_RANGE_UNSATISFIABLE,
	        "a start well past the end");
	verdict("bytes=-0", 1000, HTTP_RANGE_UNSATISFIABLE,
	        "the last zero bytes, which is syntactically fine and cannot exist");
	verdict("bytes=0-0", 0, HTTP_RANGE_UNSATISFIABLE,
	        "any range of an empty file");
	verdict("bytes=-1", 0, HTTP_RANGE_UNSATISFIABLE,
	        "the last byte of an empty file");

	/* --- headers to ignore, which is not the same as to refuse ---------------
	 *
	 * A malformed Range gets the whole body. That is always a correct
	 * answer to a GET, and refusing would break clients over a header they
	 * could have left out. */
	verdict("", 1000, HTTP_RANGE_NONE, "an empty header");
	verdict("bytes=", 1000, HTTP_RANGE_NONE, "a unit and nothing else");
	verdict("bytes=-", 1000, HTTP_RANGE_NONE, "a dash and nothing else");
	verdict("bytes=abc-def", 1000, HTTP_RANGE_NONE, "letters");
	verdict("bytes=0", 1000, HTTP_RANGE_NONE, "a number with no dash");
	verdict("items=0-9", 1000, HTTP_RANGE_NONE,
	        "a unit this does not implement");
	verdict("0-9", 1000, HTTP_RANGE_NONE, "no unit at all");
	verdict("bytes=0-9x", 1000, HTTP_RANGE_NONE, "trailing rubbish");
	verdict(0, 1000, HTTP_RANGE_NONE, "no header at all");

	/* `strtoul` accepts every one of these and reports success. */
	verdict("bytes=+0-9", 1000, HTTP_RANGE_NONE, "a signed start");
	verdict("bytes=0-+9", 1000, HTTP_RANGE_NONE, "a signed end");
	verdict("bytes= 0-9", 1000, HTTP_RANGE_OK,
	        "space after the unit is allowed, and only that");

	/* Overflow answers "ignore", never a wrapped number. A wrapped range is
	 * a small one where an enormous one was written. */
	verdict("bytes=99999999999999999999-", 1000, HTTP_RANGE_NONE,
	        "a start that overflows");
	verdict("bytes=0-99999999999999999999", 1000, HTTP_RANGE_NONE,
	        "an end that overflows");

	/* --- backwards, which is not silently swapped ---------------------------- */
	verdict("bytes=500-100", 1000, HTTP_RANGE_NONE,
	        "a backwards range is ignored, not reversed");

	/* --- a list, refused by serving everything -------------------------------
	 *
	 * A few hundred bytes of header can ask for ten thousand one-byte
	 * ranges, each needing its own boundary and its own seek. Small
	 * request, enormous response, and a great deal of work. */
	verdict("bytes=0-99,200-299", 1000, HTTP_RANGE_NONE,
	        "a list is ignored rather than answered as multipart");
	verdict("bytes=-100,-200", 1000, HTTP_RANGE_NONE,
	        "a list of suffix ranges too");

	/* --- If-Range ------------------------------------------------------------- */
	{
		const char *tag = "\"4aa-abc\"";

		ok(http_if_range(0, tag) == 1,
		   "no If-Range means the range stands");
		ok(http_if_range("", tag) == 1, "an empty If-Range likewise");
		ok(http_if_range("\"4aa-abc\"", tag) == 1,
		   "a matching tag honours the range");
		ok(http_if_range("  \"4aa-abc\"", tag) == 1,
		   "with leading space");
		ok(http_if_range("\"different\"", tag) == 0,
		   "a different tag sends the whole file instead");

		/* The one place the comparison is strong rather than weak. A
		 * weak validator says "near enough the same representation",
		 * which is fine for deciding whether to re-send a body and not
		 * fine for stitching half a file onto another half. */
		ok(http_if_range("W/\"4aa-abc\"", tag) == 0,
		   "a weak tag never satisfies If-Range, even matching");

		/* No clock here, so a date cannot be compared. Not a match
		 * sends the whole file: a wasted body rather than a corrupt
		 * one. */
		ok(http_if_range("Wed, 16 Sep 2026 00:00:00 GMT", tag) == 0,
		   "a date is not a match, because there is nothing to compare it to");

		ok(http_if_range("\"4aa-ab\"", tag) == 0,
		   "a tag that is a prefix of ours does not match");
		ok(http_if_range("\"4aa-abcd\"", tag) == 0,
		   "nor one that extends it");
		ok(http_if_range("\"unterminated", tag) == 0,
		   "an unterminated tag");
		ok(http_if_range("\"x\"", "") == 0,
		   "nothing to compare against sends the whole file");
		ok(http_if_range("\"x\"", 0) == 0, "no server tag at all");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
