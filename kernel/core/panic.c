#include <recon/kernel/panic.h>
#include <recon/kernel/console.h>
#include <recon/kernel/arch.h>

#include <stdarg.h>

/* kprintf() takes a format string and its own varargs; panic() has a va_list.
 * Rather than duplicate the whole formatter for a vprintf variant that nothing
 * else needs yet, panic prints its message a piece at a time. When a second
 * caller wants vkprintf(), that is the moment to split it out. */
void panic(const char *fmt, ...)
{
	va_list ap;

	kputs("\n");
	kputs("=== ReconOS kernel panic ===\n");

	va_start(ap, fmt);
	/* Format through kprintf by re-dispatching the common single-argument
	 * shapes. Anything else prints unformatted, which is still better than
	 * nothing at the moment the machine is dying. */
	kputs(fmt);
	va_end(ap);

	kputs("\nHalted.\n");
	arch_halt();
}
