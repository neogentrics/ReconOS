/* xHCI: the USB host controller, and the bus underneath a USB stick.
 *
 * --- Why xHCI only ----------------------------------------------------------
 *
 * There are four generations of USB host controller and this driver implements
 * one. UHCI and OHCI are USB 1, EHCI is USB 2, xHCI is USB 3 and *also* speaks
 * to USB 1 and 2 devices through the same register set -- which is the whole
 * reason it exists and the reason this is the only one worth writing.
 *
 * Every machine this project targets has xHCI. A machine old enough to have
 * only EHCI is old enough that its USB ports are not how somebody installs an
 * operating system in 2026, and a second controller driver is a second thing to
 * keep correct for a case nobody is in.
 *
 * --- What this layer is, and what it is not ---------------------------------
 *
 * This file gets the controller running and finds out what is plugged into it.
 * It does not know what a disk is. Mass storage is a *class* on top of USB, and
 * it lives in usb_storage.c -- the same split as block.c and nvme.c, and for
 * the same reason: the bus and the thing on the end of it fail differently and
 * should be read separately.
 *
 * --- The shape of the hardware ----------------------------------------------
 *
 * Four register sets, at four offsets that the controller itself tells you:
 *
 *   capability   at BAR0. Fixed. Says where the other three are.
 *   operational  at BAR0 + CAPLENGTH. Reset, run, and the ports.
 *   runtime      at BAR0 + RTSOFF. Interrupters and the event ring.
 *   doorbell     at BAR0 + DBOFF. One per slot; writing rings it.
 *
 * And three rings, all of them arrays of 16-byte Transfer Request Blocks:
 *
 *   command ring   driver -> controller, one command at a time
 *   event ring     controller -> driver, everything that ever happens
 *   transfer ring  one per endpoint, the actual data
 *
 * The event ring is the one with the trick in it: the driver does not own the
 * write pointer, the controller does, and the driver follows it by watching a
 * cycle bit that flips each time the ring wraps. Getting that wrong produces a
 * driver that reads the same completion for ever, or one that never sees any.
 */
#include <recon/kernel/block.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/time.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/xhci.h>
#include <recon/kernel/work.h>
#include <recon/kernel/timer.h>

/* --- capability registers -------------------------------------------------- */

#define XHCI_CAPLENGTH		0x00	/* u8, and HCIVERSION at 0x02 */
#define XHCI_HCSPARAMS1		0x04
#define XHCI_HCSPARAMS2		0x08
#define XHCI_HCCPARAMS1		0x10
#define XHCI_DBOFF		0x14
#define XHCI_RTSOFF		0x18

/* --- operational registers, relative to BAR0 + CAPLENGTH ------------------- */

#define XHCI_USBCMD		0x00
#define XHCI_USBSTS		0x04
#define XHCI_PAGESIZE		0x08
#define XHCI_CRCR		0x18
#define XHCI_DCBAAP		0x30
#define XHCI_CONFIG		0x38
#define XHCI_PORTSC(p)		(0x400 + ((p) - 1) * 0x10)

#define USBCMD_RS		(1u << 0)
#define USBCMD_HCRST		(1u << 1)
#define USBCMD_INTE		(1u << 2)

#define USBSTS_HCH		(1u << 0)	/* halted */
#define USBSTS_CNR		(1u << 11)	/* controller not ready */

#define PORTSC_CCS		(1u << 0)	/* something is connected */
#define PORTSC_PED		(1u << 1)	/* port enabled */
#define PORTSC_PR		(1u << 4)	/* port reset */
#define PORTSC_PP		(1u << 9)	/* port power */
#define PORTSC_CSC		(1u << 17)	/* connect status changed */
#define PORTSC_PRC		(1u << 21)	/* port reset changed */

/* Bits that are cleared by writing a one. Preserving them on a read-modify-
 * write clears them by accident, which loses the very change the driver is
 * waiting for -- so every write masks them out unless it means to clear one. */
#define PORTSC_RW1CS		(PORTSC_CSC | PORTSC_PRC | (1u << 18) | \
				 (1u << 19) | (1u << 20) | (1u << 22))

/* --- runtime registers, relative to BAR0 + RTSOFF -------------------------- */

#define XHCI_IMAN(i)		(0x20 + (i) * 32 + 0x00)
#define XHCI_IMOD(i)		(0x20 + (i) * 32 + 0x04)
#define XHCI_ERSTSZ(i)		(0x20 + (i) * 32 + 0x08)
#define XHCI_ERSTBA(i)		(0x20 + (i) * 32 + 0x10)
#define XHCI_ERDP(i)		(0x20 + (i) * 32 + 0x18)

#define XHCI_MAX		2
#define RING_TRBS		64	/* one page holds 256; 64 is plenty */

static struct xhci controllers[XHCI_MAX];
static unsigned controller_count;

/* Transfer completions that belonged to another endpoint. Not an error and
 * not rare: an abandoned transfer completes eventually. Counted because a
 * number here climbing fast means something is timing out that should not.
 */
static u64 foreign_events;

/* --- register access -------------------------------------------------------
 *
 * Every one of these is a device register, so every access is volatile and none
 * of them may be reordered or merged. A compiler that caches a status register
 * in a temporary produces a driver that waits for a bit that has already
 * changed. */

static u32 cap32(struct xhci *x, u32 off)
{
	return *(volatile u32 *)((volatile u8 *)x->cap + off);
}

static u32 op32(struct xhci *x, u32 off)
{
	return *(volatile u32 *)((volatile u8 *)x->op + off);
}

static void op32_set(struct xhci *x, u32 off, u32 v)
{
	*(volatile u32 *)((volatile u8 *)x->op + off) = v;
}

static void op64_set(struct xhci *x, u32 off, u64 v)
{
	/* Two 32-bit writes rather than one 64-bit, low half first.
	 *
	 * The specification allows a controller to implement these as a pair of
	 * 32-bit registers, and one that does will latch on the write to the
	 * high half. A single 64-bit store is not guaranteed to arrive as one
	 * transaction across every bus this might sit behind. */
	volatile u32 *p = (volatile u32 *)((volatile u8 *)x->op + off);

	p[0] = (u32)v;
	p[1] = (u32)(v >> 32);
}

static void rt32_set(struct xhci *x, u32 off, u32 v)
{
	*(volatile u32 *)((volatile u8 *)x->rt + off) = v;
}

static void rt64_set(struct xhci *x, u32 off, u64 v)
{
	volatile u32 *p = (volatile u32 *)((volatile u8 *)x->rt + off);

	p[0] = (u32)v;
	p[1] = (u32)(v >> 32);
}

/* --- bringing it up -------------------------------------------------------- */

/* A wait measured in milliseconds, against the monotonic clock rather than a
 * spin count. A loop counted in iterations means something different on every
 * machine, and this hardware has real timing requirements: a port needs 20ms of
 * power before it is worth talking to, and getting that from a counter tuned on
 * one processor is how a driver works in QEMU and not on a laptop. */
static void busy_ms(unsigned ms)
{
	u64 deadline = time_monotonic_ns() + (u64)ms * 1000000ULL;

	while (time_monotonic_ns() < deadline)
		sched_yield();
}

/* Waits for a bit to clear, in milliseconds, and says so rather than spinning
 * for ever. A controller that never becomes ready is a fact to report, not a
 * machine to hang. */
static bool wait_clear(struct xhci *x, u32 off, u32 mask, unsigned ms)
{
	u64 deadline = time_monotonic_ns() + (u64)ms * 1000000ULL;

	while (op32(x, off) & mask)
		if (time_monotonic_ns() > deadline)
			return false;

	return true;
}

static bool reset_controller(struct xhci *x)
{
	/* Stop first. Resetting a running controller is undefined, and the one
	 * thing worse than a controller that will not start is one that is
	 * half-way through something nobody asked for. */
	op32_set(x, XHCI_USBCMD, op32(x, XHCI_USBCMD) & ~USBCMD_RS);

	{
		u64 deadline = time_monotonic_ns() + 100000000ULL;

		while (!(op32(x, XHCI_USBSTS) & USBSTS_HCH))
			if (time_monotonic_ns() > deadline) {
				kputs("xhci: the controller would not halt\n");
				return false;
			}
	}

	op32_set(x, XHCI_USBCMD, op32(x, XHCI_USBCMD) | USBCMD_HCRST);

	/* Two waits, not one. HCRST clearing says the reset finished; CNR
	 * clearing says the controller is willing to be talked to. Writing
	 * registers between those two is writing to a device that has said it
	 * is not ready. */
	if (!wait_clear(x, XHCI_USBCMD, USBCMD_HCRST, 1000)) {
		kputs("xhci: the reset did not complete\n");
		return false;
	}

	if (!wait_clear(x, XHCI_USBSTS, USBSTS_CNR, 1000)) {
		kputs("xhci: the controller stayed not-ready after reset\n");
		return false;
	}

	return true;
}

