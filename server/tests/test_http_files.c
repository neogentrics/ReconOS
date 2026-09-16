/*
 * Serving a file, and every way of serving the wrong one.
 *
 * The handler is called directly with a hand-built request rather than through
 * a socket. That is deliberate: the socket path already has its own suite, and
 * what is under test here is *which file gets opened and what comes back*, a
 * question a listener only makes slower to ask.
 *
 * **The cases that matter are the ones about a file that is not the one asked
 * for**: a path that climbs out of the root, a directory served as a listing,
 * a file cut short and answered 200, a binary file truncated at its first zero
 * byte. Each of those is a served file that is wrong rather than a served
 * error, which is the harder failure to notice.
 *
 * **Watched failing first, and the second attempt is the interesting one.**
 * Run against a handler with `escapes()` removed, the four traversal cases
 * failed as they should. Relaxing the over-size check to serve what fitted
 * changed nothing -- the suite still passed, because the handler guards that
 * twice: once where the file is read and again on the length before it is
 * answered. Only with *both* removed did the truncation case fail.
 *
 * That is worth writing down rather than tidying away. The doubled guard is
 * deliberate and it means no single edit to `files.c` can quietly start
 * serving cut files -- but it also means a suite run against one broken half
 * proves less than it appears to. A check that survives the removal of the
 * thing it is checking is not evidence about that thing.
 */

#include "../http/files.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

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

#define ROOT "/tmp/recon_files_test"

static void put(const char *rel, const void *data, size_t len)
{
	char p[512];
	int fd;

	snprintf(p, sizeof(p), "%s%s", ROOT, rel);
	fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		printf("  FAIL  could not create %s\n", p);
		failures++;
		return;
	}
	if (len && write(fd, data, len) != (ssize_t)len) {
		printf("  FAIL  short write creating %s\n", p);
		failures++;
	}
	close(fd);
}

/* Ask the handler for a path, as the parser would have handed it over: already
 * decoded, normalised and rooted. */
static int ask(const struct http_files *files, const char *target,
               struct http_response *out)
{
	struct http_request r;

	memset(&r, 0, sizeof(r));
	snprintf(r.method, sizeof(r.method), "GET");
	snprintf(r.target, sizeof(r.target), "%s", target);
	r.minor = 1;

	return http_files_handler(&r, 0, 0, out, (void *)files);
}

static void serves(const struct http_files *f, const char *target,
                   const char *expect, size_t expect_len, const char *type,
                   const char *what)
{
	struct http_response res;
	int rc = ask(f, target, &res);

	checks++;
	if (rc != HTTP_OK || res.status != 200) {
		failures++;
		printf("  FAIL  %s: rc=%d status=%d\n", what, rc, res.status);
		return;
	}
	if (res.body_len != expect_len
	    || memcmp(res.body, expect, expect_len) != 0) {
		failures++;
		printf("  FAIL  %s: got %lu bytes, wanted %lu\n", what,
		       (unsigned long)res.body_len, (unsigned long)expect_len);
		return;
	}
	if (type && strcmp(res.content_type, type) != 0) {
		failures++;
		printf("  FAIL  %s: type \"%s\", wanted \"%s\"\n", what,
		       res.content_type, type);
	}
}

static void answers(const struct http_files *f, const char *target,
                    int status, const char *what)
{
	struct http_response res;
	int rc = ask(f, target, &res);

	checks++;
	if (rc != HTTP_OK || res.status != status) {
		failures++;
		printf("  FAIL  %s: rc=%d status=%d, wanted %d\n", what, rc,
		       res.status, status);
	}
}

