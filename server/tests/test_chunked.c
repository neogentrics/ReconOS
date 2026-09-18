/*
 * Reading a chunked body, and every way of framing one two ways.
 *
 * Chunked framing is where request smuggling lives. Not because the format is
 * hard, but because it has several places where a parser can meet text it does
 * not understand and carry on -- and a parser that carries on is a parser that
 * disagrees with the one in front of it about where this message ends and the
 * next one begins.
 *
 * So the shape of this suite is the shape of `test_http.c`: a handful of
 * checks that a correct body decodes, and a long list of bodies that must be
 * **refused**. The second list is the file's reason for existing.
 *
 * Every case is a string literal and the decoder takes bytes, so this runs
 * anywhere in milliseconds with no socket and no clock.
 *
 * --- One thing that is checked here and cannot be checked by eye ---
 *
 * The decoder is **incremental**: it is fed whatever has arrived and called
 * again with more. Most of the cases below are therefore run twice -- once
 * with the whole body at once, and once **a byte at a time**, which is the
 * arrangement a slow network produces and the one where a state machine that
 * keeps something in a local variable falls apart. A decoder can be perfectly
 * correct on whole inputs and wrong on every real connection.
 */

#include "../http/chunked.h"

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

/* Decode a whole literal in one call. `text` is copied, because the decoder
 * writes over its input. */
static int whole(const char *text, char *into, size_t room, size_t *out,
                 size_t *in)
{
	struct http_chunked dec;
	size_t len = strlen(text);
	int rc;

	memcpy(into, text, len);
	http_chunked_begin(&dec);
	rc = http_chunked_feed(&dec, into, len, room);
	if (out)
		*out = dec.out;
	if (in)
		*in = dec.in;
	return rc;
}

/*
 * The same input, delivered one byte at a time.
 *
 * `have` grows by one and the decoder is called again, which is exactly what
 * `serve.c` does as bytes arrive. A decoder that was rewriting its input
 * buffer incorrectly, or keeping a partial chunk size in a local, passes the
 * whole-input case and fails this one.
 */
static int dribbled(const char *text, char *into, size_t room, size_t *out,
                    size_t *in)
{
	struct http_chunked dec;
	size_t len = strlen(text);
	size_t at;
	int rc = HTTP_PARTIAL;

	memcpy(into, text, len);
	http_chunked_begin(&dec);
	for (at = 1; at <= len; at++) {
		rc = http_chunked_feed(&dec, into, at, room);
		if (rc != HTTP_PARTIAL)
			break;
	}
	if (out)
		*out = dec.out;
	if (in)
		*in = dec.in;
	return rc;
}

/* A body that must decode, checked both ways, with its result. */
static void decodes(const char *text, const char *wanted, const char *what)
{
	char buf[1024];
	size_t out = 0, in = 0;
	size_t want = strlen(wanted);

	checks++;
	if (whole(text, buf, sizeof(buf), &out, &in) != HTTP_OK) {
		failures++;
		printf("  FAIL  %s -- not accepted\n", what);
		return;
	}
	checks++;
	if (out != want || memcmp(buf, wanted, want) != 0) {
		failures++;
		printf("  FAIL  %s -- decoded %lu bytes, wanted %lu\n", what,
		       (unsigned long)out, (unsigned long)want);
		return;
	}
	checks++;
	if (in != strlen(text)) {
		failures++;
		printf("  FAIL  %s -- consumed %lu of %lu\n", what,
		       (unsigned long)in, (unsigned long)strlen(text));
	}

	/* And again, a byte at a time. */
	checks++;
	if (dribbled(text, buf, sizeof(buf), &out, &in) != HTTP_OK) {
		failures++;
		printf("  FAIL  %s -- not accepted a byte at a time\n", what);
		return;
	}
	checks++;
	if (out != want || memcmp(buf, wanted, want) != 0) {
		failures++;
		printf("  FAIL  %s -- decoded differently a byte at a time\n",
		       what);
	}
}

