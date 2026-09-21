/* Walking an SDP record.
 *
 * See `sdp.h` for what it is for and where the numbers were checked. This
 * file is the walk, and it is the third tree-walker on this branch after the
 * USB descriptor chain and the HID report descriptor. They fail the same ways.
 *
 * --- The size index, which is not a size ---
 *
 * Three bits, and they do not count bytes. Indices 0 to 4 mean 1, 2, 4, 8 and
 * 16; indices 5, 6 and 7 mean the length is in the next 1, 2 or 4 bytes.
 *
 * This is the same shape as the HID item length that encodes 0, 1, 2, 4 --
 * a small table where the obvious reading is right for the common cases and
 * wrong for one of them. Here the trap is index 4: a walker that treats the
 * index as a count advances by four instead of sixteen on a 128-bit UUID, and
 * 128-bit UUIDs appear in exactly the service class lists a HID record starts
 * with.
 *
 * --- And nil, which breaks the table ---
 *
 * Type 0 with size index 0 carries **no** data, where every other type with
 * index 0 carries one byte. One element in the format is special, and a walker
 * that misses it steps one byte too far every time a record contains a nil --
 * landing mid-element and reading a length out of somebody's data.
 */
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/hid_report.h>
#include <recon/kernel/sdp.h>

static u32 get_be16(const u8 *p)
{
	return ((u32)p[0] << 8) | p[1];
}

static u32 get_be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

bool sdp_element_parse(const u8 *p, u32 len, struct sdp_element *out)
{
	u8 index;

	if (!len)
		return false;

	out->type = (u8)(p[0] >> 3);
	index = (u8)(p[0] & 0x07u);

	switch (index) {
	case 0:
		/* The exception. Nil is the only type whose index 0 means no
		 * data at all. */
		out->header_len = 1;
		out->data_len = (out->type == SDP_DE_NIL) ? 0u : 1u;
		break;
	case 1:
		out->header_len = 1;
		out->data_len = 2;
		break;
	case 2:
		out->header_len = 1;
		out->data_len = 4;
		break;
	case 3:
		out->header_len = 1;
		out->data_len = 8;
		break;
	case 4:
		/* Sixteen, not four. */
		out->header_len = 1;
		out->data_len = 16;
		break;
	case 5:
		if (len < 2)
			return false;
		out->header_len = 2;
		out->data_len = p[1];
		break;
	case 6:
		if (len < 3)
			return false;
		out->header_len = 3;
		out->data_len = get_be16(p + 1);
		break;
	default:
		if (len < 5)
			return false;
		out->header_len = 5;
		out->data_len = get_be32(p + 1);
		break;
	}

	/* Declared longer than what is here. Refused rather than clamped.
	 *
	 * **Written as a subtraction because the addition wraps.** A size
	 * index of 7 takes `data_len` from four bytes of the record, so the
	 * wire can say 0xFFFFFFFF -- and `5 + 0xFFFFFFFF` is 4, which is less
	 * than almost any `len`. The check passed and handed the caller a
	 * four-gigabyte length pointing five bytes into a 700-byte buffer.
	 *
	 * `len >= out->header_len` is established first, so the subtraction
	 * cannot wrap in its turn. Found by listing every additive bounds
	 * check rather than by a test; a test would have needed to think of
	 * the value first. */
	if (out->header_len > len || out->data_len > len - out->header_len)
		return false;

	out->data = p + out->header_len;
	return true;
}

u32 sdp_element_total(const struct sdp_element *e)
{
	return e->header_len + e->data_len;
}

bool sdp_uint(const struct sdp_element *e, u32 *out)
{
	if (e->type != SDP_DE_UINT)
		return false;

	/* Big-endian, unlike everything else in this stack.
	 *
	 * L2CAP and HCI are little-endian on the wire and SDP is not, which is
	 * the same hazard `usb_storage.c` names for SCSI inside its wrappers:
	 * two byte orders a few bytes apart, and the wrong helper gives a
	 * plausible number. */
	switch (e->data_len) {
	case 1:
		*out = e->data[0];
		return true;
	case 2:
		*out = get_be16(e->data);
		return true;
	case 4:
		*out = get_be32(e->data);
		return true;
	default:
		/* Eight or sixteen bytes. Refused rather than truncated: the
		 * low half of a 64-bit value is a number, and returning it
		 * would be an answer. */
		return false;
	}
}

bool sdp_find_attribute(const u8 *list, u32 len, u16 attribute,
			struct sdp_element *out)
{
	u32 at = 0;

