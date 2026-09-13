/* Work that should happen, but not here and not now.
 *
 * An interrupt handler runs with interrupts masked, on whatever stack it
 * landed on, on a processor that is not doing anything else until it finishes.
 * Everything it does is done at the expense of the whole machine, and it cannot
 * do the two things ordinary kernel code does constantly: take a lock that
 * somebody might hold, and wait.
 *
 * So every real kernel splits a handler in two. The half that must happen now
 * -- read the status register, acknowledge the device, take the bytes out of
 * the buffer before it overflows -- happens in the handler. Everything else is
 * handed to a thread. Linux calls the halves top and bottom, Windows calls the
 * second a DPC; the shape is the same everywhere because the constraint is.
 *
 * This kernel had neither half separated: handlers did their work inline, which
 * was tolerable only because no handler did much. The timer wheel is what makes
 * that stop being tolerable, since a timer callback is an interrupt handler
 * that arbitrary code can now ask for.
 *
 * --- What it is not ---
 *
 * Not a thread pool for parallelism. One worker thread runs everything, in the
 * order it was queued, which makes two guarantees worth more here than
 * throughput: work items cannot race each other, and a slow one delays the
 * queue rather than multiplying threads. When something needs to run
 * concurrently with other work it should have a thread of its own and say so.
 */
#ifndef RECON_KERNEL_WORK_H
#define RECON_KERNEL_WORK_H

#include <recon/kernel/types.h>

/* One item. Provided by the caller and usually a field of the thing the work
 * is about, for the same reason a timer is: allocating one would mean work that
 * cannot be scheduled when memory is short, which is when it matters most.
 *
 * Zeroed is valid and idle. */
struct work {
	struct work *next;
	void (*fn)(void *);
	void *arg;
	bool queued;
};

void work_init(struct work *w, void (*fn)(void *), void *arg);

/* Queues it. Safe from an interrupt handler, and that is the point of it.
 *
 * Returns false if the item is already queued -- which is not an error and is
 * the ordinary answer when the same event happens twice before the worker has
 * caught up. It means "it will run", not "it did not work". */
bool work_schedule(struct work *w);

/* Starts the worker thread. After the scheduler and after wait queues. */
void work_init_queue(void);

/* Waits until everything queued before this call has run. For tests and for
 * shutdown; not for an interrupt handler, which cannot wait for anything. */
bool work_drain(void);

void work_print_summary(void);
bool work_self_test(void);

#endif /* RECON_KERNEL_WORK_H */
