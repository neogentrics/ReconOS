/*
 * Escaping text for HTML.
 *
 * See `escape.h` for the fault this removes and for why the list of characters
 * is five rather than two.
 *
 * --- What was tried and rejected ---
 *
 * **Escaping in place, growing the buffer as needed.** Every entity is longer
 * than the character it replaces, so in-place means shifting the tail on each
 * one -- quadratic on a string of quotes, and a caller has no way to know how
 * much room to leave. Two buffers and a length check is both simpler and
 * honest about what it needs.
 *
 * **Writing as much as fits and reporting the length.** It is what `snprintf`
 * does and it is wrong here. A cut in the middle of `&quot;` leaves `&qu` on
 * the page, and a browser reading what follows as the rest of an entity is the
 * confusion this file exists to prevent. Refusing is the only safe answer, and
 * it is why the return value has to be checked.
 */

#include "escape.h"

long http_escape_n(const char *in, size_t len, char *out, size_t room)
{
	size_t i, at = 0;

	if (!out || room == 0)
		return -1;
	if (!in)
		return -1;

	for (i = 0; i < len; i++) {
		const char *entity = 0;
		size_t elen = 0;

		switch (in[i]) {
		case '&':  entity = "&amp;";  elen = 5; break;
		case '<':  entity = "&lt;";   elen = 4; break;
		case '>':  entity = "&gt;";   elen = 4; break;
		case '"':  entity = "&quot;"; elen = 6; break;
		case '\'': entity = "&#39;";  elen = 5; break;
		default:   break;
		}

		if (entity) {
			size_t k;

			/* Room for the whole entity, or nothing. Half an
			 * entity is worse than no output at all. */
			if (at + elen + 1 > room)
				return -1;
			for (k = 0; k < elen; k++)
				out[at++] = entity[k];
			continue;
		}

		if (at + 2 > room)
			return -1;
		out[at++] = in[i];
	}

	out[at] = '\0';
	return (long)at;
}

long http_escape(const char *in, char *out, size_t room)
{
	size_t n = 0;

	if (!in)
		return -1;
	while (in[n])
		n++;
	return http_escape_n(in, n, out, room);
}
