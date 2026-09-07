/* Verifying an RSA-2048 signature, so the loader can refuse a kernel that is
 * not the one it expects.
 *
 * --- Why hand-writing this is defensible here -------------------------------
 *
 * "Do not implement your own cryptography" is good advice about *secrets*.
 * Signing needs a private key, constant-time arithmetic, and blinding, and
 * getting any of that wrong leaks the key. **Verifying needs none of it.**
 *
 * Everything this function touches is public: the signature travels beside the
 * kernel on a partition anybody can read, the modulus is compiled into a
 * loader anybody can disassemble, and the digest is of a file anybody can hash
 * themselves. There is no secret to leak through timing, so the ordinary reason
 * to reach for a library does not apply.
 *
 * What is left is arithmetic that either matches the reference implementations
 * or does not, and that is checked by signing with `openssl` and requiring this
 * to accept it -- then corrupting each of the kernel, the signature and the key
 * in turn and requiring it to refuse.
 *
 * --- Why RSA and not something modern ---------------------------------------
 *
 * Ed25519 is the better choice on every axis except the one that matters here:
 * verifying it needs field arithmetic over 2^255-19, point decompression and a
 * scalar multiply -- roughly a thousand lines whose bugs are silent. RSA
 * verification with a fixed public exponent is a modular exponentiation and a
 * padding check, and the padding check is a byte comparison.
 *
 * A smaller thing that can be read in one sitting is worth more here than a
 * better thing that cannot.
 */
#include "efi.h"
#include "boot_internal.h"

#define LIMBS 64		/* 64 x 32 bits = 2048 */

struct big {
	UINT32 v[LIMBS * 2];	/* twice the width, for products */
};

static void big_zero(struct big *a)
{
	unsigned i;

	for (i = 0; i < LIMBS * 2; i++)
		a->v[i] = 0;
}

/* Big-endian bytes, as every RSA value is transmitted, into little-endian
 * limbs, which is how the arithmetic below wants them. Getting this backwards
 * produces a verifier that rejects everything, which is the safe direction to
 * be wrong in and is caught by the first valid signature. */
static void big_from_bytes(struct big *a, const UINT8 *b, unsigned len)
{
	unsigned i;

	big_zero(a);

	for (i = 0; i < len; i++) {
		unsigned limb = (len - 1 - i) / 4;
		unsigned shift = ((len - 1 - i) % 4) * 8;

		if (limb < LIMBS * 2)
			a->v[limb] |= (UINT32)b[i] << shift;
	}
}

static void big_to_bytes(const struct big *a, UINT8 *b, unsigned len)
{
	unsigned i;

	for (i = 0; i < len; i++) {
		unsigned limb = (len - 1 - i) / 4;
		unsigned shift = ((len - 1 - i) % 4) * 8;

		b[i] = (limb < LIMBS * 2) ? (UINT8)(a->v[limb] >> shift) : 0;
	}
}

static int big_cmp(const struct big *a, const struct big *b, unsigned n)
{
	int i;

	for (i = (int)n - 1; i >= 0; i--) {
		if (a->v[i] != b->v[i])
			return a->v[i] > b->v[i] ? 1 : -1;
	}
	return 0;
}

/* a -= b, assuming a >= b. */
static void big_sub(struct big *a, const struct big *b, unsigned n)
{
	UINT64 borrow = 0;
	unsigned i;

	for (i = 0; i < n; i++) {
		UINT64 d = (UINT64)a->v[i] - b->v[i] - borrow;

		a->v[i] = (UINT32)d;
		borrow = (d >> 32) & 1;
	}
}

/* out = a * b, schoolbook. 64 limbs squared is four thousand multiplies, once
 * per boot for seventeen exponentiation steps -- a few milliseconds, and the
 * clarity is worth more here than Karatsuba would be. */
static void big_mul(struct big *out, const struct big *a, const struct big *b,
		    unsigned n)
{
	unsigned i, j;

	big_zero(out);

	for (i = 0; i < n; i++) {
		UINT64 carry = 0;

		for (j = 0; j < n; j++) {
			UINT64 t = (UINT64)a->v[i] * b->v[j] +
				   out->v[i + j] + carry;

			out->v[i + j] = (UINT32)t;
			carry = t >> 32;
		}
		out->v[i + n] = (UINT32)carry;
	}
}