	/* Identifier, value, identifier, value. Both are elements, so the walk
	 * takes two at a time and a list that ends after an identifier is
	 * malformed rather than simply finished. */
	while (at < len) {
		struct sdp_element id, value;
		u32 got = 0;

		if (!sdp_element_parse(list + at, len - at, &id))
			return false;

		/* An identifier that is not an unsigned integer. The list is
		 * not what it claims to be, and guessing where the next pair
		 * starts would invent one. */
		if (!sdp_uint(&id, &got))
			return false;

		at += sdp_element_total(&id);

		/* An identifier with no value after it. Malformed rather than
		 * simply finished -- they come in pairs. */
		if (at >= len)
			return false;

		if (!sdp_element_parse(list + at, len - at, &value))
			return false;

		if (got == attribute) {
			*out = value;
			return true;
		}

		at += sdp_element_total(&value);
	}

	return false;
}

/* Unwraps the outer sequence a record is, and hands back its contents. */
static bool record_contents(const u8 *record, u32 len, const u8 **inner,
			    u32 *inner_len)
{
	struct sdp_element seq;

	if (!sdp_element_parse(record, len, &seq))
		return false;

	if (seq.type != SDP_DE_SEQ)
		return false;

	*inner = seq.data;
	*inner_len = seq.data_len;

	return true;
}

bool sdp_hid_report_descriptor(const u8 *record, u32 len, const u8 **desc,
			       u32 *desc_len)
{
	const u8 *inner;
	u32 inner_len, at;
	struct sdp_element list;

	if (!record_contents(record, len, &inner, &inner_len))
		return false;

	if (!sdp_find_attribute(inner, inner_len,
				SDP_ATTR_HID_DESCRIPTOR_LIST, &list))
		return false;

	if (list.type != SDP_DE_SEQ)
		return false;

	/* A list of sequences, each a descriptor type and the bytes. More than
	 * one is legal -- a device may offer a physical descriptor too -- so
	 * the type is checked rather than the first entry taken. */
	for (at = 0; at < list.data_len; ) {
		struct sdp_element entry, kind, bytes;
		u32 got = 0;

		if (!sdp_element_parse(list.data + at, list.data_len - at,
				       &entry))
			return false;

		at += sdp_element_total(&entry);

		if (entry.type != SDP_DE_SEQ)
			continue;

		if (!sdp_element_parse(entry.data, entry.data_len, &kind))
			continue;

		if (!sdp_uint(&kind, &got) || got != SDP_HID_DESC_TYPE_REPORT)
			continue;

		if (!sdp_element_parse(entry.data + sdp_element_total(&kind),
				       entry.data_len -
				       sdp_element_total(&kind), &bytes))
			continue;

		if (bytes.type != SDP_DE_TEXT)
			continue;

		*desc = bytes.data;
		*desc_len = bytes.data_len;
		return true;
	}

	return false;
}

bool sdp_hid_boot_device(const u8 *record, u32 len, bool *supported)
{
	const u8 *inner;
	u32 inner_len;
	struct sdp_element e;

	if (!record_contents(record, len, &inner, &inner_len))
		return false;

	if (!sdp_find_attribute(inner, inner_len, SDP_ATTR_HID_BOOT_DEVICE,
				&e))
		return false;

	if (e.type != SDP_DE_BOOL || e.data_len != 1)
		return false;

	*supported = e.data[0] != 0;
	return true;
}

/* --- the self-test --------------------------------------------------------
 *
 * The record below is the shape a HID mouse publishes, cut down to the two
 * attributes this file reads. The cases are chosen the same way as the other
 * walkers':
 *
 *   - size index 4 means sixteen bytes, not four, and 128-bit UUIDs are
 *     exactly what a service class list contains;
 *   - nil carries no data where everything else with index 0 carries one,
 *     and missing that steps one byte into the next element;
 *   - SDP is big-endian where the rest of this stack is not;
 *   - an element declaring more than is present must be refused, not clamped;
 *   - an attribute that is absent is not an error, and must not return the
 *     next one along.
 */
