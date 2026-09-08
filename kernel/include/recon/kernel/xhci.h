/* The USB host controller.
 *
 * Checkpoint 11b, and the bus a USB stick sits on. Split from the class driver
 * above it the same way block.c is split from nvme.c: this file is about
 * getting a controller running and finding out what is plugged into it, and it
 * does not know that a disk exists.
 *
 * xHCI only. UHCI, OHCI and EHCI are the three older generations, and xHCI
 * speaks to USB 1 and 2 devices through the same register set -- which is why
 * it is the only one worth implementing, and why a machine old enough to need
 * one of the others is not a machine somebody installs this from.
 */
#ifndef RECON_KERNEL_XHCI_H
#define RECON_KERNEL_XHCI_H

#include <recon/kernel/types.h>

struct pci_device;

/* Slots the controller may have, capped by this driver. A controller that
 * offers more is used at this many rather than refused: nothing here needs 256
 * devices, and the array is allocated for whatever it says. */
#define XHCI_MAX_SLOTS 16

/* One device on the bus, once it has been given a slot and an address.
 *
 * "Addressed" is a state, not a property: the controller holds a context for
 * it, the driver holds a transfer ring for its control endpoint, and both are
 * meaningless if the device is unplugged. Nothing here survives that, which is
 * why the port is remembered -- it is the only part that is about the machine
 * rather than about the device. */
struct usb_ring {
	volatile void *trb;
	paddr_t phys;
	unsigned index;
	unsigned cycle;		/* flipped on every wrap, never reset */
};

/* Pages behind one device, in the order they are allocated. The transfer rings
 * fill a page each and the buffer needs one of its own: a ring that shares a
 * page with anything is a ring that overwrites it on the wrap, and the wrap is
 * exactly the case no short test reaches. */
#define USB_PAGE_INPUT		0
#define USB_PAGE_DEVICE		1
#define USB_PAGE_CONTROL_RING	2
#define USB_PAGE_IN_RING	3
#define USB_PAGE_OUT_RING	4
#define USB_PAGE_BUFFER		5
#define USB_PAGES		6

struct usb_device {
	unsigned slot;		/* zero means this entry is unused */
	unsigned port;

	paddr_t backing;	/* USB_PAGES pages, contiguous */
	paddr_t input_phys;
	paddr_t device_phys;

	struct usb_ring control;	/* endpoint 0, from Address Device on */
	struct usb_ring in;		/* bulk in, once configured */
	struct usb_ring out;		/* bulk out */

	void   *buffer;		/* descriptors and command blocks */
	paddr_t buffer_phys;

	u32 max_packet;
	u16 vendor;
	u16 product;

	/* Class, subclass and protocol come from the *interface*, not from the
	 * device descriptor: a mass storage device reports class 0 there and
	 * defers the answer to the configuration it offers. */
	u8  usb_class;
	u8  usb_subclass;
	u8  usb_protocol;
	u8  interface;

	u8  in_ep, out_ep;	/* endpoint addresses; zero means not found */
	u16 in_packet, out_packet;
	bool configured;
};

struct xhci {
	volatile void *cap;	/* capability registers; the rest are found through these */
	volatile void *op;	/* operational */
	volatile void *rt;	/* runtime */
	volatile void *db;	/* doorbells */

	paddr_t backing;
	unsigned backing_pages;

	volatile u64 *dcbaa;	/* device context base address array */
	paddr_t dcbaa_phys;

	volatile void *cmd;	/* command ring: driver to controller */
	paddr_t cmd_phys;
	unsigned cmd_index;
	unsigned cmd_cycle;

	volatile void *event;	/* event ring: controller to driver */
	paddr_t event_phys;
	unsigned event_index;
	unsigned event_cycle;

	volatile void *erst;	/* the table naming the event ring segment */
	paddr_t erst_phys;

	unsigned max_slots;
	unsigned max_ports;
	unsigned scratchpads;
	unsigned ports_enabled;

	/* Whether context entries are 64 bytes rather than 32, from
	 * HCCPARAMS1. Reading this wrong puts every field at half its offset
	 * and produces a device context full of plausible nonsense. */
	bool context_64;

	struct usb_device devices[XHCI_MAX_SLOTS];
	unsigned device_count;
};

/* Claims a PCI device if it is an xHCI controller, brings it up, and powers and
 * resets its ports. Returns false for anything else, including the older USB
 * controller generations, which share a subclass and share nothing else. */
bool xhci_attach(const struct pci_device *d);

unsigned xhci_controller_count(void);
struct xhci *xhci_at(unsigned index);

/* How many ports currently have something on them. Asked of the hardware each
 * time rather than remembered: a stick can be pulled out while the answer is
 * being used. */
unsigned xhci_ports_connected(struct xhci *x);

/* Reads a device's descriptor over its control endpoint, filling in the vendor,
 * product and class. The first thing that proves the command ring, the event
 * ring and the cycle bits are all right together -- everything after it is the
 * same machinery pointed somewhere else. */
bool xhci_describe_device(struct xhci *x, struct usb_device *ud);

/* Reads the device's configuration, chooses its first interface, and hands the
 * controller rings for that interface's bulk endpoints. A device that has not
 * been through this has exactly one endpoint the controller knows about -- the
 * control one -- and bulk transfers on it go nowhere silently. */
bool xhci_configure_device(struct xhci *x, struct usb_device *ud);

/* Moves bytes over a configured bulk endpoint. `transferred` is filled in with
 * how many actually moved, which is not always how many were asked for: a
 * device is allowed to answer with fewer, and that is a result rather than a
 * failure. */
bool xhci_bulk_transfer(struct xhci *x, struct usb_device *ud, bool in,
			void *buf, u32 length, u32 *transferred);

/* A control transfer on endpoint zero, for the requests a class defines for
 * itself. `request_type` carries the direction in its top bit, as the USB
 * specification lays it out. */
bool xhci_control_transfer(struct xhci *x, struct usb_device *ud,
			   u8 request_type, u8 request, u16 value, u16 index,
			   void *buf, u16 length);

/* Claims a device if it is SCSI mass storage over Bulk-Only Transport, and
 * registers it as a block device. Returns false for anything else, which is
 * not an error -- most of what is plugged into a machine is not a disk. */
bool usb_storage_attach(struct xhci *x, struct usb_device *ud);
unsigned usb_storage_count(void);

#endif /* RECON_KERNEL_XHCI_H */
