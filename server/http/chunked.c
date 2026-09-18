/*
 * The chunked decoder.
 *
 * See `chunked.h` for the rule and for the three deliberate narrowings. This
 * file is the state machine and the refusals.
 *
 * --- What was tried and rejected ---
 *
 * **Reading a whole line into a scratch buffer and then parsing it.** It is
 * how the head parser works and it is wrong here: a line arrives across as
 * many reads as the network chooses, so the scratch buffer would have to
 * survive between calls, be bounded, and be copied into -- three things to get
 * wrong in place of a state that already has to exist. The machine below
 * consumes one byte at a time and keeps only what it needs.
 *
 * **Accepting a bare LF as a line ending.** Every other reader in this project
 * requires CRLF, `request.c` refuses a bare CR in a header value, and a
 * chunked parser that takes `1\n` from a client whose proxy took `1` and
 * waited is the smuggling case exactly. Refused, and the suite has it.
 *
 * **Treating `0\r\n\r\n` as the whole terminator and stopping there.** It is
 * the common case and it is a special case of *last chunk, then a trailer
 * section that happens to be empty*. Writing it as the special case means the
 * non-empty trailer section is handled by a second piece of code, and the two
 * disagree the first time one is edited. There is one path: the zero chunk
 * ends the data, and the trailer section ends the body.
 */

#include "chunked.h"

/* The states. Named rather than numbered because the whole point of the
 * machine is that a reader can see where a byte goes. */
enum {
	WANT_SIZE = 0,	/* hex digits of a chunk size */
	WANT_SIZE_CR,	/* the CR that ends the size line */
	WANT_SIZE_LF,	/* and its LF */
	WANT_DATA,	/* `size` bytes of body */
	WANT_DATA_CR,	/* the CR after the data */
	WANT_DATA_LF,	/* and its LF */
	WANT_TRAILER,	/* a trailer line, or the blank line that ends them */
	WANT_TRAILER_LF,/* the LF of a trailer line's CRLF */
	WANT_END_LF,	/* the LF of the blank line that ends the body */
	DONE
};