static bool alloc_structures(struct xhci *x)
{
	u32 hcsparams2 = cap32(x, XHCI_HCSPARAMS2);
	u32 scratch = ((hcsparams2 >> 21) & 0x1F) |
		      (((hcsparams2 >> 27) & 0x1F) << 5);
	unsigned i;

	/* One page each for the device context array, the command ring, the
	 * event ring and its table. All of them have to be physically
	 * contiguous and none of them is large, so a page apiece is simpler
	 * than an allocator and wastes a few kilobytes once. */
	x->backing = pmm_alloc_pages(4 + scratch);
	if (!x->backing)
		return false;

	x->backing_pages = 4 + scratch;
	kmemset(phys_to_virt(x->backing), 0, (4 + scratch) * PAGE_SIZE);

	x->dcbaa_phys = x->backing;
	x->dcbaa      = phys_to_virt(x->dcbaa_phys);

	x->cmd_phys = x->backing + PAGE_SIZE;
	x->cmd      = phys_to_virt(x->cmd_phys);

	x->event_phys = x->backing + 2 * PAGE_SIZE;
	x->event      = phys_to_virt(x->event_phys);

	x->erst_phys = x->backing + 3 * PAGE_SIZE;
	x->erst      = phys_to_virt(x->erst_phys);

	/* Scratchpad buffers: memory the *controller* uses for itself, which it
	 * asks for by number and reaches through an array at slot 0 of the
	 * device context array. A controller that asks for them and is not
	 * given them does not report an error; it misbehaves later. */
	if (scratch) {
		u64 *array = phys_to_virt(x->backing + 4 * PAGE_SIZE);

		for (i = 0; i < scratch; i++)
			array[i] = x->backing + (4 + i) * PAGE_SIZE;

		/* The array itself lives in the first scratchpad page, and slot
		 * zero of the DCBAA points at it. */
		x->dcbaa[0] = x->backing + 4 * PAGE_SIZE;
	}

	x->scratchpads = scratch;
	return true;
}

static void start_controller(struct xhci *x)
{
	u32 hcsparams1 = cap32(x, XHCI_HCSPARAMS1);
	u32 max_slots = hcsparams1 & 0xFF;

	if (max_slots > XHCI_MAX_SLOTS)
		max_slots = XHCI_MAX_SLOTS;

	/* Context size, before anything builds a context. HCCPARAMS1 bit 2 is
	 * the only place this is stated, and every context field's offset
	 * depends on it. */
	x->context_64 = (cap32(x, XHCI_HCCPARAMS1) & (1u << 2)) != 0;

	x->max_slots = max_slots;
	x->max_ports = (hcsparams1 >> 24) & 0xFF;

	op32_set(x, XHCI_CONFIG, max_slots);
	op64_set(x, XHCI_DCBAAP, x->dcbaa_phys);

	/* The command ring, with the cycle bit set: the controller and the
	 * driver agree that entries matching this bit are new, and the bit
	 * flips each time either of them wraps. */
	x->cmd_cycle = 1;
	x->cmd_index = 0;
	op64_set(x, XHCI_CRCR, x->cmd_phys | 1);

	/* One event ring segment, and the table that names it. */
	{
		volatile u64 *erst = (volatile u64 *)x->erst;

		erst[0] = x->event_phys;
		erst[1] = RING_TRBS;		/* size, in TRBs */
	}

	x->event_cycle = 1;
	x->event_index = 0;

	rt32_set(x, XHCI_ERSTSZ(0), 1);
	rt64_set(x, XHCI_ERDP(0), x->event_phys);
	rt64_set(x, XHCI_ERSTBA(0), x->erst_phys);

	/* Interrupts are not enabled. This driver polls the event ring, because
	 * everything it is asked to do happens while somebody is waiting for a
	 * block to arrive -- and an interrupt path that is only ever exercised
	 * during a synchronous read is a second mechanism carrying no extra
	 * information. When something asks for a block *without* waiting, this
	 * is where that changes. */
	op32_set(x, XHCI_USBCMD, op32(x, XHCI_USBCMD) | USBCMD_RS);
}

/* --- the rings -------------------------------------------------------------
 *
 * A Transfer Request Block is sixteen bytes: two words of parameter, one of
 * status, one of control. The bottom bit of the control word is the **cycle
 * bit**, and it is the whole synchronisation mechanism between driver and
 * controller.
 *
 * Both sides walk their ring forwards and flip an expected cycle value each
 * time they wrap. An entry belongs to the reader when its cycle bit matches
 * what the reader expects. That is the entire protocol, and getting it wrong
 * gives a driver that reads one completion for ever or never sees any -- both
 * of which look like the hardware ignoring you.
 */

struct trb {
	u64 parameter;
	u32 status;
	u32 control;
};

#define TRB_TYPE(t)		((u32)(t) << 10)
#define TRB_CYCLE		(1u << 0)
#define TRB_TYPE_OF(c)		(((c) >> 10) & 0x3F)

#define TRB_NORMAL		1
#define TRB_SETUP		2
#define TRB_DATA		3
#define TRB_STATUS		4
#define TRB_LINK		6
#define TRB_ENABLE_SLOT		9
#define TRB_ADDRESS_DEVICE	11
#define TRB_CONFIGURE_ENDPOINT	12
#define TRB_TRANSFER_EVENT	32
#define TRB_COMMAND_COMPLETE	33
#define TRB_PORT_STATUS_CHANGE	34

/* Standard requests and descriptor types, from the USB specification rather
 * than from xHCI: the controller carries these, it does not define them. */
#define USB_GET_DESCRIPTOR	6
#define USB_SET_CONFIGURATION	9

#define USB_DESC_INTERFACE	4
#define USB_DESC_ENDPOINT	5

#define COMP_SUCCESS		1
#define COMP_SHORT_PACKET	13	/* not an error: fewer bytes than asked for */

/* Rings the doorbell for a slot. Slot 0 is the command ring; slots 1 and up
 * are devices, and the target selects an endpoint within them. */
static void doorbell(struct xhci *x, unsigned slot, u32 target)
{
	*(volatile u32 *)((volatile u8 *)x->db + slot * 4) = target;
}

/* Takes one event off the event ring, waiting up to `ms` for one to appear.
 *
 * The controller writes ahead of the driver's index and flips the cycle bit on
 * each wrap; an entry is ours when its cycle matches what we expect. Nothing
 * here removes anything -- the ring is not consumed, only *read past*, and the
 * dequeue pointer written back afterwards is how the controller learns it may
 * overwrite what we have seen.
 */
/* One event if the controller has posted one, without waiting.
 *
 * Split out of next_event so that a caller can drain the ring without
 * committing to a wait -- which is what routing completions to the endpoint
 * they belong to needs, and what lets a poller ask about several devices
 * without blocking on any one of them. */
static bool poll_event(struct xhci *x, struct trb *out)
{
	volatile struct trb *ring = x->event;

	{
		volatile struct trb *e = &ring[x->event_index];
		u32 control = e->control;

		if ((control & TRB_CYCLE) == x->event_cycle) {
			/* Read the rest only after the cycle bit says the
			 * entry is complete. The controller writes the body
			 * first and the cycle bit last, and a read that
			 * overtakes that ordering sees half an event. */
			__atomic_thread_fence(__ATOMIC_ACQUIRE);

			out->parameter = e->parameter;
			out->status    = e->status;
			out->control   = control;

			if (++x->event_index == RING_TRBS) {
				x->event_index = 0;
				x->event_cycle ^= 1;
			}

			rt64_set(x, XHCI_ERDP(0),
				 (x->event_phys + x->event_index *
				  sizeof(struct trb)) | (1ULL << 3));
			return true;
		}
	}

	return false;
}

static bool next_event(struct xhci *x, struct trb *out, unsigned ms)
{
	u64 deadline = time_monotonic_ns() + (u64)ms * 1000000ULL;

	for (;;) {
		if (poll_event(x, out))
			return true;

		if (time_monotonic_ns() > deadline)
			return false;

		sched_yield();
	}
}

/* Puts a completion in the mailbox of the device it belongs to.
 *
 * A transfer event names its slot and its endpoint. Finding the device by slot
 * is what makes the ring shareable at all: without it, every waiter takes the
 * first completion it sees and calls it its own. */
