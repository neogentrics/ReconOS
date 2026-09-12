/* I2C, and the SMBus controller that is on nearly every PC.
 *
 * SMBus is I2C with the ambiguity removed: the same two wires and the same
 * addressing, plus a protocol layer that says how a transfer is framed. A
 * controller that speaks SMBus speaks to I2C devices, which is why this file
 * is `i2c.c` and the driver inside it is named for the chip.
 *
 * --- The PIIX4, and why it is worth driving ---
 *
 * `8086:7113` at `0:1.3`. It is the ACPI and SMBus function of the PIIX4
 * southbridge, and its descendants -- ICH, PCH -- kept the same register layout
 * for twenty years. A machine that has this has it at an I/O port base recorded
 * in configuration space rather than in a BAR, which is the one unusual thing
 * about it and the reason the base is read from offset 0x90 rather than from
 * `d->bar[0]`.
 *
 * --- Polled, and deliberately ---
 *
 * A transfer on this bus takes tens of microseconds. An interrupt to cover that
 * costs more than it saves, and it would mean this driver needed a vector, a
 * handler and a wait queue before anything in the system had asked it a
 * question. The loop has a bound and reports a timeout as a timeout.
 */
#include "../arch/x86_64/x86_64.h"

#include <recon/kernel/i2c.h>
#include <recon/kernel/pci.h>
#include <recon/kernel/console.h>

/* Registers, from the base in configuration space. */
#define SMB_HSTSTS	0x00
#define SMB_HSTCNT	0x02
#define SMB_HSTCMD	0x03
#define SMB_HSTADD	0x04
#define SMB_HSTDAT0	0x05
#define SMB_HSTDAT1	0x06

/* Status bits. */
#define STS_HOST_BUSY	0x01
#define STS_INTR	0x02	/* the transfer finished */
#define STS_DEV_ERR	0x04	/* nothing answered, or it refused */
#define STS_BUS_ERR	0x08
#define STS_FAILED	0x10
#define STS_ALL		0x1F

/* Control: the protocol is bits 4:2, and bit 6 starts it. */
#define CNT_START	0x40
#define PROTO_QUICK	(0 << 2)
#define PROTO_BYTE	(1 << 2)
#define PROTO_BYTE_DATA	(2 << 2)
#define PROTO_WORD_DATA	(3 << 2)

#define SMB_BASE_REG	0x90	/* where the I/O base lives in config space */
#define SMB_HOSTC	0xD2	/* host configuration; bit 0 enables it */

#define BUSES_MAX	2

struct i2c_bus {
	u16 base;
	const char *name;
};

static struct i2c_bus buses[BUSES_MAX];
static unsigned bus_count;

/* Counted so the summary can say something true about a bus nothing uses. */
static u64 transfers, refusals, timeouts;

const char *i2c_status_name(enum i2c_status s)
{
	switch (s) {
	case I2C_OK:			return "ok";
	case I2C_ERR_NO_CONTROLLER:	return "no controller";
	case I2C_ERR_NO_DEVICE:		return "nothing answered";
	case I2C_ERR_BUS:		return "the bus faulted";
	case I2C_ERR_TIMEOUT:		return "the controller never finished";
	case I2C_ERR_ARG:		return "an address this bus cannot carry";
	}

	return "unknown";
}

unsigned i2c_bus_count(void)
{
	return bus_count;
}

unsigned spi_controller_count(void)
{
	/* Zero, and this is where the number comes from: no SPI controller
	 * appears on either machine this kernel runs on. Not a placeholder --
	 * a driver for hardware that cannot be attached is code nothing has
	 * executed, presented as a feature. */
	return 0;
}

/* Waits for the controller to stop being busy, then clears what it reported.
 *
 * Returns the status it saw. The caller decides what a bit means, because
 * "nothing answered" is a failure for a read and the whole point of a probe. */
static enum i2c_status run(const struct i2c_bus *b, u8 control, u8 *status)
{
	unsigned spin;
	u8 s;

