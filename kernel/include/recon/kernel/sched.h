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
 * --- More than one processor ---
 *
 * The run queue is shared and guarded by one lock, and a thread marked RUNNING
 * is never scheduled again until it stops. That single check is what keeps one
 * thread from landing on two processors with one stack between them -- which
 * corrupts both within a few instructions and looks like nothing in particular
 * afterwards.
 *
 * `current` is per-processor. It was one pointer meaning "the running thread";
 * with four processors there are four running threads and the question has no
 * single answer, so every reader now has to say *whose*.
 *
 * --- What is deliberately still absent ---
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

struct personality;

#define THREAD_NAME_MAX 24

/* How many ticks a thread gets before the scheduler looks elsewhere. At a
 * hundred ticks a second this is fifty milliseconds, which is short enough that
 * a person cannot see the gap and long enough that switching is not most of
 * what the machine does. */
#define SCHED_SLICE_TICKS 5

enum thread_state {
	THREAD_READY = 0,
	THREAD_RUNNING,

	/* Waiting for something, and not a candidate for any processor until
	 * somebody wakes it. Distinct from READY on purpose: a blocked thread
	 * that the scheduler could still pick is a thread that runs before the
	 * thing it is waiting for has happened. */
	THREAD_BLOCKED,

	THREAD_FINISHED,
};

/* Room for the vector unit's state, plus the control words that go after it.
 * 512 for x86_64's FXSAVE image or aarch64's thirty-two V registers, and the
 * rest so that aarch64's FPSR and FPCR have somewhere of their own to go.
 * A multiple of 64 so the alignment the store instructions want is kept. */
#define VECTOR_STATE_MAX 576

/* Written after the array and checked after every save. "VECGUARD". */
#define VECTOR_GUARD 0x5645434755415244ULL

struct thread {
	/* First, and at a known offset, because the context switch writes it
	 * from assembly. Moving it means changing two files. */
	void *stack_pointer;

	struct thread *next;		/* circular: the run queue is a ring */
	enum thread_state state;

	u64 id;

	/* Which processor is running it, or -1 for none. Not decoration: it is
	 * what makes a thread's presence on a processor visible to the others,
	 * and it is checked before anything is scheduled. */
	int cpu;

	/* Which processor this is the idle thread *of*, or -1 for an ordinary
	 * thread.
	 *
	 * An idle thread belongs to one processor and must never be run by
	 * another. It looks like an ordinary ready thread in a ring every
	 * processor picks from, so without this a processor takes somebody
	 * else's idle thread, and the processor it belonged to is left with a
	 * ring in which everything is running elsewhere and nothing at all it
	 * may run. */
	int idle_for;

	/* The processor this thread may run on, or -1 for any of them.
	 *
	 * Separate from idle_for, which says the same thing for a different
	 * reason: an idle thread is pinned because taking it would leave its
	 * processor with nothing it is allowed to run, and a pinned thread is
	 * pinned because it is doing something that belongs to one processor.
	 *
	 * There is one of the latter: the boot thread. It brings up the
	 * machine, and a good deal of what it touches on the way -- calibrating
	 * one processor's timer, walking firmware tables, starting the others --
	 * is written on the assumption that it is processor 0 doing it.
	 *
	 * It was already pinned there, by accident: sched_init cleared the
	 * thread and left idle_for at zero, which reads as processor 0's idle
	 * thread. That pinned it and also made it unable to ever block, since
	 * an idle thread must not. Splitting the two fields keeps the property
	 * that was wanted and drops the one that was not. */
	int pinned_to;

	/* False from the moment this thread is chosen to run until the
	 * processor it was running on has actually left it.
	 *
	 * READY is not enough to say a thread may be taken, and that was a
	 * real fault rather than a nicety. sched_switch marked the outgoing
	 * thread READY *before* arch_context_switch had saved its registers
	 * and written its stack pointer -- so another processor could pick
	 * it up in between and resume it from a stale stack pointer, with
	 * two processors standing on one stack. The same window let the
	 * reaper free a finished thread's stack while the processor that
	 * finished it was still executing on it, which is how it was found:
	 * a page allocator test's marker word turned up in a link register.
	 *
	 * A waker has the same problem from the other side. wait_sleep marks
	 * a thread BLOCKED and then switches away, and a waker on another
	 * processor can make it READY in that gap.
	 *
	 * So the rule is not about who set which state. It is that a thread
	 * is not available to any processor until the processor it was on
	 * has finished with it, and this is that fact. */
	volatile bool off_cpu;

	/* What this thread was actually asked to run, and its argument.
	 *
	 * Kept here because every thread now starts in a wrapper rather than at
	 * its entry point directly: the first thing a new thread must do is
	 * release the thread that gave up the processor to it, and a thread
	 * that has never run has no other place to do it. */
	void (*entry)(void *);
	void *entry_arg;

	u64 slice_left;
	u64 ran_ticks;			/* total, for the summary */

	/* The kernel stack a trap from user mode lands on for this thread, or
	 * zero for a thread that never runs in user mode.
	 *
	 * It belongs to the thread and not to the processor, and that is the
	 * whole point of it being here. It used to be recorded once, into the
	 * block of whichever processor happened to run arch_enter_user -- and
	 * a comment in that function said, correctly, that this would have to
	 * move into the context switch once there was more than one program.
	 * A system call is preemptible, so a thread can enter one on one
	 * processor and return from it on another; the processor it returns on
	 * had the wrong stack recorded, or none at all. */
	void *entry_stack;

