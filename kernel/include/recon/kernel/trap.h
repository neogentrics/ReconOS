/* Faults, and what the kernel does about them.
 *
 * Until now, a mistake in kernel code ends the machine. On x86 it triple-faults
 * and the CPU resets; on ARM it takes an exception with nowhere to go. Either
 * way what you get is a reboot and no information, and the only reason the last
 * three bugs were findable is that they happened inside an emulator that keeps
 * its own log of every exception. Real hardware keeps no such log.
 *
 * After this, a fault prints where it happened, what kind it was, what address
 * it touched and what was in the registers -- on the serial console, on any
 * machine. That is the difference between a bug that costs an evening and one
 * that costs a week, and it is why this comes before the timer and the
 * scheduler rather than after them.
 *
 * --- The second thing this buys ---
 *
 * A fault that can be *caught* rather than merely reported. The self-test below
 * takes a page fault on purpose and continues. That mechanism is the beginning
 * of what eventually lets a page fault mean "this page needs fetching" instead
 * of "the kernel is dead", which is what demand paging and memory-mapped files
 * are built on.
 */
#ifndef RECON_KERNEL_TRAP_H
#define RECON_KERNEL_TRAP_H

#include <recon/kernel/types.h>

/* Installs the fault handlers. Per architecture: an interrupt descriptor table
 * on x86_64, an exception vector table on aarch64. */
void trap_init(void);

/* --- Catching a fault on purpose ---------------------------------------
 *
 * Set by the code that is about to do something it expects to fault, read by
 * the architecture's handler. When `trap_expecting` is set, the handler records
 * that it caught something and resumes at `trap_recovery` instead of reporting
 * a fatal fault.
 *
 * Deliberately three plain variables rather than anything cleverer: this has to
 * work inside a fault handler, where the heap may be the thing that is broken.
 */
extern volatile bool  trap_expecting;
extern volatile bool  trap_caught;
extern volatile void *trap_recovery;

extern volatile uintptr_t trap_test_address;
extern volatile unsigned  trap_catches;

/* Takes a fault on purpose and comes back. Implemented per architecture, and
 * that is the honest place for it: resuming from a fault means naming the
 * instruction to resume at, and only assembly can promise that the resume point
 * stays next to the faulting instruction.
 *
 * The first version of this was portable C using a label address. The compiler
 * deleted the label's block as unreachable -- taking a label's address does not
 * make its block reachable -- and pointed the resume at the function prologue,
 * so every recovery re-ran the setup and faulted again, forever. The lesson is
 * not about labels: it is that "resume execution somewhere else" is not a thing
 * C can express, and code that pretends otherwise is relying on the optimiser's
 * mood. */
bool trap_provoke_fault(void);

/* Faults on purpose, recovers, and reports whether the handler did its job.
 * Runs at boot like the others, because a fault handler that is quietly wrong
 * is worse than none: it turns every later bug into two. */
bool trap_self_test(void);

/* Called by an architecture handler when it catches an expected fault. Counted
 * so that a resume which does not resume shows up as a failed test rather than
 * as a machine that hangs taking the same fault forever. */
void trap_note_catch(void);

#endif /* RECON_KERNEL_TRAP_H */
