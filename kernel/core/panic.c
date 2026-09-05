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

	/* Every line here bypasses the console lock, and that is deliberate.
	 *
	 * A panic is reached when something has already gone wrong -- and one of
	 * the things that can go wrong is a fault taken while the console lock is
	 * held. Taking it here would turn the report into a hang, which is the
	 * one outcome worse than the fault itself. Output may interleave with
	 * another processor's; a garbled report can still be read. */
	kputs_unlocked("\n=== ReconOS kernel panic ===\n");

	va_start(ap, fmt);
	/* Printed unformatted. A vkprintf would need the formatter split in two
	 * for one caller, and printing the format string verbatim is still far
	 * better than nothing at the moment the machine is dying. */
	kputs_unlocked(fmt);
	va_end(ap);

	kputs_unlocked("\nHalted.\n");
	arch_halt();
}
