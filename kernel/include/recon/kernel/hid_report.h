/* HID report descriptors: what a device says about its own reports.
 *
 * **This is transport-neutral on purpose.** A report descriptor is the same
 * bytes whether it arrived over USB or over Bluetooth, so this file names
 * neither -- it is under `core/` and it includes nothing about either bus.
 *
 * --- Why it exists ---
 *
 * Two drivers want it and neither has it. `usb_hid.c` says so directly:
 *
 *   "A device that does not offer it describes itself in a report descriptor
 *    and this driver cannot read one -- saying so beats guessing at a layout."
 *
 * and refuses every non-boot-protocol device on that basis. The Bluetooth side
 * has the same gap and a sharper version of it: **a Bluetooth HID device is
 * not required to offer boot mode at all**, so "fall back to the boot
 * protocol" is a fallback that may not exist.
 *
 * --- What this first pass answers, and what it does not ---
 *
 * It answers the question `bt_hid.c` currently takes as an argument because it
 * cannot answer it: **does this device put a report ID byte in front of its
 * reports?** That is one Global item's presence, it decides where every field
 * in every report begins, and getting it wrong shifts a mouse's buttons into
 * its X and still moves the pointer.
 *
 * It also answers how long an input report is, by accumulating report size
 * times report count over the Input items -- but **only for a device with no
 * report IDs**, where there is one input report and the question has one
 * answer. With report IDs the length is per-ID, and a single number would be a
 * wrong answer rather than a missing one, so none is given.
 *
 * It does **not** yet map fields to usages -- which bits are the buttons,
 * which byte is X. That needs the local-item state to be tracked across Main
 * items and is the next pass. Everything here is what a walker has to get
 * right before any of that is worth attempting.
 */
#ifndef RECON_KERNEL_HID_REPORT_H
#define RECON_KERNEL_HID_REPORT_H

#include <recon/kernel/types.h>

/* Item types, in bits 3 and 2 of the prefix byte. */
#define HID_ITEM_MAIN		0
#define HID_ITEM_GLOBAL		1
#define HID_ITEM_LOCAL		2
#define HID_ITEM_RESERVED	3

/* Main item tags, in the top four bits. */
#define HID_MAIN_INPUT		0x8
#define HID_MAIN_OUTPUT		0x9
#define HID_MAIN_COLLECTION	0xA
#define HID_MAIN_FEATURE	0xB
#define HID_MAIN_END_COLLECTION	0xC

/* Global item tags. */
#define HID_GLOBAL_USAGE_PAGE	0x0
#define HID_GLOBAL_REPORT_SIZE	0x7
#define HID_GLOBAL_REPORT_ID	0x8
#define HID_GLOBAL_REPORT_COUNT	0x9
#define HID_GLOBAL_PUSH		0xA
#define HID_GLOBAL_POP		0xB

/* The prefix that means a long item rather than a short one. Long items carry
 * their own size byte and this kernel understands none of them -- but it has
 * to skip them by the right amount, because skipping by the wrong amount
 * leaves the walk reading data bytes as item prefixes, which produces items
 * that look real. */
#define HID_LONG_ITEM_PREFIX	0xFE

/* How many distinct report IDs this will remember. A device with more than
 * this is not refused; the count keeps rising and the list stops filling, and
 * `report_ids_truncated` says so rather than the number quietly being wrong. */
#define HID_MAX_REPORT_IDS	8

/* --- the second pass: what each part of a report means -------------------- */

/* The two usage pages a pointing device lives on. */
#define HID_PAGE_GENERIC_DESKTOP	0x01
#define HID_PAGE_BUTTON			0x09

/* On the Generic Desktop page. */
#define HID_USAGE_MOUSE			0x02
#define HID_USAGE_X			0x30
#define HID_USAGE_Y			0x31
#define HID_USAGE_WHEEL			0x38

/* Local item tags. Usage is Local, usage *page* is Global -- which is the
 * distinction that makes the state machine below have two halves. */
#define HID_LOCAL_USAGE			0x0
#define HID_LOCAL_USAGE_MINIMUM		0x1
#define HID_LOCAL_USAGE_MAXIMUM		0x2

/* The bits of an Input item's own data byte. Only three matter here.
 *
 * Constant means padding with no usage -- and it still occupies its bits, so
 * a walker that skips it puts every field after it at the wrong offset.
 * Variable means one value per field; its opposite, an array, is how keyboards
 * report and is not handled by this pass. */
#define HID_INPUT_CONSTANT		0x01
#define HID_INPUT_VARIABLE		0x02
#define HID_INPUT_RELATIVE		0x04

/* How many fields of an input report this will describe. A boot mouse needs
 * six -- three buttons, a padding field, and two axes -- and a five-button
 * mouse with a wheel needs ten. Beyond this the map stops filling and
 * `fields_truncated` says so. */
#define HID_MAX_FIELDS			32

/* One piece of an input report. */
struct hid_field {
	u32 bit_offset;
	u32 bit_size;
	u16 usage_page;
	u16 usage;
	bool constant;		/* padding: occupies bits, means nothing */
	bool relative;		/* a delta, like a mouse axis */
};

