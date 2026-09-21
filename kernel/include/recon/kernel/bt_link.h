/* Bringing up an ACL link: finding a device and connecting to it.
 *
 * The half `bt_mouse.c` starts after. It was left unwritten because it looked
 * like guesswork -- and then BlueZ's `hci.h` turned out to carry every opcode,
 * every event code and every structure layout this needs, so it is as
 * checkable as L2CAP was. **The objection was to writing from memory, not to
 * the work**, and it does not apply once there is something to check against.
 *
 * --- The trap that matters most here ---
 *
 * **A command that returns Command Status is not finished.** Create Connection
 * answers immediately with a status meaning *"accepted, I am working on it"*,
 * and the real answer arrives later as a Connection Complete event. Reading
 * the Command Status as the result reports a link that does not exist yet, and
 * everything above then talks to a handle the controller has not issued.
 *
 * That is the same shape as L2CAP's Connection Response *pending*, and as the
 * opcode match in `bluetooth.c`, and as KF-248's endpoint id underneath all of
 * them. Four layers, one rule: **an answer that arrives early, or that does
 * not name what it is answering, is not the answer.**
 *
 * --- Two byte orders that are easy to get backwards ---
 *
 * A **BD_ADDR is little-endian on the wire** and written big-endian by people.
 * The address printed `00:11:22:33:44:55` goes out as `55 44 33 22 11 00`.
 * Reversed, it is a valid-looking address for a device that is not there, so
 * the failure is a connection attempt that times out rather than an error.
 *
 * The **inquiry access code is three bytes, also little-endian**. The general
 * one is `0x9E8B33`, which goes out as `33 8B 9E`.
 *
 * --- Where these numbers come from ---
 *
 * Checked against BlueZ 5.72's `hci.h`: the OGF groups, `OCF_INQUIRY 0x0001`,
 * `OCF_CREATE_CONN 0x0005`, `OCF_DISCONNECT 0x0006`, the event codes, the
 * packet-type bits, `ACL_LINK 0x01`, and the field order of `inquiry_cp`,
 * `inquiry_info`, `create_conn_cp`, `evt_conn_complete` and
 * `evt_disconn_complete`.
 *
 * It also confirmed two opcodes this branch already had: `OCF_RESET 0x0003` in
 * `OGF_HOST_CTL` is 0x0C03, and `OCF_READ_BD_ADDR 0x0009` in `OGF_INFO_PARAM`
 * is 0x1009.
 *
 * **One value is not from there.** `BT_GIAC` is from the specification and
 * appears in no header available here, so it is written from memory like the
 * HIDP constants once were. Said plainly rather than left looking as checked
 * as its neighbours.
 */
#ifndef RECON_KERNEL_BT_LINK_H
#define RECON_KERNEL_BT_LINK_H

#include <recon/kernel/bluetooth.h>
#include <recon/kernel/bt_bytes.h>
#include <recon/kernel/types.h>

/* `BT_ADDR_LEN` and the byte-order helpers come from `bt_bytes.h`, which is
 * where `bt_addr_equal` lives -- and that had to be somewhere both this file
 * and `bt_pair.c` could reach, since each had its own copy of it. */

/* Opcode groups, and the commands this needs from them. */
#define HCI_OGF_LINK_CTL	0x01

#define HCI_OP_INQUIRY		HCI_OPCODE(HCI_OGF_LINK_CTL, 0x0001)
#define HCI_OP_INQUIRY_CANCEL	HCI_OPCODE(HCI_OGF_LINK_CTL, 0x0002)
#define HCI_OP_CREATE_CONN	HCI_OPCODE(HCI_OGF_LINK_CTL, 0x0005)
#define HCI_OP_DISCONNECT	HCI_OPCODE(HCI_OGF_LINK_CTL, 0x0006)

/* Events. */
#define HCI_EV_INQUIRY_COMPLETE	0x01
#define HCI_EV_INQUIRY_RESULT	0x02
#define HCI_EV_CONN_COMPLETE	0x03
#define HCI_EV_DISCONN_COMPLETE	0x05
#define HCI_EV_PIN_CODE_REQ	0x16
#define HCI_EV_LINK_KEY_REQ	0x17

/* The general inquiry access code. **Not checked against a header** -- see the
 * note at the top. Three bytes, little-endian on the wire. */
#define BT_GIAC			0x9E8B33u

/* The packet types a connection is willing to use. DM1, DH1, DM3, DH3, DM5 and
 * DH5, which is 0xCC18 -- and the sum of BlueZ's individual bits comes to
 * exactly that, which is how the constant was checked rather than recalled. */
#define HCI_PKT_TYPE_DEFAULT	0xCC18

#define HCI_LINK_TYPE_ACL	0x01

enum bt_link_state {
	BT_LINK_IDLE,
	BT_LINK_INQUIRING,
	BT_LINK_CONNECTING,
	BT_LINK_UP,

	/* The controller asked for a PIN or a stored link key. Pairing is not
	 * implemented, and this state exists so that a device wanting it is
	 * *reported* rather than waited on for ever. */
	BT_LINK_NEEDS_PAIRING,

	BT_LINK_FAILED
};

struct bt_inquiry_result {
	u8  addr[BT_ADDR_LEN];	/* wire order: least significant byte first */
	u8  pscan_rep_mode;
	u32 device_class;	/* raw; interpreting it is not done here */
	u16 clock_offset;
};

struct bt_link {
	enum bt_link_state state;

	u8   peer[BT_ADDR_LEN];
	bool have_peer;
	u8   peer_pscan_rep_mode;
	u16  peer_clock_offset;

	u16  handle;		/* twelve bits, once the link is up */
	u8   failure;		/* the HCI status, when the state is FAILED */

	unsigned results;	/* inquiry results seen */

	/* An event that named a device this is not talking to. Counted
	 * because taking one would act on another link's answer. */
	u64 wrong_address;
};

/* --- building commands ---------------------------------------------------- */

/* `length` is in units of 1.28 seconds, `num_rsp` zero for unlimited. */
u32 hci_inquiry_build(u8 *out, u8 length, u8 num_rsp);

u32 hci_create_connection_build(u8 *out, const u8 *addr, u16 pkt_type,
				u8 pscan_rep_mode, u16 clock_offset,
				bool allow_role_switch);

u32 hci_disconnect_build(u8 *out, u16 handle, u8 reason);

/* --- reading events ------------------------------------------------------- */

/* One entry out of an Inquiry Result, which carries several. `n` selects
 * which; false when there are fewer than that. */
bool hci_inquiry_result_parse(const u8 *ev, u32 len, unsigned n,
			      struct bt_inquiry_result *out);

/* --- the state machine ---------------------------------------------------- */

void bt_link_init(struct bt_link *l);

/* Names the device to connect to, so that a Connection Complete for anything
 * else can be told apart from this one's. */
void bt_link_target(struct bt_link *l, const u8 *addr);

/* Feeds one complete HCI event in, and writes any command it should draw to
 * `out`. Returns that command's length, or zero. */
u32 bt_link_event(struct bt_link *l, const u8 *ev, u32 len, u8 *out,
		  u32 outmax);

bool bt_link_self_test(void);

#endif /* RECON_KERNEL_BT_LINK_H */
