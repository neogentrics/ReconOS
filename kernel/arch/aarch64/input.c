/* There is no 8042 on this architecture, and there never was.
 *
 * The chip is an ISA device from 1984 and ARM machines have neither the bus
 * nor the chip. Input here comes from USB HID -- checkpoint 11b built the
 * stack it needs, and the driver on top is a separate piece of work rather
 * than a variation on the PS/2 one.
 *
 * These are real functions rather than an absence, because the alternative is
 * `#ifdef` in core/, which is the one thing the arch boundary exists to
 * prevent -- and because a machine with no keyboard should say so rather than
 * print nothing and leave somebody wondering which of the two it was.
 */
#include <recon/kernel/input.h>
#include <recon/kernel/console.h>

void arch_input_probe(void)
{
}

void arch_input_print(void)
{
	kputs("  keyboard     : none -- this architecture has no 8042, and USB HID\n"
	      "                 is not built yet\n");
}