/* out = x mod m, by long division on limbs.
 *
 * Bit at a time rather than a quotient estimate: 2048 iterations of a shift and
 * a conditional subtract is slow and obviously correct, and a wrong quotient
 * digit in a fast version is the kind of bug that only shows on one input in a
 * million -- which for a verifier means one kernel in a million rejected, on
 * somebody's machine, with no way to tell why.
 */
static void big_mod(struct big *out, const struct big *x, const struct big *m,
		    unsigned n)
{
	struct big r;
	int bit;

	big_zero(&r);

	for (bit = (int)(n * 2 * 32) - 1; bit >= 0; bit--) {
		unsigned i;
		UINT32 top = 0;

		/* r <<= 1 */
		for (i = 0; i < n + 1; i++) {
			UINT32 next = r.v[i] >> 31;

			r.v[i] = (r.v[i] << 1) | top;
			top = next;
		}

		/* bring down one bit of x */
		r.v[0] |= (x->v[bit / 32] >> (bit % 32)) & 1;

		if (big_cmp(&r, m, n + 1) >= 0)
			big_sub(&r, m, n + 1);
	}

	/* Copied a limb at a time rather than assigned: a struct assignment
	 * compiles to memcpy, and a freestanding loader has no libc to call
	 * into. The linker says so, which is the right place to find out. */
	{
		unsigned i;

		for (i = 0; i < LIMBS * 2; i++)
			out->v[i] = r.v[i];
	}
}

/* out = base^65537 mod m.
 *
 * The exponent is fixed at 65537 -- 2^16 + 1 -- which is what essentially every
 * RSA key in use has, and fixing it removes a whole category of question about
 * what a hostile exponent could do. A key with a different one is refused by the
 * caller rather than handled here.
 */
static void big_powmod_65537(struct big *out, const struct big *base,
			     const struct big *m, unsigned n)
{
	struct big acc, tmp;
	unsigned i;

	big_mod(&acc, base, m, n);

	for (i = 0; i < 16; i++) {
		big_mul(&tmp, &acc, &acc, n);
		big_mod(&acc, &tmp, m, n);
	}

	big_mul(&tmp, &acc, base, n);
	big_mod(out, &tmp, m, n);
}

/* The DigestInfo prefix for SHA-256, as PKCS#1 v1.5 defines it: an ASN.1
 * structure naming the hash algorithm, followed by the digest. Written as bytes
 * because that is what it is on the wire, and because parsing ASN.1 to check a
 * constant would be adding a parser to a verifier. */
static const UINT8 sha256_prefix[19] = {
	0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86,
	0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05,
	0x00, 0x04, 0x20
};

/* Verifies `sig` over `digest`, against the modulus `n` with exponent 65537.
 *
 * Both are 256 bytes, big-endian, exactly as `openssl` writes them.
 */
BOOLEAN rsa2048_verify(const UINT8 *modulus, const UINT8 *sig,
		       const UINT8 digest[32])
{
	struct big s, m, r;
	UINT8 out[256];
	unsigned i;

	big_from_bytes(&s, sig, 256);
	big_from_bytes(&m, modulus, 256);

	/* A signature at or above the modulus is not a signature. Refused
	 * rather than reduced: the reduction would succeed and produce a
	 * plausible answer for a value the signer could not have produced. */
	if (big_cmp(&s, &m, LIMBS) >= 0)
		return FALSE;

	big_powmod_65537(&r, &s, &m, LIMBS);
	big_to_bytes(&r, out, 256);

	/* EMSA-PKCS1-v1_5: 00 01 FF...FF 00 <DigestInfo> <digest>
	 *
	 * Every byte is checked, including the padding. A verifier that only
	 * compares the last 32 bytes accepts a forgery that anybody can
	 * construct -- this is the Bleichenbacher signature forgery, and it
	 * works precisely because the padding was treated as decoration. */
	if (out[0] != 0x00 || out[1] != 0x01)
		return FALSE;

	for (i = 2; i < 256 - 19 - 32 - 1; i++)
		if (out[i] != 0xFF)
			return FALSE;

	if (out[256 - 19 - 32 - 1] != 0x00)
		return FALSE;

	for (i = 0; i < 19; i++)
		if (out[256 - 19 - 32 + i] != sha256_prefix[i])
			return FALSE;

	for (i = 0; i < 32; i++)
		if (out[256 - 32 + i] != digest[i])
			return FALSE;

	return TRUE;
}
