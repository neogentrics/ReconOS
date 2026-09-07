/* SHA-256 and RSA-2048 verification, shared by both loaders.
 *
 * Its own header rather than a section of boot_internal.h, because there are
 * now two callers and only one of them is a UEFI application. The BIOS loader
 * in ../bios compiles the same sha256.c and rsa.c for 16-bit real mode and
 * includes this; it has no use for the rest of boot_internal.h, and pulling
 * that in collided with its own `print`.
 *
 * **The point of sharing the sources is that there is one signature check.**
 * Two implementations would be two chances to be wrong with one set of tests
 * between them, and the untested one is the one that will be wrong. A BIOS path
 * that verifies differently from the UEFI path is a BIOS path an attacker
 * prefers.
 *
 * Requires the integer types from efi.h, which the BIOS loader includes for
 * those alone.
 */
#ifndef RECONBOOT_CRYPTO_H
#define RECONBOOT_CRYPTO_H

/* A SHA-256 of one buffer. See sha256.c for why a hash is written out here
 * rather than borrowed. */
void sha256(const void *data, UINTN len, UINT8 out[32]);
BOOLEAN sha256_self_test(void);

/* Incremental, for a caller that cannot hold the whole message at once.
 *
 * The BIOS loader is that caller: real mode addresses one megabyte, the kernel
 * is loaded above it, and the only moment each byte is reachable is while its
 * cluster is passing through a low buffer. `sha256()` is written in terms of
 * these three, so there is one implementation and the tested path is the one
 * both loaders use. */
struct sha256_ctx {
	UINT32 h[8];
	UINT8  buf[64];
	UINTN  buffered;
	UINT64 bits;
};

void sha256_init(struct sha256_ctx *c);
void sha256_update(struct sha256_ctx *c, const void *data, UINTN len);
void sha256_final(struct sha256_ctx *c, UINT8 out[32]);

/* RSA-2048, PKCS#1 v1.5, verification only -- which is the half that touches
 * no secret, and is why writing it out is defensible here. See rsa.c. */
BOOLEAN rsa2048_verify(const UINT8 *modulus, const UINT8 *sig,
		       const UINT8 digest[32]);

#endif /* RECONBOOT_CRYPTO_H */
