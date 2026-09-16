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
};

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
