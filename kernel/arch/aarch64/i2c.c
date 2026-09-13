/* No I2C and no SPI on the boards this kernel runs on.
 *
 * QEMU's `virt` machine is deliberately minimal: a GIC, a generic timer, virtio
 * over memory-mapped I/O and PCI, and nothing else. There is no SMBus
 * controller because there is no southbridge to put one in.
 *
 * That is the honest state and not a gap in this file. A real ARM board -- a
 * Raspberry Pi, a laptop with an embedded controller -- has both buses, usually
 * memory-mapped at an address the device tree names. The device tree walker
 * already exists; what is missing is a board to test against, and a driver
 * whose registers nobody can reach is a driver nobody can say works.
 */
#include <recon/kernel/i2c.h>
#include <recon/kernel/console.h>

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

void i2c_init(void) { }

unsigned i2c_bus_count(void) { return 0; }
unsigned spi_controller_count(void) { return 0; }

bool i2c_probe(unsigned bus, u8 address) { return false; }

enum i2c_status i2c_read_byte(unsigned bus, u8 address, u8 reg, u8 *out)
{
	return I2C_ERR_NO_CONTROLLER;
}

enum i2c_status i2c_read_word(unsigned bus, u8 address, u8 reg, u16 *out)
{
	return I2C_ERR_NO_CONTROLLER;
}

enum i2c_status i2c_write_byte(unsigned bus, u8 address, u8 reg, u8 value)
{
	return I2C_ERR_NO_CONTROLLER;
}

void i2c_print_summary(void)
{
	kputs("  i2c          : no controller on this board -- virt has no "
	      "southbridge\n");
	kputs("  spi          : 0 controllers, same reason\n");
}

bool i2c_self_test(void)
{
	kputs("  i2c: no controller here, so there is nothing to check\n");
	return true;
}
