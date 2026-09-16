/*
 * Serving a file, and every way of serving the wrong one.
 *
 * The handler is driven over a socketpair rather than through a whole server.
 * The socket path has its own suite; what is under test here is *which file
 * gets opened and what comes back*, and a listener only makes that slower to
 * ask. It is still a real socket, because the handler streams and `send` is
 * what it streams with.
 *
 * **The cases that matter are the ones about a file that is not the one asked
 * for**: a path that climbs out of the root, a directory served as a listing,
 * a binary file truncated at its first zero byte, a body that does not match
 * the length declared beside it. Each of those is a served file that is wrong
 * rather than a served error, which is the harder failure to notice.
 *
 * **Every case checks the declared length as well as the bytes.** A body that
 * is right next to a header that is wrong desynchronises a kept connection,
 * and a check that only compares bytes cannot see it.
 *
 * --- Watched failing, three times, and each time taught something ---
 *
 * Run against a handler with `escapes()` removed, the four traversal cases
 * failed as they should.
 *
 * Relaxing the old over-size check changed **nothing** -- the suite still
 * passed, because the handler guarded that twice and only one half had been
 * broken. A check that survives the removal of the thing it is checking is not
 * evidence about that thing. (Streaming has since removed the cap entirely;
 * the case now proves the opposite, that a large file is served whole.)
 *
 * And the suite caught a real one on conversion. `open_and_size` asked `lseek`
 * whether something was a file, on the reasoning that a directory cannot be
 * seeked to its end. A directory can. `/docs` was served as a zero-length file
 * of its own instead of falling through to the index inside it.
 */

#include "../http/files.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

/*
 * Ask the handler for a path and collect everything it put on the wire.
 *
 * The handler streams, so there is no response structure left to inspect --
 * what it produces is bytes, and bytes are what this reads back. A socketpair
 * rather than a file, because the sink sends with `send`, which a regular
 * descriptor refuses.
 *
 * **Forked, and that is not ceremony.** A socketpair holds a few hundred
 * kilobytes before it blocks, and one case below deliberately serves more than
 * that. A single process writing and then reading would deadlock at exactly
 * the size that case exists to prove works.
 */
