/* The kernel's own console: portable formatting on top of arch_console_putc().
 *
 * Deliberately not printf. It supports the conversions the kernel actually
 * uses and refuses the rest loudly, because a format string that silently
 * does the wrong thing in a panic handler is worse than one that does not
 * compile.
 *
 * Supported: %s %c %d %i %u %x %p %% and the length modifiers l and ll.
 */
#ifndef RECON_KERNEL_CONSOLE_H
#define RECON_KERNEL_CONSOLE_H

#include <recon/kernel/types.h>
#include <recon/kernel/compiler.h>

void kputc(char c);
void kputs(const char *s);

/* The same, without taking the console lock. Only for panic and the fault
 * reporter: they run when something has already gone wrong, possibly while the
 * lock is held by the code that went wrong, and taking it would turn a report
 * into a hang. Output may interleave, which is the right trade -- a garbled
 * report can be read, a missing one cannot. */
void kputs_unlocked(const char *s);

RK_PRINTF(1, 2) void kprintf(const char *fmt, ...);

#endif /* RECON_KERNEL_CONSOLE_H */
