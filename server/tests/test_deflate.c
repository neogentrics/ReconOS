/*
 * Compressing, and decompressing it again with something that did not write it.
 *
 * --- Why this suite has a decoder in it ---
 *
 * **A compressor cannot be checked by reading it.** Every fault it can have
 * produces bytes that look exactly as plausible as correct ones: a Huffman code
 * written least-significant-bit first, a distance emitted with the literal
 * table instead of the five-bit one, a length whose extra bits are off by one.
 * None of that is visible; all of it decodes to rubbish or to nothing.
 *
 * And a round trip through the same author's compress-then-decompress proves
 * only that the two agree with each other. Two functions written from the same
 * misreading of the specification agree perfectly.
 *
 * So the decoder below is written **from RFC 1951 rather than from
 * `deflate.c`**, and deliberately in the other shape: it reads the fixed tables
 * by their bit patterns where the encoder writes them by their ranges. Where
 * the two disagree, one of them is wrong about the specification, which is
 * exactly the disagreement worth having.
 *
 * `scripts/gzip-probe.py` is the third opinion: it feeds the same output to
 * Python's `zlib`, which was written by somebody else entirely. That one needs
 * a host with Python; this one runs anywhere the suites do.
 */

#include "../http/deflate.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void ok(int cond, const char *what)
{
	checks++;
	if (!cond) {
		failures++;
		printf("  FAIL  %s\n", what);
	}
}

/* --- an inflater, from the specification ----------------------------------- */

struct reader {
	const unsigned char *in;
	size_t len;
	size_t at;		/* byte */
	int    bit;		/* bit within it, 0..7 */
	int    failed;
};

/* One bit, least-significant first, which is how the stream is filled. */
static int get_bit(struct reader *r)
{
	int value;

	if (r->at >= r->len) {
		r->failed = 1;
		return 0;
	}
	value = (r->in[r->at] >> r->bit) & 1;
	r->bit++;
	if (r->bit == 8) {
		r->bit = 0;
		r->at++;
	}
	return value;
}

/* A number: least-significant bit first. */
static int get_bits(struct reader *r, int n)
{
	int value = 0;
	int i;

	for (i = 0; i < n; i++)
		value |= get_bit(r) << i;
	return value;
}

/* A Huffman code: most-significant bit first. The asymmetry in the format, and
 * the thing most easily got wrong in both directions at once. */
static int get_code(struct reader *r, int n)
{
	int value = 0;
	int i;

	for (i = 0; i < n; i++)
		value = (value << 1) | get_bit(r);
	return value;
}

static void align_byte(struct reader *r)
{
	if (r->bit) {
		r->bit = 0;
		r->at++;
	}
}

/*
 * A fixed-Huffman symbol, decoded by the bit patterns in RFC 1951 section
 * 3.2.6 rather than by the ranges the encoder writes.
 *
 *   7 bits  0000000 .. 0010111   -> 256..279
 *   8 bits  00110000 .. 10111111 -> 0..143
 *   8 bits  11000000 .. 11000111 -> 280..287
 *   9 bits  110010000 .. 111111111 -> 144..255
 */
static int get_symbol(struct reader *r)
{
	int v = get_code(r, 7);

	if (v <= 0x17)
		return 256 + v;

	v = (v << 1) | get_bit(r);
	if (v >= 0x30 && v <= 0xBF)
		return v - 0x30;
	if (v >= 0xC0 && v <= 0xC7)
		return 280 + (v - 0xC0);

	v = (v << 1) | get_bit(r);
	if (v >= 0x190 && v <= 0x1FF)
		return 144 + (v - 0x190);

	r->failed = 1;
	return -1;
}

static const unsigned short LEN_BASE[] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char LEN_EXTRA[] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short D_BASE[] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
	12289, 16385, 24577
};
static const unsigned char D_EXTRA[] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* Decompress a whole gzip stream. Returns the length, or -1. */
static long ungzip(const unsigned char *in, size_t len, unsigned char *out,
                   size_t room)
{
	struct reader r;
	size_t at = 0;
	int last = 0;

	if (len < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8
	    || in[3] != 0)
		return -1;

	r.in = in;
	r.len = len - 8;		/* the trailer is not deflate data */
	r.at = 10;
	r.bit = 0;
	r.failed = 0;

	while (!last && !r.failed) {
		int type;

		last = get_bit(&r);
		type = get_bits(&r, 2);

		if (type == 0) {
			int n, complement;
			int i;

			align_byte(&r);
			n = get_bits(&r, 16);
			complement = get_bits(&r, 16);
			if (((n ^ 0xFFFF) & 0xFFFF) != complement)
				return -1;
			for (i = 0; i < n; i++) {
				if (at >= room || r.at >= r.len)
					return -1;
				out[at++] = r.in[r.at++];
			}
			continue;
		}

		if (type != 1)
			return -1;	/* dynamic Huffman: not emitted here */

		for (;;) {
			int symbol = get_symbol(&r);

			if (r.failed)
				return -1;
			if (symbol == 256)
				break;
			if (symbol < 256) {
				if (at >= room)
					return -1;
				out[at++] = (unsigned char)symbol;
				continue;
			}
			{
				int li = symbol - 257;
				int length, di, distance, i;

				if (li < 0 || li > 28)
					return -1;
				length = LEN_BASE[li]
				         + get_bits(&r, LEN_EXTRA[li]);

				di = get_code(&r, 5);
				if (di < 0 || di > 29)
					return -1;
				distance = D_BASE[di]
				           + get_bits(&r, D_EXTRA[di]);

				if ((size_t)distance > at)
					return -1;
				for (i = 0; i < length; i++) {
					if (at >= room)
						return -1;
					out[at] = out[at - (size_t)distance];
					at++;
				}
			}
		}
	}

	if (r.failed)
		return -1;

	/* The trailer, which is the format's own opinion of whether any of the
	 * above was right. */
	{
		unsigned long crc = 0, size = 0;
		int i;

		for (i = 0; i < 4; i++)
			crc |= (unsigned long)in[len - 8 + i] << (8 * i);
		for (i = 0; i < 4; i++)
			size |= (unsigned long)in[len - 4 + i] << (8 * i);

		if (size != (unsigned long)at)
			return -1;
		if (crc != deflate_crc32(out, at))
			return -1;
	}

	return (long)at;
}

