/* CRC-32, done bit by bit.
 *
 * See crc32.h for which CRC-32 this is and why the processor's own instruction
 * is not used for it.
 *
 * --- Bitwise, not a table, and that is a decision rather than an omission ---
 *
 * The usual implementation precomputes 256 entries and consumes a byte at a
 * time, which is about eight times faster and costs a kilobyte. This one shifts
 * eight times per byte and costs nothing.
 *
 * The reason is what actually uses it. A GPT header is ninety-two bytes and its
 * entry array is sixteen kilobytes, checksummed twice each at boot -- call it a
 * hundred thousand iterations, once, on a machine doing nothing else. A
 * kilobyte of table to save that is a kilobyte spent on a boot that is already
 * waiting for a disk.
 *
 * When something checksums every block of a filesystem, that calculation
 * changes, and the table goes *behind this same function* with the same vectors
 * proving it agrees. Written down so that the person who needs it fast does not
 * have to work out whether replacing this is allowed.
 */
#include <recon/kernel/crc32.h>

#include <recon/kernel/console.h>

/* The polynomial, reflected. The bits of 0x04C11DB7 in the other order, which
 * is what lets the loop shift right instead of left and is the reason almost
 * every implementation of this in the world uses this constant rather than the
 * one the specification names. */
#define POLY 0xEDB88320u

u32 crc32_update(u32 running, const void *data, size_t len)
{
	const u8 *p = data;

	for (size_t i = 0; i < len; i++) {
		running ^= p[i];

		for (unsigned bit = 0; bit < 8; bit++) {
			/* Branchless would be running = (running >> 1) ^
			 * (POLY & -(running & 1)). Written as a branch because
			 * it is the form that can be read against the
			 * specification, and nothing here is hot. */
			if (running & 1)
				running = (running >> 1) ^ POLY;
			else
				running >>= 1;
		}
	}

	return running;
}

u32 crc32_final(u32 running)
{
	return running ^ 0xFFFFFFFFu;
}

u32 crc32(const void *data, size_t len)
{
	return crc32_final(crc32_update(CRC32_INIT, data, len));
}

/* --- The self-test ---------------------------------------------------------
 *
 * Known answers, and they are known from somewhere else. Every value below was
 * produced by a different implementation before being written here, for the
 * same reason the partition fixtures are built by sgdisk: a checksum verified
 * against itself is a checksum verified against nothing, and the failure mode
 * of a wrong CRC is that it agrees with itself perfectly and with the rest of
 * the world not at all.
 *
 * 0xCBF43926 over "123456789" is the check value the specification itself
 * publishes, which makes it the one vector here that is not merely a second
 * opinion but the definition.
 */
bool crc32_self_test(void)
{
	static const char nine[] = "123456789";
	static const char fox[] =
		"The quick brown fox jumps over the lazy dog";
	static const u8 zeros[32];

	u8 every_byte[256];
	bool ok = true;
	u32 got;

	got = crc32("", 0);
	if (got != 0x00000000u) {
		kprintf("  crc32: empty input gave %x, not 0\n", got);
		ok = false;
	}

	got = crc32("a", 1);
	if (got != 0xE8B7BE43u) {
		kprintf("  crc32: \"a\" gave %x, not e8b7be43\n", got);
		ok = false;
	}

	/* The published check value. If this one is wrong, everything is. */
	got = crc32(nine, sizeof(nine) - 1);
	if (got != 0xCBF43926u) {
		kprintf("  crc32: the specification's own check value came out "
			"as %x, not cbf43926\n", got);
		ok = false;
	}

	got = crc32(fox, sizeof(fox) - 1);
	if (got != 0x414FA339u) {
		kprintf("  crc32: the fox gave %x, not 414fa339\n", got);
		ok = false;
	}

	/* Thirty-two zero bytes. Included because a broken implementation that
	 * returns its input, or that never enters the polynomial branch, gives
	 * zero here and passes an eyeball. The right answer is not zero. */
	got = crc32(zeros, sizeof(zeros));
	if (got != 0x190A55ADu) {
		kprintf("  crc32: thirty-two zeroes gave %x, not 190a55ad\n", got);
		ok = false;
	}

	for (unsigned i = 0; i < 256; i++)
		every_byte[i] = (u8)i;

	got = crc32(every_byte, sizeof(every_byte));
	if (got != 0x29058C73u) {
		kprintf("  crc32: every byte value gave %x, not 29058c73\n", got);
		ok = false;
	}

	/* And that feeding it in pieces gives the same answer as feeding it
	 * whole -- which is the property the GPT header check depends on, since
	 * that checksum is computed over a structure with a hole in the middle
	 * of it. A split that changes the answer would be found here rather
	 * than as an unreadable disk. */
	{
		u32 whole = crc32(every_byte, sizeof(every_byte));
		u32 running = CRC32_INIT;

		running = crc32_update(running, every_byte, 1);
		running = crc32_update(running, every_byte + 1, 100);
		running = crc32_update(running, every_byte + 101, 0);
		running = crc32_update(running, every_byte + 101, 155);

		if (crc32_final(running) != whole) {
			kprintf("  crc32: in four pieces gave %x, whole gave "
				"%x\n", crc32_final(running), whole);
			ok = false;
		}
	}

	return ok;
}
