/* SHA-256, written out rather than borrowed.
 *
 * The loader needs a hash to verify the kernel it is about to run, and this is
 * the one place in the project where writing a well-known algorithm by hand is
 * the *safe* option rather than the reckless one:
 *
 *   - There is no secret material anywhere near it. A verifier holds a public
 *     key and a public hash, so the timing side-channels that make hand-rolled
 *     cryptography dangerous have nothing to leak.
 *   - The failure modes are both loud. Get it wrong and either every valid
 *     signature is rejected -- noticed on the first boot -- or the digest does
 *     not match the published test vectors, which is checked below.
 *
 * The constants are the first thirty-two bits of the fractional parts of the
 * cube roots of the first sixty-four primes, and the initial state is the same
 * of the square roots of the first eight. They are written here as the
 * specification lists them; deriving them at build time would be showing off at
 * the cost of being able to compare them against the document.
 */
#include "efi.h"
#include "boot_internal.h"

static const UINT32 K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
	0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
	0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
	0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
	0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static UINT32 ror(UINT32 x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

static void block(UINT32 *h, const UINT8 *p)
{
	UINT32 w[64];
	UINT32 a, b, c, d, e, f, g, hh;
	unsigned i;

	for (i = 0; i < 16; i++)
		w[i] = ((UINT32)p[i * 4] << 24) | ((UINT32)p[i * 4 + 1] << 16) |
		       ((UINT32)p[i * 4 + 2] << 8) | (UINT32)p[i * 4 + 3];

	for (i = 16; i < 64; i++) {
		UINT32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^
			    (w[i - 15] >> 3);
		UINT32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^
			    (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	a = h[0]; b = h[1]; c = h[2]; d = h[3];
	e = h[4]; f = h[5]; g = h[6]; hh = h[7];

	for (i = 0; i < 64; i++) {
		UINT32 S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
		UINT32 ch = (e & f) ^ ((~e) & g);
		UINT32 t1 = hh + S1 + ch + K[i] + w[i];
		UINT32 S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
		UINT32 maj = (a & b) ^ (a & c) ^ (b & c);
		UINT32 t2 = S0 + maj;

		hh = g; g = f; f = e;
		e = d + t1;
		d = c; c = b; b = a;
		a = t1 + t2;
	}

	h[0] += a; h[1] += b; h[2] += c; h[3] += d;
	h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* The whole message at once. The loader has the kernel in memory before it can
 * do anything else with it, so there is no case here for a streaming interface
 * and none is offered -- an unused incremental API is a second code path that
 * only ever gets exercised by its own test. */
void sha256(const void *data, UINTN len, UINT8 out[32])
{
	UINT32 h[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};
	const UINT8 *p = data;
	UINT8 tail[128];
	UINTN full = len / 64;
	UINTN rest = len % 64;
	UINTN tail_len;
	UINT64 bits = (UINT64)len * 8;
	UINTN i;

	for (i = 0; i < full; i++)
		block(h, p + i * 64);

	/* The padding: the message, a single 1 bit, zeroes, and the length in
	 * bits as a 64-bit big-endian number. The length is of the *original*
	 * message, which is the field that is easy to fill in from the padded
	 * length by accident and produces a hash that is wrong for every input
	 * rather than for some. */
	for (i = 0; i < rest; i++)
		tail[i] = p[full * 64 + i];

	tail[rest] = 0x80;
	tail_len = rest + 1;

	while ((tail_len % 64) != 56)
		tail[tail_len++] = 0;

	for (i = 0; i < 8; i++)
		tail[tail_len + i] = (UINT8)(bits >> (56 - i * 8));
	tail_len += 8;

	for (i = 0; i < tail_len; i += 64)
		block(h, tail + i);

	for (i = 0; i < 8; i++) {
		out[i * 4]     = (UINT8)(h[i] >> 24);
		out[i * 4 + 1] = (UINT8)(h[i] >> 16);
		out[i * 4 + 2] = (UINT8)(h[i] >> 8);
		out[i * 4 + 3] = (UINT8)h[i];
	}
}

/* Checked against the published vectors, at boot, every time.
 *
 * Three inputs, chosen because they exercise different parts of the padding:
 * the empty message (padding only), a message shorter than one block, and one
 * that is 56 bytes -- exactly the length at which the length field no longer
 * fits in the same block and a second block is required. That last is where a
 * hand-written implementation goes wrong, and where an implementation tested
 * only on "abc" passes.
 */
BOOLEAN sha256_self_test(void)
{
	static const struct {
		const char *in;
		UINTN len;
		UINT8 want[32];
	} vectors[] = {
		{ "", 0, {
			0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
			0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
			0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
			0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55 } },
		{ "abc", 3, {
			0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
			0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
			0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
			0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad } },
		{ "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, {
			0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,
			0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
			0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,
			0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1 } },
	};

	unsigned v, i;

	for (v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
		UINT8 got[32];

		sha256(vectors[v].in, vectors[v].len, got);

		for (i = 0; i < 32; i++)
			if (got[i] != vectors[v].want[i])
				return FALSE;
	}

	return TRUE;
}
