/* The whole sequence: from nothing to a mouse that moves.
 *
 * Every state machine on this branch runs one conversation. This runs them in
 * order, and the order is the part none of them can test:
 *
 *     reset          the controller
 *     inquire        find a device
 *     connect        an ACL link to it
 *     pair           if it asks, and only if invited
 *     control chan   L2CAP PSM 0x0011
 *     interrupt chan L2CAP PSM 0x0013
 *     boot protocol  SET_PROTOCOL on the control channel
 *     running        reports arrive and the pointer moves
 *
 * **Two channels, and the order between them matters.** A HID device expects
 * control to exist before interrupt: the protocol is set on control, and a
 * device that receives reports requests before it has been configured may
 * simply not answer. Opening them the other way round is the kind of thing
 * that works against one device and not another, so the sequence is fixed
 * here and asserted.
 *
 * --- Why a descriptor is not fetched ---
 *
 * The report layout could come from SDP -- `sdp.c` reads it out of attribute
 * 0x0206 -- but SDP is a third L2CAP channel and a request/response protocol
 * this branch has not written. Boot mode needs no descriptor at all: the
 * layout is fixed, which is the whole point of it.
 *
 * So this asks for boot protocol and uses `hid_mouse_boot_layout`. That is
 * not a guess -- the self-test in `hid_report.c` asserts the hardcoded layout
 * is identical to what parsing a real boot descriptor produces.
 *
 * **It will not work on every mouse.** A Bluetooth HID device is not obliged
 * to offer boot mode, and one that refuses SET_PROTOCOL needs the descriptor
 * and therefore SDP. That device is reported, not driven.
 */
#ifndef RECON_KERNEL_BT_STACK_H
#define RECON_KERNEL_BT_STACK_H

#include <recon/kernel/bluetooth.h>
#include <recon/kernel/bt_link.h>
#include <recon/kernel/bt_mouse.h>
#include <recon/kernel/bt_pair.h>
#include <recon/kernel/l2cap.h>
#include <recon/kernel/types.h>

enum bt_stack_state {
	BT_ST_IDLE,
	BT_ST_RESETTING,
	BT_ST_INQUIRING,
	BT_ST_CONNECTING,
	BT_ST_OPENING_CONTROL,
	BT_ST_OPENING_INTERRUPT,
	BT_ST_SETTING_PROTOCOL,
	BT_ST_RUNNING,
	BT_ST_FAILED
};

/* This side's channel identifiers. Dynamic CIDs start at 0x0040 and these are
 * the first two; nothing else on this branch allocates any. */
#define BT_CID_CONTROL		0x0040
#define BT_CID_INTERRUPT	0x0041

struct bt_stack {
	enum bt_stack_state state;
	const char *failure;		/* in words, for a boot log */

	struct bt_link link;
	struct bt_pairing pairing;
	struct l2cap_channel control;
	struct l2cap_channel interrupt;
	struct bt_mouse mouse;

	u8 next_ident;			/* L2CAP signalling identifiers */

	/* Counted so a boot log can say what happened rather than only where
	 * it stopped. */
	u64 reports;
	u64 commands_sent;
};

void bt_stack_init(struct bt_stack *s);

/* Starts the sequence. `addr` may be null to inquire for anything, or a
 * specific device. Returns the first command to send. */
u32 bt_stack_start(struct bt_stack *s, const u8 *addr, u8 *out, u32 outmax);

/* Feeds one complete HCI event in. Returns the command it draws, or zero. */
u32 bt_stack_event(struct bt_stack *s, const u8 *ev, u32 len, u8 *out,
		   u32 outmax);

/* Feeds one ACL fragment in. Returns an L2CAP PDU to send **in reply**, or
 * zero, and sets `moved` when a mouse report was decoded into `state`. */
u32 bt_stack_acl(struct bt_stack *s, u16 handle, u8 pb, const u8 *payload,
		 u32 len, u8 *out, u32 outmax, bool *moved,
		 struct hid_mouse_state *state);

/* Emits the next command this sequence owes on its own initiative, if one is
 * due. Returns its length, or zero when there is nothing to send.
 *
 * **Separate from `bt_stack_acl` because a reply and a next step are not the
 * same thing**, and the first version of this conflated them. Answering their
 * Configure Request and opening the next channel are both things to send, and
 * a function that can return only one of them returns the reply and silently
 * drops the step -- which it did: the channel-opened check sat after an early
 * return that signalling almost always took, so the sequence stopped with the
 * control channel open and nothing following it.
 *
 * A driver calls this after every input and whenever it has nothing else to
 * do. It is safe to call at any time and returns zero when idle. */
u32 bt_stack_poll(struct bt_stack *s, u8 *out, u32 outmax);

bool bt_stack_self_test(void);

#endif /* RECON_KERNEL_BT_STACK_H */
