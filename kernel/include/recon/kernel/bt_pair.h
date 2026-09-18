/* Pairing: agreeing a link key with a device.
 *
 * --- None of the cryptography is here, and that is not a shortcut ---
 *
 * For BR/EDR the host does no crypto at all. Legacy pairing derives its key
 * with E21/E22 **in the controller**; Secure Simple Pairing does its elliptic
 * curve exchange and its confirmation values **in the controller** too. What
 * the host does is answer questions: *what can you display, do you accept this
 * number, what is the PIN, here is the key I stored last time.*
 *
 * So this file is plumbing, and it is worth saying so plainly rather than
 * leaving the impression that a key is being computed somewhere in it.
 *
 * --- What pairing a mouse can and cannot promise ---
 *
 * Secure Simple Pairing picks its association model from what the two ends can
 * do. Numeric Comparison needs both to show a six-digit number and take a yes.
 * Passkey Entry needs one to show and the other to type.
 *
 * **A mouse has no display and no keyboard.** Whatever this kernel claims it
 * can do, the model that results is *Just Works* -- and Just Works performs
 * the key exchange without authenticating either end, so a device in range
 * that answers first is indistinguishable from the intended one. That is a
 * property of pairing a device with no way to show you anything, not a
 * weakness introduced here, and no choice available to this file removes it.
 *
 * What this file can do is limit *when* it applies, and it does two things:
 *
 *   1. **Pairing is refused unless a window is open.** The window is opened
 *      deliberately, by something that knows a device is meant to be paired.
 *      A controller that asks unprompted gets a negative reply.
 *   2. **Only the address being paired with is answered.** A request naming
 *      any other device is refused, whatever the window says.
 *
 * Refused, not ignored. A controller that asked and got nothing waits, and so
 * does everything above it -- the same reasoning `bt_link.c` gives for
 * reporting a pairing request rather than dropping it. Every refusal here is
 * an explicit negative reply.
 *
 * --- Where the numbers come from ---
 *
 * Checked against BlueZ 5.72's `hci.h`: `OCF_LINK_KEY_REPLY 0x000B`,
 * `OCF_LINK_KEY_NEG_REPLY 0x000C`, `OCF_PIN_CODE_REPLY 0x000D`,
 * `OCF_PIN_CODE_NEG_REPLY 0x000E`, `OCF_AUTH_REQUESTED 0x0011`,
 * `OCF_SET_CONN_ENCRYPT 0x0013`, `OCF_IO_CAPABILITY_REPLY 0x002B`,
 * `OCF_USER_CONFIRM_REPLY 0x002C`, `OCF_USER_CONFIRM_NEG_REPLY 0x002D`,
 * `OCF_IO_CAPABILITY_NEG_REPLY 0x0034`, the event codes, and the layouts of
 * `pin_code_reply_cp`, `link_key_reply_cp`, `io_capability_reply_cp`,
 * `evt_link_key_notify`, `evt_user_confirm_request` and
 * `evt_simple_pairing_complete`.
 *
 * **The IO capability and authentication-requirement values are not in that
 * header** -- only the structure sizes are -- so those are from the
 * specification and written from memory, like `BT_GIAC`. Named here so they
 * do not look as checked as the rest.
 */
#ifndef RECON_KERNEL_BT_PAIR_H
#define RECON_KERNEL_BT_PAIR_H

#include <recon/kernel/bt_link.h>
#include <recon/kernel/types.h>

#define BT_LINK_KEY_LEN		16
#define BT_PIN_MAX		16

/* Commands, all in the link control group. */
#define HCI_OP_LINK_KEY_REPLY	   HCI_OPCODE(HCI_OGF_LINK_CTL, 0x000B)
#define HCI_OP_LINK_KEY_NEG_REPLY  HCI_OPCODE(HCI_OGF_LINK_CTL, 0x000C)
#define HCI_OP_PIN_CODE_REPLY	   HCI_OPCODE(HCI_OGF_LINK_CTL, 0x000D)
#define HCI_OP_PIN_CODE_NEG_REPLY  HCI_OPCODE(HCI_OGF_LINK_CTL, 0x000E)
#define HCI_OP_AUTH_REQUESTED	   HCI_OPCODE(HCI_OGF_LINK_CTL, 0x0011)
#define HCI_OP_SET_CONN_ENCRYPT	   HCI_OPCODE(HCI_OGF_LINK_CTL, 0x0013)
#define HCI_OP_IO_CAP_REPLY	   HCI_OPCODE(HCI_OGF_LINK_CTL, 0x002B)
#define HCI_OP_USER_CONFIRM_REPLY  HCI_OPCODE(HCI_OGF_LINK_CTL, 0x002C)
#define HCI_OP_USER_CONFIRM_NEG	   HCI_OPCODE(HCI_OGF_LINK_CTL, 0x002D)
#define HCI_OP_IO_CAP_NEG_REPLY	   HCI_OPCODE(HCI_OGF_LINK_CTL, 0x0034)

