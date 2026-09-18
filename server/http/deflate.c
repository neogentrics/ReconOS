/*
 * The compressor.
 *
 * See `deflate.h` for what is implemented and what is not. This file is the
 * bit writer, the match finder, and the two block types.
 *
 * --- What was tried and rejected ---
 *
 * **Dynamic Huffman codes.** They are what a general-purpose compressor uses
 * and they cost roughly as much code again -- counting frequencies, building
 * two trees, emitting the code lengths through a third tree -- for a few per
 * cent on bodies the size of a console page. Fixed codes are in the
 * specification rather than in the data, which means there is nothing here that
 * can be built wrongly and still decode.
 *
 * **Compressing unconditionally.** On short or already-compressed bodies the
 * compressed form is larger, and a response that grew is a cost with nothing
 * bought. The encoder emits both shapes and keeps the smaller.
 *
 * **A greedy matcher with no lazy evaluation.** Kept, deliberately: lazy
 * matching (taking a shorter match now when a longer one starts at the next
 * byte) is a real gain and it is another state machine to get wrong. The
 * bottleneck here is the wire, not the last four per cent.
 *
 * --- Why the bit order is worth stating ---
 *
 * Deflate writes Huffman codes **most-significant bit first** into a stream
 * that is otherwise filled **least-significant bit first**. That is not a
 * mistake in the specification and it is the single easiest thing to get wrong
 * in this file: everything that is a number -- a length, a distance's extra
 * bits, a stored block's size -- goes in LSB first, and everything that is a
 * Huffman code goes in MSB first. The two writers below are named for it.
 */

#include "deflate.h"

/* --- CRC-32 ---------------------------------------------------------------- */

/*
 * Computed bitwise rather than from a table.
 *
 * A 1 KiB table would be faster and this is not on a path where that matters:
 * it runs once over a body that is about to be written to a socket at a
 * kilobyte a second. The table would also be 256 constants nobody can check by
 * eye, where the polynomial below is one constant that appears in the
 * specification.
 */