static void route_completion(struct xhci *x, const struct trb *e)
{
	unsigned slot = (e->control >> 24) & 0xFF;
	unsigned code = (e->status >> 24) & 0xFF;
	unsigned i;

	for (i = 0; i < XHCI_MAX_SLOTS; i++) {
		struct usb_device *ud = &x->devices[i];

		/* Zero is what an unused entry holds, so a slot of zero must
		 * not match one. No transfer event should carry it -- but an
		 * event that did would otherwise be filed against every device
		 * that does not exist, and the first one to be plugged in
		 * later would start with somebody else's completion waiting. */
		if (!ud->slot || ud->slot != slot)
			continue;

		/* The residue, not the count: a transfer event reports how many
		 * bytes were *not* moved, so a short read is a success with a
		 * number attached rather than a failure. The length is not
		 * known here, so the residue is kept and the waiter subtracts
		 * it from what it asked for. */
		ud->completion_bytes = e->status & 0xFFFFFF;
		ud->completion_ok = (code == COMP_SUCCESS ||
				     code == COMP_SHORT_PACKET);
		ud->have_completion = true;
		return;
	}

	foreign_events++;
}

/* Puts one command on the command ring and waits for its completion.
 *
 * The last entry of the ring is a Link TRB pointing back at the start, so the
 * ring is a circle the controller walks without ever running off the end. The
 * cycle bit on the link is what tells it the wrap happened. */
static bool command(struct xhci *x, u64 parameter, u32 status, u32 control,
		    struct trb *result)
{
	volatile struct trb *ring = x->cmd;
	volatile struct trb *slot = &ring[x->cmd_index];

	slot->parameter = parameter;
	slot->status    = status;

	/* The cycle bit last, and after a barrier: it is what hands the entry
	 * over, and a controller that sees it before the rest of the entry has
	 * landed executes a command that is half written. */
	__atomic_thread_fence(__ATOMIC_RELEASE);
	slot->control = control | (u32)x->cmd_cycle;

	if (++x->cmd_index == RING_TRBS - 1) {
		volatile struct trb *link = &ring[RING_TRBS - 1];

		link->parameter = x->cmd_phys;
		link->status    = 0;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		link->control   = TRB_TYPE(TRB_LINK) | (1u << 1) |
				  (u32)x->cmd_cycle;

		x->cmd_index = 0;
		x->cmd_cycle ^= 1;
	}

	doorbell(x, 0, 0);

	/* Events that are not this command's completion are discarded rather
	 * than treated as an answer: a port change can arrive at any moment,
	 * and reading one as a command result would be reading a status field
	 * that means something else entirely. */
	for (;;) {
		struct trb e;

		if (!next_event(x, &e, 1000))
			return false;

		if (TRB_TYPE_OF(e.control) == TRB_COMMAND_COMPLETE) {
			if (result)
				*result = e;
			return ((e.status >> 24) & 0xFF) == COMP_SUCCESS;
		}
	}
}

/* --- ports ----------------------------------------------------------------- */

/* Powers a port and resets whatever is on it, which is what makes a device
 * answer to address zero. Returns true when the port ends up enabled. */
static bool reset_port(struct xhci *x, unsigned port)
{
	u32 sc = op32(x, XHCI_PORTSC(port));
	u64 deadline;

	if (!(sc & PORTSC_CCS))
		return false;

	if (!(sc & PORTSC_PP)) {
		op32_set(x, XHCI_PORTSC(port),
			 (sc & ~PORTSC_RW1CS) | PORTSC_PP);
		busy_ms(20);
	}

	/* Clear the connect-change bit before resetting, so that what is read
	 * afterwards is this reset's result and not the plug event's. */
	op32_set(x, XHCI_PORTSC(port),
		 (op32(x, XHCI_PORTSC(port)) & ~PORTSC_RW1CS) | PORTSC_CSC);

	op32_set(x, XHCI_PORTSC(port),
		 (op32(x, XHCI_PORTSC(port)) & ~PORTSC_RW1CS) | PORTSC_PR);

	deadline = time_monotonic_ns() + 500000000ULL;
	for (;;) {
		sc = op32(x, XHCI_PORTSC(port));

		if (sc & PORTSC_PRC)
			break;

		if (time_monotonic_ns() > deadline) {
			kprintf("xhci: port %u did not finish resetting\n",
				port);
			return false;
		}
	}

	/* Acknowledge the reset-complete bit. */
	op32_set(x, XHCI_PORTSC(port),
		 (op32(x, XHCI_PORTSC(port)) & ~PORTSC_RW1CS) | PORTSC_PRC);

	return (op32(x, XHCI_PORTSC(port)) & PORTSC_PED) != 0;
}

/* --- addressing a device ---------------------------------------------------
 *
 * Four structures, and the confusing part is that two of them describe the same
 * device:
 *
 *   device context   what the *controller* knows about the device. The driver
 *                    allocates it and never writes it; the controller fills it
 *                    in and the driver reads it back.
 *   input context    what the driver is *asking for*. Handed to the controller
 *                    with a command, and forgotten afterwards.
 *
 * The input context begins with a control section saying which of the entries
 * after it the controller should look at -- so "configure endpoint 1" is a bit
 * in a mask rather than a separate command per endpoint.
 *
 * Context entries are 32 bytes, or 64 on a controller that says so in
 * HCCPARAMS1. Getting that wrong reads every field at half the right offset,
 * which produces a device context full of plausible nonsense rather than an
 * error.
 */

#define CTX_SIZE(x)	((x)->context_64 ? 64u : 32u)

static u32 *slot_ctx(struct xhci *x, u8 *input)
{
	return (u32 *)(input + CTX_SIZE(x));		/* after the control section */
}

static u32 *ep_ctx(struct xhci *x, u8 *input, unsigned dci)
{
	/* Endpoint contexts are indexed by "device context index": the default
	 * control endpoint is 1, and each numbered endpoint after it has an out
	 * and an in, so endpoint n is 2n or 2n+1.
	 *
	 * The index is into the *device* context. An input context carries the
	 * control section in front of the same entries, so every one of them
	 * sits one place further along. Counting from the input base instead
	 * puts endpoint 0 on top of the slot context, and the controller then
	 * reads a device on whatever port the endpoint's second word happens to
	 * spell -- which it rejects without saying which field it disliked. */
	return (u32 *)(input + CTX_SIZE(x) * (dci + 1));
}

/* Speed is reported by the port, and the default control endpoint's maximum
 * packet size follows from it. Guessing this wrong means the first descriptor
 * read either truncates or overruns. */
/* The starting packet size for endpoint zero, by speed.
 *
 * Taken as a speed rather than as a PORTSC value because a device behind a hub
 * has no PORTSC of its own -- its speed comes from the hub's port status, and
 * the root port it hangs under may be running at a different one entirely. */
static u32 max_packet_for_speed(unsigned speed)
{
	switch (speed & 0x0F) {
	case 1:  return 64;	/* full speed: 8, 16, 32 or 64 -- 64 is legal
				 * to start with and is corrected once the
				 * descriptor has been read */
	case 2:  return 8;	/* low speed */
	case 3:  return 64;	/* high speed */
	case 4:  return 512;	/* super speed */
	default: return 64;
	}
}

static u32 max_packet_for(u32 portsc)
{
	return max_packet_for_speed((portsc >> 10) & 0x0F);
}

/* bMaxPacketSize0 as the device reports it. Below super speed it is the size
 * itself; at super speed it is the exponent, so a device saying 9 means 512 and
 * not nine. Reading it literally gives a packet size no transfer can use and no
 * error to say why. */
static u32 packet_size_from_descriptor(u8 field, u32 portsc)
{
	if (((portsc >> 10) & 0x0F) == 4)
		return field < 16 ? (1u << field) : 512;

	return field;
}

/* --- transfer rings -------------------------------------------------------
 *
 * One per endpoint, each a page, each ending in a Link TRB back to its own
 * start. Everything below hands entries to the controller the same way the
 * command ring does: body first, barrier, then the cycle bit, which is the
 * only thing that says the entry is ready. */

static void ring_init(struct usb_ring *r, paddr_t phys)
{
	r->trb   = phys_to_virt(phys);
	r->phys  = phys;
	r->index = 0;
	r->cycle = 1;

	kmemset((void *)r->trb, 0, PAGE_SIZE);
}

/* Writes one entry and advances, wrapping through the Link TRB. Returns the
 * physical address of the entry written, which is what a transfer event names
 * when it reports on it. */
