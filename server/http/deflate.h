/*
 * Compressing a response, in the one format every client already has.
 *
 * --- Why this is worth building here ---
 *
 * Not because compression is a feature a web server is supposed to have. On
 * this machine it is arithmetic against a measured constraint: VF-013 recorded
 * a burst arriving at roughly a kilobyte a second, and `docs/SERVER.md` carries
 * the send side's own note about `HTTP_SEND_CHUNK`. **Bytes on this wire are
 * expensive**, and the console page, the JSON log and the files on the volume
 * are all text, which is the thing deflate is best at.
 *
 * It is also the last widely-used transformation of a response this server
 * cannot do, and every client already speaks it -- `Accept-Encoding: gzip` is
 * sent by every browser and by curl on request.
 *
 * --- What is implemented, and what deliberately is not ---
 *
 * **Deflate, with stored blocks and fixed Huffman codes.** Not dynamic Huffman:
 * a dynamic block has to build and emit its own code lengths, which is roughly
 * as much code again for a few per cent on bodies this size. Fixed codes are a
 * complete, conforming deflate stream that every decompressor in the world
 * reads, and the tables are in the specification rather than in the data.
 *
 * **A stored block whenever the compressed form would be larger.** Compression
 * that makes a response bigger is a cost with no benefit, and it happens on
 * short bodies and on anything already compressed. The encoder measures rather
 * than assumes: it compresses, compares, and emits whichever is smaller.
 *
 * **`gzip`, not `deflate`, as the content coding.** Both are on offer in HTTP
 * and only one of them is unambiguous: `Content-Encoding: deflate` was
 * specified as zlib-wrapped and is sent raw by enough software that clients
 * guess, which is a format two readers read differently -- the same fault as
 * everything else this project refuses. gzip has a header, a CRC and a length,
 * and nobody disagrees about it.
 *
 * --- The check that makes this trustworthy ---
 *
 * A compressor cannot be verified by reading it. The suite therefore checks
 * every case **against an independent decompressor**: `scripts/gzip-probe.py`
 * feeds the output to Python's `zlib`, which was not written here and knows
 * nothing about this code. A round trip through the same author's two
 * functions proves only that they agree with each other.
 */

#ifndef RECON_HTTP_DEFLATE_H
#define RECON_HTTP_DEFLATE_H

#include <stddef.h>

/*
 * The window, and the shortest and longest match.
 *
 * 32768 is deflate's own window and is not a choice. The match lengths are:
 * three is the shortest run worth encoding as a match rather than as literals,
 * and 258 is the longest the format can express.
 */
#define DEFLATE_WINDOW      32768
#define DEFLATE_MATCH_MIN       3
#define DEFLATE_MATCH_MAX     258

/*
 * How many places to look for a match before taking the best so far.
 *
 * A compressor's whole speed-against-size decision, in one number. This one is
 * deliberately small: on a machine where the bottleneck is the wire rather
 * than the processor the difference between a good match and the best match is
 * a few per cent, and the processor here is also serving every other
 * connection in the pool.
 */
#define DEFLATE_CHAIN_MAX      16

/* Verdicts. */
#define DEFLATE_OK          0
#define DEFLATE_EROOM     (-1)	/* the output buffer is too small */
#define DEFLATE_ETOOBIG   (-2)	/* more input than this will take */

/* The largest input this will compress in one call. Bounded because the whole
 * thing is done in memory, with no allocation, in a program whose buffers are
 * already the largest thing it owns. */
#define DEFLATE_INPUT_MAX  262144

/*
 * Compress `in` into `out` as a **gzip** stream: header, deflate data, CRC and
 * length.
 *
 * Returns the number of bytes written, or a negative verdict. `room` must be
 * large enough for the worst case, which is an incompressible input: the
 * stored form is the input plus five bytes for every 65535, plus the eighteen
 * bytes of gzip framing. `deflate_bound` gives that number.
 *
 * Deterministic: the same input always produces the same bytes, which is what
 * lets a suite compare against a fixture and what would let an `ETag` be
 * computed over the compressed form if one is ever wanted.
 */
long deflate_gzip(const void *in, size_t len, void *out, size_t room);

/* The largest output `deflate_gzip` can produce for an input of `len`. Always
 * enough; a caller that allocates this never gets `DEFLATE_EROOM`. */
size_t deflate_bound(size_t len);

/*
 * CRC-32, as gzip uses it.
 *
 * Exposed because it is a rule rather than a step -- the suite checks it
 * against the published value for `"123456789"`, which is the standard vector
 * for exactly this polynomial and is the one thing here that can be verified
 * against a number somebody else wrote down.
 */
unsigned long deflate_crc32(const void *data, size_t len);

#endif