	/* Anything left over from last time, cleared before starting. A status
	 * bit from a previous transfer read as this one's result is a
	 * transfer that reports somebody else's outcome. */
	outb(b->base + SMB_HSTSTS, STS_ALL);

	/* Still busy after the clear means the bus is wedged, and starting a
	 * transfer into that produces a result belonging to neither. */
	if (inb(b->base + SMB_HSTSTS) & STS_HOST_BUSY)
		return I2C_ERR_BUS;

	outb(b->base + SMB_HSTCNT, control | CNT_START);

	for (spin = 0; spin < 500000; spin++) {
		s = inb(b->base + SMB_HSTSTS);

		if (!(s & STS_HOST_BUSY) && (s & (STS_INTR | STS_DEV_ERR |
						  STS_BUS_ERR | STS_FAILED)))
			break;

		__asm__ __volatile__("pause" ::: "memory");
	}

	if (spin == 500000) {
		timeouts++;
		outb(b->base + SMB_HSTSTS, STS_ALL);
		return I2C_ERR_TIMEOUT;
	}

	if (status)
		*status = s;

	outb(b->base + SMB_HSTSTS, STS_ALL);
	transfers++;

	if (s & (STS_BUS_ERR | STS_FAILED))
		return I2C_ERR_BUS;

	if (s & STS_DEV_ERR) {
		refusals++;
		return I2C_ERR_NO_DEVICE;
	}

	return I2C_OK;
}

/* The address byte: seven bits of address, and the low bit says which way the
 * bytes are about to go. */
static u8 addr_byte(u8 address, bool reading)
{
	return (u8)((address << 1) | (reading ? 1 : 0));
}

bool i2c_probe(unsigned bus, u8 address)
{
	const struct i2c_bus *b;

	if (bus >= bus_count || address > 0x7F)
		return false;

	b = &buses[bus];

	outb(b->base + SMB_HSTADD, addr_byte(address, false));

	return run(b, PROTO_QUICK, NULL) == I2C_OK;
}

enum i2c_status i2c_read_byte(unsigned bus, u8 address, u8 reg, u8 *out)
{
	const struct i2c_bus *b;
	enum i2c_status s;

	if (bus >= bus_count)
		return I2C_ERR_NO_CONTROLLER;

	if (address > 0x7F || !out)
		return I2C_ERR_ARG;

	b = &buses[bus];

	outb(b->base + SMB_HSTADD, addr_byte(address, true));
	outb(b->base + SMB_HSTCMD, reg);

	s = run(b, PROTO_BYTE_DATA, NULL);

	if (s != I2C_OK)
		return s;

	*out = inb(b->base + SMB_HSTDAT0);
	return I2C_OK;
}

enum i2c_status i2c_read_word(unsigned bus, u8 address, u8 reg, u16 *out)
{
	const struct i2c_bus *b;
	enum i2c_status s;

	if (bus >= bus_count)
		return I2C_ERR_NO_CONTROLLER;

	if (address > 0x7F || !out)
		return I2C_ERR_ARG;

	b = &buses[bus];

	outb(b->base + SMB_HSTADD, addr_byte(address, true));
	outb(b->base + SMB_HSTCMD, reg);

	s = run(b, PROTO_WORD_DATA, NULL);

	if (s != I2C_OK)
		return s;

	/* Low byte first, which is what SMBus says and the opposite of what a
	 * reader expecting network order would assume. */
	*out = (u16)(inb(b->base + SMB_HSTDAT0) |
		     ((u16)inb(b->base + SMB_HSTDAT1) << 8));

	return I2C_OK;
}

enum i2c_status i2c_write_byte(unsigned bus, u8 address, u8 reg, u8 value)
{
	const struct i2c_bus *b;

	if (bus >= bus_count)
		return I2C_ERR_NO_CONTROLLER;

	if (address > 0x7F)
		return I2C_ERR_ARG;

	b = &buses[bus];

	outb(b->base + SMB_HSTADD, addr_byte(address, false));
	outb(b->base + SMB_HSTCMD, reg);
	outb(b->base + SMB_HSTDAT0, value);

