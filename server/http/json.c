/*
 * Escaping text for JSON.
 *
 * See `json.h` for the fault this removes and for why a byte above 0x7F is
 * refused rather than passed through.
 *
 * --- What was tried and rejected ---
 *
 * **Escaping only the quote and the backslash.** They are the two that break
 * the *syntax*, and stopping there is the version that ships. A raw newline or
 * tab inside a JSON string is invalid -- parsers differ on whether they accept
 * it, which means two readers of one document disagree about where a string
 * ends. That is the same class of fault as two HTTP parsers disagreeing about
 * where a body ends, and it gets the same answer: emit nothing a reader could
 * take two ways.
 *
 * **`\u00XX` for everything below 0x20, without the short forms.** Correct, and
 * it turns every newline in a log line into six bytes of noise a person has to
 * decode by eye. The five characters with short escapes get them; the rest,
 * which are genuinely rare in text, get the long form.
 *
 * **Passing bytes above 0x7F through.** See `json.h`. It produces a document
 * that is invalid UTF-8 and therefore invalid JSON, which some parsers reject
 * and others quietly repair -- and a document two parsers read differently is
 * the thing this file exists to prevent.
 */

#include "json.h"

/* One hex digit, lower case. Upper would be equally valid and this picks one
 * rather than leaving it to whichever branch ran. */
static char hex(unsigned v)
{
	static const char DIGITS[] = "0123456789abcdef";

	return DIGITS[v & 0xF];
}

long json_escape_n(const char *in, size_t len, char *out, size_t room)
{
	size_t i, at = 0;

	if (!in || !out || room == 0)
		return -1;

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)in[i];
		const char *shortform = 0;

		switch (c) {
		case '"':  shortform = "\\\""; break;
		case '\\': shortform = "\\\\"; break;
		case '\n': shortform = "\\n";  break;
		case '\r': shortform = "\\r";  break;
		case '\t': shortform = "\\t";  break;
		case '\b': shortform = "\\b";  break;
		case '\f': shortform = "\\f";  break;
		default:   break;
		}

		if (shortform) {
			if (at + 2 + 1 > room)
				return -1;
			out[at++] = shortform[0];
			out[at++] = shortform[1];
			continue;
		}

		/*
		 * Everything else below a space. Rare in text and invalid raw
		 * inside a JSON string, so it gets the long form rather than
		 * being dropped -- a dropped byte is a value that is not the
		 * value that arrived.
		 */
		if (c < 0x20) {
			if (at + 6 + 1 > room)
				return -1;
			out[at++] = '\\';
			out[at++] = 'u';
			out[at++] = '0';
			out[at++] = '0';
			out[at++] = hex(c >> 4);
			out[at++] = hex(c);
			continue;
		}

		/* Above ASCII. Refused -- see `json.h`. */
		if (c > 0x7F)
			return -1;

		if (at + 1 + 1 > room)
			return -1;
		out[at++] = (char)c;
	}

	out[at] = '\0';
	return (long)at;
}

long json_escape(const char *in, char *out, size_t room)
{
	size_t n = 0;

	if (!in)
		return -1;
	while (in[n])
		n++;
	return json_escape_n(in, n, out, room);
}