static paddr_t ring_push(struct usb_ring *r, u64 parameter, u32 status,
			 u32 control)
{
	volatile struct trb *ring = r->trb;
	volatile struct trb *slot = &ring[r->index];
	paddr_t at = r->phys + r->index * sizeof(struct trb);

	slot->parameter = parameter;
	slot->status    = status;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	slot->control   = control | (u32)r->cycle;

	if (++r->index == RING_TRBS - 1) {
		volatile struct trb *link = &ring[RING_TRBS - 1];

		link->parameter = r->phys;
		link->status    = 0;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		link->control   = TRB_TYPE(TRB_LINK) | (1u << 1) |
				  (u32)r->cycle;

		r->index = 0;
		r->cycle ^= 1;
	}

	return at;
}

/* Brings one port's device to the point where it will answer control
 * transfers: a slot, a context, an address. */
/* Where a device is, which stops being "a port number" the moment a hub is
 * plugged in.
 *
 * xHCI does not address a device by the chain of hubs it hangs off. It addresses
 * it by the **root port** it is ultimately under, plus a twenty-bit **route
 * string**: five four-bit hop numbers, the first hop in the lowest nibble. A
 * device plugged straight into the machine has a route of zero, which is why
 * everything worked until now without this existing.
 *
 * `parent_slot` and `parent_port` are only consulted for a full or low speed
 * device behind a high speed hub -- that hub has to translate for it, and the
 * controller needs to know which hub and which of its ports. For everything
 * else they are zero and the controller ignores them, which is why they are not
 * conditional here: writing a field the hardware is going to ignore is cheaper
 * than a branch that has to be right.
 */
struct usb_path {
	unsigned root_port;	/* the port on the machine itself */
	u32 route;		/* five nibbles, first hop lowest */
	unsigned speed;		/* as the port reported it */
	unsigned parent_slot;	/* the hub, when one is translating */
	unsigned parent_port;	/* which of its ports */
	unsigned depth;		/* hops from the root, 0 for a direct device */
};

static bool address_device(struct xhci *x, const struct usb_path *path,
			   struct usb_device *ud)
{
	struct trb result;
	unsigned port = path->root_port;
	u32 portsc = op32(x, XHCI_PORTSC(port));
	u8 *input;
	u32 *sc, *ep0;
	paddr_t pages;
	unsigned slot;

	if (!command(x, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &result)) {
		kprintf("  xhci         : port %u would not give up a slot\n",
			port);
		return false;
	}

	slot = (result.control >> 24) & 0xFF;
	if (!slot || slot >= XHCI_MAX_SLOTS)
		return false;

	/* Contexts, one transfer ring per endpoint, and a buffer -- a page
	 * each, contiguous and zeroed. Every context field not set here is
	 * required to be zero, and a controller reading rubbish out of one
	 * reports nothing. */
	pages = pmm_alloc_pages(USB_PAGES);
	if (!pages)
		return false;

	kmemset(phys_to_virt(pages), 0, USB_PAGES * PAGE_SIZE);

	ud->backing      = pages;
	ud->slot         = slot;
	ud->port         = port;
	ud->input_phys   = pages + USB_PAGE_INPUT * PAGE_SIZE;
	ud->device_phys  = pages + USB_PAGE_DEVICE * PAGE_SIZE;
	ud->buffer_phys  = pages + USB_PAGE_BUFFER * PAGE_SIZE;
	ud->buffer       = phys_to_virt(ud->buffer_phys);

	ring_init(&ud->control, pages + USB_PAGE_CONTROL_RING * PAGE_SIZE);
	ring_init(&ud->in,      pages + USB_PAGE_IN_RING * PAGE_SIZE);
	ring_init(&ud->out,     pages + USB_PAGE_OUT_RING * PAGE_SIZE);

	input = phys_to_virt(ud->input_phys);

	/* Add the slot and endpoint zero: bits 0 and 1 of the add mask. */
	((u32 *)input)[1] = 0x3;

	sc = slot_ctx(x, input);
	sc[0] = (1u << 27) |				/* context entries: 1 */
		((path->speed & 0x0F) << 20) |		/* speed */
		(path->route & 0x000FFFFF);		/* route string */
	sc[1] = (u32)port << 16;			/* root hub port */

	/* Only meaningful when a high speed hub is translating for a slower
	 * device below it. Zero otherwise, and ignored by the controller. */
	sc[2] = (u32)(path->parent_slot & 0xFF) |
		((u32)(path->parent_port & 0xFF) << 8);

	ep0 = ep_ctx(x, input, 1);
	ep0[1] = (4u << 3) |				/* control endpoint */
		 (max_packet_for_speed(path->speed) << 16) |
		 (3u << 1);				/* error count */
	((u64 *)ep0)[1] = ud->control.phys | 1;		/* dequeue pointer, cycle */

	x->dcbaa[slot] = ud->device_phys;

	if (!command(x, ud->input_phys, 0,
		     TRB_TYPE(TRB_ADDRESS_DEVICE) | ((u32)slot << 24),
		     &result)) {
		/* The completion code, not just the failure. The controller
		 * says which field it objected to -- 17 is a parameter error,
		 * meaning a context field is wrong, and guessing which of a
		 * dozen candidates that is from silence is exactly what this
		 * number exists to prevent. */
		kprintf("  xhci         : port %u would not take an address "
			"(completion code %u)\n",
			port, (result.status >> 24) & 0xFF);
		pmm_free_pages(pages, USB_PAGES);
		ud->slot = 0;
		return false;
	}

	ud->max_packet = max_packet_for(portsc);
	return true;
}

/* One control transfer: setup, optional data, status. Three TRBs on the
 * endpoint's ring, one doorbell, one transfer event. */
static bool control_transfer(struct xhci *x, struct usb_device *ud,
			     u8 request_type, u8 request, u16 value,
			     u16 index, void *buf, u16 length)
{
	paddr_t buf_phys = buf ? virt_to_phys(buf) : 0;
	struct trb e;

	/* Setup stage. The eight bytes of the request go *in the parameter
	 * field itself* rather than in a buffer somewhere -- an immediate
	 * payload, which is why this TRB looks unlike the others. */
	ring_push(&ud->control,
		  (u64)request_type | ((u64)request << 8) |
		  ((u64)value << 16) | ((u64)index << 32) |
		  ((u64)length << 48),
		  8,
		  TRB_TYPE(TRB_SETUP) | (1u << 6) |	/* immediate data */
		  ((u32)(length ? (request_type & 0x80 ? 3 : 2) : 0) << 16));

	if (length)
		ring_push(&ud->control, buf_phys, length,
			  TRB_TYPE(TRB_DATA) |
			  ((request_type & 0x80) ? (1u << 16) : 0));

	/* Status stage, in the opposite direction to the data, and the one that
	 * asks for an event -- so a completion means the whole transfer
	 * finished rather than that the setup packet was accepted. */
	ring_push(&ud->control, 0, 0,
		  TRB_TYPE(TRB_STATUS) | (1u << 5) |	/* interrupt on complete */
		  ((request_type & 0x80) ? 0 : (1u << 16)));

	doorbell(x, ud->slot, 1);	/* endpoint 0 */

	for (;;) {
		if (!next_event(x, &e, 1000))
			return false;

		if (TRB_TYPE_OF(e.control) == TRB_TRANSFER_EVENT) {
			unsigned code = (e.status >> 24) & 0xFF;

			return code == COMP_SUCCESS || code == COMP_SHORT_PACKET;
		}
	}
}

/* Control transfers, for class drivers that need their own requests. Mass
 * storage needs exactly one -- how many logical units are behind the
 * interface -- and a driver that cannot ask has to guess. */
bool xhci_control_transfer(struct xhci *x, struct usb_device *ud,
			   u8 request_type, u8 request, u16 value, u16 index,
			   void *buf, u16 length)
{
	return control_transfer(x, ud, request_type, request, value, index,
				buf, length);
}

/* The device descriptor: who this is, and how big its control packets are. */
bool xhci_describe_device(struct xhci *x, struct usb_device *ud)
{
	u8 *buf = ud->buffer;
	u32 portsc = op32(x, XHCI_PORTSC(ud->port));

	kmemset(buf, 0, 32);

	if (!control_transfer(x, ud, 0x80, USB_GET_DESCRIPTOR,
			      0x0100 /* device */, 0, buf, 18))
		return false;

	ud->vendor  = (u16)(buf[8] | (buf[9] << 8));
	ud->product = (u16)(buf[10] | (buf[11] << 8));

	/* A maximum packet size read from the device replaces the one guessed
	 * from the port speed. They agree on every device that follows the
	 * specification, and the ones that do not are why it is read. */
	if (buf[7])
		ud->max_packet = packet_size_from_descriptor(buf[7], portsc);

	return true;
}

