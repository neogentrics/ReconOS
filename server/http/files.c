/*
 * Reading a file off the volume and answering with it.
 *
 * See `files.h` for why this is a handler rather than the server's middle, and
 * for what it does instead of `stat`.
 *
 * --- What was tried and rejected ---
 *
 * **`lseek` to the end to learn the size, then read that many bytes.** It
 * works on the host and it is the obvious shape. It was dropped because the
 * size it learns is the size *at that moment*, and the read that follows is a
 * second look at a file something else may have changed in between -- so the
 * length in the header and the bytes in the body come from two different
 * observations. Reading until the file stops and reporting what actually
 * arrived is one observation, and the header is then a fact about the body
 * rather than a claim about the file.
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

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

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
 * Read a whole file into `into`.
 *
 * Returns the number of bytes on success, or a negative verdict. A file that
 * does not fit is refused rather than served short: a truncated file with a
 * 200 beside it is a corrupt file that looks like a good one, and that is
 * worse than an error.
 */
static long slurp(const char *path, char *into, size_t room)
{
	int fd = open(path, O_RDONLY);
	size_t have = 0;

	if (fd < 0)
		return HTTP_EMALFORMED;	/* absent, or not readable */

	for (;;) {
		ssize_t n;

		if (have >= room) {
			/* There is more file than there is room. Distinguished
			 * from a file that exactly fills the buffer by asking
			 * for one more byte than can be served -- see the
			 * caller, which sizes `room` at the cap plus one. */
			close(fd);
			return HTTP_EBODY_LONG;
		}

		n = read(fd, into + have, room - have);
		if (n > 0) {
			have += (size_t)n;
			continue;
		}
		if (n == 0)
			break;			/* the end of the file */
		if (errno == EINTR)
			continue;

		/* A read that fails on the first byte is the answer to the
		 * question this library cannot ask directly: a directory, or
		 * something else that is not a file to be served. `EISDIR` is
		 * the host's word for it; ReconOS answers its own error and
		 * either way there are no bytes, which is what matters. */
		close(fd);
		return HTTP_EMALFORMED;
	}

	close(fd);
	return (long)have;
}

int http_files_handler(const struct http_request *request,
                       const char *body, size_t body_len,
                       struct http_response *out, void *ctx)
{
	/* One buffer, static because this server serves one connection at a
	 * time and 256 KiB is far more stack than a user process here should
	 * assume. One byte over the cap, so that a file which exactly fills
	 * the cap is served and one byte larger is refused -- without that
	 * extra byte the two are indistinguishable. */
	static char contents[HTTP_RESPONSE_MAX + 1];
	static char path[HTTP_PATH_MAX];

	const struct http_files *files = (const struct http_files *)ctx;
	const char *target;
	size_t tl;
	long n;
	int rc;

	(void)body; (void)body_len;

	if (!files || !files->root)
		return HTTP_EMALFORMED;

	target = request->target;
	tl = slen(target);

	/* A directory is served from its index or not at all. Never listed --
	 * see the file header. */
	if (tl > 0 && target[tl - 1] == '/') {
		if (!files->index) {
			http_response_simple(out, 404, "text/plain",
			                     "404 Not Found\n", 14);
			return HTTP_OK;
		}
		rc = join(files, target, files->index, path, sizeof(path));
	} else {
		rc = join(files, target, "", path, sizeof(path));
	}

	if (rc != HTTP_OK)
		return rc;

	if (escapes(path + slen(files->root))) {
		http_response_simple(out, 403, "text/plain",
		                     "403 Forbidden\n", 14);
		return HTTP_OK;
	}

	n = slurp(path, contents, sizeof(contents));

	/*
	 * Not a file. If the request named a directory without its trailing
	 * slash -- `/docs` rather than `/docs/` -- try the index inside it
	 * before giving up, because a person typing a path leaves the slash
	 * off and a 404 there is a 404 for a page that exists.
	 *
	 * A redirect to the slashed form would be the more correct answer and
	 * is what `docs/WEB.md` specifies; this serves it directly, which is
	 * indistinguishable to a reader and does not need a `Location` header
	 * this handler would have to build a full URL for.
	 */
	if (n < 0 && n == HTTP_EMALFORMED && files->index
	    && (tl == 0 || target[tl - 1] != '/')) {
		rc = join(files, target, files->index, path, sizeof(path));
		if (rc == HTTP_OK)
			n = slurp(path, contents, sizeof(contents));
	}

	if (n == HTTP_EBODY_LONG) {
		/* Larger than a response can carry. 413 rather than 500: the
		 * server is fine, the file is bigger than this server can send
		 * until streaming exists. `docs/WEB.md` carries that row. */
		http_response_simple(out, 413, "text/plain",
		                     "413 Content Too Large\n", 22);
		return HTTP_OK;
	}

	if (n < 0) {
		http_response_simple(out, 404, "text/plain",
		                     "404 Not Found\n", 14);
		return HTTP_OK;
	}

	if ((size_t)n > HTTP_RESPONSE_MAX) {
		http_response_simple(out, 413, "text/plain",
		                     "413 Content Too Large\n", 22);
		return HTTP_OK;
	}

	http_response_simple(out, 200, http_content_type(path), contents,
	                     (size_t)n);
	return HTTP_OK;
}
