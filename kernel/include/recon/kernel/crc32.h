/* CRC-32, the one everybody means when they say CRC-32.
 *
 * A GPT header and its array of partition entries are each protected by one,
 * and a filesystem that wants to notice a torn write needs one too. It is the
 * kernel's first checksum and it will not be its last.
 *
 * --- Which CRC-32, because there are two and they share a name ---
 *
 * This is the reflected polynomial 0xEDB88320, initialised to all ones and
 * inverted at the end. It is what GPT uses, what PNG uses, what zip uses, and
 * what "CRC-32" means without qualification.
 *
 * It is NOT CRC-32C, the Castagnoli polynomial, and the difference is a trap
 * this kernel is already standing next to. Checkpoint 3 detects a processor's
 * CRC32 capability and reports it: `sse4.2` on x86_64, `crc32` on aarch64. The
 * obvious next thought is to use the hardware instruction and skip the
 * arithmetic below.
 *
 * On x86_64 that is wrong. The SSE4.2 CRC32 instruction computes *Castagnoli*
 * and only Castagnoli. Given "123456789" it produces 0xE3069283, where this
 * function produces 0xCBF43926. Both are correct CRC-32s of the same bytes
 * under different polynomials, and a kernel that used the instruction here
 * would reject every GPT on earth while reporting a checksum mismatch --
 * which reads like a corrupt disk rather than like a wrong polynomial.
 *
 * On aarch64 the hardware happens to offer both: `crc32w` is this one and
 * `crc32cw` is the other. So the instruction is usable there and not here,
 * which is exactly the kind of asymmetry that produces a kernel working on one
 * architecture and quietly failing on the other.
 *
 * So: software, both architectures, one answer. If something ever needs this
 * fast enough to matter, the place to use the hardware is behind this same
 * function, on the architecture that has the right instruction, checked against
 * the same vectors.
 */
#ifndef RECON_KERNEL_CRC32_H
#define RECON_KERNEL_CRC32_H

#include <recon/kernel/types.h>

/* The whole of a buffer. */
u32 crc32(const void *data, size_t len);

/* Piece by piece, for a checksum over things that are not contiguous -- which
 * a GPT header is, because the field holding its own checksum has to be read as
 * zero while the checksum over it is computed.
 *
 * Start with CRC32_INIT, feed each piece, finish with crc32_final. Feeding the
 * whole buffer in one call and calling crc32() must give the same answer, and
 * the self-test checks that they do. */
#define CRC32_INIT 0xFFFFFFFFu

u32 crc32_update(u32 running, const void *data, size_t len);
u32 crc32_final(u32 running);

bool crc32_self_test(void);

#endif /* RECON_KERNEL_CRC32_H */