/* --- configuration --------------------------------------------------------
 *
 * The device descriptor says almost nothing about what a device *is*: a mass
 * storage device reports class 0 there. What it is lives in the interface
 * descriptor inside the configuration, along with the endpoints to talk to it
 * through -- so the configuration has to be read before the device can be used
 * for anything, and it is read twice: once for its length, then in full.
 */

/* Walks the descriptors returned with a configuration, picking out the first
 * interface and its bulk endpoints. Descriptors are a chain of length-tagged
 * records, and a record claiming zero length would walk this forever -- hence
 * the length check, which is not about parsing at all. */
static bool read_interface(struct usb_device *ud, const u8 *buf, unsigned len)
{
	unsigned at = 0;
	bool have_interface = false;

	ud->in_ep = ud->out_ep = 0;

	while (at + 2 <= len) {
		unsigned dlen = buf[at];
		unsigned type = buf[at + 1];

		if (dlen < 2 || at + dlen > len)
			return false;

		if (type == USB_DESC_INTERFACE && dlen >= 9) {
			if (have_interface)
				break;		/* only the first, for now */

			have_interface   = true;
			ud->interface    = buf[at + 2];
			ud->usb_class    = buf[at + 5];
			ud->usb_subclass = buf[at + 6];
			ud->usb_protocol = buf[at + 7];
		} else if (type == USB_DESC_ENDPOINT && dlen >= 7 &&
			   have_interface) {
			u8  address = buf[at + 2];
			u8  attrs   = buf[at + 3];
			u16 packet  = (u16)(buf[at + 4] | (buf[at + 5] << 8));

			if ((attrs & 0x03) == 2) {		/* bulk */
				if (address & 0x80) {
					ud->in_ep     = address;
					ud->in_packet = packet;
					ud->in_is_interrupt = false;
				} else {
					ud->out_ep     = address;
					ud->out_packet = packet;
				}
			} else if ((attrs & 0x03) == 3 &&
				   (address & 0x80) && !ud->in_ep) {
				/* An interrupt IN endpoint, which is how every
				 * keyboard and mouse reports. Taken only when
				 * no bulk IN was found, so a device offering
				 * both -- some card readers do -- still looks
				 * like the storage device it is.
				 *
				 * The interval is the seventh byte and is an
				 * exponent, not a count of milliseconds. */
				ud->in_ep     = address;
				ud->in_packet = packet;
				ud->in_is_interrupt = true;
				ud->in_interval = (dlen >= 7) ? buf[at + 6] : 1;
			}
		}

		at += dlen;
	}

	return have_interface;
}

/* The device context index for an endpoint address: endpoint n is 2n for out
 * and 2n+1 for in, which is why the direction bit cannot simply be dropped. */
static unsigned dci_for(u8 address)
{
	unsigned n = address & 0x0F;

	return n * 2 + ((address & 0x80) ? 1 : 0);
}

/* Tells the controller about the bulk endpoints, so they have rings it will
 * walk. Until this succeeds the only endpoint that exists is the control one,
 * and a bulk transfer queued on a ring the controller has never been given
 * simply never happens -- no error, no event, and no timeout that says why. */
static bool configure_endpoints(struct xhci *x, struct usb_device *ud)
{
	u8 *input = phys_to_virt(ud->input_phys);
	struct trb result;
	u32 add = 1;			/* the slot context is always added */
	unsigned in_dci, out_dci, last;
	u32 *sc, *ep;

	/* An IN endpoint is required; an OUT one is not.
	 *
	 * A disk needs both, because a command goes out and its data comes
	 * back. A keyboard has only the IN one and would have been refused
	 * here -- which is how a device that works perfectly ends up reported
	 * as "would not configure". */
	if (!ud->in_ep)
		return false;

	in_dci  = dci_for(ud->in_ep);
	out_dci = ud->out_ep ? dci_for(ud->out_ep) : 0;
	last    = in_dci > out_dci ? in_dci : out_dci;

	kmemset(input, 0, PAGE_SIZE);

	add |= 1u << in_dci;

	if (out_dci)
		add |= 1u << out_dci;

	((u32 *)input)[1] = add;

	/* The slot context is copied forward from the device context and its
	 * context-entries field raised to cover the new endpoints. Leaving it
	 * at 1 describes a device with only a control endpoint, whatever the
	 * add mask says. */
	sc = slot_ctx(x, input);
	kmemcpy(sc, phys_to_virt(ud->device_phys), CTX_SIZE(x));
	sc[0] = (sc[0] & ~(0x1Fu << 27)) | ((u32)last << 27);

	ep = ep_ctx(x, input, in_dci);

	/* Type 7 is interrupt in, type 6 is bulk in. The only other difference
	 * is the interval, which lives in the top byte of the first word and
	 * says how often the controller should ask -- meaningless for bulk,
	 * where it asks whenever there is room. */
	ep[0] = ud->in_is_interrupt ? ((u32)ud->in_interval << 16) : 0;
	ep[1] = (ud->in_is_interrupt ? (7u << 3) : (6u << 3)) |
		((u32)ud->in_packet << 16) |
		(3u << 1);				/* error count */
	((u64 *)ep)[1] = ud->in.phys | ud->in.cycle;
	ep[4] = ud->in_packet;				/* average TRB length */

	if (out_dci) {
		ep = ep_ctx(x, input, out_dci);
		ep[1] = (2u << 3) |			/* bulk out */
			((u32)ud->out_packet << 16) |
			(3u << 1);
		((u64 *)ep)[1] = ud->out.phys | ud->out.cycle;
		ep[4] = ud->out_packet;
	}

	if (!command(x, ud->input_phys, 0,
		     TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((u32)ud->slot << 24),
		     &result)) {
		kprintf("  xhci         : slot %u would not take its endpoints "
			"(completion code %u)\n",
			ud->slot, (result.status >> 24) & 0xFF);
		return false;
	}

	ud->configured = true;
	return true;
}

/* Reads the configuration and puts the device into it. */
bool xhci_configure_device(struct xhci *x, struct usb_device *ud)
{
	u8 *buf = ud->buffer;
	unsigned total;

	kmemset(buf, 0, 9);

	/* Nine bytes first, because the descriptor's own length field is the
	 * only thing that says how much there is to read. */
	if (!control_transfer(x, ud, 0x80, USB_GET_DESCRIPTOR,
			      0x0200 /* configuration */, 0, buf, 9))
		return false;

	total = (unsigned)(buf[2] | (buf[3] << 8));
	if (total < 9)
		return false;
	if (total > PAGE_SIZE)
		total = PAGE_SIZE;

	kmemset(buf, 0, total);

	if (!control_transfer(x, ud, 0x80, USB_GET_DESCRIPTOR,
			      0x0200, 0, buf, (u16)total))
		return false;

	if (!read_interface(ud, buf, total))
		return false;

	/* SET_CONFIGURATION names the configuration by its value, which is the
	 * fifth byte of the descriptor and is not required to be 1. */
	if (!control_transfer(x, ud, 0x00, USB_SET_CONFIGURATION,
			      buf[5], 0, NULL, 0))
		return false;

	return configure_endpoints(x, ud);
}

/* One bulk transfer, in whichever direction the endpoint faces. The caller
 * owns the buffer and the meaning of what is in it; this moves bytes. */