int main(void)
{
	struct http_files site = { ROOT, "index.html" };
	struct http_files noindex = { ROOT, 0 };
	static char big[HTTP_RESPONSE_MAX + 16];
	static const char PAGE[] = "<!doctype html><h1>M16</h1>\n";
	static const char PLAIN[] = "ok\n";

	printf("serving a file, and every way of serving the wrong one\n");

	mkdir(ROOT, 0755);
	mkdir(ROOT "/docs", 0755);
	mkdir(ROOT "/empty", 0755);

	put("/index.html", PAGE, sizeof(PAGE) - 1);
	put("/notes.txt", PLAIN, sizeof(PLAIN) - 1);
	put("/docs/index.html", "<h1>docs</h1>", 13);

	/* --- the ordinary cases -------------------------------------------- */
	serves(&site, "/index.html", PAGE, sizeof(PAGE) - 1,
	       "text/html; charset=utf-8", "a named page");
	serves(&site, "/notes.txt", PLAIN, sizeof(PLAIN) - 1,
	       "text/plain; charset=utf-8", "a text file");
	serves(&site, "/", PAGE, sizeof(PAGE) - 1, 0,
	       "the root serves its index");
	serves(&site, "/docs/", "<h1>docs</h1>", 13, 0,
	       "a directory serves its index");
	serves(&site, "/docs", "<h1>docs</h1>", 13, 0,
	       "a directory without its trailing slash still serves");

	/* --- a binary file, which is where a C string would betray it --------
	 *
	 * Every byte value, zeros included. A handler that measured this with
	 * `strlen` would serve three bytes and call it a success. */
	{
		static unsigned char blob[512];
		int i;

		for (i = 0; i < 512; i++)
			blob[i] = (unsigned char)(i & 0xFF);
		blob[3] = 0;
		blob[4] = 0;
		put("/logo.png", blob, sizeof(blob));

		serves(&site, "/logo.png", (const char *)blob, sizeof(blob),
		       "image/png", "a binary file survives its zero bytes");
	}

	/* --- what is not there ---------------------------------------------- */
	answers(&site, "/missing.html", 404, "a file that is not there");
	answers(&site, "/docs/missing.css", 404, "a missing file in a real directory");
	answers(&site, "/empty/", 404, "a directory with no index");
	answers(&noindex, "/", 404, "a site configured to refuse directories");

	/* --- the path may not leave the root ---------------------------------
	 *
	 * The parser refuses these before the handler ever sees them. They are
	 * asked here anyway, because the handler is a separate claim about the
	 * same string and one day it may be the only one left. */
	answers(&site, "/../etc/passwd", 403, "a climb above the root");
	answers(&site, "/docs/../../etc", 403, "a climb after descending");
	answers(&site, "/..", 403, "a bare climb");
	answers(&site, "/docs\\..\\x", 403, "a backslash climb");

	/* --- size --------------------------------------------------------- */
	{
		size_t i;

		for (i = 0; i < sizeof(big); i++)
			big[i] = 'a';

		/* Exactly the cap is served. */
		put("/exact.txt", big, HTTP_RESPONSE_MAX);
		{
			struct http_response res;
			int rc = ask(&site, "/exact.txt", &res);

			ok(rc == HTTP_OK && res.status == 200
			   && res.body_len == HTTP_RESPONSE_MAX,
			   "a file exactly at the cap is served whole");
		}

		/* One byte more is refused, not served short. A truncated file
		 * with a 200 beside it is a corrupt file that looks good. */
		put("/over.txt", big, HTTP_RESPONSE_MAX + 1);
		answers(&site, "/over.txt", 413,
		        "one byte over the cap is refused, not cut");
	}

	/* --- the content type table ------------------------------------------ */
	ok(strcmp(http_content_type("/a/b.html"), "text/html; charset=utf-8") == 0,
	   "html");
	ok(strcmp(http_content_type("/a/b.HTML"), "text/html; charset=utf-8") == 0,
	   "an extension is matched without regard to case");
	ok(strcmp(http_content_type("/a/b.css"), "text/css; charset=utf-8") == 0,
	   "css");
	ok(strcmp(http_content_type("/a/b.js"), "text/javascript; charset=utf-8") == 0,
	   "js");
	ok(strcmp(http_content_type("/a/b.json"), "application/json") == 0, "json");
	ok(strcmp(http_content_type("/a/b.svg"), "image/svg+xml") == 0, "svg");
	ok(strcmp(http_content_type("/a/b.png"), "image/png") == 0, "png");
	ok(strcmp(http_content_type("/a/b.woff2"), "font/woff2") == 0, "woff2");

	/* The cases a naive scan for '.' gets wrong. */
	ok(strcmp(http_content_type("/a.css/b"),
	          "application/octet-stream") == 0,
	   "a dot in a directory name is not the file's extension");
	ok(strcmp(http_content_type("/noext"),
	          "application/octet-stream") == 0,
	   "a name with no extension");
	ok(strcmp(http_content_type("/trailing."),
	          "application/octet-stream") == 0,
	   "a name ending in a dot");
	ok(strcmp(http_content_type("/archive.tar.gz"),
	          "application/octet-stream") == 0,
	   "an unknown extension is octet-stream, never guessed");

	/* Every text type carries a charset, or a browser guesses -- and a
	 * page of UTF-8 guessed as something else has historically been an
	 * injection rather than merely mojibake. */
	{
		const char *types[] = { "/a.html", "/a.css", "/a.js", "/a.txt" };
		size_t i;
		int all = 1;

		for (i = 0; i < sizeof(types) / sizeof(types[0]); i++)
			if (!strstr(http_content_type(types[i]), "charset="))
				all = 0;
		ok(all, "every text type names its charset");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