/* A service record read off a real device.
 *
 * **Every other fixture in this file was built by the hand that wrote the
 * parser.** The last signal to the kernel session admitted what that costs:
 * if the understanding of the format is wrong, the record and the parser
 * carry the same error and agree -- *a copy agrees with itself*. The HID
 * parser escaped that only because a real descriptor could be pulled off real
 * hardware. This is the equivalent for SDP.
 *
 * 701 bytes, cached by Windows for a paired DualShock 4 (`054c:09cc`) at
 * `48:18:8d:58:96:8f`, read out of
 * `HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices`
 * on 20 September 2026. Not synthesised, not transcribed from a
 * specification: the bytes a real controller published about itself.
 *
 * It is a harder case than the hand-built one in two ways that matter:
 *
 *   - it opens with `36` -- a **SEQ16**, whose length is in the two bytes
 *     after the descriptor. Every invented record here used `35`, so the
 *     two-byte length path had never been exercised by real data;
 *   - it carries **24 attributes**, where the invented one carries two, so
 *     `sdp_find_attribute` has to walk past twenty-two pairs rather than
 *     none.
 *
 * And what it says is the case this branch wrote down as the reason SDP
 * matters at all: **attribute 0x020E is false.** This device does not support
 * boot protocol. `bt_hid.h` records that a Bluetooth HID device is not
 * obliged to, and here is one that is not.
 */