bool xhci_bulk_transfer(struct xhci *x, struct usb_device *ud, bool in,
			void *buf, u32 length, u32 *transferred)
{
	struct usb_ring *r = in ? &ud->in : &ud->out;
	u8  address = in ? ud->in_ep : ud->out_ep;
	u32 packet  = in ? ud->in_packet : ud->out_packet;
	paddr_t phys = buf ? virt_to_phys(buf) : 0;
	u32 done = 0;
	bool ok;

	if (!ud->configured || !address)
		return false;
	if (length && !phys)
		return false;

	/* One at a time on this controller. See the note on transfer_lock: two
	 * transfers in flight would each take the other's completion off the
	 * shared event ring and report somebody else's byte count. */
	mutex_lock(&x->transfer_lock);

	/* One TRB per run of bytes that does not cross a 64 KiB boundary.
	 *
	 * A TRB names one physically contiguous run, and the specification
	 * bounds that run at the next 64 KiB boundary -- so a longer transfer
	 * is several chained TRBs rather than one. A driver that hands over a
	 * single oversized TRB works for every transfer small enough to fit by
	 * luck, which is every transfer a first test does. */
	do {
		u32 chunk = length - done;
		u64 at    = phys + done;
		u64 room  = 0x10000u - (at & 0xFFFFu);
		u32 left, td_size;

		if (chunk > room)
			chunk = (u32)room;

		left = length - done - chunk;

		/* TD size is how many packets are still to come after this
		 * TRB, capped at 31. Hardware uses it to decide how much to
		 * prefetch; getting it wrong costs speed, not correctness,
		 * which is why it is computed rather than left at zero. */
		td_size = (left && packet) ? left / packet : 0;
		if (td_size > 31)
			td_size = 31;

		ring_push(r, at, chunk | (td_size << 17),
			  TRB_TYPE(TRB_NORMAL) |
			  (left ? (1u << 4)		/* chain to the next */
				: (1u << 5)));		/* or ask for the event */

		done += chunk;
	} while (done < length);

	doorbell(x, ud->slot, dci_for(address));

	{
		u64 deadline = time_monotonic_ns() + 5000ULL * 1000000ULL;

		ok = false;

		for (;;) {
			struct trb ev;

			if (ud->have_completion) {
				ud->have_completion = false;
				ok = ud->completion_ok;

				if (transferred)
					*transferred = length -
						ud->completion_bytes;
				break;
			}

			if (poll_event(x, &ev)) {
				if (TRB_TYPE_OF(ev.control) ==
				    TRB_TRANSFER_EVENT)
					route_completion(x, &ev);
				continue;
			}

			if (time_monotonic_ns() > deadline)
				break;

			/* The lock is dropped across the wait, never held
			 * over it. A mutex held while yielding would stop
			 * every other endpoint on this controller for as long
			 * as one device took to answer -- which for a keyboard
			 * nobody is touching is for ever. */
			mutex_unlock(&x->transfer_lock);
			sched_yield();
			mutex_lock(&x->transfer_lock);
		}
	}

	mutex_unlock(&x->transfer_lock);
	return ok;
}

/* --- queued, and asked about later ---------------------------------------- */

bool xhci_transfer_queue(struct xhci *x, struct usb_device *ud, void *buf,
			 u32 length)
{
	paddr_t phys = buf ? virt_to_phys(buf) : 0;

	if (!ud->configured || !ud->in_ep || (length && !phys))
		return false;

	mutex_lock(&x->transfer_lock);

	ring_push(&ud->in, phys, length, TRB_TYPE(TRB_NORMAL) | (1u << 5));
	doorbell(x, ud->slot, dci_for(ud->in_ep));

	mutex_unlock(&x->transfer_lock);
	return true;
}

bool xhci_transfer_poll(struct xhci *x, struct usb_device *ud, u32 *transferred,
			bool *ok)
{
	struct trb ev;
	bool got = false;

	mutex_lock(&x->transfer_lock);

	/* Drain whatever is there first, so that asking about one device also
	 * delivers every other device its own. A poller that only looked for
	 * its own completion would leave the others' on the ring until it
	 * filled. */
	while (poll_event(x, &ev))
		if (TRB_TYPE_OF(ev.control) == TRB_TRANSFER_EVENT)
			route_completion(x, &ev);

	if (ud->have_completion) {
		ud->have_completion = false;

		if (ok)
			*ok = ud->completion_ok;

		/* The residue is what the event carried; the caller knows what
		 * it asked for and does the subtraction. */
		if (transferred)
			*transferred = ud->completion_bytes;

		got = true;
	}

	mutex_unlock(&x->transfer_lock);
	return got;
}

unsigned xhci_ports_connected(struct xhci *x)
{
	unsigned p, n = 0;

	for (p = 1; p <= x->max_ports; p++)
		if (op32(x, XHCI_PORTSC(p)) & PORTSC_CCS)
			n++;

	return n;
}

/* --- attaching ------------------------------------------------------------- */


/* --- hubs -----------------------------------------------------------------
 *
 * A hub is an ordinary USB device that happens to have ports. It is class 9,
 * it answers a handful of class-specific requests, and everything below it is
 * enumerated exactly the way a device on a root port is -- with a route string
 * saying how to get there.
 *
 * This matters more than it sounds. A keyboard and a mouse on the front of a
 * desktop are usually behind an internal hub; every USB-C dock is a hub; and a
 * machine with one visible socket often has two ports and a hub between them.
 * Until now `ports are enumerated once at boot` also quietly meant *and only
 * what is plugged straight into the machine*.
 *
 * --- What a port has to be told, and in what order ---
 *
 * Power, then wait, then look. A hub port comes up unpowered, and a device that
 * has not been given power does not report itself connected -- so a hub read
 * immediately after being found looks like a hub with nothing in it. The wait
 * is the hub's own bPwrOn2PwrGood, in units of two milliseconds, because a hub
 * that needs a hundred milliseconds and is given twenty reports an empty port
 * for a device that is there.
 */

#define HUB_CLASS		0x09

/* Class requests. The recipient in the top bits is what makes a request go to
 * a *port* rather than to the hub itself. */
#define HUB_REQ_GET_STATUS	0x00
#define HUB_REQ_CLEAR_FEATURE	0x01
#define HUB_REQ_SET_FEATURE	0x03
#define HUB_REQ_GET_DESCRIPTOR	0x06

#define HUB_RT_GET_DESC		0xA0	/* device to host, class, device */
#define HUB_RT_PORT_GET		0xA3	/* device to host, class, other */
#define HUB_RT_PORT_SET		0x23	/* host to device, class, other */

#define PORT_FEAT_RESET		4
#define PORT_FEAT_POWER		8
#define PORT_FEAT_C_CONNECTION	16
#define PORT_FEAT_C_RESET	20

#define PORT_STAT_CONNECTION	0x0001
#define PORT_STAT_ENABLE	0x0002
#define PORT_STAT_LOW_SPEED	0x0200
#define PORT_STAT_HIGH_SPEED	0x0400

/* What the hub says about itself. Only the first seven bytes are fixed; what
 * follows is a per-port bitmap whose length depends on the port count, and
 * nothing here needs it. */
struct hub_descriptor {
	u8  length;
	u8  type;
	u8  ports;
	u16 characteristics;
	u8  power_on_to_good;	/* in 2ms units */
	u8  control_current;
} __attribute__((packed));

static unsigned hubs_found;
static unsigned behind_hubs;

/* The speed a hub port reports, translated into the numbering the slot context
 * uses. A hub reports low and high as flags and full speed as neither, which is
 * three states in two bits and the one place this is easy to get backwards. */
static unsigned speed_from_port_status(u16 status)
{
	if (status & PORT_STAT_LOW_SPEED)
		return 2;	/* low */

	if (status & PORT_STAT_HIGH_SPEED)
		return 3;	/* high */

	return 1;		/* full */
}

/* One hop added to a route string. Nibble `depth`, counting from zero, and
 * ports above 15 are pinned at 15 because that is what the specification says
 * to do rather than a shortcut -- a route nibble is four bits and a hub may
 * have more ports than that. */
static u32 route_with(u32 route, unsigned depth, unsigned port)
{
	if (depth >= 5)
		return route;		/* five hops is the architectural limit */

	if (port > 15)
		port = 15;

	return route | ((u32)port << (depth * 4));
}

static bool hub_port_status(struct xhci *x, struct usb_device *hub,
			    unsigned port, u16 *status, u16 *change)
{
	u8 *buf = hub->buffer;

	if (!xhci_control_transfer(x, hub, HUB_RT_PORT_GET, HUB_REQ_GET_STATUS,
				   0, (u16)port, buf, 4))
		return false;

	if (status)
		*status = (u16)(buf[0] | ((u16)buf[1] << 8));
	if (change)
		*change = (u16)(buf[2] | ((u16)buf[3] << 8));

	return true;
}

static bool hub_port_feature(struct xhci *x, struct usb_device *hub,
			     unsigned port, u16 feature, bool set)
{
	return xhci_control_transfer(x, hub, HUB_RT_PORT_SET,
				     set ? HUB_REQ_SET_FEATURE
					 : HUB_REQ_CLEAR_FEATURE,
				     feature, (u16)port, NULL, 0);
}

/* Resets one port and waits for the hub to say it finished.
 *
 * The reset bit clearing is not the signal -- `C_PORT_RESET` in the change word
 * is. A port read while the reset is still running reports not-enabled, which
 * is indistinguishable from a device that refused to come up. */
static bool hub_reset_port(struct xhci *x, struct usb_device *hub,
			   unsigned port, u16 *status)
{
	unsigned tries;

	if (!hub_port_feature(x, hub, port, PORT_FEAT_RESET, true))
		return false;

	for (tries = 0; tries < 50; tries++) {
		u16 st = 0, ch = 0;

		busy_ms(10);

		if (!hub_port_status(x, hub, port, &st, &ch))
			return false;

		if (ch & (1u << (PORT_FEAT_C_RESET - 16))) {
			hub_port_feature(x, hub, port, PORT_FEAT_C_RESET,
					 false);

			if (status)
				*status = st;

			return (st & PORT_STAT_ENABLE) != 0;
		}
	}

	return false;
}

