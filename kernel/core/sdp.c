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

	/* Declared longer than what is here. Refused rather than clamped. */
	if (out->header_len + out->data_len > len)
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

	return ok;
}