static const u8 real_record[] = {
	0x36, 0x02, 0xBA, 0x09, 0x00, 0x00, 0x0A, 0x00,
	0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x35, 0x03,
	0x19, 0x11, 0x24, 0x09, 0x00, 0x04, 0x35, 0x0D,
	0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x11,
	0x35, 0x03, 0x19, 0x00, 0x11, 0x09, 0x00, 0x06,
	0x35, 0x09, 0x09, 0x65, 0x6E, 0x09, 0x00, 0x6A,
	0x09, 0x01, 0x00, 0x09, 0x00, 0x09, 0x35, 0x08,
	0x35, 0x06, 0x19, 0x11, 0x24, 0x09, 0x01, 0x00,
	0x09, 0x00, 0x0D, 0x35, 0x0F, 0x35, 0x0D, 0x35,
	0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x13, 0x35,
	0x03, 0x19, 0x00, 0x11, 0x09, 0x01, 0x00, 0x25,
	0x13, 0x57, 0x69, 0x72, 0x65, 0x6C, 0x65, 0x73,
	0x73, 0x20, 0x43, 0x6F, 0x6E, 0x74, 0x72, 0x6F,
	0x6C, 0x6C, 0x65, 0x72, 0x09, 0x01, 0x01, 0x25,
	0x0F, 0x47, 0x61, 0x6D, 0x65, 0x20, 0x43, 0x6F,
	0x6E, 0x74, 0x72, 0x6F, 0x6C, 0x6C, 0x65, 0x72,
	0x09, 0x01, 0x02, 0x25, 0x1E, 0x53, 0x6F, 0x6E,
	0x79, 0x20, 0x49, 0x6E, 0x74, 0x65, 0x72, 0x61,
	0x63, 0x74, 0x69, 0x76, 0x65, 0x20, 0x45, 0x6E,
	0x74, 0x65, 0x72, 0x74, 0x61, 0x69, 0x6E, 0x6D,
	0x65, 0x6E, 0x74, 0x09, 0x02, 0x00, 0x09, 0x01,
	0x00, 0x09, 0x02, 0x01, 0x09, 0x01, 0x11, 0x09,
	0x02, 0x02, 0x08, 0x08, 0x09, 0x02, 0x03, 0x08,
	0x00, 0x09, 0x02, 0x04, 0x28, 0x00, 0x09, 0x02,
	0x05, 0x28, 0x01, 0x09, 0x02, 0x06, 0x36, 0x01,
	0xC2, 0x36, 0x01, 0xBF, 0x08, 0x22, 0x26, 0x01,
	0xBA, 0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85,
	0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09,
	0x35, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08,
	0x95, 0x04, 0x81, 0x02, 0x09, 0x39, 0x15, 0x00,
	0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x0E, 0x15, 0x00,
	0x25, 0x01, 0x75, 0x01, 0x95, 0x0E, 0x81, 0x02,
	0x75, 0x06, 0x95, 0x01, 0x81, 0x01, 0x05, 0x01,
	0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF,
	0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0x06,
	0x04, 0xFF, 0x85, 0x02, 0x09, 0x24, 0x95, 0x24,
	0xB1, 0x02, 0x85, 0xA3, 0x09, 0x25, 0x95, 0x30,
	0xB1, 0x02, 0x85, 0x05, 0x09, 0x26, 0x95, 0x28,
	0xB1, 0x02, 0x85, 0x06, 0x09, 0x27, 0x95, 0x34,
	0xB1, 0x02, 0x85, 0x07, 0x09, 0x28, 0x95, 0x30,
	0xB1, 0x02, 0x85, 0x08, 0x09, 0x29, 0x95, 0x2F,
	0xB1, 0x02, 0x85, 0x09, 0x09, 0x2A, 0x95, 0x13,
	0xB1, 0x02, 0x06, 0x03, 0xFF, 0x85, 0x03, 0x09,
	0x21, 0x95, 0x26, 0xB1, 0x02, 0x85, 0x04, 0x09,
	0x22, 0x95, 0x2E, 0xB1, 0x02, 0x85, 0xF0, 0x09,
	0x47, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF1, 0x09,
	0x48, 0x95, 0x3F, 0xB1, 0x02, 0x85, 0xF2, 0x09,
	0x49, 0x95, 0x0F, 0xB1, 0x02, 0x06, 0x00, 0xFF,
	0x85, 0x11, 0x09, 0x20, 0x15, 0x00, 0x26, 0xFF,
	0x00, 0x75, 0x08, 0x95, 0x4D, 0x81, 0x02, 0x09,
	0x21, 0x91, 0x02, 0x85, 0x12, 0x09, 0x22, 0x95,
	0x8D, 0x81, 0x02, 0x09, 0x23, 0x91, 0x02, 0x85,
	0x13, 0x09, 0x24, 0x95, 0xCD, 0x81, 0x02, 0x09,
	0x25, 0x91, 0x02, 0x85, 0x14, 0x09, 0x26, 0x96,
	0x0D, 0x01, 0x81, 0x02, 0x09, 0x27, 0x91, 0x02,
	0x85, 0x15, 0x09, 0x28, 0x96, 0x4D, 0x01, 0x81,
	0x02, 0x09, 0x29, 0x91, 0x02, 0x85, 0x16, 0x09,
	0x2A, 0x96, 0x8D, 0x01, 0x81, 0x02, 0x09, 0x2B,
	0x91, 0x02, 0x85, 0x17, 0x09, 0x2C, 0x96, 0xCD,
	0x01, 0x81, 0x02, 0x09, 0x2D, 0x91, 0x02, 0x85,
	0x18, 0x09, 0x2E, 0x96, 0x0D, 0x02, 0x81, 0x02,
	0x09, 0x2F, 0x91, 0x02, 0x85, 0x19, 0x09, 0x30,
	0x96, 0x22, 0x02, 0x81, 0x02, 0x09, 0x31, 0x91,
	0x02, 0x06, 0x80, 0xFF, 0x85, 0x82, 0x09, 0x22,
	0x95, 0x3F, 0xB1, 0x02, 0x85, 0x83, 0x09, 0x23,
	0xB1, 0x02, 0x85, 0x84, 0x09, 0x24, 0xB1, 0x02,
	0x85, 0x90, 0x09, 0x30, 0xB1, 0x02, 0x85, 0x91,
	0x09, 0x31, 0xB1, 0x02, 0x85, 0x92, 0x09, 0x32,
	0xB1, 0x02, 0x85, 0x93, 0x09, 0x33, 0xB1, 0x02,
	0x85, 0x94, 0x09, 0x34, 0xB1, 0x02, 0x85, 0xA0,
	0x09, 0x40, 0xB1, 0x02, 0x85, 0xA4, 0x09, 0x44,
	0xB1, 0x02, 0x85, 0xA7, 0x09, 0x45, 0xB1, 0x02,
	0x85, 0xA8, 0x09, 0x45, 0xB1, 0x02, 0x85, 0xA9,
	0x09, 0x45, 0xB1, 0x02, 0x85, 0xAA, 0x09, 0x45,
	0xB1, 0x02, 0x85, 0xAB, 0x09, 0x45, 0xB1, 0x02,
	0x85, 0xAC, 0x09, 0x45, 0xB1, 0x02, 0x85, 0xAD,
	0x09, 0x45, 0xB1, 0x02, 0x85, 0xB3, 0x09, 0x45,
	0xB1, 0x02, 0x85, 0xB4, 0x09, 0x46, 0xB1, 0x02,
	0x85, 0xB5, 0x09, 0x47, 0xB1, 0x02, 0x85, 0xD0,
	0x09, 0x40, 0xB1, 0x02, 0x85, 0xD4, 0x09, 0x44,
	0xB1, 0x02, 0xC0, 0x09, 0x02, 0x07, 0x35, 0x08,
	0x35, 0x06, 0x09, 0x04, 0x09, 0x09, 0x01, 0x00,
	0x09, 0x02, 0x08, 0x28, 0x00, 0x09, 0x02, 0x09,
	0x28, 0x01, 0x09, 0x02, 0x0A, 0x28, 0x01, 0x09,
	0x02, 0x0B, 0x09, 0x01, 0x00, 0x09, 0x02, 0x0C,
	0x09, 0x1F, 0x40, 0x09, 0x02, 0x0D, 0x28, 0x00,
	0x09, 0x02, 0x0E, 0x28, 0x00
};

