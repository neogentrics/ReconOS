/* Threads, and something that decides which one runs.
 *
 * This is the checkpoint where the kernel stops being one thing doing one
 * thing. Everything before it ran start to finish; after it, execution can be
 * taken away from code that did not ask to give it up.
 *
 * --- Why preemption rather than cooperation ---
 *
 * A cooperative scheduler is much simpler: threads call yield() and the
 * scheduler picks another. It is also a promise that every piece of code in the
 * system is well behaved, which is a promise no operating system can keep --
 * one loop that forgets to yield stops the machine, and the person at the
 * keyboard cannot even move the pointer to tell you about it.
 *
 * So the tick takes execution away. That is the whole point of having built
 * the timer first.
 *
 * --- What is deliberately still absent ---
 *
 * Other cores. Secondary processors are still parked in boot.S, and waking them
 * needs a way to start a CPU (a mailbox on ARM, an interrupt sequence on x86)
 * plus locking on everything this file touches. That is real work and it is
 * recorded in docs/KERNEL.md, not skipped quietly.
 *
 * Priorities. Round-robin, equal slices. A priority scheme invented before
 * there is a workload to shape it around is a priority scheme fitted to
 * nothing.
 *
 * Blocking. A thread runs or is ready. Waiting for something -- a key, a disk,
 * a lock -- needs the something to exist first.
 */
#ifndef RECON_KERNEL_SCHED_H
#define RECON_KERNEL_SCHED_H

#include <recon/kernel/types.h>
#include <recon/kernel/compiler.h>

#define THREAD_NAME_MAX 24

/* How many ticks a thread gets before the scheduler looks elsewhere. At a
 * hundred ticks a second this is fifty milliseconds, which is short enough that
 * a person cannot see the gap and long enough that switching is not most of
 * what the machine does. */
#define SCHED_SLICE_TICKS 5

enum thread_state {
	THREAD_READY = 0,
	THREAD_RUNNING,
	THREAD_FINISHED,
};

struct thread {
	/* First, and at a known offset, because the context switch writes it
	 * from assembly. Moving it means changing two files. */
	void *stack_pointer;

	struct thread *next;		/* circular: the run queue is a ring */
	enum thread_state state;

	u64 id;
	u64 slice_left;
	u64 ran_ticks;			/* total, for the summary */

	void *stack_base;
	size_t stack_pages;

	char name[THREAD_NAME_MAX];
};

void sched_init(void);

/* Creates a thread, ready to run. Returns 0 if there is no memory for a stack.
 * The thread runs until its function returns, at which point it finishes and
 * its stack is reclaimed. */
struct thread *thread_create(const char *name, void (*entry)(void *), void *arg);

/* Gives up the rest of this thread's slice. A thread that has nothing to do
 * should call this rather than spin -- although nothing is obliged to, which is
 * the point of the tick. */
void sched_yield(void);

/* Ends the calling thread. Called automatically when a thread's function
 * returns, so nothing has to remember to. */
RK_NORETURN void thread_exit(void);

/* Called from the timer interrupt. Returns true if the running thread has used
 * its slice and the architecture should switch before returning. */
bool sched_tick(void);

/* Performs the switch. Safe to call from an interrupt handler, which is what
 * makes preemption work: the outgoing thread's interrupt frame stays on its own
 * stack and is restored when it next runs. */
void sched_switch(void);

struct thread *sched_current(void);

void sched_print_summary(void);
bool sched_self_test(void);

/* --- What the architecture provides -------------------------------------- */

/* Saves the callee-saved registers on the current stack, stores the resulting
 * stack pointer through `save_to`, switches to `new_sp` and restores. Returns
 * to its caller on the *new* stack -- which is the whole trick, and the reason
 * it cannot be written in C. */
void arch_context_switch(void **save_to, void *new_sp);

/* Builds a stack that arch_context_switch() can switch *to*, arranged so that
 * the first switch lands in `entry` with `arg`, and so that returning from
 * `entry` lands in thread_exit(). */
void *arch_thread_stack_init(void *stack_top, void (*entry)(void *), void *arg);

#endif /* RECON_KERNEL_SCHED_H */
