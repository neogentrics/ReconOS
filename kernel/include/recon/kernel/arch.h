/* The architecture contract.
 *
 * This header is the whole boundary between the portable kernel and the
 * machine it runs on. Every architecture under kernel/arch/ implements
 * exactly these functions and nothing in core/ may reach past them.
 *
 * The rule that keeps it honest: no file under core/ may contain the string
 * "x86", "aarch64", inline assembly, or a hardware address. If a portable
 * file needs something machine-specific, the answer is a new function here,
 * not an #ifdef there. `make check-portable` enforces this.
 *
 * It is deliberately tiny right now. It grows one function at a time, as the
 * kernel actually needs the capability -- an interface invented before
 * anything wants it gets fixed in place before it is understood.
 */
#ifndef RECON_KERNEL_ARCH_H
#define RECON_KERNEL_ARCH_H

#include <recon/kernel/types.h>
#include <recon/kernel/compiler.h>

/* --- Identity ---------------------------------------------------------- */

/* Compile-time name of the architecture: "x86_64", "aarch64", ... */
const char *arch_name(void);

/* What this particular CPU says it is, read at run time. On x86_64 that is
 * the CPUID brand string; on aarch64 the MIDR_EL1 implementer and part.
 *
 * Note the distinction, because it matters for the install story: *which*
 * architecture is not something the kernel discovers -- the firmware already
 * decided that when it chose which binary to load. What is discovered at run
 * time is which CPU *within* that architecture, and which features it has. */
void arch_cpu_identify(char *buf, size_t len);

/* --- Bring-up ---------------------------------------------------------- */

/* Called first thing from kmain(), before anything else. Puts the CPU in a
 * known state and makes arch_console_putc() work. Must not allocate, must not
 * assume memory beyond the kernel image is usable. */
void arch_early_init(void);

/* --- Console ----------------------------------------------------------- */

/* Write one byte to the architecture's earliest possible output: the serial
 * port on x86_64, the PL011 UART on aarch64. This exists so that a kernel
 * which has nothing else working can still say what went wrong. */
void arch_console_putc(char c);

/* --- Control ----------------------------------------------------------- */

/* Stop this CPU forever, with interrupts masked. Used by panic(). */
RK_NORETURN void arch_halt(void);

/* Wait for the next interrupt without burning the CPU. The idle loop's whole
 * body -- this is where the "nothing polls" principle is paid for in silicon
 * rather than promised in a document. */
void arch_wait_for_interrupt(void);

#endif /* RECON_KERNEL_ARCH_H */
