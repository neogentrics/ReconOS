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

RK_PRINTF(1, 2) void kprintf(const char *fmt, ...);

#endif /* RECON_KERNEL_CONSOLE_H */
