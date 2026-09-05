#ifndef RECON_KERNEL_PANIC_H
#define RECON_KERNEL_PANIC_H

#include <recon/kernel/compiler.h>

RK_NORETURN RK_PRINTF(1, 2) void panic(const char *fmt, ...);

#endif /* RECON_KERNEL_PANIC_H */