	return run(b, PROTO_BYTE_DATA, NULL);
}

void i2c_init(void)
{
	unsigned i;

	for (i = 0; i < pci_device_count() && bus_count < BUSES_MAX; i++) {
		struct pci_device *d = pci_device_at(i);
		u16 base;

		if (!d || d->vendor != 0x8086)
			continue;

		/* The PIIX4 and the ICH family that followed it. Matched by
		 * device id rather than by class, because this function
		 * reports itself as a bridge -- which is why it is not found
		 * by looking for a serial bus controller. */
		if (d->device != 0x7113 && d->device != 0x2413 &&
		    d->device != 0x24C3 && d->device != 0x2930)
			continue;

		/* The base is in configuration space rather than in a BAR, and
		 * the low bit marks it as I/O. */
		base = (u16)(pci_read32(d, SMB_BASE_REG) & 0xFFF0);

		if (!base)
			continue;

		/* Enabled, if firmware left it off. */
		{
			u8 hostc = pci_read8(d, SMB_HOSTC);

			if (!(hostc & 0x01))
				pci_write16(d, SMB_HOSTC,
					    (u16)(hostc | 0x01));
		}

		buses[bus_count].base = base;
		buses[bus_count].name = "PIIX4 SMBus";
		bus_count++;
	}
}

void i2c_print_summary(void)
{
	unsigned i;

	if (!bus_count) {
		kputs("  i2c          : no controller on this machine\n");
	} else {
		for (i = 0; i < bus_count; i++)
			kprintf("  i2c%u         : %s at I/O 0x%x\n", i,
				buses[i].name, buses[i].base);

		kprintf("  i2c traffic  : %llu transfers, %llu unanswered, "
			"%llu timed out\n",
			(unsigned long long)transfers,
			(unsigned long long)refusals,
			(unsigned long long)timeouts);
	}

	/* Said every boot, because a reader who does not see SPI mentioned
	 * cannot tell "none here" from "nobody looked". */
	kprintf("  spi          : %u controllers -- none on either machine "
		"this kernel runs on\n", spi_controller_count());
}

/* --- the self-test --------------------------------------------------------
 *
 * There is nothing attached to this bus, and that is the interesting case.
 *
 * A bus with no devices on it is what almost every QEMU machine presents, and
 * it is also what a real laptop looks like at the addresses nothing happens to
 * use. So the test cannot assert that a probe succeeds. What it can assert --
 * and what actually distinguishes a working controller from a dead one -- is
 * that the controller **completes a transfer and says nobody answered**, which
 * is a different outcome from hanging, from timing out, and from claiming
 * success.
 *
 * Every address is probed for that reason. A controller that reported success
 * everywhere would be one whose status bits are not being read, and a hundred
 * and twenty-eight devices on a bus with nothing plugged into it is a result
 * obviously wrong enough to notice.
 */
bool i2c_self_test(void)
{
	unsigned found = 0;
	unsigned addr;
	u64 before;

	if (!bus_count) {
		kputs("  i2c: no controller here, so there is nothing to "
		      "check\n");
		return true;
	}

	before = transfers + timeouts;

	for (addr = 0x08; addr <= 0x77; addr++)
		if (i2c_probe(0, (u8)addr))
			found++;

	/* The controller did something. A probe loop that completed without a
	 * single transfer being counted is a loop that returned early. */
	if (transfers + timeouts == before) {
		kputs("  i2c: the probe loop ran and no transfer reached the "
		      "controller\n");
		return false;
	}

	if (timeouts) {
		kprintf("  i2c: %llu probes never finished -- the controller "
			"is not answering\n",
			(unsigned long long)timeouts);
		return false;
	}

	/* Everything answering is the failure that looks like success. */
	if (found > 16) {
		kprintf("  i2c: %u devices answered on a bus with nothing "
			"attached, so the status bits are not being read\n",
			found);
		return false;
	}

	if (found)
		kprintf("  i2c: %u device%s answered\n", found,
			found == 1 ? "" : "s");

	return true;
}
