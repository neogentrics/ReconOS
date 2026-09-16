/*
 * Reading a file off the volume and answering with it.
 *
 * See `files.h` for why this is a handler rather than the server's middle, and
 * for what it does instead of `stat`.
 *
 * --- What was tried and rejected ---
 *
 * **`lseek` to the end to learn the size, then read that many bytes** -- which
 * this file rejected when it was written, and uses now. The objection was
 * real: the size is a fact at one moment and the bytes are a fact at another,
 * so a header built from the first describes a different observation from the
 * body built from the second. While a whole response was assembled in memory
 * there was a way to avoid the gap entirely -- read until the file stops, and
 * report what actually arrived.
 *
 * Streaming removes that option, because the header goes out before the body
 * exists. What it adds is **detection**: the length becomes a declared promise
 * and `http_stream_end` compares it against what was written. The gap is still
 * there and is no longer silent, which is the trade that changed the answer.
 * See `files.h`.
 *
 * **Falling back to a directory listing when there is no index.** Refused, and
 * this is a security decision rather than a feature decision. A listing
 * publishes every name in a directory, and the names nobody meant to publish
 * -- `config.bak`, `notes.txt`, an editor's leftovers -- are exactly the ones
 * worth having. A directory with no index is answered 404, the same as a
 * directory that is not there, because the difference is not the client's
 * business.
 *
 * **Guessing the content type from the first bytes of the file.** Browsers did
 * that for years and it is the reason `X-Content-Type-Options: nosniff` had to
 * be invented. The extension is a statement by whoever put the file there; the
 * content is a statement by whoever uploaded it.
 */

#include "files.h"
#include "cache.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/*
 * What a client is told about keeping a copy.
 *
 * `no-cache` is widely misread as "do not keep this". It means the opposite:
 * keep it, and ask before using it -- which is exactly what the ETag above is
 * for. `no-store` is the one that forbids keeping, and using it here would
 * throw away every revalidation this file went to the trouble of enabling.
 */
#define CACHING "no-cache"


/* No <string.h>, for the reason the other files in here give: this is built
 * against the host's library for its suite and ReconOS's on the machine, and a
 * name that folds differently between the two is a bug that appears only on
 * the machine. */