struct hid_report_info {
	/* The answer `bt_hid.c` needs. */
	bool uses_report_id;

	u8  report_ids[HID_MAX_REPORT_IDS];
	unsigned report_id_count;
	bool report_ids_truncated;

	/* Only meaningful when `uses_report_id` is false -- see the header
	 * comment. Zero and `input_bits_known` false otherwise. */
	bool input_bits_known;
	u32  input_bits;
	u32  input_bytes;	/* rounded up; a report is whole bytes on the wire */

	/* Instrumentation. A descriptor that parsed but produced nothing is a
	 * different fact from one that was refused, and these tell them
	 * apart. */
	unsigned items;
	unsigned main_items;
	unsigned collection_depth_max;
	unsigned long_items;

	/* --- the field map, which is the second pass ---------------------
	 *
	 * Where each piece of the input report lives and what it means. Only
	 * filled when `fields_usable` -- see below, and check that before
	 * reading any of it.
	 */
	struct hid_field fields[HID_MAX_FIELDS];
	unsigned field_count;
	bool fields_truncated;

	/* **False means the map is incomplete, not that parsing failed.**
	 *
	 * Some descriptors use features this pass does not implement, and a
	 * map built while ignoring one of them is wrong in a way that still
	 * looks like a map -- a field at the wrong bit offset decodes to a
	 * number, and a mouse built on it moves. So the map is withheld
	 * rather than guessed, and `fields_unusable` says which feature did
	 * it, in words, for the boot log.
	 */
	bool fields_usable;
	const char *fields_unusable;
};

/* What a mouse needs out of all that: where the buttons are and where the
 * axes are. */
struct hid_mouse_layout {
	bool found;

	u32 buttons_offset;	/* bit offset of the first button */
	u32 buttons_count;	/* one bit each, consecutive */

	u32 x_offset, x_size;	/* bits */
	u32 y_offset, y_size;

	bool have_wheel;
	u32 wheel_offset, wheel_size;

	u32 report_bytes;
};

/* Picks a mouse out of a parsed descriptor. False when the descriptor does not
 * describe one, when the field map was withheld, or when the axes are missing
 * -- a device with buttons and no X is not a mouse this can drive. */
bool hid_report_mouse_layout(const struct hid_report_info *info,
			     struct hid_mouse_layout *out);

/* --- reading a field out of a report -------------------------------------
 *
 * The map above says where a field is. This gets its value out, and it is the
 * last step before a descriptor-driven driver can post the same events
 * `usb_hid.c` posts from hardcoded byte offsets.
 *
 * **Fields are not bytes and are not byte-aligned.** A boot mouse's three
 * buttons are bits 0, 1 and 2 of byte 0; a five-button mouse with a 12-bit
 * axis has fields that start mid-byte and end in the next one. Anything that
 * reads whole bytes works for the first device and not the second.
 *
 * **Bits are packed least-significant-first.** Button 1 is bit 0. A reader
 * that walks from the top of each byte gets the buttons in reverse and the
 * axes as nonsense, and for a symmetric byte it gets the right answer anyway,
 * which is how it survives a casual test.
 *
 * **Sign extension is from the field's own width, not from a byte.** An
 * eight-bit -5 is 0xFB and a twelve-bit -5 is 0xFFB. Sign-extending the second
 * from eight bits, or not at all, gives 4091 -- a large positive number, so
 * the pointer flies off in the wrong direction rather than not moving.
 *
 * False when the field runs past the end of the report, or when `bit_size` is
 * zero or wider than the 32 bits the result is returned in.
 */
bool hid_field_extract(const u8 *report, u32 report_len, u32 bit_offset,
		       u32 bit_size, bool is_signed, i32 *out);

/* The three values a mouse report carries, read through a layout.
 *
 * `buttons` is a bitmask in the order the descriptor declared them, so bit 0
 * is the first button. Wheel is zero when the device has none. */
struct hid_mouse_state {
	u32 buttons;
	i32 x;
	i32 y;
	i32 wheel;
};

/* Reads one report through a layout. False when the report is shorter than
 * the layout says it should be, which is a device disagreeing with its own
 * descriptor and not something to decode half of. */
bool hid_mouse_decode(const struct hid_mouse_layout *m, const u8 *report,
		      u32 len, struct hid_mouse_state *out);

/* Walks a descriptor. False means the bytes are not a well-formed descriptor
 * -- an item running past the end, or a collection nesting that never closes
 * -- and `out` is then not to be trusted. */
bool hid_report_parse(const u8 *desc, u32 len, struct hid_report_info *out);

/* How many bytes of data a short item's prefix says follow it.
 *
 * Exposed because it is the single most misread rule in the format and it is
 * worth being able to test alone: the two size bits encode 0, 1, 2 and
 * **4** -- not 3. */
u32 hid_item_data_length(u8 prefix);

bool hid_report_self_test(void);

#endif /* RECON_KERNEL_HID_REPORT_H */
