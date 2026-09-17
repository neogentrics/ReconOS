/* Bluetooth: the Host Controller Interface, carried over USB.
 *
 * HCI is the boundary every Bluetooth controller presents. Above it is this
 * kernel; below it is a radio nothing here models. Four kinds of thing cross
 * it -- commands down, events up, and ACL data both ways -- and USB carries
 * them on three endpoints plus the control one.
 *
 * What is in this header is the part that is about **HCI** rather than about
 * USB: how a command is framed, how an event is put back together, and how a
 * command's answer is found inside one. All of it is pure, all of it is
 * self-tested, and none of it changes when the transport underneath it does.
 */
#ifndef RECON_KERNEL_BLUETOOTH_H
#define RECON_KERNEL_BLUETOOTH_H

#include <recon/kernel/types.h>

struct xhci;
struct usb_device;

/* What a Bluetooth controller says it is, in its interface descriptor.
 *
 * Class 224 is "wireless", which is why the subclass and protocol are both
 * required: 224.1.1 is a Bluetooth primary controller specifically, and 224
 * alone is a family that includes things this driver must not claim. */
#define BT_USB_CLASS		0xE0
#define BT_USB_SUBCLASS		0x01
#define BT_USB_PROTOCOL		0x01

/* --- commands and events -------------------------------------------------- */

/* An opcode is two fields in one 16-bit number: a six-bit group at the top and
 * a ten-bit command within it. Written as a macro rather than as a table of
 * constants because the split is the definition -- a reader who sees 0x0C03
 * learns nothing, and one who sees HCI_OPCODE(0x03, 0x0003) learns that it is
 * the third command of the baseband group. */
#define HCI_OPCODE(ogf, ocf)	((u16)((((u16)(ogf)) << 10) | (u16)(ocf)))

#define HCI_OGF_BASEBAND	0x03
#define HCI_OGF_INFORMATIONAL	0x04

#define HCI_OP_RESET		HCI_OPCODE(HCI_OGF_BASEBAND, 0x0003)
#define HCI_OP_READ_BD_ADDR	HCI_OPCODE(HCI_OGF_INFORMATIONAL, 0x0009)

/* A command on the wire is three bytes of header and then its parameters, so
 * the longest one is 3 + 255. */
#define HCI_COMMAND_HEADER	3
#define HCI_COMMAND_MAX		(HCI_COMMAND_HEADER + 255)

/* An event is two: a code and a parameter length. */
#define HCI_EVENT_HEADER	2
#define HCI_EVENT_MAX		(HCI_EVENT_HEADER + 255)

#define HCI_EV_COMMAND_COMPLETE	0x0E
#define HCI_EV_COMMAND_STATUS	0x0F

/* --- ACL data -------------------------------------------------------------- */

#define HCI_ACL_HEADER		4

/* The first two bytes of an ACL header are not a number. They are a twelve-bit
 * connection handle with two two-bit flags packed above it, and reading them
 * as a sixteen-bit handle gives a value that is wrong in a way that still
 * looks like a handle. */
struct hci_acl_header {
	u16 handle;	/* twelve bits */
	u8  pb;		/* packet boundary: start of a message, or more of one */
	u8  bc;		/* broadcast */
	u16 length;
};

/* --- putting an event back together ---------------------------------------
 *
 * **An event does not arrive in one piece, and this is the part that is easy
 * to get wrong in a way that works.** The interrupt endpoint a controller
 * offers for events has a maximum packet size of sixteen bytes -- measured,
 * not assumed: it is `07 05 81 03 10 00 01` in the MediaTek adapter's
 * descriptor, and the `10 00` is the sixteen. An event may carry 255 bytes of
 * parameters, so a long one arrives as seventeen transfers.
 *
 * The obvious rule -- *an event ends when a packet arrives shorter than the
 * maximum* -- is wrong, and wrong in the worst way: it is right for almost
 * every event. It fails only when the total length is an exact multiple of
 * sixteen, where the last packet is full-size and the reader waits forever for
 * one that never comes. A fourteen-byte parameter block does that, and
 * fourteen-byte parameter blocks are ordinary.
 *
 * So this reassembles **by the length the event declares**, which needs no
 * assumption about how the transport chops it up.
 */
struct hci_event_reassembly {
	u8  buf[HCI_EVENT_MAX];
	u32 have;	/* bytes accumulated so far */
	u32 want;	/* total once the header has arrived; zero before that */

	u64 completed;
	u64 overruns;	/* more bytes than the event said it had, or than fit */
};