static size_t slen(const char *s)
{
	size_t n = 0;

	while (s && s[n])
		n++;
	return n;
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Compare an extension case-insensitively against a lower-case literal. */
static int ext_is(const char *ext, const char *lower_b)
{
	size_t i = 0;

	while (ext[i] && lower_b[i] && lower(ext[i]) == lower_b[i])
		i++;
	return lower(ext[i]) == lower_b[i];
}

const char *http_content_type(const char *path)
{
	const char *ext = 0;
	size_t i;

	if (!path)
		return "application/octet-stream";

	/* The last dot after the last slash. A dot in a directory name is not
	 * an extension of the file inside it. */
	for (i = 0; path[i]; i++) {
		if (path[i] == '/')
			ext = 0;
		else if (path[i] == '.')
			ext = path + i + 1;
	}

	if (!ext || !*ext)
		return "application/octet-stream";

	/* Types this console actually serves, plus the handful anything else
	 * on the volume is likely to be. Deliberately short: an entry here is
	 * a promise that a browser may treat the bytes as that type, and a
	 * long table is a long list of promises nobody checked.
	 *
	 * Every text type carries a charset. Without one a browser guesses,
	 * and a page of UTF-8 guessed as Latin-1 is mojibake -- while a page
	 * guessed the other way has, historically, been an injection. */
	if (ext_is(ext, "html") || ext_is(ext, "htm"))
		return "text/html; charset=utf-8";
	if (ext_is(ext, "css"))
		return "text/css; charset=utf-8";
	if (ext_is(ext, "js"))
		return "text/javascript; charset=utf-8";
	if (ext_is(ext, "json"))
		return "application/json";
	if (ext_is(ext, "txt") || ext_is(ext, "md"))
		return "text/plain; charset=utf-8";
	if (ext_is(ext, "svg"))
		return "image/svg+xml";
	if (ext_is(ext, "png"))
		return "image/png";
	if (ext_is(ext, "jpg") || ext_is(ext, "jpeg"))
		return "image/jpeg";
	if (ext_is(ext, "gif"))
		return "image/gif";
	if (ext_is(ext, "ico"))
		return "image/vnd.microsoft.icon";
	if (ext_is(ext, "woff2"))
		return "font/woff2";
	if (ext_is(ext, "pdf"))
		return "application/pdf";
	if (ext_is(ext, "xml"))
		return "application/xml";

	return "application/octet-stream";
}

/*
 * Does this path contain a segment that climbs, or a byte that should not be
 * in a path at all?
 *
 * The parser has already said no. This says no again, on the joined string,
 * for the reason `files.h` gives: two separate claims about one string, and
 * the day this is called from somewhere else, the second is the only one left.
 */
static int escapes(const char *path)
{
	size_t i = 0;

	while (path[i]) {
		if (path[i] == '\\')
			return 1;
		if ((unsigned char)path[i] < 0x20)
			return 1;

		if (path[i] == '/' || i == 0) {
			size_t at = (path[i] == '/') ? i + 1 : 0;

			if (path[at] == '.' && path[at + 1] == '.'
			    && (path[at + 2] == '/' || path[at + 2] == '\0'))
				return 1;
		}
		i++;
	}
	return 0;
}

/* Join root and target into `into`, refusing rather than truncating. */
static int join(const struct http_files *files, const char *target,
                const char *tail, char *into, size_t room)
{
	size_t rl = slen(files->root);
	size_t tl = slen(target);
	size_t sl = slen(tail);
	size_t at = 0, i;
	int need_slash;

	/* `target` is rooted, so it opens with '/'. A root that also closes
	 * with one would give `//`, which some filesystems treat as a
	 * different path from one slash. */
	if (rl > 0 && files->root[rl - 1] == '/')
		rl--;

	need_slash = (sl > 0 && tl > 0 && target[tl - 1] != '/');

	if (rl + tl + (size_t)need_slash + sl + 1 > room)
		return HTTP_ELINE_LONG;
	if (rl + tl + (size_t)need_slash + sl + 1 > HTTP_PATH_MAX)
		return HTTP_ELINE_LONG;

	for (i = 0; i < rl; i++)
		into[at++] = files->root[i];
	for (i = 0; i < tl; i++)
		into[at++] = target[i];
	if (need_slash)
		into[at++] = '/';
	for (i = 0; i < sl; i++)
		into[at++] = tail[i];
	into[at] = '\0';
	return HTTP_OK;
}

/*
 * Open a file and learn how long it is.
 *
 * Returns the descriptor with the cursor back at the start, or a negative
 * number. `lseek` failing is how this library asks "is that a file?" without a
 * `stat` it does not have -- a directory cannot be seeked to its end, and
 * neither can anything else that has no bytes to serve.
 */
static int open_and_size(const char *path, long *size)
{
	int fd = open(path, O_RDONLY);
	char probe;
	ssize_t got;
	off_t end;

	if (fd < 0)
		return -1;

	end = lseek(fd, 0, SEEK_END);
	if (end < 0 || lseek(fd, 0, SEEK_SET) != 0) {
		close(fd);
		return -1;
	}

	/*
	 * A trial read, because seeking is not the question.
	 *
	 * This asked `lseek` alone at first, on the reasoning that a directory
	 * cannot be seeked to its end. **That is false**, at least on the host:
	 * a directory opens, seeks, and reports a size -- so `/docs` was served
	 * as a zero-length file of its own rather than falling through to the
	 * index inside it, and the suite caught it.
	 *
	 * Reading is the operation actually wanted, so reading is what gets
	 * asked. A directory refuses it; an empty file returns zero, which is
	 * a perfectly good answer and must not be mistaken for a refusal.
	 */
	got = read(fd, &probe, 1);
	if (got < 0 || lseek(fd, 0, SEEK_SET) != 0) {
		close(fd);
		return -1;
	}

	*size = (long)end;
	return fd;
}

/* A status with a short body, written through the sink because the sink is the
 * only way out. Deliberately plain: an error page that reports what was wrong
 * tells an attacker which of their probes was noticed. */
static int say_status(struct http_sink *sink, int status, const char *text)
{
	size_t n = slen(text);

	if (http_stream_begin(sink, status, "text/plain", (long)n) != HTTP_OK)
		return HTTP_EMALFORMED;
	if (http_stream_write(sink, text, n) != HTTP_OK)
		return HTTP_EMALFORMED;
	return HTTP_OK;
}

int http_files_handler(const struct http_request *request,
                       const char *body, size_t body_len,
                       struct http_sink *sink, void *ctx)
{
	/* One block at a time. Static because this server serves one
	 * connection at a time, and because the whole point of streaming is
	 * that this buffer does not grow with the file. */
	static char block[8192];
	static char path[HTTP_PATH_MAX];
	static char etag[HTTP_ETAG_MAX];

	/* What the file hashed to before it was sent, and what it hashed to on
	 * the way out. They should agree; the comment at the bottom says what
	 * it means when they do not. */
	unsigned long long expected = 0, actual = 0;

	const struct http_files *files = (const struct http_files *)ctx;
	const char *target;
	size_t tl;
	long size = 0;
	int fd, rc;

	(void)body; (void)body_len;

	if (!files || !files->root)
		return HTTP_EMALFORMED;

	target = request->target;
	tl = slen(target);

	/* A directory is served from its index or not at all. Never listed --
	 * see the file header. */
	if (tl > 0 && target[tl - 1] == '/') {
		if (!files->index)
			return say_status(sink, 404, "404 Not Found\n");
		rc = join(files, target, files->index, path, sizeof(path));
	} else {
		rc = join(files, target, "", path, sizeof(path));
	}

	if (rc != HTTP_OK)
		return rc;

	if (escapes(path + slen(files->root)))
		return say_status(sink, 403, "403 Forbidden\n");

	fd = open_and_size(path, &size);

	/*
	 * Not a file. If the request named a directory without its trailing
	 * slash -- `/docs` rather than `/docs/` -- try the index inside it
	 * before giving up, because a person typing a path leaves the slash
	 * off and a 404 there is a 404 for a page that exists.
	 */
	if (fd < 0 && files->index && (tl == 0 || target[tl - 1] != '/')) {
		rc = join(files, target, files->index, path, sizeof(path));
		if (rc == HTTP_OK)
			fd = open_and_size(path, &size);
	}

	if (fd < 0)
		return say_status(sink, 404, "404 Not Found\n");

	/*
	 * Hash the file to build a validator, then wind back to the start.
	 *
	 * This is the extra read `cache.h` warns about, and it is the price of
	 * a strong validator on a library with no `stat`: there is no
	 * modification time to ask for, so the bytes are the only thing that
	 * can say whether this is the same file as last time.
	 */
	{
		unsigned long long h = HTTP_HASH_SEED;

		for (;;) {
			ssize_t n = read(fd, block, sizeof(block));

			if (n > 0) {
				h = http_hash(h, block, (size_t)n);
				continue;
			}
			if (n == 0)
				break;
			if (errno == EINTR)
				continue;
			close(fd);
			return say_status(sink, 404, "404 Not Found\n");
		}

		if (lseek(fd, 0, SEEK_SET) != 0) {
			close(fd);
			return say_status(sink, 404, "404 Not Found\n");
		}
		http_etag_format(etag, sizeof(etag), (unsigned long)size, h);
		expected = h;
	}

	/*
	 * Does the client already have it?
	 *
	 * A 304 carries the validator and no body. `Content-Length: 0` rather
	 * than no length at all: a 304 must not have a body, and a client that
	 * is told nothing about the length has to work that out from the
	 * framing -- which is the kind of thing that goes wrong on a kept
	 * connection. Zero says it outright.
	 */
	if (etag[0]) {
		const char *inm = http_header_get(request, "if-none-match");

		if (inm && http_if_none_match(inm, etag)) {
			close(fd);
			http_stream_header(sink, "ETag", etag);
			http_stream_header(sink, "Cache-Control", CACHING);
			if (http_stream_begin(sink, 304, 0, 0) != HTTP_OK)
				return HTTP_EMALFORMED;
			return HTTP_OK;
		}

		http_stream_header(sink, "ETag", etag);
	}

	/*
	 * `no-cache` means "you may keep it, but ask me before using it", which
	 * is the correct instruction for a console: its pages change when the
	 * machine does, and a page cached for an hour would show a figure that
	 * is an hour old with no way for a reader to tell.
	 *
	 * It is **not** `no-store`, which would forbid keeping it at all and
	 * throw away the revalidation the ETag above exists to enable.
	 */
	http_stream_header(sink, "Cache-Control", CACHING);

	/*
	 * The length is declared from what `lseek` said, and the promise is
	 * what makes that safe -- see `files.h`. A file that changes under the
	 * read writes a different number of bytes than was declared, and
	 * `http_stream_end` turns that into a closed connection rather than a
	 * header that quietly disagrees with its body.
	 */
	if (http_stream_begin(sink, 200, http_content_type(path), size)
	    != HTTP_OK) {
		close(fd);
		return HTTP_EMALFORMED;
	}

	actual = HTTP_HASH_SEED;
	for (;;) {
		ssize_t n = read(fd, block, sizeof(block));

		if (n > 0) {
			actual = http_hash(actual, block, (size_t)n);
			if (http_stream_write(sink, block, (size_t)n)
			    != HTTP_OK) {
				close(fd);
				return HTTP_EMALFORMED;
			}
			continue;
		}
		if (n == 0)
			break;
		if (errno == EINTR)
			continue;

		/* The file stopped being readable part way through. The head
		 * is already gone, so the only honest thing left is to stop
		 * and let the promise check close the connection. */
		close(fd);
		return HTTP_EMALFORMED;
	}

	close(fd);

	/*
	 * The bytes that were sent, against the bytes that were hashed.
	 *
	 * They can differ: the file was read twice and something may have
	 * edited it in between. A length change is caught by the promise; a
	 * same-length edit is caught only here.
	 *
	 * **It matters more than the length does.** A wrong length breaks one
	 * connection. A validator that does not match its bytes is stored by
	 * the client and served from that store until it expires, so one bad
	 * answer becomes every answer, and nothing at either end reports it.
	 *
	 * The tag cannot be un-sent. Refusing here closes the connection,
	 * which at least means the client does not go on to reuse it -- and
	 * leaves a fault visible rather than silent.
	 */
	if (etag[0] && actual != expected)
		return HTTP_EMALFORMED;

	return HTTP_OK;
}
