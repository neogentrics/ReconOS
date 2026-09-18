/* HID over Bluetooth: the transaction layer on top of L2CAP.
 *
 * The third of three, and the last one that is pure bytes:
 *
 *     HIDP         this file -- one header byte, then a report
 *     L2CAP        two channels, control and interrupt
 *     HCI / ACL    one link, fragmented by the controller
 *
 * Every payload on either HID channel begins with **one byte** that says what
 * kind of message it is. A mouse's movement arrives as a DATA transaction
 * carrying an INPUT report, which is the byte 0xA1, and that single byte is
 * most of what this layer is.
 *
 * --- The two nibbles, and why they are worth a test ---
 *
 * The transaction type is the **top** nibble and the parameter is the bottom
 * one. Packing them the other way round is not an error anybody's code
 * detects: 0xA1 built backwards is 0x1A, and 0x1A is a perfectly valid header
 * meaning HID_CONTROL with parameter 10. A device would receive a control
 * message instead of data, and a reader would decode one as the other.
 *
 * --- What this layer cannot decide, and says so ---
 *
 * **Whether a report ID byte sits between the header and the data depends on
 * the device's report descriptor, and this kernel cannot read one.** Guessing
 * shifts every field by one byte: a mouse's buttons are read from its X, its X
 * from its Y, and the pointer moves -- wrongly, but it moves, which is the
 * worst kind of wrong because it looks like it works.
 *
 * So it is not guessed. `bt_hid_input_report` takes it as an argument, which
 * makes the unanswered question visible at every call site instead of hidden
 * in here.
 *
 * **There is now something that can answer it.** `hid_report.h` walks a
 * device's report descriptor and returns `uses_report_id`, which is exactly
 * this argument. A caller that has fetched a descriptor should pass that
 * rather than a constant; a caller that has not still has to decide, and the
 * argument is where that decision stays visible.
 */
#ifndef RECON_KERNEL_BT_HID_H
#define RECON_KERNEL_BT_HID_H

#include <recon/kernel/types.h>

/* The two channels a HID device offers, by PSM. They are in `l2cap.h` because
 * connecting is L2CAP's business; named here as a reminder that **both** are
 * needed -- control carries the handshake, interrupt carries the reports, and
 * a driver with one of them has either a mouse that cannot be configured or
 * one that never moves. */

/* --- these were unverified, and now are not -------------------------------
 *
 * An earlier version of this comment said the constants below could not be
 * checked: BlueZ's userspace `hidp.h` carries only its ioctl interface, and
 * the protocol's transaction types live in the Linux kernel's own
 * `net/bluetooth/hidp/hidp.h`, which no available package ships.
 *
 * **They have since been checked against that header directly**, on
 * 17 September 2026, and every one agreed. The kernel writes its transaction
 * types pre-shifted -- `HIDP_TRANS_DATA 0xa0` where this file has `0xA` and
 * shifts in `bt_hid_header` -- so the comparison is of the nibbles, and all
 * seven match. The handshake results, the control parameters and the report
 * types match exactly. So does `HIDP_PROTO_BOOT 0x00`, which was the single
 * value flagged here as least certain.
 *
 * **One thing was found, and it is below**: the report type in a DATA
 * transaction is **two bits**, not four. The kernel masks it with
 * `HIDP_DATA_RTYPE_MASK 0x03` and names the other two `RSRVD`. This file was
 * comparing the whole low nibble, which is right for every compliant device
 * and wrong for one that sets a reserved bit -- an input report that would
 * then be read as an unknown type and dropped.
 */

/* --- transaction types: the top nibble ------------------------------------ */
#define HIDP_TRANS_HANDSHAKE		0x0
#define HIDP_TRANS_HID_CONTROL		0x1
#define HIDP_TRANS_GET_REPORT		0x4
#define HIDP_TRANS_SET_REPORT		0x5
#define HIDP_TRANS_GET_PROTOCOL		0x6
#define HIDP_TRANS_SET_PROTOCOL		0x7
#define HIDP_TRANS_DATA			0xA

