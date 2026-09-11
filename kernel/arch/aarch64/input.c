/* There is no 8042 on this architecture, and there never was.
 *
 * The chip is an ISA device from 1984 and ARM machines have neither the bus
 * nor the chip. Input here comes from USB HID, over the xHCI stack checkpoint
 * 11b built -- so this file has nothing to probe and nothing to report beyond
 * saying which of the two kinds of keyboard this machine cannot have.
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
	/* Says what is *not* here, and leaves what is to the USB driver, which
	 * prints itself. The first version of this line said USB HID was not
	 * built, which was true when it was written and stopped being true
	 * without the line noticing -- so it says only the thing it knows. */
	kputs("  8042         : none -- this architecture has never had one\n");
}
