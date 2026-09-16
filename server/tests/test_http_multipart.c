/*
 * Reading a multipart body, and the two bytes that ruin a file.
 *
 * Pure: a body in, parts out, no sockets. So every shape a client can send is
 * driven here directly, including the ones a browser will not produce and an
 * attacker will.
 *
 * **The case this exists for** is the CRLF before a delimiter. A parser that
 * searches for `--<boundary>` and keeps everything before it returns a part two
 * bytes too long, every time, with no error anywhere. On a text field nobody
 * notices. On an upload it is a corrupt file, and the corruption is at the end,
 * where a person checking the first few bytes will not look.
 *
 * The rest is the refusal list from `multipart.h`, each one a thing that an
 * implementation doing its best to be helpful would accept.
 *
 * **Watched failing first**, against a parser written the obvious way: three
 * changes -- splitting on `--<boundary>` rather than `CRLF--<boundary>`,
 * accepting any filename, and taking the first `name` when two arrive. **12 of
 * 71 checks failed.**
 *
 * Worth reading the list those twelve make. Seven are lengths: every single
 * value came back two bytes longer than it was sent, including the empty one,
 * which came back as two bytes of line ending. Not one of them is an error the
 * parser could have reported -- it returned HTTP_OK every time and handed back
 * data that was merely wrong. That is the whole argument for checking the
 * length of a value and not just its presence.
 */

#include "../http/multipart.h"

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

/* --- boundaries -------------------------------------------------------------- */

static void boundary_is(const char *ct, const char *want, const char *what)
{
	char out[128];
	int rc = http_multipart_boundary(ct, out, sizeof(out));

	checks++;
	if (rc != HTTP_OK) {
		failures++;
		printf("  FAIL  %s: refused (%d)\n", what, rc);
		return;
	}
	if (strcmp(out, want) != 0) {
		failures++;
		printf("  FAIL  %s: got [%s], wanted [%s]\n", what, out, want);
	}
}

static void boundary_refused(const char *ct, const char *what)
{
	char out[128];

	ok(http_multipart_boundary(ct, out, sizeof(out)) != HTTP_OK, what);
}

/* --- bodies -------------------------------------------------------------------
 *
 * Written out byte for byte rather than assembled by a helper. A helper that
 * builds a body inserts the delimiters itself, which means the suite and the
 * parser would share an idea of where a delimiter goes -- and that shared idea
 * is precisely the thing under test. */

static struct http_multipart M;

static int parse(const char *body, size_t len, const char *b)
{
	return http_multipart_parse(body, len, b, &M);
}