/* Enumerates everything on one hub.
 *
 * Depth-limited on purpose. Five hops is what the route string can express, and
 * a chain longer than that is not something this refuses politely -- it is
 * something it cannot address at all, so the limit is checked rather than
 * discovered. */
static void enumerate_hub(struct xhci *x, struct usb_device *hub,
			  const struct usb_path *hub_path);

static bool claim_device(struct xhci *x, struct usb_device *ud,
			 const struct usb_path *path, unsigned index);

static void enumerate_hub(struct xhci *x, struct usb_device *hub,
			  const struct usb_path *hub_path)
{
	struct hub_descriptor *hd = hub->buffer;
	unsigned ports, p;
	unsigned settle;

	if (hub_path->depth >= 4) {
		kprintf("  xhci         : a hub at depth %u is deeper than a "
			"route string can name; what is below it is not "
			"reachable\n", hub_path->depth);
		return;
	}

	/* The hub descriptor. Type 0x29 is the USB 2.0 one; a SuperSpeed hub
	 * uses 0x2A and this asks for 0x29 first because that is what the hubs
	 * this can actually meet report. */
	if (!xhci_control_transfer(x, hub, HUB_RT_GET_DESC,
				   HUB_REQ_GET_DESCRIPTOR, 0x2900, 0,
				   hub->buffer, sizeof(*hd))) {
		kputs("  xhci         : a hub would not describe itself\n");
		return;
	}

	ports = hd->ports;
	settle = (unsigned)hd->power_on_to_good * 2;

	if (!ports || ports > 15) {
		kprintf("  xhci         : a hub claims %u ports, which is not "
			"a number of ports\n", ports);
		return;
	}

	hubs_found++;

	kprintf("  usb hub      : %u ports, %u ms to power\n", ports, settle);

	/* Power first, every port, then wait once. Waiting per port would be
	 * correct and would take fifteen times as long for no benefit: the
	 * hub powers them independently and the settle time is the same. */
	for (p = 1; p <= ports; p++)
		hub_port_feature(x, hub, p, PORT_FEAT_POWER, true);

	busy_ms(settle < 20 ? 20 : settle);

	for (p = 1; p <= ports; p++) {
		u16 st = 0, ch = 0;
		struct usb_path path;
		struct usb_device *ud;

		if (!hub_port_status(x, hub, p, &st, &ch))
			continue;

		if (!(st & PORT_STAT_CONNECTION))
			continue;

		/* The connection change is acknowledged whether or not the
		 * device below comes up. A change left set is one the hub goes
		 * on reporting, and on a machine that ever polls this it would
		 * look like a device being plugged in over and over. */
		if (ch & (1u << (PORT_FEAT_C_CONNECTION - 16)))
			hub_port_feature(x, hub, p, PORT_FEAT_C_CONNECTION,
					 false);

		if (!hub_reset_port(x, hub, p, &st)) {
			kprintf("  xhci         : hub port %u has something in "
				"it that would not reset\n", p);
			continue;
		}

		if (x->device_count >= XHCI_MAX_SLOTS)
			return;

		kmemset(&path, 0, sizeof(path));
		path.root_port   = hub_path->root_port;
		path.route       = route_with(hub_path->route, hub_path->depth,
					      p);
		path.speed       = speed_from_port_status(st);
		path.depth       = hub_path->depth + 1;

		/* Only a high speed hub translates, and only for something
		 * slower than itself. Setting these for a device running at
		 * the hub's own speed tells the controller to route through a
		 * translator that is not involved. */
		if (hub_path->speed == 3 && path.speed != 3) {
			path.parent_slot = hub->slot;
			path.parent_port = p;
		}

		ud = &x->devices[x->device_count];
		kmemset(ud, 0, sizeof(*ud));

		if (!address_device(x, &path, ud))
			continue;

		behind_hubs++;

		if (claim_device(x, ud, &path, x->device_count))
			x->device_count++;
	}
}

unsigned xhci_hubs_found(void)
{
	return hubs_found;
}

unsigned xhci_devices_behind_hubs(void)
{
	return behind_hubs;
}

/* Everything done to a device once it has an address, wherever it hangs.
 *
 * Extracted because a device on a hub needs exactly this and nothing else --
 * and because the alternative, a second copy for the hub path, is the
 * arrangement that lets the two drift until one of them stops offering devices
 * to a class driver the other one does.
 *
 * Returns whether the slot should be kept. A device that cannot describe itself
 * is not kept; one that cannot be configured is, because it is still addressed
 * and a driver needing only control transfers can still have it.
 */
static bool claim_device(struct xhci *x, struct usb_device *ud,
			 const struct usb_path *path, unsigned index)
{
	unsigned p = path->root_port;

	if (!xhci_describe_device(x, ud)) {
		kprintf("  xhci         : port %u took an address and would "
			"not describe itself\n", p);
		return false;
	}

	if (!xhci_configure_device(x, ud))
		kprintf("  xhci         : port %u would not configure; "
			"control transfers only\n", p);

	if (path->depth)
		kprintf("  usb%u         : %04x:%04x behind a hub on port %u "
			"(route %05x), class %u.%u protocol %u, %u-byte "
			"packets\n", index, ud->vendor, ud->product, p,
			path->route, ud->usb_class, ud->usb_subclass,
			ud->usb_protocol, ud->max_packet);
	else
		kprintf("  usb%u         : %04x:%04x on port %u, class %u.%u "
			"protocol %u, %u-byte packets\n", index, ud->vendor,
			ud->product, p, ud->usb_class, ud->usb_subclass,
			ud->usb_protocol, ud->max_packet);

	if (ud->configured && ud->in_is_interrupt)
		kprintf("  usb%u         : interrupt in %02x (%u byte), "
			"interval %u\n", index, ud->in_ep, ud->in_packet,
			ud->in_interval);
	else if (ud->configured)
		kprintf("  usb%u         : bulk in %02x (%u byte), bulk out "
			"%02x (%u byte)\n", index, ud->in_ep, ud->in_packet,
			ud->out_ep, ud->out_packet);

	/* Offered to each class driver in turn. Each refuses anything that is
	 * not its own, which is most of what gets plugged in. */
	usb_storage_attach(x, ud);
	usb_hid_attach(x, ud);

	/* And if it is a hub, what is below it.
	 *
	 * After the class drivers, not before: a hub is not going to be claimed
	 * by one, and doing it in this order means the device is completely
	 * set up before anything recurses through it. The slot is counted by
	 * the caller *after* this returns, so a device found below is placed
	 * in the next slot rather than on top of this one -- which is why
	 * enumerate_hub advances device_count itself.
	 */
	if (ud->usb_class == HUB_CLASS) {
		x->device_count = index + 1;
		enumerate_hub(x, ud, path);
		return false;	/* already counted */
	}

	return true;
}

/* One port that has something on it: reset it, address it, and hand it to
 * whatever claims that kind of device.
 *
 * This is the body the boot walk used to hold inline. It is a function now so
 * that a port coming up at boot and a port coming up an hour later run exactly
 * the same code -- two paths that both claim a device are two places for the
 * next change to be made in one of.
 *
 * Returns whether the port ended up enabled, which is what the boot walk
 * counts. A port that enables and then fails to be claimed is still a port with
 * something on it. */
static bool port_arrived(struct xhci *x, unsigned p)
{
	struct usb_device *ud;
	struct usb_path path;

	if (!reset_port(x, p))
		return false;

	if (x->device_count >= XHCI_MAX_SLOTS)
		return true;

	ud = &x->devices[x->device_count];
	kmemset(ud, 0, sizeof(*ud));

	/* A device plugged straight into the machine: route zero, no
	 * translator, depth zero. Everything a hub adds is an addition to this,
	 * which is why the root case needs no special case. */
	kmemset(&path, 0, sizeof(path));
	path.root_port = p;
	path.speed = (op32(x, XHCI_PORTSC(p)) >> 10) & 0x0F;

	if (!address_device(x, &path, ud))
		return true;

	if (!claim_device(x, ud, &path, x->device_count))
		return true;

	x->device_count++;
	return true;
}

/* One port that has stopped reporting a connection.
 *
 * There is no conversation to have with the device -- it is gone, and a control
 * transfer to it would wait out its timeout and fail. So this is bookkeeping
 * only: tell whoever claimed it to give up what it was being used as, and free
 * the slot.
 *
 * The slot is freed by compacting the array, which is safe because nothing
 * outside holds an index into it -- `claim_device` is handed the index and uses
 * it immediately. If that ever stops being true this becomes a use-after-free,
 * which is why it is said here rather than assumed. */
