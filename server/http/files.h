/*
 * Serving a file, as one handler among others.
 *
 * `docs/WEB.md` sets this out and it is worth repeating where the code is:
 * **this is not what the server is, it is one thing the server does.** The
 * routing table in `serve.h` is the middle of this program, and a static file
 * arrives through the same `http_handler` signature as a JSON endpoint or a
 * form post. A server built the other way round -- find the file, send the
 * file, with everything dynamic cut in as a special case -- is the shape this
 * deliberately is not.
 *
 * --- What this cannot ask, and what it does instead ---
 *
 * There is no `stat` in this C library. So "is that a directory" is not a
 * question that can be asked before opening, and this does not pretend
 * otherwise: it opens, and reads, and treats a read that will not produce
 * bytes as a thing that is not a file. The one place that matters is a request
 * for a directory, which is answered from its index or refused -- never with a
 * listing, because a listing of a directory nobody meant to publish is how a
 * backup file becomes a download.
 *
 * --- Where safety actually comes from ---
 *
 * The parser has already refused `..` above the root, `%00`, a backslash, and
 * a `%` that is not two hex digits, and it did so *after* decoding, so
 * `%2e%2e%2f` was seen as `../`. By the time a target reaches here it is
 * normalised, rooted, and contains no segment that climbs.
 *
 * **This re-checks anyway**, on the joined path, and the reason is the rule
 * this project keeps: a check nobody performs is a check nobody fails. The
 * parser's refusal and this one are two separate claims about the same string,
 * and the day somebody calls this handler from somewhere other than the parser
 * is the day the second one is the only one left.
 */

#ifndef RECON_HTTP_FILES_H
#define RECON_HTTP_FILES_H

#include "serve.h"

/* The longest filesystem path this will build, including its terminator.
 * A path that does not fit is refused, never truncated -- a truncated path
 * names a different file, and often a directory rather than the file inside
 * it. */
#define HTTP_PATH_MAX 512

struct http_files {
	/* The directory every served path is taken relative to. No trailing
	 * slash. Nothing outside it can be reached, and that is enforced here
	 * rather than assumed from the caller. */
	const char *root;

	/* What to serve for a directory. NULL refuses directories outright,
	 * which is a legitimate configuration and not a degraded one. */
	const char *index;
};

/*
 * The handler. `ctx` is a `const struct http_files *`.
 *
 * Answers 200 with the file, 404 when it is not there or is not readable, 403
 * when the path escapes the root, and 413 when the file is larger than a
 * response can carry. It never answers 500 for a missing file: absence is an
 * ordinary outcome and reporting it as a server fault is how a monitoring
 * system learns to ignore 500s.
 */
int http_files_handler(const struct http_request *request,
                       const char *body, size_t body_len,
                       struct http_response *out, void *ctx);

/*
 * The content type for a name, by its extension.
 *
 * Never NULL. An extension this does not know gives
 * `application/octet-stream`, which is the honest answer and also the safe
 * one: a browser handed an unknown type downloads it rather than running it.
 * Guessing `text/html` from content would be the opposite trade.
 *
 * Exposed so the suite can check the table directly rather than only through a
 * served file.
 */
const char *http_content_type(const char *path);

#endif