int main(void)
{
	printf("reading a multipart body, and the two bytes that ruin a file\n");

	/* --- pulling the boundary out of a header ------------------------------ */
	boundary_is("multipart/form-data; boundary=abc", "abc", "a plain boundary");
	boundary_is("multipart/form-data; boundary=\"abc\"", "abc",
	            "a quoted boundary");
	boundary_is("Multipart/Form-Data; BOUNDARY=abc", "abc",
	            "the type and the parameter name are case-insensitive");
	boundary_is("multipart/form-data;boundary=abc", "abc", "no space");
	boundary_is("multipart/form-data; charset=utf-8; boundary=abc", "abc",
	            "after another parameter");
	boundary_is("multipart/form-data; boundary=----WebKitFormBoundaryaZ09",
	            "----WebKitFormBoundaryaZ09", "what a browser actually sends");

	/* A quoted boundary may hold characters an unquoted one may not. */
	boundary_is("multipart/form-data; boundary=\"a:b/c\"", "a:b/c",
	            "the punctuation the spec allows");

	boundary_refused("application/x-www-form-urlencoded",
	                 "the other form encoding is not this one");
	boundary_refused("multipart/mixed; boundary=abc",
	                 "multipart/mixed has the same shape and different rules");
	boundary_refused("multipart/form-data", "no boundary at all");
	boundary_refused("multipart/form-data; boundary=", "an empty boundary");
	boundary_refused("multipart/form-data; boundary=\"abc",
	                 "a quoted boundary that never closes");
	boundary_refused("multipart/form-data; boundary=a\"b",
	                 "a quote inside an unquoted boundary");
	boundary_refused("multipart/form-data; boundary=\"ab \"",
	                 "a boundary ending in a space can never be matched");

	/*
	 * Two boundaries, and the second is the whole point.
	 *
	 * A proxy taking the first and this taking the last would split the
	 * same body in two different places. That is request smuggling one
	 * layer down, and it is refused even when the two agree -- the fault is
	 * that there is something to disagree about.
	 */
	boundary_refused("multipart/form-data; boundary=a; boundary=b",
	                 "two boundaries");
	boundary_refused("multipart/form-data; boundary=a; boundary=a",
	                 "two boundaries that agree -- still refused");

	{
		char out[128];
		/* 71 characters: one past what RFC 2046 allows. */
		const char *too_long =
		    "multipart/form-data; boundary="
		    "01234567890123456789012345678901234567890123456789"
		    "012345678901234567890";

		ok(http_multipart_boundary(too_long, out, sizeof(out))
		   == HTTP_EFIELD_LONG, "a boundary past 70 characters");
		ok(http_multipart_boundary("multipart/form-data; boundary=abcdef",
		                           out, 4) == HTTP_EFIELD_LONG,
		   "a boundary that does not fit the caller's buffer");
		ok(http_multipart_boundary(0, out, sizeof(out)) != HTTP_OK,
		   "no header");
	}

	/* --- one ordinary field ------------------------------------------------- */
	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"greeting\"\r\n"
		    "\r\n"
		    "hello\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_OK,
		   "a body with one field");
		ok(M.count == 1, "one part");
		if (M.count == 1) {
			ok(strcmp(M.parts[0].name, "greeting") == 0,
			   "the field's name");
			/*
			 * Five, not seven. The CRLF before the delimiter ends
			 * the part; it is not part of the value. This is the
			 * check the file exists for.
			 */
			ok(M.parts[0].data_len == 5,
			   "the value stops before the delimiter's CRLF");
			ok(M.parts[0].data_len == 5
			   && memcmp(M.parts[0].data, "hello", 5) == 0,
			   "and holds exactly what was sent");
			ok(!M.parts[0].has_filename, "and is not a file");
		}
	}

	/* --- a value that is nothing but a CRLF ----------------------------------
	 *
	 * The case that separates a correct delimiter from a nearly correct
	 * one. The data is two bytes; a parser that eats the delimiter's CRLF
	 * from the wrong end returns zero. */
	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "\r\n"
		    "\r\n"
		    "\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_OK,
		   "a value made of line endings");
		ok(M.count == 1 && M.parts[0].data_len == 2,
		   "two bytes of value survive, and the delimiter's do not");
	}

	/* --- an empty value ------------------------------------------------------ */
	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "\r\n"
		    "\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_OK,
		   "an empty value is legal");
		ok(M.count == 1 && M.parts[0].data_len == 0, "and is empty");
	}

	/* --- binary data, including a NUL ---------------------------------------
	 *
	 * The reason nothing in this parser uses the string functions. A body
	 * carrying a NUL is an ordinary upload, and a parser that stops there
	 * covers less of the body than it reports. */
	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"f\";"
		    " filename=\"a.bin\"\r\n"
		    "Content-Type: application/octet-stream\r\n"
		    "\r\n"
		    "\x01\x00\x02\r\n"
		    "--b--\r\n";
		/* sizeof - 1 would stop at the NUL if this were a string
		 * literal length; it is not -- the array holds the NUL. */
		static const size_t len = sizeof(body) - 1;

		ok(parse(body, len, "b") == HTTP_OK, "a part holding a NUL");
		ok(M.count == 1 && M.parts[0].data_len == 3,
		   "all three bytes, the NUL among them");
		ok(M.count == 1 && M.parts[0].has_filename
		   && strcmp(M.parts[0].filename, "a.bin") == 0,
		   "the filename");
		ok(M.count == 1
		   && strcmp(M.parts[0].content_type,
		             "application/octet-stream") == 0,
		   "and the part's own content type");
	}

	/* --- several parts -------------------------------------------------------- */
	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"one\"\r\n"
		    "\r\n"
		    "1\r\n"
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"two\"\r\n"
		    "\r\n"
		    "2\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_OK, "two parts");
		ok(M.count == 2, "both read");
		ok(http_multipart_get(&M, "one") != 0
		   && http_multipart_get(&M, "two") != 0,
		   "and both can be looked up");
		ok(http_multipart_get(&M, "three") == 0, "an absent name");
	}

	/* --- a name given twice ---------------------------------------------------
	 *
	 * `form.h`'s rule, applied here. NULL for absent and NULL for
	 * ambiguous, with the count to tell them apart. */
	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "\r\n"
		    "1\r\n"
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "\r\n"
		    "2\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_OK,
		   "a body may carry a name twice");
		ok(http_multipart_count(&M, "x") == 2, "and the count says so");
		ok(http_multipart_get(&M, "x") == 0,
		   "but there is no single value, so there is no value");
	}

	/* --- a delimiter that appears inside data ---------------------------------
	 *
	 * Legal, and the sender is responsible for picking a boundary its data
	 * does not contain. What must not happen is the *header* search finding
	 * a blank line from a later part. */
	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "\r\n"
		    "a\r\n\r\nb\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_OK,
		   "a value containing a blank line");
		ok(M.count == 1 && M.parts[0].data_len == 6,
		   "the whole value, blank line included");
	}

	/* --- a preamble ------------------------------------------------------------ */
	{
		static const char body[] =
		    "this is ignored by any conforming reader\r\n"
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_OK,
		   "a preamble before the first delimiter");
		ok(M.count == 1 && M.parts[0].data_len == 1,
		   "and the part after it is read normally");
	}

	/* --- the refusals ----------------------------------------------------------
	 *
	 * Each of these is something a parser trying to be helpful accepts. */

	{	/* Truncated: the close never arrives. A parser that returns
		 * what it has hands back a file that was cut off, and says
		 * nothing. */
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "\r\n"
		    "half a fi";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a body that stops in the middle of a part");
	}

	{	/* The last delimiter is an ordinary one, not the close. */
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a body whose last delimiter promises another part");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\";"
		    " filename=\"../../etc/passwd\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a filename that climbs -- refused, not reduced to its tail");
	}

	{	/* The shape a strip-once repair produces from something that
		 * still escapes. Refusing has no fixed point; repairing does. */
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\";"
		    " filename=\"....//evil\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "the string that survives being repaired once");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\";"
		    " filename=\"C:\\\\windows\\\\evil\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a backslash is a separator too");
	}

	{	/* `a.txt\0.exe`: checked as one name, used as another. Every
		 * check above this line passes on it. */
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\";"
		    " filename=\"a.txt\x00.exe\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a NUL inside a filename");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\";"
		    " filename=\"a\"; filename*=UTF-8''b\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "filename* beside filename -- two names for one file");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"; name=\"y\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "two names on one part");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "Content-Disposition: form-data; name=\"y\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "two dispositions on one part");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Type: text/plain\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a part with no disposition at all");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: attachment; name=\"x\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a disposition that is not form-data");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a part with no name");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a part whose name is empty");
	}

	{	/* Bare LF. Tolerated here and refused in `request.c` would mean
		 * two parsers in one server disagreeing about where a header
		 * ends. */
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a bare LF ending a part header");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition : form-data; name=\"x\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a space before the colon");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data;\r\n"
		    " name=\"x\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a header folded across two lines");
	}

	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a part with no blank line after its headers");
	}

	{
		static const char body[] = "there is no delimiter here at all";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a body that is not multipart");
	}

	{	/* The delimiter must be the whole boundary. `--bb` is not
		 * `--b` followed by data. */
		static const char body[] =
		    "--bb\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--bb--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EMALFORMED,
		   "a boundary that is a prefix of the one sent");
	}

	/* --- bounds ----------------------------------------------------------------- */
	{
		static const char body[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\""
		    "0123456789012345678901234567890123456789"
		    "0123456789012345678901234567890123456789\"\r\n"
		    "\r\n"
		    "v\r\n"
		    "--b--\r\n";

		ok(parse(body, sizeof(body) - 1, "b") == HTTP_EFIELD_LONG,
		   "a name past its bound is refused, not cut");
	}

	{	/* Seventeen parts against a bound of sixteen. */
		static const char one[] =
		    "--b\r\n"
		    "Content-Disposition: form-data; name=\"x\"\r\n"
		    "\r\n"
		    "v\r\n";
		char big[2048];
		size_t at = 0, i;

		for (i = 0; i < 17; i++) {
			memcpy(big + at, one, sizeof(one) - 1);
			at += sizeof(one) - 1;
		}
		memcpy(big + at, "--b--\r\n", 7);
		at += 7;

		ok(parse(big, at, "b") == HTTP_ETOOMANY,
		   "more parts than the bound allows");
	}

	/* --- nothing at all ---------------------------------------------------------- */
	ok(http_multipart_parse(0, 0, "b", &M) != HTTP_OK, "no body");
	ok(http_multipart_parse("x", 1, 0, &M) != HTTP_OK, "no boundary");
	ok(http_multipart_parse("x", 1, "b", 0) != HTTP_OK, "nowhere to put it");
	ok(http_multipart_parse("x", 1, "", &M) != HTTP_OK, "an empty boundary");
	ok(http_multipart_get(0, "x") == 0, "looking up in nothing");
	ok(http_multipart_count(0, "x") == 0, "counting nothing");

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