static void port_departed(struct xhci *x, unsigned p)
{
	unsigned i;

	for (i = 0; i < x->device_count; i++) {
		struct usb_device *ud = &x->devices[i];

		if (ud->port != p)
			continue;

		usb_storage_release(ud);

		if (i + 1 < x->device_count)
			x->devices[i] = x->devices[x->device_count - 1];

		x->device_count--;
		kmemset(&x->devices[x->device_count], 0,
			sizeof(x->devices[0]));
		return;
	}
}

static unsigned arrivals;
static unsigned departures;

void xhci_poll(struct xhci *x)
{
	unsigned p;

	if (!x)
		return;

	for (p = 1; p <= x->max_ports && p <= XHCI_MAX_PORTS; p++) {
		u32 sc = op32(x, XHCI_PORTSC(p));
		bool now = (sc & PORTSC_CCS) != 0;

		/* Acknowledge the change bit whether or not the state moved.
		 * Left set, it stays set for ever and tells nobody anything. */
		if (sc & PORTSC_CSC)
			op32_set(x, XHCI_PORTSC(p),
				 (sc & ~PORTSC_RW1CS) | PORTSC_CSC);

		if (now == x->port_present[p])
			continue;

		x->port_present[p] = now;

		if (now) {
			arrivals++;
			port_arrived(x, p);
		} else {
			departures++;
			port_departed(x, p);
		}
	}
}

static struct work usb_work;
static struct timer usb_timer;
static bool hotplug_running;
static unsigned polls;

/* Half a second. Fast enough that plugging something in feels immediate, slow
 * enough that the cost is one register read per port twice a second. */
#define USB_POLL_NS (500ull * 1000000ull)

void usb_poll_all(void)
{
	unsigned i;

	polls++;

	for (i = 0; i < controller_count; i++)
		xhci_poll(&controllers[i]);
}

/* The poll itself, on the worker thread, and the next one armed at the end of
 * it.
 *
 * Arming the next timer *here* rather than in the timer callback is what makes
 * this reliable, and it is worth saying why. The obvious shape -- a callback
 * that schedules work and re-arms itself -- means a poll that takes eight
 * seconds has sixteen firings behind it, each refused by the work queue, each
 * arming another. Nothing else in this kernel re-arms a timer from inside its
 * own callback, so that shape was the first of its kind here and it did not
 * survive its first long claim (KF-199).
 *
 * Done this way there is **at most one timer outstanding at any moment**, it is
 * always armed from thread context, and the interval is measured from the end
 * of one poll to the start of the next. That last one is not a detail: half a
 * second *between polls* is what was wanted, and half a second between their
 * *starts* is what the first version asked for. Those differ exactly when a
 * poll is slow, which is exactly when a device has arrived and there is
 * something to do. */
static void usb_work_fn(void *arg)
{
	(void)arg;

	usb_poll_all();

	if (hotplug_running && !timer_start(&usb_timer, USB_POLL_NS)) {
		/* Said out loud rather than left as a machine that quietly
		 * stops noticing devices. That silence is what KF-199 was. */
		hotplug_running = false;
		kputs("usb: the hot-plug timer would not re-arm; ports will "
		      "not be watched again\n");
	}
}

static void usb_timer_fn(void *arg)
{
	(void)arg;

	/* Nothing but a hand-off. A timer callback runs in interrupt context,
	 * and claiming an arrived device means control transfers that wait. */
	work_schedule(&usb_work);
}

void usb_hotplug_start(void)
{
	if (hotplug_running || !controller_count)
		return;

	work_init(&usb_work, usb_work_fn, NULL);
	timer_init(&usb_timer, usb_timer_fn, NULL);

	hotplug_running = true;

	if (!timer_start(&usb_timer, USB_POLL_NS)) {
		hotplug_running = false;
		kputs("usb: the hot-plug timer would not start; ports are "
		      "read once at boot and not again\n");
	}
}

void usb_print_summary(void)
{
	if (!controller_count)
		return;

	kputs("USB\n");

	if (!hotplug_running) {
		kprintf("  hot-plug     : **not running** -- ports were read "
			"once at boot\n");
		return;
	}

	/* The poll count is here because "nothing has been plugged in" and "the
	 * poll stopped running" are the same silence otherwise. */
	kprintf("  hot-plug     : %u poll%s, %u arrived, %u left\n",
		polls, polls == 1 ? "" : "s",
		arrivals, departures);
}

unsigned usb_arrivals(void)
{
	return arrivals;
}

unsigned usb_departures(void)
{
	return departures;
}

bool xhci_attach(const struct pci_device *d)
{
	struct xhci *x;
	paddr_t base, first;
	u64 span;
	vaddr_t va;
	unsigned p, enabled = 0;

	/* Class 0x0C is a serial bus controller, subclass 3 is USB, and
	 * programming interface 0x30 is xHCI specifically. The third is what
	 * matters: subclass 3 also covers UHCI, OHCI and EHCI, whose registers
	 * are nothing like these. */
	if (d->class_code != 0x0C || d->subclass != 0x03 || d->prog_if != 0x30)
		return false;

	if (controller_count >= XHCI_MAX)
		return false;

	x = &controllers[controller_count];
	kmemset(x, 0, sizeof(*x));

	/* Named so it appears in the lock summary as itself rather than as a
	 * question mark. A zeroed mutex already works; what zero does not give
	 * it is a name. */
	mutex_init(&x->transfer_lock, "xhci");

	base = (paddr_t)d->bar[0];

	/* Said rather than returned silently.
	 *
	 * A controller this driver declines to use is a USB port that does not
	 * work, and "no devices found" is what somebody sees either way. The
	 * reason has to be on the wire, or the difference between "there is no
	 * controller" and "there is one and its registers are somewhere I did
	 * not expect" is invisible. */
	if (!base || d->bar_is_io[0] || !d->bar_size[0]) {
		kprintf("  xhci         : found a controller, but BAR0 is "
			"%s (%llx, %llu bytes)\n",
			d->bar_is_io[0] ? "an I/O port range" : "unset",
			(unsigned long long)base,
			(unsigned long long)d->bar_size[0]);
		return false;
	}

	first = PAGE_ALIGN_DOWN(base);
	span  = PAGE_ALIGN_UP((base - first) + d->bar_size[0]);
	va    = (vaddr_t)(uintptr_t)phys_to_virt(first);

	if (!vm_lookup(va) &&
	    !vm_map(va, first, span,
		    VM_READ | VM_WRITE | VM_DEVICE | VM_GLOBAL)) {
		kputs("xhci: could not map the controller's registers\n");
		return false;
	}

	x->cap = phys_to_virt(base);
	x->op  = (volatile u8 *)x->cap + (cap32(x, XHCI_CAPLENGTH) & 0xFF);
	x->rt  = (volatile u8 *)x->cap + (cap32(x, XHCI_RTSOFF) & ~0x1Fu);
	x->db  = (volatile u8 *)x->cap + (cap32(x, XHCI_DBOFF) & ~0x3u);

	/* Bus mastering, or every ring this driver builds is memory the
	 * controller is not allowed to read. */
	pci_write16(d, 0x04, pci_read16(d, 0x04) | 0x0006);

	if (!reset_controller(x))
		return false;

	if (!alloc_structures(x)) {
		kputs("xhci: not enough memory for the controller's rings\n");
		return false;
	}

	start_controller(x);

	/* Ports are powered and reset here rather than lazily, because a device
	 * that has not been reset does not answer at all, and "nothing is
	 * plugged in" and "something is plugged in and has not been spoken to"
	 * look identical from the register. */
	for (p = 1; p <= x->max_ports; p++) {
		if (port_arrived(x, p))
			enabled++;

		/* Whether or not anything was claimed, remember what the port
		 * looked like -- that is what the poll compares against. */
		if (p <= XHCI_MAX_PORTS)
			x->port_present[p] =
				(op32(x, XHCI_PORTSC(p)) & PORTSC_CCS) != 0;
	}

	x->ports_enabled = enabled;
	controller_count++;

	kprintf("  xhci         : %u slots, %u ports, %u scratchpad page%s, "
		"%u connected, %u addressed\n",
		x->max_slots, x->max_ports, x->scratchpads,
		x->scratchpads == 1 ? "" : "s",
		enabled, x->device_count);

	return true;
}

unsigned xhci_controller_count(void)
{
	return controller_count;
}

struct xhci *xhci_at(unsigned index)
{
	return index < controller_count ? &controllers[index] : 0;
}