static int hex_of(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

void http_chunked_begin(struct http_chunked *c)
{
	c->state = WANT_SIZE;
	c->size = 0;
	c->digits = 0;
	c->line = 0;
	c->trailers = 0;
	c->saw_last = 0;
	c->in = 0;
	c->out = 0;
}

int http_chunked_feed(struct http_chunked *c, char *buf, size_t have,
                      size_t room)
{
	while (c->in < have) {
		char ch = buf[c->in];

		switch (c->state) {
		case WANT_SIZE: {
			int digit = hex_of(ch);

			if (digit >= 0) {
				/*
				 * Bounded, and a refusal rather than a wrap.
				 *
				 * `strtoul` would take `ffffffffffffffffff`
				 * and report success with a saturated value,
				 * which is a body length nobody sent. The
				 * digit count is checked before the value can
				 * overflow rather than after.
				 */
				if (c->digits >= HTTP_CHUNK_DIGITS_MAX)
					return HTTP_EMALFORMED;
				c->size = (c->size << 4) | (unsigned long)digit;
				c->digits++;
				c->in++;
				continue;
			}
			if (ch == '\r') {
				/* A line with no digits at all. `\r\n` where a
				 * size belongs is not a zero chunk; it is a
				 * message with nothing where its framing
				 * should be. */
				if (c->digits == 0)
					return HTTP_EMALFORMED;
				c->state = WANT_SIZE_LF;
				c->in++;
				continue;
			}
			/*
			 * Everything else, and this is the important refusal.
			 *
			 * `;` opens a chunk extension -- legal, ignorable, and
			 * the one place in this format where a parser is
			 * invited to read past text it does not understand.
			 * `+`, `-` and a leading `0x` are what `strtoul`
			 * accepts and a stricter proxy does not, which is two
			 * readers and one message. A space before the CRLF is
			 * the same family.
			 *
			 * None of them is repaired and none is skipped. See
			 * `chunked.h` on what refusing extensions costs, which
			 * is a request nobody makes.
			 */
			return HTTP_EMALFORMED;
		}

		case WANT_SIZE_LF:
			if (ch != '\n')
				return HTTP_EMALFORMED;
			c->in++;
			c->digits = 0;
			if (c->size == 0) {
				/* The last chunk. What follows is the trailer
				 * section, which may be empty -- and the blank
				 * line that ends it is what ends the body. */
				c->saw_last = 1;
				c->state = WANT_TRAILER;
				c->line = 0;
				continue;
			}
			/*
			 * A chunk that cannot fit, refused at the moment it is
			 * declared rather than after its bytes have been
			 * received and counted.
			 *
			 * The same courtesy `Content-Length` gets: a client
			 * that announces more than this server will hold is
			 * told now, on the line that announced it, instead of
			 * spending the transfer to be refused at the end.
			 */
			if (c->size > (unsigned long)(room - c->out))
				return HTTP_EBODY_LONG;

			c->state = WANT_DATA;
			continue;

		case WANT_DATA: {
			size_t left = have - c->in;
			size_t take = left;

			if ((unsigned long)take > c->size)
				take = (size_t)c->size;

			/*
			 * The body's own bound, checked before the copy.
			 *
			 * A chunked body has no declared length, so this is
			 * the only place a size limit can be applied -- and it
			 * has to be applied as the bytes arrive rather than at
			 * the end, or the limit is enforced by a buffer that
			 * has already been overrun.
			 */
			if (c->out + take > room)
				return HTTP_EBODY_LONG;

			/* In place. Safe because `out` starts behind `in` and
			 * both advance by one per data byte -- see
			 * `chunked.h`. */
			{
				size_t i;

				for (i = 0; i < take; i++)
					buf[c->out + i] = buf[c->in + i];
			}
			c->out += take;
			c->in += take;
			c->size -= (unsigned long)take;

			if (c->size == 0)
				c->state = WANT_DATA_CR;
			continue;
		}

		case WANT_DATA_CR:
			/* Exactly CRLF after the data, and nothing else. A
			 * chunk whose data is one byte short would otherwise
			 * eat the CR and the framing would slide by one for
			 * the rest of the message. */
			if (ch != '\r')
				return HTTP_EMALFORMED;
			c->state = WANT_DATA_LF;
			c->in++;
			continue;

		case WANT_DATA_LF:
			if (ch != '\n')
				return HTTP_EMALFORMED;
			c->state = WANT_SIZE;
			c->size = 0;
			c->digits = 0;
			c->in++;
			continue;

		case WANT_TRAILER:
			if (ch == '\r') {
				c->in++;
				if (c->line == 0) {
					/* The blank line: the end of the
					 * trailer section, and of the body. */
					c->state = WANT_END_LF;
					continue;
				}
				c->state = WANT_TRAILER_LF;
				continue;
			}
			if (ch == '\n')
				return HTTP_EMALFORMED;	/* bare LF */
			/*
			 * A trailer's bytes.
			 *
			 * Counted and bounded, and **not kept**. `chunked.h`
			 * says why at length: a trailer arrives after every
			 * decision this server makes about the request, so one
			 * that became a header would be a header whose value
			 * arrived after it was read.
			 *
			 * The syntax still has to be right, because the body
			 * is not complete until the section is, and a reader
			 * that stopped at the first `0\r\n` would leave a
			 * trailer section sitting in the buffer to be read as
			 * the next request.
			 */
			if (c->line >= HTTP_CHUNK_LINE_MAX)
				return HTTP_EMALFORMED;
			c->line++;
			c->in++;
			continue;

		case WANT_TRAILER_LF:
			if (ch != '\n')
				return HTTP_EMALFORMED;
			if (c->trailers >= HTTP_CHUNK_TRAILERS_MAX)
				return HTTP_EMALFORMED;
			c->trailers++;
			c->line = 0;
			c->state = WANT_TRAILER;
			c->in++;
			continue;

		case WANT_END_LF:
			if (ch != '\n')
				return HTTP_EMALFORMED;
			c->in++;
			c->state = DONE;
			return HTTP_OK;

		default:
			/*
			 * Fed again after it finished.
			 *
			 * A refusal rather than `HTTP_OK`, because the caller
			 * asking means it does not know the body ended, and a
			 * second `HTTP_OK` would let it treat the *next*
			 * request's bytes as this one's.
			 */
			return HTTP_EMALFORMED;
		}
	}

	if (c->state == DONE)
		return HTTP_OK;

	/*
	 * Out of bytes, not out of message.
	 *
	 * Deliberately `HTTP_PARTIAL` even when the last chunk has been seen:
	 * the trailer section still has to end. A decoder that reported
	 * completion at `0\r\n` would hand the trailer section to whatever
	 * reads next -- which on a keep-alive connection is the parser looking
	 * for a request line.
	 */
	return HTTP_PARTIAL;
}
