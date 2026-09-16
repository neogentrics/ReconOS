/*
 * Putting text into JSON without it becoming structure.
 *
 * --- Why this exists, written from the fault it removes ---
 *
 * The same fault `escape.c` removed from the HTML path, found still living in
 * the JSON one. `/api/status` and the reply from `POST /api/name` both write
 * the machine's name straight into a string:
 *
 *     "{\"role\":\"server\",\"name\":\"%s\", ... }"
 *
 * That is *safe* -- because `server_name_split` admits letters, digits and the
 * hyphen and refuses everything else, so a quote cannot reach it. Exactly the
 * coincidence that was removed from the dashboard, still load-bearing one file
 * away, and found only because `/api/log` was refused as JSON on precisely
 * these grounds while two endpoints were already doing it.
 *
 * A quote in a value ends the string and everything after it becomes
 * structure: a client that can influence any text an endpoint reports can write
 * fields of its own into the document a reader is parsing.
 *
 * --- ASCII only, and that is a refusal rather than an omission ---
 *
 * JSON must be valid UTF-8. This escaper does not validate UTF-8 and therefore
 * **refuses any byte above 0x7F** rather than passing it through and producing
 * a document a parser will reject -- or worse, one it will accept and read
 * differently from the next parser.
 *
 * ASCII-only JSON is always valid JSON, so the output is correct by
 * construction. Text that genuinely needs non-ASCII needs a UTF-8-aware writer,
 * which does not exist yet; a caller reaching for one should not be handed this
 * instead. The request parser already limits a target to printable ASCII, so
 * nothing this server logs today is affected.
 */

#ifndef RECON_HTTP_JSON_H
#define RECON_HTTP_JSON_H

#include <stddef.h>

/*
 * Escape `in` for use between the quotes of a JSON string. The quotes are
 * **not** added: a caller writes them, because a caller often wants to build a
 * key and a value in one `snprintf` and adding them here would mean stripping
 * them there.
 *
 * Returns the number of bytes written, not counting the terminator, or -1 when
 * it would not fit or the text contains a byte this cannot represent.
 *
 * **Refuses rather than truncates.** A string cut mid-escape ends `\u00` and
 * the next thing a parser reads is whatever followed -- which is the confusion
 * escaping exists to prevent, arrived at from the other direction.
 */
long json_escape(const char *in, char *out, size_t room);

/* The same for `len` bytes rather than a terminated string, for text that may
 * legally contain any byte -- a form value, a log line built from one. */
long json_escape_n(const char *in, size_t len, char *out, size_t room);

#endif