/* --- the checks -------------------------------------------------------------- */

static unsigned char SRC[80000];
static unsigned char GZ[120000];
static unsigned char BACK[80000];

static void round_trips(const unsigned char *in, size_t len, const char *what)
{
	long n, back;

	checks++;
	n = deflate_gzip(in, len, GZ, sizeof(GZ));
	if (n < 0) {
		failures++;
		printf("  FAIL  %s -- compressing gave %ld\n", what, n);
		return;
	}

	checks++;
	back = ungzip(GZ, (size_t)n, BACK, sizeof(BACK));
	if (back < 0) {
		failures++;
		printf("  FAIL  %s -- the stream does not decode\n", what);
		return;
	}

	checks++;
	if ((size_t)back != len || (len && memcmp(BACK, in, len) != 0)) {
		failures++;
		printf("  FAIL  %s -- came back %ld bytes, sent %lu\n", what,
		       back, (unsigned long)len);
	}
}

static void round_trips_text(const char *text, const char *what)
{
	round_trips((const unsigned char *)text, strlen(text), what);
}

int main(void)
{
	size_t i;
	long n;

	printf("compressing, and decompressing it with something else\n");

	/* --- the one number somebody else wrote down --------------------------
	 *
	 * The standard check value for this polynomial. Everything else in this
	 * file is two implementations agreeing; this is a number from the
	 * specification. */
	ok(deflate_crc32("123456789", 9) == 0xCBF43926UL,
	   "CRC-32 of \"123456789\" is the published check value");
	ok(deflate_crc32("", 0) == 0,
	   "and of nothing is zero");

	/* --- the framing ------------------------------------------------------- */
	n = deflate_gzip("hello", 5, GZ, sizeof(GZ));
	ok(n > 0, "a short string compresses");
	ok(n > 0 && GZ[0] == 0x1F && GZ[1] == 0x8B,
	   "and starts with the gzip magic");
	ok(n > 0 && GZ[2] == 8, "with method 8, which is deflate");
	ok(n > 0 && GZ[3] == 0, "and no flags, so no name and no extra field");
	ok(n > 0 && GZ[4] == 0 && GZ[5] == 0 && GZ[6] == 0 && GZ[7] == 0,
	   "and a zero timestamp, so the same input always gives the same bytes");

	/*
	 * Deterministic, checked rather than asserted. A timestamp or any other
	 * varying field would cost every cache downstream everything it is for,
	 * and would make this suite unable to compare anything.
	 */
	{
		static unsigned char again[120000];
		long m = deflate_gzip("hello", 5, again, sizeof(again));

		ok(m == n && memcmp(again, GZ, (size_t)n) == 0,
		   "compressing the same bytes twice gives the same bytes");
	}

	/* --- round trips -------------------------------------------------------- */
	round_trips_text("", "an empty body");
	round_trips_text("a", "one byte");
	round_trips_text("ab", "two bytes");
	round_trips_text("hello world", "a short line");
	round_trips_text("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	                 "one byte repeated, which is one long match");
	round_trips_text("abcabcabcabcabcabcabcabcabcabcabcabcabcabcabcabc",
	                 "a short cycle, which is a match overlapping itself");

	/*
	 * A match that reaches back exactly three bytes and runs longer than
	 * its own distance. The decoder must copy byte by byte, not block by
	 * block -- `out[at] = out[at - distance]` while `at` advances is the
	 * whole of run-length encoding in deflate, and a memcpy here produces
	 * the wrong bytes without failing.
	 */
	round_trips_text("xyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyz"
	                 "xyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyzxyz",
	                 "a run longer than the distance it copies from");

	round_trips_text("The quick brown fox jumps over the lazy dog. "
	                 "The quick brown fox jumps over the lazy dog. "
	                 "The quick brown fox jumps over the lazy dog.",
	                 "a sentence three times over");

	/* Every byte value, so nothing depends on the input being text -- the
	 * 144..255 range uses the nine-bit codes, which is a different branch
	 * in both the encoder and the decoder. */
	for (i = 0; i < 256; i++)
		SRC[i] = (unsigned char)i;
	round_trips(SRC, 256, "every byte value once");

	for (i = 0; i < 4096; i++)
		SRC[i] = (unsigned char)(i & 0xFF);
	round_trips(SRC, 4096, "every byte value sixteen times over");

	/*
	 * Something that does not compress.
	 *
	 * A simple generator rather than real randomness, so the case is the
	 * same on every run: a suite that sometimes exercises the stored path
	 * and sometimes does not is a suite that sometimes checks it.
	 */
	{
		unsigned long seed = 12345;

		for (i = 0; i < 20000; i++) {
			seed = seed * 1103515245UL + 12345UL;
			SRC[i] = (unsigned char)((seed >> 16) & 0xFF);
		}
		round_trips(SRC, 20000, "twenty thousand bytes that do not "
		                        "compress");

		n = deflate_gzip(SRC, 20000, GZ, sizeof(GZ));
		ok(n > 0 && (size_t)n <= 20000 + 64,
		   "and the answer is not meaningfully larger than the input, "
		   "because a stored block is used when compressing would grow it");
	}

	/* And something that compresses enormously, to prove the matcher is
	 * doing anything at all. */
	{
		for (i = 0; i < 40000; i++)
			SRC[i] = (unsigned char)('a' + (i % 7));
		round_trips(SRC, 40000, "forty thousand bytes of a short cycle");

		n = deflate_gzip(SRC, 40000, GZ, sizeof(GZ));
		ok(n > 0 && n < 2000,
		   "and it compresses to under a twentieth, which literals "
		   "alone could never do");
	}

	/*
	 * --- something shaped like prose, and the one check the matcher earns
	 *
	 * **This case exists because a mutant survived.** Deleting the loop
	 * that registers the positions a match covered leaves a perfectly
	 * correct stream -- every round trip above still passed -- that simply
	 * compresses worse, because the next match cannot see back past the
	 * previous one. A fault that only costs bytes is invisible to a suite
	 * that only checks bytes come back.
	 *
	 * Measured on this repository's own text, with and without that loop:
	 *
	 *     server/README.md   42546 -> 21184 with, 23727 without
	 *     docs/WEB.md        29644 -> 14976 with, 16800 without
	 *     server/http/serve.c 48763 -> 19836 with, 22530 without
	 *
	 * About eleven per cent, consistently. So the corpus below is built to
	 * be shaped like that -- words drawn from a small vocabulary, which is
	 * what makes text compressible -- and the bound is set between the two
	 * numbers it produces.
	 */
	{
		static const char *const WORDS[] = {
			"the ", "server ", "request ", "and ", "a ",
			"response ", "which ", "is "
		};
		unsigned long seed = 7;
		size_t at = 0;

		while (at < 30000) {
			const char *w;
			size_t k;

			seed = seed * 1103515245UL + 12345UL;
			w = WORDS[(seed >> 16) & 7];
			for (k = 0; w[k] && at < sizeof(SRC); k++)
				SRC[at++] = (unsigned char)w[k];
		}
		round_trips(SRC, at, "thirty thousand bytes shaped like prose");

		n = deflate_gzip(SRC, at, GZ, sizeof(GZ));
		ok(n > 0 && n < 6400,
		   "and the matcher registers the bytes a match covered -- "
		   "without that it is about a tenth larger");
	}

	/* --- what it refuses ------------------------------------------------------ */
	{
		static unsigned char tiny[8];

		ok(deflate_gzip("hello", 5, tiny, sizeof(tiny))
		   == DEFLATE_EROOM,
		   "an output buffer too small to hold even the header is "
		   "refused");
		ok(deflate_gzip("hello world, and some more text to make this "
		                "longer than the room given to it", 76,
		                GZ, 24) == DEFLATE_EROOM,
		   "and one that runs out part way through");
	}

	/* --- the bound ------------------------------------------------------------
	 *
	 * A caller that allocates `deflate_bound(len)` must never be refused.
	 * Checked against the case that actually exercises it: input that does
	 * not compress. */
	{
		unsigned long seed = 999;
		size_t len = 30000;

		for (i = 0; i < len; i++) {
			seed = seed * 1103515245UL + 12345UL;
			SRC[i] = (unsigned char)((seed >> 16) & 0xFF);
		}
		ok(deflate_bound(len) >= len + 18,
		   "the bound leaves room for the framing");
		n = deflate_gzip(SRC, len, GZ, deflate_bound(len));
		ok(n > 0, "and compressing into exactly the bound succeeds");
		ok(n > 0 && (size_t)n <= deflate_bound(len),
		   "and the answer fits inside it");
	}

	printf("  %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
