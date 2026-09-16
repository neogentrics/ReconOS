/*
 * One HTTP request, and every way of writing two.
 *
 * There is no host function to be differential against -- POSIX does not parse
 * HTTP -- so this is a table of messages and the verdict each must draw. The
 * accepting cases are here to prove the parser is usable. **The refusing cases
 * are the point.**
 *
 * Nearly every entry below is a real technique rather than an invented one:
 * request smuggling by double framing, header injection through a value,
 * `%2e%2e%2f` traversal, the NUL-byte extension trick, a space before a colon.
 * A parser that accepts any of them is not slightly wrong.
 *
 * **Watched failing first.** The suite was run against a parser with the
 * duplicate-`Content-Length` check removed and the `..` climb clamped to the
 * root instead of refused -- the two shortcuts a reasonable person would take
 * -- and the smuggling and traversal cases failed as they should before the
 * real parser was believed.
 */

#include "../http/http.h"

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

/* `msg` is a C string here only for convenience; the parser is handed its
 * measured length and never relies on the terminator. */
static int verdict(const char *msg, struct http_request *into)
{
	return http_request_parse(msg, strlen(msg), into);
}

static void refuses(const char *msg, int why, const char *what)
{
	struct http_request r;
	int rc = verdict(msg, &r);

	checks++;
	if (rc != why) {
		failures++;
		printf("  FAIL  %s: got %d, wanted %d\n", what, rc, why);
	}
}

static void accepts(const char *msg, const char *method, const char *target,
                    const char *query)
{
	struct http_request r;
	int rc = verdict(msg, &r);

	checks++;
	if (rc != HTTP_OK) {
		failures++;
		printf("  FAIL  \"%s ...\" refused with %d\n", method, rc);
		return;
	}
	if (strcmp(r.method, method) != 0 || strcmp(r.target, target) != 0
	    || strcmp(r.query, query) != 0) {
		failures++;
		printf("  FAIL  parsed (%s, %s, %s), wanted (%s, %s, %s)\n",
		       r.method, r.target, r.query, method, target, query);
	}
}

