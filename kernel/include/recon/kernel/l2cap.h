/* L2CAP: the layer between an ACL link and anything that wants to use one.
 *
 * HCI carries bytes between two devices. L2CAP is what makes those bytes
 * addressable -- it multiplexes several conversations over the one ACL link
 * and puts them back together when the link has chopped them up.
 *
 * On the way to a mouse it is the middle of three layers:
 *
 *     HID          reports, which is what the input layer wants
 *     L2CAP        two channels, one for control and one for reports
 *     HCI / ACL    one link to the device, fragmented by the controller
 *
 * A Bluetooth mouse needs **two** L2CAP channels, not one: PSM 0x0011 for
 * control and PSM 0x0013 for interrupt, and the reports arrive on the second.
 * That is why this layer exists before anything HID-shaped can be written --
 * one channel would carry either the handshake or the reports, not both.
 *
 * As with `bluetooth.h`, what is here is the part that is a pure function of
 * bytes. There is no connection state machine: nothing can hold a connection
 * until the transport can carry HCI at all, and structure for a connection
 * that cannot exist is structure nothing can check.
 *
 * --- Where these numbers come from ---
 *
 * Every constant and every field order below was written from memory and then
 * **checked against BlueZ 5.72's headers** on 17 September 2026 -- `l2cap.h`
 * and `hci.h` out of `libbluetooth-dev`, read rather than linked against.
 * All of it agreed: the signalling codes, the connection results, the
 * configuration option types, `L2CAP_DEFAULT_MTU 672`, the four-byte header as
 * length-then-CID, the command header as code/ident/length, and the field
 * order of the connection and configuration responses. The response sizes
 * there also match the length guards in `l2cap.c` -- 8 for a connection
 * response, 6 for a configuration one.
 *
 * It is worth saying that this was a *check* and not a source. Writing from
 * memory and then verifying finds a wrong memory; copying would have found
 * nothing, because a copy agrees with itself.
 */
#ifndef RECON_KERNEL_L2CAP_H
#define RECON_KERNEL_L2CAP_H

#include <recon/kernel/types.h>

/* --- the basic frame ------------------------------------------------------
 *
 * Four bytes: a length and a channel, both little-endian.
 *
 * **The length excludes the header**, so a whole PDU is four bytes longer than
 * the number in it. HCI's event length excludes its header too, which makes
 * the two rules sound identical and the constants different -- 2 there, 4
 * here. Getting this wrong by four produces a PDU short by exactly its header,
 * which for a mouse report is still a plausible-looking run of bytes.
 */
#define L2CAP_HEADER		4

/* The largest payload this kernel will accept on a channel.
 *
 * 672 is the specification's default for BR/EDR, and it is used here for both
 * ends of the same fact: it is the size of the reassembly buffer **and** the
 * value put into the Configure Request's MTU option. A peer told 672 may not
 * send more, so the buffer cannot be overrun by a device that is behaving --
 * and because it is one constant, the advertised number and the allocated
 * number cannot drift apart later.
 */
#define L2CAP_MTU		672
#define L2CAP_PDU_MAX		(L2CAP_HEADER + L2CAP_MTU)

/* Channels that are fixed rather than negotiated. */
#define L2CAP_CID_NULL		0x0000
#define L2CAP_CID_SIGNALLING	0x0001
#define L2CAP_CID_CONNECTIONLESS 0x0002

/* Negotiated channels start here. A CID below this in a Connection Response is
 * a device answering with a channel it does not own. */
#define L2CAP_CID_DYNAMIC_FIRST	0x0040

/* The two a HID device offers. */
#define L2CAP_PSM_HID_CONTROL	0x0011
#define L2CAP_PSM_HID_INTERRUPT	0x0013

struct l2cap_header {
	u16 length;	/* payload only -- the whole PDU is this plus four */
	u16 cid;
};

/* --- fragments ------------------------------------------------------------
 *
 * The controller splits a PDU to fit its ACL buffer and flags each piece. Only
 * one of the four values means "more of what came before"; the rest begin
 * something.
 */
#define ACL_PB_START_NON_FLUSHABLE	0x00
#define ACL_PB_CONTINUATION		0x01
#define ACL_PB_START_FLUSHABLE		0x02
#define ACL_PB_COMPLETE_LE		0x03

/* Putting a PDU back together from those pieces.
 *
 * **One link at a time, deliberately.** ACL fragments from two connections
 * interleave freely, so a reassembler that ignores the handle will splice one
 * device's bytes into another's and produce a PDU that parses. This holds one
 * partial PDU and the handle it belongs to, and refuses a fragment from any
 * other handle rather than appending it. A kernel talking to two devices at
 * once needs one of these per link; that is a table, and a table is worth
 * adding when there is a second device rather than before.
 */
struct l2cap_reassembly {
	u8  buf[L2CAP_PDU_MAX];
	u32 have;
	u32 want;	/* whole PDU length once the header has arrived */
	u16 handle;
	bool active;

	u64 completed;
	u64 orphans;	/* a continuation with nothing to continue */
	u64 crossed;	/* a fragment from a different link than the one open */
	u64 restarts;	/* a new PDU begun while one was unfinished */
	u64 oversized;	/* a PDU larger than the MTU this kernel advertises */
};

/* --- signalling ----------------------------------------------------------- */

#define L2CAP_SIG_HEADER		4