bool sdp_self_test(void)
{
	struct sdp_element e;
	bool ok = true;
	u32 v;

	/* --- the descriptor byte comes apart the way the header says ---- */
	{
		static const u8 u16e[3] = { 0x09, 0x12, 0x34 };

		if (!sdp_element_parse(u16e, 3, &e)) {
			kputs("  sdp: a two-byte unsigned element was "
			      "refused\n");
			ok = false;
		} else if (e.type != SDP_DE_UINT || e.data_len != 2 ||
			   e.header_len != 1) {
			kprintf("  sdp: 0x09 read as type %u, %u bytes of "
				"data, %u of header -- expected uint, 2, 1\n",
				e.type, e.data_len, e.header_len);
			ok = false;
		} else if (!sdp_uint(&e, &v) || v != 0x1234) {
			kprintf("  sdp: it read as %u, expected 0x1234 -- SDP "
				"is big-endian where L2CAP and HCI are not\n",
				v);
			ok = false;
		}
	}

	/* --- index 4 is sixteen bytes --------------------------------- */
	{
		u8 uuid128[17];

		kmemset(uuid128, 0xAB, sizeof(uuid128));
		uuid128[0] = 0x1C;		/* UUID, size index 4 */

		if (!sdp_element_parse(uuid128, 17, &e)) {
			kputs("  sdp: a 128-bit UUID was refused\n");
			ok = false;
		} else if (e.data_len != 16) {
			kprintf("  sdp: size index 4 read as %u bytes, "
				"expected 16 -- the index is not a count, and "
				"a service class list is where 128-bit UUIDs "
				"live\n", e.data_len);
			ok = false;
		}

		/* And one byte short must be refused rather than clamped. */
		if (sdp_element_parse(uuid128, 16, &e)) {
			kputs("  sdp: a 128-bit UUID with only 15 bytes of "
			      "value was accepted\n");
			ok = false;
		}
	}

	/* --- nil is the exception -------------------------------------- */
	{
		static const u8 nil_then_uint[4] = { 0x00, 0x08, 0x2A, 0x00 };

		if (!sdp_element_parse(nil_then_uint, 4, &e)) {
			kputs("  sdp: a nil element was refused\n");
			ok = false;
		} else if (e.data_len != 0 || sdp_element_total(&e) != 1) {
			kprintf("  sdp: nil read as %u bytes of data, "
				"expected 0 -- it is the one type whose size "
				"index 0 means nothing rather than one\n",
				e.data_len);
			ok = false;
		} else {
			/* And the element after it must land correctly. This
			 * is what a wrong nil actually costs. */
			struct sdp_element next;
			u32 after = sdp_element_total(&e);

			if (!sdp_element_parse(nil_then_uint + after,
					       4 - after, &next) ||
			    !sdp_uint(&next, &v) || v != 0x2A) {
				kprintf("  sdp: the element after a nil read "
					"as %u, expected 42 -- the walk "
					"stepped into it\n", v);
				ok = false;
			}
		}
	}

	/* --- an eight-byte integer is refused, not truncated ----------- */
	{
		static const u8 u64e[9] = { 0x0B, 1, 2, 3, 4, 5, 6, 7, 8 };

		if (!sdp_element_parse(u64e, 9, &e) || e.data_len != 8) {
			kputs("  sdp: a 64-bit unsigned element did not "
			      "parse\n");
			ok = false;
		} else if (sdp_uint(&e, &v)) {
			kprintf("  sdp: a 64-bit value came back as %u; its "
				"low half is a number and returning it would "
				"be an answer\n", v);
			ok = false;
		}
	}

	/* --- a length chosen so the bounds check overflows ---------------
	 *
	 * Size index 7 takes the length from four bytes of the record, so the
	 * wire can name 0xFFFFFFFF. Added to a five-byte header that is 4,
	 * which passes almost any comparison -- the guard becomes a
	 * permission, and the caller is handed four gigabytes starting five
	 * bytes into the buffer.
	 *
	 * Not a value a test would think of unprompted. It came from listing
	 * every additive bounds check on this branch and asking which of them
	 * could wrap.
	 */
	{
		static const u8 wrapping[9] = {
			0x3F,				/* SEQ, 4-byte length */
			0xFF, 0xFF, 0xFF, 0xFF,		/* 4294967295         */
			0x00, 0x00, 0x00, 0x00
		};

		if (sdp_element_parse(wrapping, sizeof(wrapping), &e)) {
			kprintf("  sdp: an element declaring %u bytes inside "
				"nine was accepted -- the header plus that "
				"length wraps to 4\n", e.data_len);
			ok = false;
		}

		/* And one just below the wrap, which is an ordinary refusal. */
		{
			static const u8 big[9] = {
				0x3F, 0x00, 0x01, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00
			};

			if (sdp_element_parse(big, sizeof(big), &e)) {
				kputs("  sdp: an element declaring 65536 "
				      "bytes inside nine was accepted\n");
				ok = false;
			}
		}

		/* A length that exactly fits must still work, or the fix has
		 * turned the guard into a refusal of everything. */
		{
			static const u8 exact[6] = {
				0x35, 0x04, 0x08, 0x2A, 0x08, 0x2B
			};

			if (!sdp_element_parse(exact, sizeof(exact), &e) ||
			    e.data_len != 4) {
				kputs("  sdp: an element that exactly fills "
				      "its buffer was refused\n");
				ok = false;
			}
		}
	}

	/* --- a length that runs past the end --------------------------- */
	{
		static const u8 lying[3] = { 0x35, 0x40, 0x00 };

		if (sdp_element_parse(lying, 3, &e)) {
			kputs("  sdp: a sequence declaring 64 bytes inside a "
			      "3-byte buffer was accepted\n");
			ok = false;
		}
	}

	/* --- a whole record, and the two things read out of it ---------
	 *
	 * Built here rather than captured, so every byte is accounted for:
	 *
	 *   35 14                     sequence, 20 bytes of content
	 *     09 02 06                uint16 0x0206  HIDDescriptorList      3
	 *     35 0A                     sequence, 10 bytes                  2
	 *       35 08                     sequence, 8 bytes                 2
	 *         08 22                     uint8 0x22 (report descriptor)  2
	 *         25 04 05 01 09 02         text, 4 bytes                   6
	 *     09 02 0E                uint16 0x020E  HIDBootDevice          3
	 *     28 01                   boolean true                          2
	 *                                                           total  20
	 *
	 * The column is there because the first version of this said 23 and
	 * carried 20, and the parser refused the record -- correctly. The
	 * check below comparing the declared length against `sizeof` exists
	 * for the same reason, though the refusal beat it to the report.
	 */
	{
		static const u8 record[] = {
			0x35, 0x14,
			0x09, 0x02, 0x06,
			0x35, 0x0A,
			0x35, 0x08,
			0x08, 0x22,
			0x25, 0x04, 0x05, 0x01, 0x09, 0x02,
			0x09, 0x02, 0x0E,
			0x28, 0x01
		};
		const u8 *desc = 0;
		u32 desc_len = 0;
		bool boot = false;

		if (!sdp_element_parse(record, sizeof(record), &e)) {
			kputs("  sdp: the record's outer sequence did not "
			      "parse\n");
			ok = false;
		} else if (sdp_element_total(&e) != sizeof(record)) {
			kprintf("  sdp: the record says %u bytes and is %u; "
				"the test data is wrong before the parser "
				"is\n", sdp_element_total(&e),
				(unsigned)sizeof(record));
			ok = false;
		}

		if (!sdp_hid_report_descriptor(record, sizeof(record), &desc,
					       &desc_len)) {
			kputs("  sdp: no report descriptor found in a record "
			      "that carries one\n");
			ok = false;
		} else if (desc_len != 4 || desc[0] != 0x05 ||
			   desc[1] != 0x01 || desc[3] != 0x02) {
			kprintf("  sdp: the descriptor came back %u bytes "
				"starting %02x %02x, expected 4 starting 05 "
				"01\n", desc_len, desc[0], desc[1]);
			ok = false;
		}

		if (!sdp_hid_boot_device(record, sizeof(record), &boot)) {
			kputs("  sdp: the boot-device attribute was not "
			      "found -- it is what says whether this device "
			      "can be driven without a descriptor parser\n");
			ok = false;
		} else if (!boot) {
			kputs("  sdp: the boot-device attribute read as "
			      "false and the record says true\n");
			ok = false;
		}

		/* An attribute that is not there is absent, not the next one
		 * along. */
		{
			const u8 *inner;
			u32 inner_len;
			struct sdp_element found;

			if (record_contents(record, sizeof(record), &inner,
					    &inner_len) &&
			    sdp_find_attribute(inner, inner_len,
					       SDP_ATTR_HID_PARSER_VERSION,
					       &found)) {
				kputs("  sdp: an absent attribute was found, "
				      "which means the walk returned a "
				      "neighbour\n");
				ok = false;
			}
		}
	}

	/* --- the real record, and the descriptor inside it ---------------
	 *
	 * One blob, both parsers. SDP extracts the descriptor and
	 * `hid_report.c` walks it, so a fault in either shows up here against
	 * bytes neither of them was written from.
	 */
	{
		const u8 *desc = 0;
		u32 desc_len = 0;
		bool boot = true;

		/* The outer sequence must account for exactly the blob. A
		 * SEQ16 whose length disagreed would be caught here before
		 * anything else is believed. */
		if (!sdp_element_parse(real_record, sizeof(real_record), &e)) {
			kputs("  sdp: a real service record was refused\n");
			ok = false;
		} else if (e.type != SDP_DE_SEQ || e.header_len != 3 ||
			   sdp_element_total(&e) != sizeof(real_record)) {
			kprintf("  sdp: the real record's outer element is "
				"type %u with a %u-byte header, totalling %u "
				"of %u bytes -- expected a SEQ16 accounting "
				"for all of it\n", e.type, e.header_len,
				sdp_element_total(&e),
				(unsigned)sizeof(real_record));
			ok = false;
		}

		/* **Boot protocol: declared false by a real device.** This is
		 * the case `bt_hid.h` describes and nothing had ever proved
		 * existed. */
		if (!sdp_hid_boot_device(real_record, sizeof(real_record),
					 &boot)) {
			kputs("  sdp: the real record's boot-device "
			      "attribute was not found\n");
			ok = false;
		} else if (boot) {
			kputs("  sdp: the real record says it supports boot "
			      "protocol, and the bytes say 0x020E is false\n");
			ok = false;
		}

		if (!sdp_hid_report_descriptor(real_record,
					       sizeof(real_record), &desc,
					       &desc_len)) {
			kputs("  sdp: no report descriptor found in a real "
			      "record that carries one\n");
			ok = false;
		} else if (desc_len != 442) {
			kprintf("  sdp: the real descriptor came back %u "
				"bytes, expected 442\n", desc_len);
			ok = false;
		} else {
			/* --- and hand it to the other parser ---
			 *
			 * A game controller, not a mouse: 215 items and
			 * **forty-four** report ids. What is checked is that
			 * the refusals fire, because this is the device they
			 * were written for -- `HID_MAX_REPORT_IDS` is eight,
			 * and a layout built across more than one report's
			 * fields would place them at offsets no report uses.
			 */
			static struct hid_report_info info;
			struct hid_mouse_layout m;

			if (!hid_report_parse(desc, desc_len, &info)) {
				kputs("  sdp: the real descriptor was "
				      "refused by the report parser\n");
				ok = false;
			} else {
				if (!info.uses_report_id) {
					kputs("  sdp: a descriptor with "
					      "forty-four report ids was "
					      "read as using none\n");
					ok = false;
				}

				if (!info.report_ids_truncated) {
					kprintf("  sdp: %u report ids were "
						"recorded out of forty-four "
						"and the truncation was not "
						"flagged -- a silent partial "
						"list is worse than a short "
						"one\n",
						info.report_id_count);
					ok = false;
				}

				/* **Which refusal fires, not merely that one
				 * does.**
				 *
				 * The first version asserted only that no
				 * mouse layout came out, and that held with
				 * the multi-report-id guard deleted -- so it
				 * proved nothing about the guard it looked
				 * like it was about. The controller is
				 * turned away one step earlier, by the
				 * usages check, and saying so is the
				 * difference between a test and a
				 * coincidence.
				 *
				 * The guard itself is covered in
				 * `hid_report.c`, on a descriptor built to
				 * reach it. */
				if (!info.fields_usable) {
					if (!info.fields_unusable) {
						kputs("  sdp: the real "
						      "controller's map was "
						      "withheld with no "
						      "reason attached\n");
						ok = false;
					}
				} else {
					kputs("  sdp: the real controller's "
					      "field map was usable; it has "
					      "more fields than usages and "
					      "the map cannot be trusted\n");
					ok = false;
				}

				/* **It must refuse to be a mouse**, whichever
				 * guard gets there first. */
				if (hid_report_mouse_layout(&info, &m)) {
					kputs("  sdp: a game controller with "
					      "forty-four reports produced a "
					      "mouse layout; its fields are "
					      "placed across every report at "
					      "once and match no single "
					      "one\n");
					ok = false;
				}
			}
		}
	}

	/* --- a record whose boot-device flag is false ------------------ */
	{
		static const u8 no_boot[] = {
			0x35, 0x05,
			0x09, 0x02, 0x0E,
			0x28, 0x00
		};
		bool boot = true;

		if (!sdp_hid_boot_device(no_boot, sizeof(no_boot), &boot)) {
			kputs("  sdp: a false boot-device attribute was not "
			      "found\n");
			ok = false;
		} else if (boot) {
			kputs("  sdp: a device declaring no boot mode read "
			      "as supporting it, which is how a driver ends "
			      "up asking for a protocol the device will "
			      "refuse\n");
			ok = false;
		}
	}

	/* --- a four-byte integer, which is where the byte order lives ----
	 *
	 * `get_be32` had **no test at all**, and it was not obvious: the one
	 * place a test reached it was the wrap case, which reads
	 * `FF FF FF FF`. That value is the same read either way round, so it
	 * exercises the arithmetic and says nothing whatever about the byte
	 * order -- and SDP is the one big-endian protocol in this stack,
	 * sitting a few bytes from two little-endian ones.
	 *
	 * Found by `scripts/mutate-bluetooth.py`: turning `case 4:`'s
	 * `return true` into `return false` in `sdp_uint` changed no result
	 * anywhere, which is what an unreachable success path looks like.
	 *
	 * 01 02 03 04 is deliberately asymmetric. Read the wrong way round it
	 * is 0x04030201, which is a plausible number rather than an error --
	 * the same failure shape as the BD_ADDR byte order in `bt_link.h`.
	 */
	{
		static const u8 u32_element[] = {
			0x0A,			/* UINT, size index 2: 4 bytes */
			0x01, 0x02, 0x03, 0x04
		};
		struct sdp_element e;
		u32 v = 0;

		if (!sdp_element_parse(u32_element, sizeof(u32_element), &e)) {
			kputs("  sdp: a four-byte UINT element did not "
			      "parse\n");
			ok = false;
		} else if (e.type != SDP_DE_UINT || e.data_len != 4) {
			kprintf("  sdp: a four-byte UINT read as type %u with "
				"%u bytes, expected type %u with 4\n", e.type,
				e.data_len, (unsigned)SDP_DE_UINT);
			ok = false;
		} else if (!sdp_uint(&e, &v)) {
			kputs("  sdp: a four-byte UINT was refused by "
			      "sdp_uint, so the 32-bit path is unreachable\n");
			ok = false;
		} else if (v != 0x01020304u) {
			kprintf("  sdp: 01 02 03 04 read as %08x, expected "
				"01020304 -- SDP is big-endian and everything "
				"beneath it is not\n", v);
			ok = false;
		}
	}

	/* --- the smallest buffer each header length fits in ---------------
	 *
	 * Three size indices put the length in the bytes after the
	 * descriptor: one byte, two, or four. Each has a guard refusing a
	 * buffer too short to hold that length, and each guard was tested
	 * only with buffers well clear of the boundary -- so `len < 2` could
	 * have been `len <= 2` in all three places and nothing would have
	 * said so.
	 *
	 * What that costs is not a crash. It is an element at the very end of
	 * a record being refused, and the walk above it then reporting the
	 * record as malformed. A 701-byte record whose last attribute happens
	 * to end flush would be thrown away whole.
	 *
	 * Each of these is an empty sequence, which is the shortest thing the
	 * encoding can express at that header size, and `header_len > len` in
	 * the shared bounds check is on the same boundary.
	 */
	{
		static const u8 seq8[]  = { 0x35, 0x00 };
		static const u8 seq16[] = { 0x36, 0x00, 0x00 };
		static const u8 seq32[] = { 0x37, 0x00, 0x00, 0x00, 0x00 };
		struct sdp_element e;
		unsigned i;

		const u8 *bufs[3];
		u32 lens[3];
		u32 heads[3];

		bufs[0] = seq8;  lens[0] = sizeof(seq8);  heads[0] = 2;
		bufs[1] = seq16; lens[1] = sizeof(seq16); heads[1] = 3;
		bufs[2] = seq32; lens[2] = sizeof(seq32); heads[2] = 5;

		for (i = 0; i < 3; i++) {
			if (!sdp_element_parse(bufs[i], lens[i], &e)) {
				kprintf("  sdp: an empty sequence in exactly "
					"its %u header bytes was refused\n",
					heads[i]);
				ok = false;
				continue;
			}

			if (e.header_len != heads[i] || e.data_len != 0) {
				kprintf("  sdp: an empty sequence read as %u "
					"header bytes and %u of data, "
					"expected %u and 0\n", e.header_len,
					e.data_len, heads[i]);
				ok = false;
			}

			/* And one byte short of it must still be refused, or
			 * the guard has moved rather than gone. */
			if (sdp_element_parse(bufs[i], lens[i] - 1, &e)) {
				kprintf("  sdp: an element needing %u header "
					"bytes parsed out of %u\n", heads[i],
					lens[i] - 1);
				ok = false;
			}
		}
	}

	return ok;
}