/* A body that must be refused, checked both ways. */
static void refuses(const char *text, int verdict, const char *what)
{
	char buf[1024];
	int got;

	checks++;
	got = whole(text, buf, sizeof(buf), 0, 0);
	if (got != verdict) {
		failures++;
		printf("  FAIL  %s -- verdict %d, wanted %d\n", what, got,
		       verdict);
	}

	checks++;
	got = dribbled(text, buf, sizeof(buf), 0, 0);
	if (got != verdict) {
		failures++;
		printf("  FAIL  %s -- a byte at a time gave %d, wanted %d\n",
		       what, got, verdict);
	}
}

int main(void)
{
	printf("reading a chunked body, and every way of framing one twice\n");

	/* --- bodies that must decode ------------------------------------------ */
	decodes("5\r\nhello\r\n0\r\n\r\n", "hello", "one chunk");
	decodes("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", "hello world",
	        "two chunks, joined with nothing between them");
	decodes("0\r\n\r\n", "", "an empty body is a body");
	decodes("1\r\na\r\n1\r\nb\r\n1\r\nc\r\n0\r\n\r\n", "abc",
	        "a chunk per byte");

	/* Hex, and both cases of it. `A` and `a` are the same size and a
	 * decoder that took only one of them would drop bodies from whichever
	 * client wrote the other. */
	decodes("A\r\n0123456789\r\n0\r\n\r\n", "0123456789",
	        "an upper-case hex size");
	decodes("a\r\n0123456789\r\n0\r\n\r\n", "0123456789",
	        "a lower-case hex size");
	decodes("1f\r\n0123456789012345678901234567890\r\n0\r\n\r\n",
	        "0123456789012345678901234567890", "a two-digit hex size");

	/* Leading zeros are legal: the grammar is `1*HEXDIG` and says nothing
	 * about a canonical spelling. Bounded, not refused -- see the digit
	 * limit below. */
	decodes("005\r\nhello\r\n0\r\n\r\n", "hello",
	        "a size with leading zeros");
	decodes("5\r\nhello\r\n000\r\n\r\n", "hello",
	        "and a last chunk with leading zeros");

	/* Bytes that are not text. A body is bytes, and a decoder that treated
	 * it as a string would stop at the first NUL and frame the rest of the
	 * message as something else. */
	{
		char buf[64];
		static const char WITH_NUL[] = "5\r\na\0b\0c\r\n0\r\n\r\n";
		struct http_chunked dec;

		memcpy(buf, WITH_NUL, sizeof(WITH_NUL) - 1);
		http_chunked_begin(&dec);
		ok(http_chunked_feed(&dec, buf, sizeof(WITH_NUL) - 1,
		                     sizeof(buf)) == HTTP_OK,
		   "a chunk containing NUL bytes is a chunk");
		ok(dec.out == 5 && memcmp(buf, "a\0b\0c", 5) == 0,
		   "and they arrive unchanged");
	}

	/* --- trailers ---------------------------------------------------------- */
	decodes("5\r\nhello\r\n0\r\nX-Checksum: 42\r\n\r\n", "hello",
	        "a trailer after the last chunk");
	decodes("5\r\nhello\r\n0\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n", "hello",
	        "several trailers");

	/*
	 * The one that decides where the next request starts.
	 *
	 * A decoder that stopped at `0\r\n` would leave `X-A: 1\r\n\r\n` in the
	 * buffer, and on a keep-alive connection the next thing to read it is
	 * the parser looking for a request line. `X-A: 1` is not a request
	 * line, so the client gets a 400 for a request it sent correctly --
	 * and a cleverer trailer is a request line.
	 */
	{
		char buf[256];
		size_t out = 0, in = 0;
		static const char BODY[] =
			"5\r\nhello\r\n0\r\nX-A: 1\r\n\r\nGET /next HTTP/1.1\r\n";

		ok(whole(BODY, buf, sizeof(buf), &out, &in) == HTTP_OK,
		   "a body followed by another request decodes");
		ok(out == 5, "with only its own five bytes");
		ok(in == sizeof("5\r\nhello\r\n0\r\nX-A: 1\r\n\r\n") - 1,
		   "and consumes the trailer section but not a byte more");
	}

	/* --- and every way of getting the framing wrong ------------------------ */

	/*
	 * A bare LF where CRLF belongs.
	 *
	 * The oldest smuggling trick there is: a permissive reader takes `5\n`
	 * as a chunk header and a strict one waits for the CR, so the two
	 * disagree about every byte after it. `request.c` refuses a bare CR in
	 * a header for the same reason.
	 */
	refuses("5\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a bare LF after the size");
	refuses("5\r\nhello\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a bare LF after the data");
	refuses("5\r\nhello\r\n0\n\r\n", HTTP_EMALFORMED,
	        "a bare LF after the last chunk");
	refuses("5\r\nhello\r\n0\r\n\n", HTTP_EMALFORMED,
	        "a bare LF ending the trailer section");
	refuses("\n", HTTP_EMALFORMED, "a bare LF where a size belongs");

	/* A chunk extension. Legal syntax, ignorable by permission, and the
	 * one place in this format where a parser is invited to read past text
	 * it does not understand. See `chunked.h`. */
	refuses("5;a=b\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a chunk extension");
	refuses("5;\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "an empty chunk extension");
	refuses("0;a=b\r\n\r\n", HTTP_EMALFORMED,
	        "an extension on the last chunk");

	/*
	 * Sizes that `strtoul` would accept and a stricter reader would not.
	 *
	 * Every one of these is a size two implementations read differently,
	 * which is the whole mechanism. `+5` is five to one reader and a
	 * malformed line to another; `0x5` is five to one and zero -- the
	 * *last chunk* -- to another, which ends the body in one and not the
	 * other.
	 */
	refuses("+5\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED, "a size with a +");
	refuses("-5\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED, "a negative size");
	refuses("0x5\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a size with an 0x prefix -- which is also a last chunk to a "
	        "reader that stops at the x");
	refuses(" 5\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a size with a space in front");
	refuses("5 \r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a size with a space behind");
	refuses("5\t\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a size with a tab behind");
	refuses("\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a size line with no digits at all");
	refuses("g\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a size that is not hexadecimal");

	/* A size longer than any body can be. Refused on the digits rather
	 * than after an overflow, because a wrapped size is a body length
	 * nobody sent. */
	refuses("00000000000000005\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a size padded past the digit bound");
	refuses("ffffffffffffffff\r\n", HTTP_EBODY_LONG,
	        "a size at the top of the range is refused for its size, "
	        "not its digits");

	/* The data and its terminator. A chunk one byte short eats the CR and
	 * every byte after it is framed one place out. */
	refuses("5\r\nhell\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a chunk shorter than it said");
	refuses("4\r\nhello\r\n0\r\n\r\n", HTTP_EMALFORMED,
	        "a chunk longer than it said");
	refuses("5\r\nhelloXX0\r\n\r\n", HTTP_EMALFORMED,
	        "data not followed by CRLF");
	refuses("5\r\nhello\rX0\r\n\r\n", HTTP_EMALFORMED,
	        "a CR that is not followed by an LF");

	/* --- what is not an answer yet ----------------------------------------- */
	{
		char buf[128];

		ok(whole("5\r\nhello", buf, sizeof(buf), 0, 0) == HTTP_PARTIAL,
		   "a chunk that has not finished is not a refusal");
		ok(whole("5\r\nhello\r\n", buf, sizeof(buf), 0, 0)
		   == HTTP_PARTIAL, "nor is a body with no terminator");
		ok(whole("", buf, sizeof(buf), 0, 0) == HTTP_PARTIAL,
		   "nor is nothing at all");

		/*
		 * The one that would be easy to get wrong, and dangerous.
		 *
		 * `0\r\n` is the last chunk and **is not the end of the body**:
		 * the trailer section still has to end. A decoder that
		 * answered `HTTP_OK` here would hand whatever follows to the
		 * next reader -- and what follows is attacker-chosen text
		 * arriving where a request line is expected.
		 */
		ok(whole("5\r\nhello\r\n0\r\n", buf, sizeof(buf), 0, 0)
		   == HTTP_PARTIAL,
		   "the last chunk alone does not end the body");
		ok(whole("5\r\nhello\r\n0\r\nX-A: 1\r\n", buf, sizeof(buf), 0, 0)
		   == HTTP_PARTIAL,
		   "nor does a trailer without the blank line after it");
	}

	/* --- bounds -------------------------------------------------------------- */
	{
		/*
		 * Large enough for the *encoded* text, which is not the same
		 * number as the room given to the decoder. The first version
		 * of this block conflated them -- a 256-byte buffer handed a
		 * kilobyte of trailers -- and the suite died in `memcpy`
		 * before reaching a single check. `room` below is deliberately
		 * small; the buffer holding the message is not.
		 */
		char buf[2048];
		static char TOO_MANY[1024];
		size_t at = 0;
		int i;

		/* A body past what the caller will hold. Refused as a body
		 * that is too long, which is a 413, rather than as a malformed
		 * one, which is a 400 -- the client's message was fine and its
		 * size was not. */
		ok(whole("20\r\n01234567890123456789012345678901\r\n0\r\n\r\n",
		         buf, 16, 0, 0) == HTTP_EBODY_LONG,
		   "a chunk larger than the room is refused as too long");
		ok(whole("8\r\n01234567\r\n8\r\n89012345\r\n0\r\n\r\n",
		         buf, 12, 0, 0) == HTTP_EBODY_LONG,
		   "and so is a body that only exceeds it across two chunks");

		/* More trailers than this holds. A refusal, because a trailer
		 * section with no bound is a connection somebody keeps open by
		 * sending header lines for ever. */
		at += (size_t)sprintf(TOO_MANY + at, "0\r\n");
		for (i = 0; i < HTTP_CHUNK_TRAILERS_MAX + 1; i++)
			at += (size_t)sprintf(TOO_MANY + at, "X-%d: y\r\n", i);
		sprintf(TOO_MANY + at, "\r\n");
		ok(whole(TOO_MANY, buf, sizeof(buf), 0, 0) == HTTP_EMALFORMED,
		   "more trailers than this server holds");

		/* A trailer line longer than the bound. */
		at = (size_t)sprintf(TOO_MANY, "0\r\nX-Long: ");
		for (i = 0; i < HTTP_CHUNK_LINE_MAX + 8; i++)
			TOO_MANY[at++] = 'y';
		sprintf(TOO_MANY + at, "\r\n\r\n");
		ok(whole(TOO_MANY, buf, sizeof(buf), 0, 0) == HTTP_EMALFORMED,
		   "a trailer line past the bound");
	}

	/* --- starting again ------------------------------------------------------
	 *
	 * The connection pool reuses its structures, so a decoder that kept
	 * anything across a `begin` would carry one request's body into the
	 * next one on the same connection. */
	{
		char buf[128];
		struct http_chunked dec;

		memcpy(buf, "5\r\nhello\r\n0\r\n\r\n", 15);
		http_chunked_begin(&dec);
		ok(http_chunked_feed(&dec, buf, 15, sizeof(buf)) == HTTP_OK,
		   "one body decodes");

		memcpy(buf, "3\r\nbye\r\n0\r\n\r\n", 13);
		http_chunked_begin(&dec);
		ok(http_chunked_feed(&dec, buf, 13, sizeof(buf)) == HTTP_OK,
		   "and the same decoder, begun again, reads another");
		ok(dec.out == 3 && memcmp(buf, "bye", 3) == 0,
		   "with nothing left over from the first");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
