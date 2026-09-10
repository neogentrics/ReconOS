/* Who wants which interrupt.
 *
 * The kernel has been able to *receive* device interrupts since checkpoint 7 and
 * to *route* them to any processor since the I/O APIC landed. What it has never
 * had is anywhere to put a handler: `trap.c` acknowledged every line and
 * returned, because the timer was the only device that wanted one and it was
 * wired in by hand. The audit's note on MSI says the same thing from the other
 * side -- "no driver asks for either yet" -- and this is what a driver asks.
 *
 * --- What a handler may do ---
 *
 * Almost nothing, and the rule is not style. It runs with interrupts masked, on
 * whatever stack was interrupted, possibly in the middle of a critical section
 * somewhere else in the kernel. So it may not allocate, may not take a lock that
 * anything outside interrupt context takes, and may not wait for anything at
 * all.
 *
 * What it *should* do is take the one thing the hardware will lose if nobody
 * takes it -- a byte from a one-byte buffer, a completion out of a ring -- and
 * hand the rest to `work_schedule`. That is why deferred work was built before
 * this, and it is the shape the PS/2 keyboard uses: the 8042 holds exactly one
 * byte and the next keypress overwrites it.
 *
 * --- Sharing ---
 *
 * One handler per line. Not because sharing is hard, but because a shared line
 * needs every handler to be able to say "not mine", and a device that cannot
 * answer that -- which is most of the legacy ISA devices this covers -- turns a
 * shared line into a guess. PCI devices share, and they will want MSI instead,
 * where there is no line to share.
 *
 * A second registration on a taken line is **refused and says so**, rather than
 * replacing the first. Silently replacing is how one driver stops working when
 * an unrelated one is added.
 *
 * --- Acknowledging ---
 *
 * Not the handler's job. The controller is told after the handler returns,
 * always, by the code that dispatched -- so a handler that forgets cannot stop
 * every subsequent interrupt on the machine, which is the failure mode that
 * looks like a freeze with no message.
 */
#ifndef RECON_KERNEL_IRQ_H
#define RECON_KERNEL_IRQ_H

#include <recon/kernel/types.h>

/* The sixteen ISA lines. Not a limit on what the machine can deliver -- MSI has
 * its own vectors and does not come through here -- but the whole of what a
 * legacy line can be. */
#define IRQ_LINES 16

/* Claims a line. False if the number is out of range or somebody already has it.
 *
 * `name` is for the summary and must outlive the registration, which every
 * string literal does. */
bool irq_register(unsigned line, void (*fn)(void *), void *arg,
		  const char *name);

/* Gives it back. A device that goes away and leaves its handler registered is a
 * handler that will eventually run with a stale argument. */
void irq_release(unsigned line);

/* Called by the architecture's dispatch, before it acknowledges. Returns whether
 * anything wanted it -- which is worth knowing rather than assuming, because a
 * line nothing claims that keeps firing is a device nobody turned off, and that
 * is a live-lock the machine cannot escape. */
bool irq_dispatch(unsigned line);

/* How many times each line has fired, and how many arrived with nobody to take
 * them. The second number is the interesting one. */
void irq_print_summary(void);
bool irq_self_test(void);

#endif /* RECON_KERNEL_IRQ_H */
