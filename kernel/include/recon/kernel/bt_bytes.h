/* The three things every Bluetooth file here was writing for itself.
 *
 * --- Why this exists, having argued for it twice ---
 *
 * This branch removed `bt_mouse_post` because `hid_boot_mouse` already did
 * it, and the kernel session lifted the boot decoders out of `usb_hid.c` for
 * the same reason. The argument both times was that **two copies of one thing
 * drift**, and that a copy is worse than a dependency.
 *
 * Then an audit of this branch's own files found `put_le16` written out four
 * times, `get_le16` twice, and the address comparison twice **under two
 * different names** — `same_address` in `bt_link.c` and `same_addr` in
 * `bt_pair.c`. The drift had already started; it had just reached the names
 * before it reached the behaviour.
 *
 * --- These are not equally risky, and it is worth saying which ---
 *
 * `bt_get_le16` and `bt_put_le16` are four lines with no state and no
 * protocol knowledge. Four copies of them were unlikely to diverge in any way
 * that mattered, and they are here mostly because they were in the way of the
 * one that does.
 *
 * **`bt_addr_equal` is the one that matters.** Its two copies guarded, between
 * them: which device a Connection Complete is about, which device may be
 * answered during pairing, and whether a stored link key belongs to the
 * device asking for it. The last two are the gates that decide whether this
 * machine pairs with something — tested by deleting them and watching an
 * uninvited device get let in. A future change to one copy would have left
 * the other behind, and only one of them is load-bearing for that.
 */
#ifndef RECON_KERNEL_BT_BYTES_H
#define RECON_KERNEL_BT_BYTES_H

#include <recon/kernel/types.h>

/* A Bluetooth device address: six bytes, little-endian on the wire and
 * written the other way round by people. Defined here rather than in
 * `bt_link.h` so that `bt_addr_equal` can be here too. */
#define BT_ADDR_LEN	6

static inline u16 bt_get_le16(const u8 *p)
{
	return (u16)(p[0] | ((u16)p[1] << 8));
}

static inline void bt_put_le16(u8 *p, u16 v)
{
	p[0] = (u8)(v & 0xFFu);
	p[1] = (u8)(v >> 8);
}

/* Whole address or nothing.
 *
 * Deliberately not `kmemcmp`-shaped: it returns a bool rather than an
 * ordering, because every caller wants *is this the same device* and a
 * caller that got a signed comparison back could get the sense inverted
 * without the compiler minding. */
static inline bool bt_addr_equal(const u8 *a, const u8 *b)
{
	unsigned i;

	for (i = 0; i < BT_ADDR_LEN; i++)
		if (a[i] != b[i])
			return false;

	return true;
}

#endif /* RECON_KERNEL_BT_BYTES_H */