/* Events. */
#define HCI_EV_AUTH_COMPLETE	   0x06
#define HCI_EV_LINK_KEY_NOTIFY	   0x18
#define HCI_EV_IO_CAP_REQUEST	   0x31
#define HCI_EV_IO_CAP_RESPONSE	   0x32
#define HCI_EV_USER_CONFIRM_REQ	   0x33
#define HCI_EV_USER_PASSKEY_REQ	   0x34
#define HCI_EV_SIMPLE_PAIR_COMPLETE 0x36

/* IO capabilities. **From the specification, not from a header.** */
#define BT_IO_DISPLAY_ONLY	0x00
#define BT_IO_DISPLAY_YES_NO	0x01
#define BT_IO_KEYBOARD_ONLY	0x02
#define BT_IO_NO_INPUT_OUTPUT	0x03

/* Authentication requirements, same provenance. "General bonding" means the
 * key is kept for next time, which is the point of pairing a mouse once. */
#define BT_AUTH_NO_MITM_GENERAL	0x04
#define BT_AUTH_MITM_GENERAL	0x05

#define BT_OOB_NOT_PRESENT	0x00

enum bt_pair_state {
	BT_PAIR_IDLE,
	BT_PAIR_RUNNING,
	BT_PAIR_DONE,
	BT_PAIR_FAILED
};

struct bt_pairing {
	/* **Closed by default.** Nothing pairs unless something opened this
	 * on purpose. */
	bool window_open;
	u8   target[BT_ADDR_LEN];
	bool have_target;

	/* What this kernel says it can do. A machine with a screen could
	 * honestly claim DisplayYesNo; with a mouse at the other end the
	 * model is Just Works either way, so the default says so. */
	u8 io_capability;
	u8 auth_requirements;

	/* The PIN offered for legacy pairing, and its length. Legacy devices
	 * overwhelmingly use "0000", and a wrong one is a refusal rather than
	 * a risk.
	 *
	 * **Set these with `bt_pairing_set_pin`, not directly.** `pin_len`
	 * feeds a copy into a fixed buffer, and it was bounded only by the
	 * convention that nothing wrote it except `bt_pairing_init`. It is
	 * checked at the copy now as well, but a length that never becomes
	 * wrong is better than one caught later. */
	u8 pin[BT_PIN_MAX];
	u8 pin_len;

	enum bt_pair_state state;
	u8 failure;

	/* One key. A table belongs with a second device. */
	bool have_key;
	u8   key_addr[BT_ADDR_LEN];
	u8   key[BT_LINK_KEY_LEN];
	u8   key_type;

	/* The number the peer wanted compared. Kept so that something with a
	 * screen could show it later; nothing does today. */
	u32 last_passkey;

	/* Requests answered with a negative reply because they were outside
	 * the window or named another device. Counted rather than silent --
	 * a machine being asked to pair when nobody asked it to is worth
	 * seeing in a boot log. */
	u64 refused;
};

void bt_pairing_init(struct bt_pairing *p);

/* Opens the window for one device, and closes it again. */
void bt_pairing_allow(struct bt_pairing *p, const u8 *addr);
void bt_pairing_close(struct bt_pairing *p);

/* Sets the legacy PIN. False for an empty one or one longer than the field
 * holds, which is a configuration mistake worth hearing about where it is
 * made rather than where a device asks. */
bool bt_pairing_set_pin(struct bt_pairing *p, const u8 *pin, u8 len);

/* Hands a previously stored key back, so a known device does not pair twice. */
void bt_pairing_remember(struct bt_pairing *p, const u8 *addr, const u8 *key,
			 u8 key_type);

/* Feeds one HCI event in and writes the command it draws to `out`. Returns
 * that command's length, or zero when the event needs no answer.
 *
 * **Every pairing request draws an answer**, positive or negative. A zero
 * here means the event was not one of pairing's. */
u32 bt_pairing_event(struct bt_pairing *p, const u8 *ev, u32 len, u8 *out,
		     u32 outmax);

/* Asks for the link to be authenticated, and then encrypted. */
u32 hci_auth_requested_build(u8 *out, u16 handle);
u32 hci_set_conn_encrypt_build(u8 *out, u16 handle, bool on);

bool bt_pairing_self_test(void);

#endif /* RECON_KERNEL_BT_PAIR_H */