#define L2CAP_SIG_COMMAND_REJECT	0x01
#define L2CAP_SIG_CONNECT_REQUEST	0x02
#define L2CAP_SIG_CONNECT_RESPONSE	0x03
#define L2CAP_SIG_CONFIG_REQUEST	0x04
#define L2CAP_SIG_CONFIG_RESPONSE	0x05
#define L2CAP_SIG_DISCONNECT_REQUEST	0x06
#define L2CAP_SIG_DISCONNECT_RESPONSE	0x07

/* Connection Response results. Pending is not a failure and is the one most
 * easily read as one: it means "ask again", and a driver that treats it as a
 * refusal gives up on a device that was about to say yes. */
#define L2CAP_CONN_SUCCESS		0x0000
#define L2CAP_CONN_PENDING		0x0001
#define L2CAP_CONN_REFUSED_PSM		0x0002
#define L2CAP_CONN_REFUSED_SECURITY	0x0003
#define L2CAP_CONN_REFUSED_RESOURCES	0x0004

#define L2CAP_CONF_SUCCESS		0x0000

/* Configuration options are type-length-value, and the **top bit of the type
 * is a hint** rather than part of it: an option marked as a hint may be
 * ignored, and a reader that does not mask it sees an unknown option type for
 * one it knows perfectly well. */
#define L2CAP_CONF_OPT_MTU		0x01
#define L2CAP_CONF_OPT_FLUSH_TIMEOUT	0x02
#define L2CAP_CONF_OPT_QOS		0x03
#define L2CAP_CONF_OPT_HINT		0x80

struct l2cap_signal {
	u8  code;
	u8  id;
	u16 length;
	const u8 *data;	/* points into the caller's buffer, `length` bytes */
};

/* --- what this layer can do today ----------------------------------------- */

bool l2cap_header_parse(const u8 *p, u32 len, struct l2cap_header *out);
u32  l2cap_header_build(u8 *out, u16 cid, u16 payload_length);

void l2cap_reassembly_reset(struct l2cap_reassembly *r);

/* Feeds one ACL fragment in. Returns true when `r->buf` holds one whole PDU,
 * `r->want` bytes long including its header. */
bool l2cap_acl_feed(struct l2cap_reassembly *r, u16 handle, u8 pb,
		    const u8 *payload, u32 len);

bool l2cap_signal_parse(const u8 *p, u32 len, struct l2cap_signal *out);
u32  l2cap_signal_build(u8 *out, u8 code, u8 id, const u8 *data, u16 dlen);

u32  l2cap_connect_request_build(u8 *out, u8 id, u16 psm, u16 scid);
bool l2cap_connect_response_parse(const u8 *data, u32 len, u16 *dcid,
				  u16 *scid, u16 *result, u16 *status);

u32  l2cap_config_request_build(u8 *out, u8 id, u16 dcid, u16 mtu);
bool l2cap_config_response_parse(const u8 *data, u32 len, u16 *scid,
				 u16 *flags, u16 *result);

/* Walks a configuration option list for the MTU. False when there is none,
 * which is not an error -- an absent MTU option means the default. */
bool l2cap_config_find_mtu(const u8 *opts, u32 len, u16 *mtu);

/* --- a channel, and getting one open --------------------------------------
 *
 * Connecting is four messages and a trap.
 *
 *     ->  Connection Request      here is the PSM I want
 *     <-  Connection Response     granted, and here is my channel id
 *     ->  Configure Request       here is the largest thing I can receive
 *     <-  Configure Response      accepted
 *
 * That is the half of it this side drives. **The other half runs at the same
 * time in the other direction** -- the peer sends its own Configure Request
 * and waits for this side's Configure Response -- and a channel is not open
 * until *both* have finished.
 *
 * That is the trap, and it is worth naming because getting it wrong produces a
 * channel that works. A driver that opens as soon as its own Configure
 * Response arrives has a channel the peer has not finished configuring; it
 * will usually still carry data, because the peer is usually ready by then.
 * Usually.
 *
 * So the state below tracks the two directions separately and opens on
 * neither alone.
 */
enum l2cap_channel_state {
	L2CAP_CH_CLOSED,
	L2CAP_CH_WAIT_CONNECT,
	L2CAP_CH_WAIT_CONFIG,
	L2CAP_CH_OPEN,
	L2CAP_CH_REFUSED
};

struct l2cap_channel {
	enum l2cap_channel_state state;

	u16 psm;
	u16 scid;		/* this side's channel id */
	u16 dcid;		/* the peer's, learned from the response */

	/* The two halves of configuration. Both, or it is not open. */
	bool config_out_done;	/* the peer accepted ours */
	bool config_in_done;	/* this side answered theirs */

	u16 peer_mtu;		/* what the peer said it can receive */
	u16 refused_result;	/* why, when the state is REFUSED */

	/* The identifier of the request this side is waiting on. Signalling
	 * identifiers exist to pair a response with its request, and a
	 * response carrying a different one belongs to something else. */
	u8 pending_ident;

	/* Counted rather than silently dropped -- the same reasoning as the
	 * opcode check in `bluetooth.c`, and the same failure it prevents. */
	u64 wrong_ident;
	u64 wrong_channel;
};

void l2cap_channel_init(struct l2cap_channel *c, u16 psm, u16 scid);

/* Builds the Connection Request that starts it. Returns its length. */
u32 l2cap_channel_start(struct l2cap_channel *c, u8 *out, u8 ident);

/* Feeds one inbound signalling command in and writes any reply to `out`.
 * Returns the reply's length, or zero when there is nothing to send. */
u32 l2cap_channel_input(struct l2cap_channel *c, const u8 *sig, u32 len,
			u8 *out, u32 outmax);

bool l2cap_self_test(void);

#endif /* RECON_KERNEL_L2CAP_H */