/* --- report types: the bottom *two bits* of a DATA or GET/SET_REPORT -----
 *
 * Two, not four. The other two bits of the low nibble are reserved, and the
 * Linux kernel names them so -- `HIDP_DATA_RTYPE_MASK 0x03` against
 * `HIDP_DATA_RSRVD_MASK 0x0c`. Anything reading the report type must mask,
 * or a device that sets a reserved bit has its input reports read as some
 * unknown type and thrown away.
 */
#define HIDP_REPORT_TYPE_MASK		0x03

#define HIDP_REPORT_OTHER		0x0
#define HIDP_REPORT_INPUT		0x1
#define HIDP_REPORT_OUTPUT		0x2
#define HIDP_REPORT_FEATURE		0x3

/* The one byte that opens every inbound mouse report. Written as the two
 * fields it is made of, and checked against the literal in the self-test --
 * a macro that builds the wrong number builds it consistently, so a test
 * using the macro on both sides would agree with itself and prove nothing. */
#define HIDP_INPUT_DATA_HEADER \
	((u8)((HIDP_TRANS_DATA << 4) | HIDP_REPORT_INPUT))	/* 0xA1 */

/* --- handshake results: the bottom nibble of a HANDSHAKE ------------------ */
#define HIDP_HSHK_SUCCESSFUL		0x0
#define HIDP_HSHK_NOT_READY		0x1
#define HIDP_HSHK_ERR_INVALID_REPORT_ID	0x2
#define HIDP_HSHK_ERR_UNSUPPORTED_REQUEST 0x3
#define HIDP_HSHK_ERR_INVALID_PARAMETER	0x4
#define HIDP_HSHK_ERR_UNKNOWN		0x0E
#define HIDP_HSHK_ERR_FATAL		0x0F

/* --- HID_CONTROL parameters ----------------------------------------------- */
#define HIDP_CTRL_SUSPEND		0x3
#define HIDP_CTRL_EXIT_SUSPEND		0x4
#define HIDP_CTRL_VIRTUAL_CABLE_UNPLUG	0x5

/* --- SET_PROTOCOL parameters ----------------------------------------------
 *
 * Boot means the fixed layouts `usb_hid.c` already decodes -- eight bytes for
 * a keyboard, three or four for a mouse -- and report means whatever the
 * device's descriptor says. This kernel can only handle the first, and unlike
 * USB, **a Bluetooth device is not required to offer it**. A device that
 * refuses boot mode needs the descriptor parser neither driver has. */
#define HIDP_PROTO_BOOT			0x0
#define HIDP_PROTO_REPORT		0x1

struct bt_hid_message {
	u8 transaction;
	u8 parameter;
	const u8 *payload;	/* points into the caller's buffer */
	u32 length;
};

/* An input report, once the framing is off it. */
struct bt_hid_report {
	bool have_id;
	u8   id;
	const u8 *data;
	u32  length;
};

/* Builds the one-byte header. */
u8 bt_hid_header(u8 transaction, u8 parameter);

/* Splits an L2CAP payload into its header and the rest. False for an empty
 * one, which is not a message. */
bool bt_hid_parse(const u8 *pdu, u32 len, struct bt_hid_message *out);

/* Pulls an input report out of a parsed message.
 *
 * `uses_report_id` is the caller's answer to a question this kernel cannot
 * answer for itself -- see the note at the top. False for any message that is
 * not a DATA/INPUT transaction, and for one carrying no report. */
bool bt_hid_input_report(const struct bt_hid_message *m, bool uses_report_id,
			 struct bt_hid_report *out);

/* Whether a handshake parameter means the request succeeded. Separate from
 * "is it fatal", because NOT_READY is neither -- it means ask again, and a
 * driver that reads it as a refusal gives up on a device that was about to
 * work. */
bool bt_hid_handshake_ok(u8 parameter);
bool bt_hid_handshake_retryable(u8 parameter);

bool bt_hid_self_test(void);

#endif /* RECON_KERNEL_BT_HID_H */
