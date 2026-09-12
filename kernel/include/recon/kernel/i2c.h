/* The two serial buses the blueprint asks for, behind one interface.
 *
 * I2C and SPI are not the same bus, and pretending they are would be wrong.
 * What they share is the shape of what somebody wants from them: *reach a
 * small device that is not on PCI and is not memory-mapped, and move a few
 * bytes to or from a numbered register on it.* Temperature sensors, battery
 * gauges, lid switches, the chip that holds a laptop's serial number -- none of
 * them is worth a bus of its own in the kernel's vocabulary.
 *
 * --- Why these are here at all, honestly ---
 *
 * Nothing in ReconOS needs one yet. They are on the blueprint because they are
 * what a real laptop has, and because a kernel that reaches checkpoint 17 on
 * somebody's actual machine will meet them. The audit has said "nothing needs
 * them yet" for as long as the row has existed, which is true and is not the
 * same as saying the row can stay empty.
 *
 * --- What is built, and what is refused ---
 *
 * **I2C is real.** This machine has a PIIX4 SMBus controller at `0:1.3`, and
 * `i2c_piix4.c` drives it: SMBus is I2C with a protocol layer on top, and the
 * controller is the one nearly every PC chipset has carried since 1996.
 *
 * **SPI is declared and has no driver**, and that is a measurement rather than
 * a shrug. There is no SPI controller on either machine this kernel runs on --
 * QEMU's i440FX and virt boards expose none. Writing a driver for hardware that
 * cannot be attached would be code nothing has ever executed, presented as a
 * built feature. `spi_controller_count()` answers zero and says where that
 * number comes from.
 */
#ifndef RECON_KERNEL_I2C_H
#define RECON_KERNEL_I2C_H

#include <recon/kernel/types.h>

enum i2c_status {
	I2C_OK = 0,
	I2C_ERR_NO_CONTROLLER,	/* there is no bus to talk on */
	I2C_ERR_NO_DEVICE,	/* nothing answered at that address */
	I2C_ERR_BUS,		/* the transfer started and did not finish */
	I2C_ERR_TIMEOUT,	/* the controller never reported done */
	I2C_ERR_ARG,		/* an address or length this bus cannot carry */
};

const char *i2c_status_name(enum i2c_status s);

/* How many I2C buses were found. */
unsigned i2c_bus_count(void);

/* Is anything at this address?
 *
 * A *quick* transfer: the address is put on the bus and nothing is read or
 * written. That is the one probe that cannot disturb a device -- reading a
 * register means guessing which register is safe to read, and on some devices
 * the answer is none of them.
 */
bool i2c_probe(unsigned bus, u8 address);

/* One byte from a numbered register, and one byte to it. */
enum i2c_status i2c_read_byte(unsigned bus, u8 address, u8 reg, u8 *out);
enum i2c_status i2c_write_byte(unsigned bus, u8 address, u8 reg, u8 value);

/* Two bytes, which is what most sensors report a reading in. */
enum i2c_status i2c_read_word(unsigned bus, u8 address, u8 reg, u16 *out);

void i2c_init(void);
void i2c_print_summary(void);
bool i2c_self_test(void);

/* --- SPI ------------------------------------------------------------------
 *
 * Zero on every machine this kernel has run on. Kept as a call rather than
 * left out so that the answer is a fact reported at run time, not a file that
 * does not exist. */
unsigned spi_controller_count(void);

#endif
