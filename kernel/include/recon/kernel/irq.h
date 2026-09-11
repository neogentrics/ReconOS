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

/* --- vectors, which are what a device without a wire has ------------------
 *
 * An ISA line is a wire, and there are sixteen of them. A message-signalled
 * interrupt has no wire at all: the device writes a value to an address, the
 * local APIC decodes the write as an interrupt, and **the number is the whole
 * of the routing**. So a driver that wants one does not ask for a line, it asks
 * for a number nobody else is using.
 *
 * Until now there was exactly one such number, `VECTOR_MSI`, and it belonged to
 * the self-test. The audit named this as one of the three cheap things worth
 * doing: *a vector allocator, so a driver can ask for an MSI rather than being
 * handed the one number the self-test uses.* This is that.
 *
 * Sixteen of them, at 0x50 to 0x5F, because each one is an assembly stub in the
 * image whether it is claimed or not. Running out is reported rather than
 * papered over -- a driver handed a vector somebody else holds would take
 * another device's completions, which is the fault the xHCI event ring had.
 */
#define IRQ_VECTOR_FIRST 0x50
#define IRQ_VECTOR_COUNT 16

/* Claims a free vector and installs a handler on it. Returns the vector, or
 * zero when they are all taken -- zero being a vector no device may use, so it
 * is unambiguous as a failure. */
u8 irq_claim_vector(void (*fn)(void *), void *arg, const char *name);

/* Gives one back. A device that goes away holding a vector is a vector nothing
 * can reuse and a handler that will run with a stale argument. */
void irq_release_vector(u8 vector);

/* Called by the architecture's dispatch for a vector in the block above, before
 * it acknowledges. Same shape as irq_dispatch, and false for the same reason:
 * a vector arriving with nobody to take it is worth counting. */
bool irq_dispatch_vector(u8 vector);

/* How many requests for a vector have been refused, and how many of those were
 * somebody proving on purpose that they run out.
 *
 * The self-test exhausts the block deliberately -- that is the assertion worth
 * making about an allocator -- so without the second number the boot summary
 * reports two refusals on a machine where nothing went wrong, on every boot.
 * A number that is never zero is a number a reader stops seeing, which is how
 * the one that mattered would be missed. */
u64 irq_vector_refusals(void);
void irq_note_expected_refusals(u64 n);

/* How many times each line has fired, and how many arrived with nobody to take
 * them. The second number is the interesting one. */
void irq_print_summary(void);
bool irq_self_test(void);

#endif /* RECON_KERNEL_IRQ_H */
