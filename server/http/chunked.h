/*
 * Reading a chunked request body.
 *
 * --- Why this arrives now, and what it had to satisfy first ---
 *
 * `docs/WEB.md` has carried this sentence since 0.0.2, next to the row saying
 * `Transfer-Encoding` was refused outright:
 *
 *     When chunked is implemented it must be implemented *fully*, including
 *     trailers and the `0\r\n\r\n` terminator, and the dual-framing rejection
 *     stays.
 *
 * That was written as a condition on a future version rather than as a plan,
 * and this file is what it asked for. **The refusal of a request framed two
 * ways does not move**: `Content-Length` and `Transfer-Encoding` together is
 * still `HTTP_ESMUGGLE`, two `Transfer-Encoding` headers still are, and a
 * coding this does not implement still is.
 *
 * What changes is that a request framed *one* way, chunked, is now read
 * instead of refused -- which matters because it is the only framing a client
 * can use when it does not know the length in advance, and that is every
 * upload that is produced as it is sent.
 *
 * --- The rule ---
 *
 * **Refuse rather than skip.** Chunked framing is where request smuggling
 * lives, and it lives there for one reason: the format has places where a
 * parser can encounter text it does not understand and carry on. A chunk size
 * with a `+` in front of it, a `0x` prefix, a bare LF where CRLF belongs, a
 * chunk extension nobody reads, a trailer that repeats a header. Every one of
 * those has been an advisory, and every one of them is a place where two
 * implementations read one message differently.
 *
 * So every one of them is a refusal here. Not a repair, not a skip.
 *
 * --- Three deliberate narrowings, and what each costs ---
 *
 * **1. Chunk extensions are refused.** `1;name=value\r\n` is legal syntax and
 * a recipient is permitted to ignore it. Ignoring it means reading to the next
 * CRLF through text with no rules this parser enforces, which is precisely the
 * *skip what you do not understand* shape above. Nothing in ordinary use sends
 * one -- not a browser, not `curl`, not any client this server will meet -- so
 * what the refusal costs is a request nobody makes, and what it buys is that
 * there is no text in this format that is passed over unexamined.
 *
 * **2. Trailers are read, checked, and discarded.** The grammar is honoured:
 * a trailer section is parsed, bounded, and must end with the blank line, so
 * the body is not complete until the terminator really has arrived. But no
 * trailer ever becomes a header.
 *
 * That is not laziness, it is the point. A trailer arrives **after** the body,
 * and everything this server decides about a request -- which site, which
 * route, whether the guard permits it -- is decided from the headers. A
 * trailer merged into them would be a header whose value depends on bytes that
 * arrived later, and `Authorization` in a trailer would be a credential
 * presented after the decision to accept it. RFC 9112 forbids framing headers
 * in a trailer for the same family of reasons; this goes further and forbids
 * all of them from mattering.
 *
 * **3. There is a bound on everything**: the digits in a chunk size, the
 * length of a line, the number of trailers, and the total decoded body. Each
 * is a refusal rather than a truncation.
 *
 * --- Decoding in place ---
 *
 * The decoded body is written over the encoded one, in the same buffer. That
 * is safe for a reason worth stating rather than assuming: the writer can
 * never overtake the reader, because every byte of output is a byte of input
 * that has already been consumed, and the first chunk is preceded by at least
 * three bytes of header (`1\r\n`) that produce no output. The output index
 * starts behind and each data byte advances both by one, so it stays behind.
 *
 * It matters because the alternative is a second buffer the size of the first,
 * on a machine where the connection pool's buffers are already the largest
 * thing this program owns.
 */

#ifndef RECON_HTTP_CHUNKED_H
#define RECON_HTTP_CHUNKED_H

#include <stddef.h>

#include "http.h"

/*
 * Bounds.
 *
 * `HTTP_CHUNK_DIGITS_MAX` is generous and finite. The grammar is `1*HEXDIG`
 * with no limit, so `000...0001` is a legal chunk size with as many leading
 * zeros as somebody cares to send -- a line that costs nothing to send and is
 * read forever. Sixteen digits is a 64-bit size, which is more than this
 * server's body limit by a factor no machine will reach.
 *
 * `HTTP_CHUNK_LINE_MAX` bounds a chunk-size line and a trailer line, for the
 * same reason `HTTP_VALUE_MAX` bounds a header.
 */
#define HTTP_CHUNK_DIGITS_MAX    16
#define HTTP_CHUNK_LINE_MAX     512
#define HTTP_CHUNK_TRAILERS_MAX  16

/*
 * Where a decode has got to.
 *
 * Opaque to the caller apart from `out` and `in`, which are the two answers it
 * needs: how long the body is, and where the next request starts.
 */
struct http_chunked {
	int           state;
	unsigned long size;	/* bytes still owed by the current chunk */
	unsigned      digits;	/* hex digits seen in the current size */
	unsigned      line;	/* bytes in the current line, for the bound */
	unsigned      trailers;	/* trailer lines seen */
	int           saw_last;	/* the zero-sized chunk has been read */

	/*
	 * How many encoded bytes have been consumed, and how many decoded
	 * bytes have been produced.
	 *
	 * On `HTTP_OK` these are the whole answer: the body is `out` bytes
	 * long, starting where the encoded body started, and **anything after
	 * `in` belongs to the next request**. A pipelining client is entitled
	 * to send one, and a server that took the rest of the buffer as part
	 * of this body would answer the next request as this one's tail.
	 */
	size_t        in;
	size_t        out;
};

/* Start. Safe to call on a structure that has been used before: everything is
 * reset, so a pooled connection cannot inherit half a body from the request
 * before it. */
void http_chunked_begin(struct http_chunked *c);

/*
 * Decode as much as `have` bytes allow, in place.
 *
 * `buf` points at the first byte of the *encoded* body and `have` is how many
 * bytes of it exist so far; more may arrive later, and calling again with a
 * larger `have` continues exactly where this stopped. `room` bounds the
 * decoded body -- `HTTP_BODY_MAX` for this server -- and a body that exceeds
 * it is `HTTP_EBODY_LONG` rather than a truncated body somebody then acts on.
 *
 * Returns:
 *
 *   `HTTP_OK`       the terminator and its trailer section have arrived. The
 *                   body is `c->out` bytes at `buf`, and `c->in` bytes of
 *                   `buf` were this request's.
 *   `HTTP_PARTIAL`  correct so far; call again with more.
 *   a refusal       `HTTP_EMALFORMED` for anything the grammar does not allow,
 *                   `HTTP_EBODY_LONG` for a body past `room`. Once one of
 *                   these is returned the framing is in doubt and the
 *                   connection must be closed rather than reused -- there is
 *                   no way to know where the next request would start.
 */
int http_chunked_feed(struct http_chunked *c, char *buf, size_t have,
                      size_t room);

#endif