unsigned long deflate_crc32(const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned long crc = 0xFFFFFFFFUL;
	size_t i;
	int k;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (k = 0; k < 8; k++) {
			if (crc & 1)
				crc = (crc >> 1) ^ 0xEDB88320UL;
			else
				crc >>= 1;
		}
	}
	return (crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

/* --- the bit writer --------------------------------------------------------- */

struct bits {
	unsigned char *out;
	size_t         room;
	size_t         at;		/* bytes written */
	unsigned long  hold;		/* bits not yet flushed */
	int            count;		/* how many of them */
	int            failed;
};

/* A number: least-significant bit first, which is how deflate writes
 * everything that is not a Huffman code. */
static void put_bits(struct bits *b, unsigned long value, int n)
{
	if (b->failed)
		return;

	b->hold |= (value & ((1UL << n) - 1UL)) << b->count;
	b->count += n;

	while (b->count >= 8) {
		if (b->at >= b->room) {
			b->failed = 1;
			return;
		}
		b->out[b->at++] = (unsigned char)(b->hold & 0xFF);
		b->hold >>= 8;
		b->count -= 8;
	}
}

/* A Huffman code: most-significant bit first. See the file header -- this is
 * the one asymmetry in the format and the easiest thing here to get wrong. */
static void put_code(struct bits *b, unsigned long code, int n)
{
	int i;

	for (i = n - 1; i >= 0; i--)
		put_bits(b, (code >> i) & 1UL, 1);
}

static void align_byte(struct bits *b)
{
	if (b->count > 0)
		put_bits(b, 0, 8 - b->count);
}

/* --- the fixed Huffman tables ---------------------------------------------- */

/*
 * The literal/length code, from RFC 1951 section 3.2.6. Written as the
 * specification writes it, as four ranges, rather than as a table of 288
 * numbers nobody can check.
 *
 *   0..143    8 bits, 00110000 through 10111111
 *   144..255  9 bits, 110010000 through 111111111
 *   256..279  7 bits, 0000000 through 0010111
 *   280..287  8 bits, 11000000 through 11000111
 */
static void put_literal(struct bits *b, int symbol)
{
	if (symbol < 144)
		put_code(b, 0x30UL + (unsigned long)symbol, 8);
	else if (symbol < 256)
		put_code(b, 0x190UL + (unsigned long)(symbol - 144), 9);
	else if (symbol < 280)
		put_code(b, (unsigned long)(symbol - 256), 7);
	else
		put_code(b, 0xC0UL + (unsigned long)(symbol - 280), 8);
}

/* Length codes 257..285: the base length each one starts at, and how many
 * extra bits follow. Straight out of the specification's table. */
static const unsigned short LENGTH_BASE[] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char LENGTH_EXTRA[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/* Distance codes 0..29, the same shape. */
static const unsigned short DIST_BASE[] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
	12289, 16385, 24577
};
static const unsigned char DIST_EXTRA[] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void put_match(struct bits *b, int length, int distance)
{
	int i;

	for (i = 28; i >= 0; i--) {
		if (length >= (int)LENGTH_BASE[i])
			break;
	}
	put_literal(b, 257 + i);
	if (LENGTH_EXTRA[i])
		put_bits(b, (unsigned long)(length - LENGTH_BASE[i]),
		         LENGTH_EXTRA[i]);

	for (i = 29; i >= 0; i--) {
		if (distance >= (int)DIST_BASE[i])
			break;
	}
	/* A distance code is five bits, fixed, and **not** the literal code.
	 * Using `put_literal` here is the other classic way to write a stream
	 * that looks plausible and decodes to rubbish. */
	put_code(b, (unsigned long)i, 5);
	if (DIST_EXTRA[i])
		put_bits(b, (unsigned long)(distance - DIST_BASE[i]),
		         DIST_EXTRA[i]);
}

/* --- finding matches -------------------------------------------------------- */

/*
 * A hash of three bytes, and a chain of where that hash was last seen.
 *
 * `head` is the most recent position for each hash; `prev` links each position
 * to the one before it with the same hash. Walking that chain backwards gives
 * candidate matches in order of increasing distance, which is also decreasing
 * cost -- so the first long enough is usually the one to take.
 */
#define HASH_BITS  15
#define HASH_SIZE  (1 << HASH_BITS)

struct matcher {
	int head[HASH_SIZE];
	int prev[DEFLATE_WINDOW];
};

static unsigned hash3(const unsigned char *p)
{
	return (unsigned)(((unsigned)p[0] << 10) ^ ((unsigned)p[1] << 5)
	                  ^ (unsigned)p[2]) & (HASH_SIZE - 1);
}

/* --- the two block types ---------------------------------------------------- */

/*
 * A stored block: no compression, byte-aligned, with its length and the length's
 * complement.
 *
 * The complement is the format's own check that a decoder and an encoder agree
 * about where the block ends, and writing it wrong produces a stream that every
 * decompressor rejects -- which is the good kind of wrong.
 */
static void put_stored(struct bits *b, const unsigned char *in, size_t len,
                       int last)
{
	size_t at = 0;

	do {
		size_t take = len - at;
		size_t i;

		if (take > 65535)
			take = 65535;

		put_bits(b, (unsigned long)(last && at + take == len), 1);
		put_bits(b, 0, 2);		/* type 00: stored */
		align_byte(b);

		put_bits(b, (unsigned long)(take & 0xFF), 8);
		put_bits(b, (unsigned long)((take >> 8) & 0xFF), 8);
		put_bits(b, (unsigned long)(~take & 0xFF), 8);
		put_bits(b, (unsigned long)((~take >> 8) & 0xFF), 8);

		for (i = 0; i < take; i++)
			put_bits(b, in[at + i], 8);
		at += take;
	} while (at < len);

	/*
	 * An empty input needs no special case, and the first draft gave it
	 * one anyway -- an extra empty block after the loop. The loop is a
	 * `do`, so it had already written one, and the stream carried **two
	 * final blocks**. Python's `gzip` read the first, stopped, and
	 * reported a CRC failure over the wrong number of bytes, which is a
	 * long way from the cause.
	 *
	 * Kept as a comment because the fault is invisible by reading: both
	 * blocks are individually correct.
	 */
}

static void put_fixed(struct bits *b, struct matcher *m,
                      const unsigned char *in, size_t len)
{
	size_t at = 0;
	size_t i;

	put_bits(b, 1, 1);		/* final */
	put_bits(b, 1, 2);		/* type 01: fixed Huffman */

	for (i = 0; i < HASH_SIZE; i++)
		m->head[i] = -1;
	for (i = 0; i < DEFLATE_WINDOW; i++)
		m->prev[i] = -1;

	while (at < len) {
		int best_len = 0;
		int best_dist = 0;

		if (at + DEFLATE_MATCH_MIN <= len) {
			unsigned h = hash3(in + at);
			int candidate = m->head[h];
			int tries = 0;

			while (candidate >= 0 && tries < DEFLATE_CHAIN_MAX) {
				size_t dist = at - (size_t)candidate;
				size_t k = 0;
				size_t max = len - at;

				if (dist == 0 || dist > DEFLATE_WINDOW)
					break;
				if (max > DEFLATE_MATCH_MAX)
					max = DEFLATE_MATCH_MAX;

				while (k < max
				       && in[candidate + (int)k] == in[at + k])
					k++;

				if ((int)k > best_len) {
					best_len = (int)k;
					best_dist = (int)dist;
					if (best_len >= DEFLATE_MATCH_MAX)
						break;
				}
				candidate = m->prev[(size_t)candidate
				                    % DEFLATE_WINDOW];
				tries++;
			}

			/* Record this position, whether or not a match was
			 * found -- the next occurrence needs to find it. */
			m->prev[at % DEFLATE_WINDOW] = m->head[h];
			m->head[h] = (int)at;
		}

		if (best_len >= DEFLATE_MATCH_MIN) {
			int step;

			put_match(b, best_len, best_dist);

			/* Every byte the match covered still has to be
			 * registered, or the next match cannot see back past
			 * it. Missing this produces a correct stream that
			 * compresses badly, which is the kind of fault nothing
			 * reports. */
			for (step = 1; step < best_len; step++) {
				size_t p = at + (size_t)step;

				if (p + DEFLATE_MATCH_MIN > len)
					break;
				{
					unsigned h = hash3(in + p);

					m->prev[p % DEFLATE_WINDOW] = m->head[h];
					m->head[h] = (int)p;
				}
			}
			at += (size_t)best_len;
			continue;
		}

		put_literal(b, in[at]);
		at++;
	}

	put_literal(b, 256);		/* end of block */
	align_byte(b);
}

/* --- gzip ------------------------------------------------------------------- */

size_t deflate_bound(size_t len)
{
	/* A stored block holds 65535 bytes and costs five for its header, plus
	 * the eighteen bytes of gzip framing, plus one more block's header for
	 * an empty input. */
	return len + ((len / 65535) + 1) * 5 + 18 + 5;
}

long deflate_gzip(const void *in, size_t len, void *out, size_t room)
{
	/*
	 * Static, and the reason is the same as everywhere else in this
	 * program: `struct matcher` is a quarter of a megabyte and this
	 * program's stack is not the place for it. One compression happens at
	 * a time -- the server is single-process and `conn_answer` runs to
	 * completion -- so one of these is enough, and a second caller would
	 * be a change to that shape rather than to this line.
	 */
	static struct matcher MATCHER;

	const unsigned char *src = (const unsigned char *)in;
	unsigned char *dst = (unsigned char *)out;
	struct bits b;
	unsigned long crc;
	size_t compressed_end;
	size_t stored_end;
	size_t head = 10;
	int i;

	if (len > DEFLATE_INPUT_MAX)
		return DEFLATE_ETOOBIG;
	if (room < head + 8 + 5)
		return DEFLATE_EROOM;

	/*
	 * The gzip header: magic, method 8 (deflate), no flags, no timestamp,
	 * no extra flags, and an unknown operating system.
	 *
	 * The timestamp is zero on purpose. A real one would make the output
	 * different for the same input on every request, which costs any
	 * caching downstream everything it is for -- and would make this
	 * function non-deterministic, which a suite cannot check.
	 */
	dst[0] = 0x1F;
	dst[1] = 0x8B;
	dst[2] = 8;
	dst[3] = 0;
	dst[4] = dst[5] = dst[6] = dst[7] = 0;
	dst[8] = 0;
	dst[9] = 0xFF;

	/* --- try the compressed form ---------------------------------------- */
	b.out = dst + head;
	b.room = room - head - 8;
	b.at = 0;
	b.hold = 0;
	b.count = 0;
	b.failed = 0;

	if (len > 0)
		put_fixed(&b, &MATCHER, src, len);
	else
		put_stored(&b, src, 0, 1);
	compressed_end = b.failed ? (size_t)-1 : b.at;

	/*
	 * --- and the stored form, if the compressed one is not clearly better
	 *
	 * Measured, not assumed. On short bodies and on anything already
	 * compressed the fixed-Huffman form is larger than the input, and a
	 * response that grew is a cost with nothing bought.
	 */
	stored_end = (size_t)-1;
	if (len > 0 && (compressed_end == (size_t)-1 || compressed_end >= len)) {
		struct bits s;

		s.out = dst + head;
		s.room = room - head - 8;
		s.at = 0;
		s.hold = 0;
		s.count = 0;
		s.failed = 0;
		put_stored(&s, src, len, 1);
		if (!s.failed)
			stored_end = s.at;

		if (stored_end != (size_t)-1
		    && (compressed_end == (size_t)-1
		        || stored_end < compressed_end)) {
			compressed_end = stored_end;
		} else if (compressed_end != (size_t)-1) {
			/* The compressed form was better after all, and the
			 * stored attempt has just overwritten it. Do it again
			 * rather than keeping two buffers: this only happens
			 * when the two are within a few bytes of each other,
			 * which is rare, and a second buffer the size of the
			 * body is not rare at all. */
			b.out = dst + head;
			b.room = room - head - 8;
			b.at = 0;
			b.hold = 0;
			b.count = 0;
			b.failed = 0;
			put_fixed(&b, &MATCHER, src, len);
			compressed_end = b.failed ? (size_t)-1 : b.at;
		}
	}

	if (compressed_end == (size_t)-1)
		return DEFLATE_EROOM;

	/* --- the trailer: CRC and length, both little-endian ----------------- */
	crc = deflate_crc32(src, len);
	for (i = 0; i < 4; i++)
		dst[head + compressed_end + i] =
			(unsigned char)((crc >> (8 * i)) & 0xFF);
	for (i = 0; i < 4; i++)
		dst[head + compressed_end + 4 + i] =
			(unsigned char)(((unsigned long)len >> (8 * i)) & 0xFF);

	return (long)(head + compressed_end + 8);
}