/* Frames a command into `out`, which must hold HCI_COMMAND_MAX. Returns how
 * many bytes it wrote. */
u32 hci_command_build(u8 *out, u16 opcode, const u8 *params, u8 plen);

void hci_reassembly_reset(struct hci_event_reassembly *r);

/* Feeds one transport packet in. Returns true when `r->buf` holds one complete
 * event, `r->want` bytes long; the caller reads it before the next feed, which
 * clears it. */
bool hci_event_feed(struct hci_event_reassembly *r, const u8 *pkt, u32 len);

/* Finds which command an event is answering, and what it answered.
 *
 * Command Complete and Command Status both carry an opcode and a status and
 * **put them in a different order**, so one read with the other's layout
 * produces a plausible wrong opcode rather than an error. Both are handled
 * here so that no caller has to know which is which. False for any other
 * event, which is not a failure -- most events answer nothing. */
bool hci_event_command_result(const u8 *ev, u32 len, u16 *opcode, u8 *status);

bool hci_acl_parse(const u8 *hdr, u32 len, struct hci_acl_header *out);

/* --- the driver ----------------------------------------------------------- */

/* --- the transport under HCI ----------------------------------------------
 *
 * Four ways in and out, which is exactly the four USB offers: commands out on
 * the control pipe, events in on the interrupt pipe, ACL data both ways on the
 * bulk pipes.
 *
 * **Named as an interface so that the thing underneath can be a test.**
 * KF-248 is designed and not built, so no real transport exists yet -- and the
 * kernel session's advice was to write the layers above it and exercise them
 * against a fake one rather than wait. Everything below this line can be
 * driven by a scripted controller that lives in an array.
 *
 * When the real one lands it is one implementation of this struct, and their
 * note says the swap is one call site per direction. Nothing above here has to
 * know which it is talking to, which is the point: if a byte count comes back
 * wrong against real hardware and right against the fake, the fault is in the
 * transport and not in any of this.
 *
 * Polled rather than blocking, because a test has no scheduler to yield to.
 * The real implementation yields inside `poll_*` and the callers above cannot
 * tell.
 */
struct bt_transport {
	/* A command, as a class request on the interface. False means the
	 * transport refused it -- not that the controller did. */
	bool (*send_command)(void *ctx, const u8 *pdu, u32 len);

	/* One transport packet of an event, if one has arrived. Returns its
	 * length, or zero when nothing is waiting. Not one *event* -- an event
	 * longer than the endpoint's packet size arrives in pieces, and
	 * putting them together is `hci_event_feed`'s job. */
	u32 (*poll_event)(void *ctx, u8 *buf, u32 max);

	bool (*send_acl)(void *ctx, const u8 *pdu, u32 len);
	u32 (*poll_acl)(void *ctx, u8 *buf, u32 max);

	void *ctx;
};

/* One controller, and what has happened on it. */
struct bt_hci {
	const struct bt_transport *t;
	struct hci_event_reassembly rx;

	u64 commands_sent;
	u64 events_seen;

	/* Counted apart on purpose. An event that answers *no* command is
	 * ordinary -- a controller reports connections and disconnections
	 * unasked. An event that answers a command nobody sent is not, and
	 * reading one as the answer to the command actually outstanding is
	 * the mistake this layer exists to not make. */
	u64 unsolicited;
	u64 wrong_opcode;
};

void bt_hci_init(struct bt_hci *h, const struct bt_transport *t);

/* Sends a command and pumps the transport until that command's answer comes
 * back. `budget` bounds the polling so a silent controller ends the call
 * rather than the boot.
 *
 * **The answer is matched by opcode.** An answer carrying a different one is
 * counted and discarded, never returned -- a controller that answers an older
 * command late would otherwise hand this command a status that belongs to
 * something else, which is a success with the wrong number attached. */
bool bt_hci_command(struct bt_hci *h, u16 opcode, const u8 *params, u8 plen,
		    u8 *status, unsigned budget);

/* Claims a device if it is a Bluetooth controller. False for anything else,
 * which is not an error. */
bool bt_hci_attach(struct xhci *x, struct usb_device *ud);

/* How many Bluetooth controllers have been offered to the driver -- **not** how
 * many it took, which is currently always none. Counted this way deliberately:
 * a caller asking "is there Bluetooth hardware on this machine" wants the first
 * number, and the second one is a constant zero until the transport can carry
 * HCI. */
unsigned bt_hci_count(void);
void bt_hci_print_summary(void);
bool bt_hci_self_test(void);

#endif /* RECON_KERNEL_BLUETOOTH_H */
