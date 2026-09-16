/*
 * Reading a `multipart/form-data` body.
 *
 * The other half of `form.c`. A browser sends `urlencoded` for a plain form
 * and this for a form carrying a file, and a server that reads one and not the
 * other cannot accept an upload.
 *
 * --- Why this is not `form.c` with a different separator ---
 *
 * **Nothing here is percent-decoded.** A multipart value is the bytes between
 * two delimiters, exactly as sent -- no `%20`, no `+`, no escapes of any kind.
 * That is the whole point of the format: it carries a JPEG without having to
 * spell one.
 *
 * So a decoder shared with `form.c` would corrupt every upload it touched, and
 * -- worse -- would turn `%00` in a filename into a real NUL, which is the
 * classic way a name that passed a check stops being the name that gets used.
 * `form.h` already explains why `+` made these two functions instead of one
 * with a flag. This is the same argument with more at stake: there, a flag
 * passed wrong changes a plus into a space; here it changes a file.
 *
 * --- The delimiter, and the reason the leading CRLF belongs to it ---
 *
 * A part ends at `CRLF--<boundary>`. The CRLF is **part of the delimiter, not
 * part of the data** -- a body ending `...data\r\n--b--` holds `...data`, and a
 * parser that searches for `--<boundary>` and keeps everything before it hands
 * back a file two bytes too long. Every byte matters when the bytes are a file:
 * two extra at the end of an archive is a corrupt archive.
 *
 * The first delimiter is the exception, having nothing before it to end.
 *
 * --- What is refused, and why refusing beats repairing ---
 *
 * **A filename containing `/`, `\`, `..`, or a NUL is refused outright.** Not
 * stripped, not rewritten, not reduced to its last segment. `request.c` refuses
 * `..` in a target for the same reason rather than clamping it: every repair is
 * a transformation, every transformation has a fixed point somebody can aim
 * for, and `....//` is what a strip-once repair produces from a string that
 * still escapes. A refusal has no fixed point.
 *
 * A caller is still not permitted to treat an accepted filename as a path. It
 * is a label a client chose; the checks here make it harmless to *record*, not
 * safe to open.
 *
 * **A truncated body is refused.** A body whose closing `--<boundary>--` never
 * arrives is an upload that was cut off, and a parser that returns the parts it
 * managed to read hands a caller a file that looks complete and is not. There
 * is no error anywhere in that story, which is what makes it worth a refusal.
 *
 * **`filename*` (RFC 2231) is refused rather than half-read.** Supporting
 * `filename` and ignoring a `filename*` beside it means the name this server
 * records and the name the client believes it sent are different strings. Two
 * readings of one name is the fault this whole file is arranged against.
 *
 * **A part name given twice answers NULL**, exactly as in `form.c`, and for the
 * reason set out there: agreement between readers is not what makes a message
 * safe, being unambiguous is.
 *
 * --- What this does not do ---
 *
 * It parses a body that has already arrived **whole**, in one buffer, within
 * `HTTP_BODY_MAX`. Real file upload needs the request side to stream, which
 * does not exist: `serve.c` reads a request into one buffer and a body larger
 * than that buffer is refused with 413 before any of this runs.
 *
 * So this is the parser for that day, usable today for the uploads that fit.
 * It is written now because the parsing is the part with the sharp edges, and
 * the streaming, when it arrives, should not also be the week this format is
 * being learnt.
 */

#ifndef RECON_HTTP_MULTIPART_H
#define RECON_HTTP_MULTIPART_H

#include "http.h"

/*
 * Bounds. Each refuses rather than truncating -- a truncated filename is a
 * different filename, and a part silently dropped is a field somebody filled
 * in and this server did not read.
 */
#define HTTP_PARTS_MAX             16
#define HTTP_PART_NAME_MAX         64
#define HTTP_PART_FILENAME_MAX    128
#define HTTP_PART_TYPE_MAX         64

/* RFC 2046 puts a boundary at 70 characters, not counting the leading `--`.
 * Written as the spec's number rather than a round one, so a reader can check
 * it against the spec instead of against somebody's judgement. */
#define HTTP_BOUNDARY_MAX          70

struct http_part {
	char name[HTTP_PART_NAME_MAX];
	char filename[HTTP_PART_FILENAME_MAX];
	int  has_filename;	/* a filename may legally be empty, so its
				 * presence and its content are separate
				 * questions -- and "is this a file field"
				 * is answered by the former */
	char content_type[HTTP_PART_TYPE_MAX];	/* "" when the part sent none */

	/*
	 * The part's bytes, **pointing into the caller's buffer**. Not copied:
	 * a 64 KiB body does not fit sixteen times over, and copying is how a
	 * bound gets invented that the format does not have.
	 *
	 * So `data` is valid exactly as long as the buffer passed to
	 * `http_multipart_parse` is. A caller that keeps a part past the
	 * request that carried it must copy it first.
	 */
	const char *data;
	size_t      data_len;
};

struct http_multipart {
	struct http_part parts[HTTP_PARTS_MAX];
	size_t count;
};

/*
 * Pull the boundary out of a `Content-Type` header value.
 *
 * Writes the boundary itself, without the leading `--`, into `out`.
 *
 * Returns HTTP_OK, or:
 *   HTTP_EMALFORMED  not `multipart/form-data`; no `boundary` parameter; an
 *                    empty boundary; a boundary holding a character the spec
 *                    does not allow; an unterminated quoted string; or
 *                    **`boundary` given twice**, even with the same value --
 *                    the rule the request parser applies to `Content-Length`,
 *                    because two readers picking different ones is the fault,
 *                    not disagreement about which is right
 *   HTTP_EFIELD_LONG a boundary past HTTP_BOUNDARY_MAX
 */
int http_multipart_boundary(const char *content_type, char *out, size_t room);

/*
 * Parse `len` bytes of body against `boundary` (without the leading `--`).
 *
 * `body` need not be NUL-terminated and is never read past `len`.
 *
 * Returns HTTP_OK, or:
 *   HTTP_EMALFORMED  a missing or malformed delimiter; a body that never
 *                    closes; a part with no `Content-Disposition`, or one that
 *                    is not `form-data`, or one given twice; a part with no
 *                    `name`, or two; a bare CR or bare LF in a part header; a
 *                    header continued with obs-fold; a `filename*`; a filename
 *                    holding a separator, a `..`, or a NUL
 *   HTTP_EFIELD_LONG a name, filename or content type past its bound
 *   HTTP_ETOOMANY    more parts than HTTP_PARTS_MAX
 */
int http_multipart_parse(const char *body, size_t len, const char *boundary,
                         struct http_multipart *into);

/*
 * The part called `name`, or NULL.
 *
 * NULL means "no single answer": absent, or sent more than once. See `form.h`;
 * this is deliberately the same rule and the same silence.
 */
const struct http_part *http_multipart_get(const struct http_multipart *m,
                                           const char *name);

/* How many parts carry `name`: 0, 1, or more. */
size_t http_multipart_count(const struct http_multipart *m, const char *name);

#endif