	void *stack_base;
	size_t stack_pages;

	/* Which system calls this thread's program means when it makes one.
	 * Null for a kernel thread, which makes none. See user.h -- it is one
	 * pointer now and a rewrite later. */
	const struct personality *personality;

	char name[THREAD_NAME_MAX];

	/* The vector unit's registers, saved around a switch.
	 *
	 * Not decoration and not optional once the unit is enabled: two threads
	 * that both use vector registers and share them corrupt each other's
	 * arithmetic, silently, with every line of both programs correct. The
	 * kernel itself is built without floating point and does not touch
	 * them; user programs will, and the day one does is the day this has to
	 * already be here.
	 *
	 * IT WAS 512 BYTES AND THAT WAS SIXTEEN TOO FEW. (BG-146)
	 *
	 * The comment that stood here said 512 covered "x86_64's FXSAVE image,
	 * and aarch64's thirty-two 128-bit V registers with their two status
	 * words". Thirty-two registers of sixteen bytes *is* 512, so there was
	 * nothing left for the status words -- and the aarch64 save wrote them
	 * at byte 512 and 520, into whatever field of this structure came next.
	 * The sentence asserted the arithmetic it got wrong.
	 *
	 * What it overwrote was `process`, sixteen bytes further on, so a user
	 * program lost its identity the first time it was preempted -- and,
	 * once processes had address spaces, lost its memory with it and took
	 * an instruction abort on its own code. x86_64 never showed it: FXSAVE
	 * writes exactly 512 bytes and not one more.
	 *
	 * It does **not** cover AVX or SVE, whose state is larger and whose size
	 * is a runtime question; those units are deliberately left disabled
	 * rather than enabled and half-saved.
	 *
	 * Aligned to sixty-four because the instruction that writes it requires
	 * alignment and faults rather than working slowly without it. */
	u8 vector_state[VECTOR_STATE_MAX] RK_ALIGNED(64);

	/* And a value that must still be there afterwards.
	 *
	 * The bug above was silent for three checkpoints because nothing after
	 * the array mattered yet. A guard costs one comparison per context
	 * switch and turns "the next field is wrong for no reason" into a
	 * counted, reported failure at the moment it happens. */
	u64 vector_guard;

	/* The next thread on whatever queue this one is waiting on, or null.
	 * One pointer rather than a list node, because a thread waits for at
	 * most one thing at a time -- and a thread on two queues would be woken
	 * twice and run once. */
	struct thread *wait_next;

	/* The process this thread belongs to, by identifier rather than by
	 * pointer, and zero for a kernel thread that belongs to none.
	 *
	 * An identifier because a process can end while somebody still holds a
	 * reference to one of its threads, and a stale pointer to a reused slot
	 * is a thread that appears to belong to a different program. */
	u32 process;

	/* Who is still accounting for this thread, which is *not* the same
	 * fact as the line above and is why there are two.
	 *
	 * `process` means "the process this thread is running as", and it
	 * correctly becomes nothing the moment the thread ends -- a finished
	 * thread is not running as anybody. This one means "the process that
	 * cannot be reaped until this thread has been", and it has to outlive
	 * the first by exactly the window between ending and being freed.
	 *
	 * They shared a field once, and the reaper found every thread already
	 * detached and reaped no process at all. */
	u32 counted_by;
};

void sched_init(void);

/* Creates a thread, ready to run. Returns 0 if there is no memory for a stack.
 * The thread runs until its function returns, at which point it finishes and
 * its stack is reclaimed. */
struct thread *thread_create(const char *name, void (*entry)(void *), void *arg);

/* The same, but not yet runnable. For callers that must set something on the
 * thread before any processor can pick it up -- which in practice means giving
 * it a process, because the scheduler reads that to decide which address space
 * to run it in. Putting a thread in the ring first and setting the field second
 * is a race against every other processor, and the thread runs with the field
 * unset when it is lost. (BG-148) */
struct thread *thread_create_stopped(const char *name, void (*entry)(void *),
				     void *arg);

/* Hands a stopped thread to the scheduler. From here it may be running on any
 * processor before this returns. */
void thread_start(struct thread *t);

/* Gives up the rest of this thread's slice. A thread that has nothing to do
 * should call this rather than spin -- although nothing is obliged to, which is
 * the point of the tick. */
void sched_yield(void);

/* Called by a thread running for the first time, before its entry point. */
void sched_thread_first_run(void);

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

/* A secondary processor joining the scheduler, adopting the idle thread that
 * was made for it in advance. Called once, by that processor, on itself. */
void sched_adopt_idle(struct thread *idle);

/* Arms the deferred reaper. After the worker thread exists, and before
 * anything is allowed to finish. */
void sched_reaper_init(void);

/* How many finished threads have actually been freed. Zero on a kernel whose
 * reaper is never asked to run, which is what BG-183 was. */
u64 sched_threads_reaped(void);

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

/* Told to the processor that is about to run `t`: the stack a trap from user
 * mode lands on, and anything else the architecture keeps per processor that
 * is really a property of the thread. Called with the run-queue lock held,
 * immediately before the switch. */
void arch_thread_switched_in(struct thread *t);

#endif /* RECON_KERNEL_SCHED_H */
