/* SDP: reading what a device says it can do.
 *
 * Service Discovery is how a Bluetooth device's capabilities are found, and on
 * the path to a mouse it answers two questions nothing else can:
 *
 *   - **where is the report descriptor?** Attribute 0x0206 carries it, which
 *     is what makes `hid_report.c` usable over Bluetooth at all. Over USB the
 *     descriptor is fetched with a control transfer; here it is an SDP record.
 *   - **does this device support boot mode?** Attribute 0x020E. `bt_hid.h`
 *     says a Bluetooth HID device is not obliged to offer it, and this is the
 *     attribute that says whether a given one does. Without it a driver finds
 *     out by asking and being refused.
 *
 * --- Everything here is one shape ---
 *
 * An SDP record is a tree of **data elements**, and a data element is one
 * descriptor byte followed by its value. The descriptor byte is two fields:
 *
 *     bits 7..3   what it is -- nil, integer, UUID, string, sequence
 *     bits 2..0   how big, as an *index* rather than a count
 *
 * The size index is the part worth reading twice. Indices 0 to 4 mean 1, 2, 4,
 * 8 and 16 bytes; indices 5, 6 and 7 mean the length is in the next 1, 2 or 4
 * bytes. So a byte of 0x35 is a sequence whose length follows in one byte, and
 * 0x19 is a two-byte UUID.
 *
 * **Except for nil**, whose size index is 0 and whose data is 0 bytes rather
 * than 1. One element in the whole format breaks the table, and a walker that
 * misses it steps one byte too far every time a nil appears.
 *
 * --- Where these numbers come from ---
 *
 * Written from the encoding and then checked against BlueZ 5.72's `sdp.h`,
 * read out of `libbluetooth-dev` without installing it. `SDP_UINT16 0x09`,
 * `SDP_UUID16 0x19`, `SDP_SEQ8 0x35`, `SDP_BOOL 0x28`, `SDP_TEXT_STR8 0x25`
 * and the rest all decompose exactly as the two fields above predict, and the
 * HID attribute identifiers below are theirs verbatim.
 *
 * That matters here more than usual: `bt_hid.h` records that its own HIDP
 * constants could *not* be verified, so this file says plainly that these
 * could.
 */
#ifndef RECON_KERNEL_SDP_H
#define RECON_KERNEL_SDP_H

#include <recon/kernel/types.h>

/* The type, once the descriptor byte is taken apart. */
#define SDP_DE_NIL	0
#define SDP_DE_UINT	1
#define SDP_DE_INT	2
#define SDP_DE_UUID	3
#define SDP_DE_TEXT	4
#define SDP_DE_BOOL	5
#define SDP_DE_SEQ	6
#define SDP_DE_ALT	7
#define SDP_DE_URL	8

/* Attribute identifiers, from BlueZ's `sdp.h`. */
#define SDP_ATTR_SVCLASS_ID_LIST	0x0001
#define SDP_ATTR_PROTO_DESC_LIST	0x0004

#define SDP_ATTR_HID_PARSER_VERSION	0x0201
#define SDP_ATTR_HID_DESCRIPTOR_LIST	0x0206
#define SDP_ATTR_HID_BOOT_DEVICE	0x020E

/* And the identifiers that say a record is a HID device's. */
#define SDP_UUID_HID_SVCLASS		0x1124
#define SDP_UUID_L2CAP			0x0100
#define SDP_UUID_HIDP			0x0011

/* The descriptor type inside a HIDDescriptorList entry that means "this is
 * the report descriptor". The same 0x22 USB uses, because it is the same
 * number from the same specification. */
#define SDP_HID_DESC_TYPE_REPORT	0x22

struct sdp_element {
	u8  type;
	u32 header_len;		/* the descriptor byte, plus any length bytes */
	u32 data_len;
	const u8 *data;		/* points into the caller's buffer */
};

/* Takes one element apart. False when the buffer is too short for the header
 * or for the data the header declares -- never a partial element, because a
 * partial element is a value assembled from bytes that belong to something
 * else. */
bool sdp_element_parse(const u8 *p, u32 len, struct sdp_element *out);

/* The whole element's length: header plus data. Convenient for walking. */
u32 sdp_element_total(const struct sdp_element *e);

/* Reads an unsigned integer element of 1, 2 or 4 bytes. False for anything
 * else, including an 8- or 16-byte one, which does not fit the result and is
 * refused rather than truncated. */
bool sdp_uint(const struct sdp_element *e, u32 *out);

/* Finds an attribute in an attribute list.
 *
 * `list` is the *contents* of the enclosing sequence -- the caller unwraps it
 * -- and holds alternating attribute identifiers and values. Returns the
 * value element. */
bool sdp_find_attribute(const u8 *list, u32 len, u16 attribute,
			struct sdp_element *out);

/* The two HID answers this file exists for, each taking a whole record --
 * the outer sequence included. */
bool sdp_hid_report_descriptor(const u8 *record, u32 len, const u8 **desc,
			       u32 *desc_len);
bool sdp_hid_boot_device(const u8 *record, u32 len, bool *supported);

bool sdp_self_test(void);

#endif /* RECON_KERNEL_SDP_H */