int main(void)
{
	struct http_request r;

	printf("one HTTP request, and every way of writing two\n");

	/* --- ordinary requests --------------------------------------------- */
	accepts("GET / HTTP/1.1\r\nHost: m16\r\n\r\n", "GET", "/", "");
	accepts("GET /dashboard HTTP/1.1\r\nHost: m16\r\n\r\n",
	        "GET", "/dashboard", "");
	accepts("HEAD /a/b/c.html HTTP/1.1\r\nHost: m16\r\n\r\n",
	        "HEAD", "/a/b/c.html", "");
	accepts("GET /api/v1/status?verbose=1&fmt=json HTTP/1.1\r\nHost: m16\r\n\r\n",
	        "GET", "/api/v1/status", "verbose=1&fmt=json");
	accepts("POST /api/v1/service HTTP/1.1\r\nHost: m16\r\n"
	        "Content-Length: 0\r\n\r\n", "POST", "/api/v1/service", "");

	/* A bare LF is accepted as a line ending; plenty of clients send it. */
	accepts("GET / HTTP/1.1\nHost: m16\n\n", "GET", "/", "");

	/* HTTP/1.0 is understood, and does not keep the connection. */
	{
		ok(verdict("GET / HTTP/1.0\r\n\r\n", &r) == HTTP_OK
		   && r.minor == 0 && r.keep_alive == 0,
		   "HTTP/1.0 closes by default");
		ok(verdict("GET / HTTP/1.1\r\nHost: m16\r\n\r\n", &r) == HTTP_OK
		   && r.keep_alive == 1,
		   "HTTP/1.1 keeps by default");
		ok(verdict("GET / HTTP/1.1\r\nHost: m16\r\n"
		           "Connection: close\r\n\r\n", &r) == HTTP_OK
		   && r.keep_alive == 0,
		   "Connection: close is honoured");
		ok(verdict("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n",
		           &r) == HTTP_OK && r.keep_alive == 1,
		   "Connection is matched without regard to case");
	}

	/* --- percent-decoding, and normalising ----------------------------- */
	accepts("GET /a%20b HTTP/1.1\r\nHost: m16\r\n\r\n", "GET", "/a b", "");
	accepts("GET /a//b///c HTTP/1.1\r\nHost: m16\r\n\r\n", "GET", "/a/b/c", "");
	accepts("GET /a/./b HTTP/1.1\r\nHost: m16\r\n\r\n", "GET", "/a/b", "");
	accepts("GET /a/b/.. HTTP/1.1\r\nHost: m16\r\n\r\n", "GET", "/a", "");
	accepts("GET /a/b/../ HTTP/1.1\r\nHost: m16\r\n\r\n", "GET", "/a", "");
	/* The query is kept raw. Decoding it here would make a '&' inside a
	 * value indistinguishable from the separator between values. */
	accepts("GET /s?q=a%26b HTTP/1.1\r\nHost: m16\r\n\r\n",
	        "GET", "/s", "q=a%26b");

	/* --- traversal ------------------------------------------------------ */
	refuses("GET /../etc/passwd HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_ETRAVERSAL, "a plain .. above the root");
	refuses("GET /a/../../etc HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_ETRAVERSAL, "a .. that climbs out after descending");
	refuses("GET /%2e%2e/etc HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_ETRAVERSAL, "an encoded .. -- decoded before normalising");
	refuses("GET /%2e%2e%2fetc HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_ETRAVERSAL, "an encoded ../ with the slash encoded too");
	refuses("GET /a\\..\\b HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_ETRAVERSAL, "a backslash is refused, not translated");

	/* The NUL-byte trick: a name that passes an extension check and opens
	 * something else the moment it reaches a C interface. */
	refuses("GET /safe.html%00.txt HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_EMALFORMED, "%00 in a target");

	/* A % that is not two hex digits is refused rather than passed on. */
	refuses("GET /a%zz HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_EMALFORMED, "% followed by non-hex");
	refuses("GET /a%4 HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_EMALFORMED, "% with one digit before the end");
	refuses("GET /a% HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_EMALFORMED, "% at the very end");

	/* --- request smuggling ---------------------------------------------- */
	refuses("POST / HTTP/1.1\r\nHost: m16\r\nContent-Length: 6\r\n"
	        "Transfer-Encoding: chunked\r\n\r\n",
	        HTTP_ESMUGGLE, "Content-Length and Transfer-Encoding together");
	refuses("POST / HTTP/1.1\r\nHost: m16\r\n"
	        "Transfer-Encoding: chunked\r\n\r\n",
	        HTTP_ESMUGGLE, "Transfer-Encoding at all -- chunked is not implemented");
	refuses("POST / HTTP/1.1\r\nHost: m16\r\nContent-Length: 6\r\n"
	        "Content-Length: 5\r\n\r\n",
	        HTTP_ESMUGGLE, "two Content-Lengths that disagree");
	refuses("POST / HTTP/1.1\r\nHost: m16\r\nContent-Length: 6\r\n"
	        "Content-Length: 6\r\n\r\n",
	        HTTP_ESMUGGLE, "two Content-Lengths that agree -- still two");

	/* A length that is not a plain number. `strtoul` accepts every one of
	 * these and reports success. */
	refuses("POST / HTTP/1.1\r\nHost: m16\r\nContent-Length: +5\r\n\r\n",
	        HTTP_EMALFORMED, "a signed Content-Length");
	refuses("POST / HTTP/1.1\r\nHost: m16\r\nContent-Length: 5abc\r\n\r\n",
	        HTTP_EMALFORMED, "a Content-Length with a tail");
	refuses("POST / HTTP/1.1\r\nHost: m16\r\nContent-Length: 0x10\r\n\r\n",
	        HTTP_EMALFORMED, "a hexadecimal Content-Length");
	refuses("POST / HTTP/1.1\r\nHost: m16\r\nContent-Length: \r\n\r\n",
	        HTTP_EMALFORMED, "an empty Content-Length");
	refuses("POST / HTTP/1.1\r\nHost: m16\r\n"
	        "Content-Length: 99999999999999999999\r\n\r\n",
	        HTTP_EBODY_LONG, "a Content-Length that overflows");

	/* A body-bearing method with no framing at all. */
	refuses("POST / HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_EMALFORMED, "POST with no Content-Length");

	/* --- header syntax --------------------------------------------------- */
	refuses("GET / HTTP/1.1\r\nHost : m16\r\n\r\n",
	        HTTP_EMALFORMED, "a space before the colon");
	refuses("GET / HTTP/1.1\r\nHost\r\n\r\n",
	        HTTP_EMALFORMED, "a header with no colon");
	refuses("GET / HTTP/1.1\r\n: m16\r\n\r\n",
	        HTTP_EMALFORMED, "a header with an empty name");
	refuses("GET / HTTP/1.1\r\nHost: m16\r\n X-Extra: 1\r\n\r\n",
	        HTTP_EMALFORMED, "obs-fold is refused, not unfolded");
	refuses("GET / HTTP/1.1\r\nHo st: m16\r\n\r\n",
	        HTTP_EMALFORMED, "a space inside a field name");

	/* Values are trimmed of surrounding space and kept otherwise verbatim. */
	{
		ok(verdict("GET / HTTP/1.1\r\nHost:   m16   \r\n\r\n", &r)
		   == HTTP_OK
		   && strcmp(http_header_get(&r, "host"), "m16") == 0,
		   "a value is trimmed of surrounding whitespace");
		ok(verdict("GET / HTTP/1.1\r\nHOST: m16\r\n\r\n", &r) == HTTP_OK
		   && http_header_get(&r, "host") != 0,
		   "a field name is matched without regard to case");
		ok(http_header_get(&r, "x-absent") == 0,
		   "an absent header reads as absent");
	}

	/* --- the request line ------------------------------------------------ */
	refuses("GET  / HTTP/1.1\r\n\r\n", HTTP_EMALFORMED, "two spaces after the method");
	refuses(" GET / HTTP/1.1\r\n\r\n", HTTP_EMALFORMED, "a leading space");
	refuses("GET /\r\n\r\n", HTTP_EMALFORMED, "no version");
	refuses("GET / HTTP/1.1 extra\r\n\r\n", HTTP_EMALFORMED, "a trailing field");
	refuses("GET / HTTP/2.0\r\n\r\n", HTTP_EVERSION, "a version this server does not speak");
	refuses("GET / HTTP/1.9\r\n\r\n", HTTP_EVERSION, "a 1.x minor that does not exist");
	refuses("GET / HTTP/ONE\r\n\r\n", HTTP_EMALFORMED, "a version that is not a number");
	refuses("GET http://elsewhere/ HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_EMALFORMED, "absolute-form -- this is not a proxy");
	refuses("DELETE / HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_EMETHOD, "a method this server does not implement");
	refuses("get / HTTP/1.1\r\nHost: m16\r\n\r\n",
	        HTTP_EMETHOD, "a method is case-sensitive");

	/* --- incomplete, which is not an error -------------------------------- */
	refuses("GET / HTTP/1.1\r\n", HTTP_PARTIAL, "no blank line yet");
	refuses("GET / HTTP/1.1\r\nHost: m1", HTTP_PARTIAL, "a header cut in half");
	refuses("GE", HTTP_PARTIAL, "two bytes");
	refuses("", HTTP_PARTIAL, "nothing at all");

	/* A bare CR is refused. It is the byte used to forge a line ending. */
	refuses("GET / HTTP/1.1\rHost: m16\r\n\r\n",
	        HTTP_EMALFORMED, "a bare CR mid-line");

	/* A NUL inside the head. The parser is given a length, so this is a
	 * byte in the message rather than the end of it. */
	{
		const char msg[] = "GET / HTTP/1.1\r\nHost: m\0 16\r\n\r\n";

		checks++;
		if (http_request_parse(msg, sizeof(msg) - 1, &r)
		    != HTTP_EMALFORMED) {
			failures++;
			printf("  FAIL  a NUL inside the head\n");
		}
	}

	/* --- bounds ----------------------------------------------------------- */
	{
		static char big[HTTP_REQUEST_MAX + 64];
		size_t i;

		/* More headers than there is room for. */
		{
			static char many[HTTP_REQUEST_MAX];
			int n = 0;
			size_t at = 0;
			const char *head = "GET / HTTP/1.1\r\n";

			for (i = 0; head[i]; i++)
				many[at++] = head[i];
			while (n < HTTP_HEADERS_MAX + 4
			       && at < sizeof(many) - 32) {
				at += (size_t)snprintf(many + at,
				                       sizeof(many) - at,
				                       "X-N%d: v\r\n", n);
				n++;
			}
			many[at++] = '\r';
			many[at++] = '\n';
			ok(http_request_parse(many, at, &r) == HTTP_ETOOMANY,
			   "more headers than there is room for");
		}

		/* A request line longer than the buffer. */
		for (i = 0; i < sizeof(big); i++)
			big[i] = 'a';
		ok(http_request_parse(big, sizeof(big), &r) == HTTP_ELINE_LONG,
		   "a head larger than HTTP_REQUEST_MAX");
	}

	/* --- statuses ---------------------------------------------------------- */
	ok(http_status_for(HTTP_ESMUGGLE) == 400, "smuggling answers 400");
	ok(http_status_for(HTTP_EMETHOD) == 501, "an unimplemented method answers 501");
	ok(http_status_for(HTTP_EVERSION) == 505, "a bad version answers 505");
	ok(http_status_for(HTTP_ETOOMANY) == 431, "too many headers answers 431");
	ok(http_status_for(HTTP_EBODY_LONG) == 413, "an over-long body answers 413");
	ok(http_status_for(HTTP_OK) == 0, "a good request has no error status");
	ok(http_status_for(HTTP_PARTIAL) == 0, "an incomplete request has no status");
	ok(strcmp(http_reason(404), "Not Found") == 0, "404 has its phrase");
	ok(strcmp(http_reason(999), "Unknown") != 0 ? 0 : 1,
	   "an unknown status still has a phrase");

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