static size_t ask(const struct http_files *files, const char *target,
                  char *into, size_t room)
{
	int sv[2];
	pid_t child;
	size_t have = 0;

	into[0] = '\0';
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		return 0;

	child = fork();
	if (child < 0) {
		close(sv[0]);
		close(sv[1]);
		return 0;
	}

	if (child == 0) {
		struct http_request r;
		struct http_sink sink;

		alarm(20);
		close(sv[0]);

		memset(&r, 0, sizeof(r));
		snprintf(r.method, sizeof(r.method), "GET");
		snprintf(r.target, sizeof(r.target), "%s", target);
		r.minor = 1;

		memset(&sink, 0, sizeof(sink));
		sink.fd = sv[1];
		sink.minor = 1;
		sink.declared = HTTP_LENGTH_UNKNOWN;
		sink.server_name = "ReconOS/files";

		http_files_handler(&r, 0, 0, &sink, (void *)files);
		http_stream_end(&sink);

		close(sv[1]);
		_exit(0);
	}

	close(sv[1]);
	for (;;) {
		ssize_t n = read(sv[0], into + have, room - 1 - have);

		if (n > 0) {
			have += (size_t)n;
			if (have >= room - 1)
				break;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
	into[have] = '\0';
	close(sv[0]);
	waitpid(child, 0, 0);
	return have;
}

/* The status line's code, or 0. */
static int status_of(const char *reply)
{
	if (strncmp(reply, "HTTP/1.1 ", 9) != 0)
		return 0;
	return atoi(reply + 9);
}

/* Where the body begins, or NULL. */
static const char *body_of(const char *reply)
{
	const char *at = strstr(reply, "\r\n\r\n");

	return at ? at + 4 : 0;
}

/* A header must be in the head, not merely somewhere in the reply. A body that
 * happens to contain the text is not the server having sent it. */
static int has_header(const char *reply, const char *line)
{
	const char *head_end = strstr(reply, "\r\n\r\n");
	const char *at = strstr(reply, line);

	return at && head_end && at < head_end;
}

static char REPLY[HTTP_RESPONSE_MAX + 65536];

static void serves(const struct http_files *f, const char *target,
                   const char *expect, size_t expect_len, const char *type,
                   const char *what)
{
	size_t n = ask(f, target, REPLY, sizeof(REPLY));
	const char *b = body_of(REPLY);
	size_t got;
	char want[96];

	checks++;
	if (status_of(REPLY) != 200) {
		failures++;
		printf("  FAIL  %s: status=%d\n", what, status_of(REPLY));
		return;
	}
	if (!b) {
		failures++;
		printf("  FAIL  %s: no head terminator\n", what);
		return;
	}
	got = n - (size_t)(b - REPLY);

	/* The declared length and the delivered length, both, and from one
	 * response. A body that is right beside a header that is wrong is the
	 * failure that desynchronises a kept connection, and it is invisible to
	 * a check that only looks at the bytes. */
	snprintf(want, sizeof(want), "Content-Length: %lu\r\n",
	         (unsigned long)expect_len);
	if (!has_header(REPLY, want)) {
		failures++;
		printf("  FAIL  %s: did not declare %lu bytes\n", what,
		       (unsigned long)expect_len);
		return;
	}
	if (got != expect_len || memcmp(b, expect, expect_len) != 0) {
		failures++;
		printf("  FAIL  %s: delivered %lu, wanted %lu\n", what,
		       (unsigned long)got, (unsigned long)expect_len);
		return;
	}
	if (type) {
		snprintf(want, sizeof(want), "Content-Type: %s\r\n", type);
		if (!has_header(REPLY, want)) {
			failures++;
			printf("  FAIL  %s: wrong content type\n", what);
		}
	}
}

static void answers(const struct http_files *f, const char *target,
                    int status, const char *what)
{
	int got;

	ask(f, target, REPLY, sizeof(REPLY));
	got = status_of(REPLY);

	checks++;
	if (got != status) {
		failures++;
		printf("  FAIL  %s: status=%d, wanted %d\n", what, got, status);
	}
}

int main(void)
{
	struct http_files site = { ROOT, "index.html" };
	struct http_files noindex = { ROOT, 0 };
	static char big[HTTP_RESPONSE_MAX + 8192];
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

	/* --- size, whose meaning streaming changed ---------------------------
	 *
	 * There used to be a cap here. A whole response was assembled in
	 * memory, so a file larger than `HTTP_RESPONSE_MAX` was refused with
	 * 413 rather than served short -- a truncated file with a 200 beside it
	 * being a corrupt file that looks good.
	 *
	 * Streaming removes the cap, and these cases are the evidence: a file
	 * well past that size is served in full, with a declared length that
	 * matches what arrives. The handler's own buffer is 8 KiB and does not
	 * grow with the file, which is the property that makes it possible and
	 * the one that would silently regress if somebody went back to reading
	 * the whole thing into memory. */
	{
		size_t i;

		for (i = 0; i < sizeof(big); i++)
			big[i] = (char)('a' + (i % 26));

		put("/large.txt", big, HTTP_RESPONSE_MAX + 5000);
		serves(&site, "/large.txt", big, HTTP_RESPONSE_MAX + 5000,
		       "text/plain; charset=utf-8",
		       "a file far larger than a whole response is served entire");

		ok(HTTP_RESPONSE_MAX + 5000 > HTTP_RESPONSE_MAX,
		   "and it is past what the old cap allowed");

		/* An empty file is a file. Zero bytes, a 200, and a declared
		 * length of zero -- not a 404, which would say it was absent
		 * when it is there and empty. */
		put("/empty.txt", "", 0);
		serves(&site, "/empty.txt", "", 0, "text/plain; charset=utf-8",
		       "an empty file is served as empty, not as missing");
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
