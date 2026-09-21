/* A Bluetooth mouse, from an ACL fragment to a pointer that moves.
 *
 * Every layer on this branch exists to reach this file. What it does is join
 * them, and the join is the part none of their own tests cover:
 *
 *     ACL fragment            from the controller, possibly a piece
 *       -> l2cap reassembly   a whole PDU, for the right link
 *       -> channel check      is this the interrupt channel, or signalling?
 *       -> HIDP transaction   is it DATA carrying an INPUT report?
 *       -> report id          present or not, and the descriptor decides
 *       -> field extraction   through the layout the descriptor produced
 *       -> input events       buttons and motion
 *
 * Each of those is tested alone. **A chain of correct links can still be
 * joined wrongly** -- a CID compared against the wrong field, a report handed
 * on with its transaction byte still attached, a layout built from one
 * device's descriptor used on another's report. Those faults live in the
 * joins, and only something that runs the whole chain can find them.
 *
 * --- Where the report-id question finally gets answered ---
 *
 * `bt_hid_input_report` takes "does this device use report IDs" as an
 * argument, because that layer cannot know. `hid_report_parse` answers it
 * from the descriptor. This file is where the answer is carried from one to
 * the other, which is the whole reason both were written that way rather than
 * with a guess in the middle.
 *
 * --- Deliberately not here ---
 *
 * **Connecting.** Bringing up an ACL link -- inquiry, paging, pairing -- is
 * HCI command sequencing this branch has not written, and would be writing
 * from memory rather than from anything checkable. This file starts from a
 * link and an open channel and says so; `bt_mouse_configure` is handed both.
 *
 * Nothing is lost by that split. The connecting half is what the transport
 * blocks on anyway (KF-248), and the half here is the half that can be tested
 * today.
 */
#ifndef RECON_KERNEL_BT_MOUSE_H
#define RECON_KERNEL_BT_MOUSE_H

#include <recon/kernel/hid_report.h>
#include <recon/kernel/l2cap.h>
#include <recon/kernel/types.h>

struct bt_mouse {
	bool ready;

	u16 acl_handle;		/* the link this mouse is on */
	u16 intr_cid;		/* this side's CID for the interrupt channel */

	struct l2cap_reassembly rx;
	struct hid_mouse_layout layout;

	/* From the descriptor, and handed to the HIDP layer, which cannot
	 * work it out for itself. */
	bool uses_report_id;

	/* What has been seen. Separated because they mean different things:
	 * traffic for another channel is ordinary, a malformed report is not,
	 * and a zero in one says nothing about the other. */
	u64 reports;
	u64 other_channel;
	u64 not_input;
	u64 malformed;
};

/* Sets a mouse up from its report descriptor, the link it is on, and the
 * channel its reports arrive on.
 *
 * False when the descriptor does not describe a mouse this can drive -- no
 * axes, a field map that had to be withheld, or report IDs in a shape the
 * layout cannot express. Refusing here is the point: the alternative is a
 * pointer that moves wrongly. */
bool bt_mouse_configure(struct bt_mouse *m, const u8 *descriptor, u32 len,
			u16 acl_handle, u16 intr_cid);

/* Feeds one ACL fragment in. Returns true and fills `out` when that fragment
 * completed a report on this mouse's interrupt channel.
 *
 * Separate from posting so that the whole chain can be checked by value in a
 * self-test, with no input layer to read back from. */
bool bt_mouse_acl(struct bt_mouse *m, u16 handle, u8 pb, const u8 *payload,
		  u32 len, struct hid_mouse_state *out);

/* The same, given a PDU that is **already whole**.
 *
 * Split out because a caller that owns the reassembly itself must not have
 * this one feed it a second time. `bt_stack.c` does own it: it shares one
 * buffer across the signalling channel and the reports, and having both this
 * and the stack call `l2cap_acl_feed` meant the stack could then act on a
 * PDU that had not finished arriving -- the feed said "not yet", the mouse
 * returned false for that reason, and the stack read the buffer anyway.
 *
 * `bt_mouse_acl` is now this plus a feed, so there is one place that decides
 * whether a PDU is complete. */
bool bt_mouse_pdu(struct bt_mouse *m, const u8 *pdu, u32 len,
		  struct hid_mouse_state *out);

/* Posting lives in `hid_boot.c` -- `hid_boot_mouse` takes the report bytes
 * `bt_hid_input_report` hands back. There is no copy here, deliberately:
 * one decoder for both transports is the whole point of it being moved
 * out of `usb_hid.c`. */

bool bt_mouse_self_test(void);

#endif /* RECON_KERNEL_BT_MOUSE_H */
