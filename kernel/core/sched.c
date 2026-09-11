#include <recon/kernel/sched.h>
#include <recon/kernel/work.h>
#include <recon/kernel/process.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/console.h>
#include <recon/kernel/time.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/panic.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/smp.h>

#define THREAD_STACK_PAGES 4	/* 16KB, which is generous for a kernel thread */

/* The run queue, and the lock that makes it safe for more than one processor.
 *
 * Interrupt-safe, because the timer interrupt calls into the scheduler: a lock
 * held by ordinary code and then wanted by that code's own timer interrupt
 * deadlocks the processor holding it. */
static struct thread *ring;	/* any member; the ring is circular */
static struct spinlock ring_lock = SPINLOCK_INIT("sched");

static u64 next_id;
static u64 switches;

/* Counted apart from `switches`, and the distinction is the whole point of the
 * self-test below: a voluntary switch proves cooperation works, and only a
 * preemption proves the tick is taking execution away from code that did not
 * offer it. A test that counts both together passes on either. */
static u64 preemptions;

/* Times a thread's vector-state guard was found broken after a save. Zero on
 * every machine this has ever run on except the one that found BG-146, and
 * asserted rather than merely printed. */
static u64 vector_overruns;

/* The thread each processor was already running when it joined the scheduler.
 * Not created but *adopted*: it has a stack and a context already, and the first
 * switch away from it is what fills in its stack pointer. Without one there
 * would be nothing to switch away from, and the first switch would have nowhere
 * to save the state it was replacing.
 *
 * One per processor, because that is the whole shape of this change: a
 * processor's notion of what it is running cannot be a global once there is
 * more than one processor to have a notion. */
static struct thread boot_threads[MAX_CPUS];

/* Times an idle thread was scheduled while real work was waiting.
 *
 * This is an invariant with a number attached rather than a statistic. An idle
 * thread exists so a processor has something to do when nothing else will have
 * it; running one while a ready thread is sitting in the ring is not slightly
 * wrong, it is the processor doing nothing on purpose.
 *
 * It exists because that happened and nothing noticed. Giving processor 0 an
 * idle thread of its own put one in the ring alongside the boot thread for the
 * first time, and the round-robin handed them alternate slices -- and because
 * idle_loop waits for an interrupt, the slice it was given was not a short one,
 * it was the rest of the tick. **Every self-test still passed.** What failed was
 * seven paths of the verification run that write to a disk, all of which wrote
 * correct data and ran out of time doing it.
 *
 * The same shape as BG-157: a fault whose only symptom is how long things take,
 * against which the entire test suite is blind. */
static u64 idle_over_work;

static void ring_insert(struct thread *t)
{
	if (!ring) {
		ring = t;
		t->next = t;
		return;
	}

	t->next = ring->next;
	ring->next = t;
}

static void ring_remove(struct thread *t)
{
	struct thread *p = ring;

	if (!ring)
		return;

	while (p->next != t) {
		p = p->next;
		if (p == ring)
			return;		/* not in the ring */
	}

	if (t->next == t) {
		ring = 0;
		return;
	}

	p->next = t->next;
	if (ring == t)
		ring = p;
}

struct thread *sched_current(void)
{
	return this_cpu()->current;
}

void sched_init(void)
{
	struct thread *boot = &boot_threads[arch_cpu_id()];

	/* Not re-initialised here: the declaration above already does it,
	 * and a second spin_init would tell the lock registry this is a
	 * lock whose lifetime is not the kernel's -- which is how the
	 * registry keeps stack locks out of itself. */

	kmemset(boot, 0, sizeof(*boot));
	kstrlcpy(boot->name, "boot", sizeof(boot->name));
	boot->id = 0;
	boot->state = THREAD_RUNNING;
	boot->slice_left = SCHED_SLICE_TICKS;
	boot->cpu = (int)arch_cpu_id();
	boot->vector_guard = VECTOR_GUARD;

	/* NOT anybody's idle thread, said explicitly because zero does not mean
	 * that.
	 *
	 * `idle_for` is a processor number, and -1 is "no processor" -- so the
	 * kmemset above left this thread claiming to be processor 0's idle
	 * thread. Two things followed, and both had been true since the field
	 * was added. It could only ever be scheduled on processor 0, because
	 * an idle thread must not be taken by another processor. And **it could
	 * never block**: wait_sleep refuses an idle thread, since a blocked
	 * idle thread is a processor that has stopped.
	 *
	 * Nothing noticed because nothing on the boot thread had ever waited
	 * for anything -- the boot path ran to completion and the self-tests
	 * that use wait queues all create threads of their own. The first
	 * caller to try was timer_sleep_ns, which reported that a thread could
	 * not sleep and was right.
	 *
	 * Same shape as BG-147: a structure cleared wholesale, and one field
	 * whose zero is a meaningful and wrong value. */
	boot->idle_for = -1;

	/* And pinned where it already was. The boot sequence runs on this
	 * thread and a good deal of it assumes it is processor 0 doing the
	 * work -- so the pinning that idle_for was providing by accident is
	 * kept, deliberately, while the inability to block goes away. */
	boot->pinned_to = (int)arch_cpu_id();
	boot->off_cpu = false;	/* it is running, here, now */

	ring = 0;
	ring_insert(boot);
	this_cpu()->current = boot;
	next_id = 1;
	switches = 0;
	preemptions = 0;
	vector_overruns = 0;
	idle_over_work = 0;
}

/* A secondary processor joining. It arrives already running on a stack of its
 * own, so like the boot processor it adopts rather than creates -- and the
 * thread it adopts is the idle thread made for it in advance, which is already
 * in the ring. */
void sched_adopt_idle(struct thread *idle)
{
	u64 flags = spin_lock_irq(&ring_lock);

	idle->state = THREAD_RUNNING;
	idle->off_cpu = false;
	idle->cpu = (int)arch_cpu_id();
	idle->slice_left = SCHED_SLICE_TICKS;
	this_cpu()->current = idle;

	spin_unlock_irq(&ring_lock, flags);
}

/* Creates a thread and starts it, which is what almost every caller wants: a
 * kernel thread has nothing to set on it before it runs. A caller that *does*
 * -- anything giving the thread a process -- must use thread_create_stopped and
 * start it once it has. */
struct thread *thread_create(const char *name, void (*entry)(void *), void *arg)
{
	struct thread *t = thread_create_stopped(name, entry, arg);

	thread_start(t);
	return t;
}

/* Where every new thread begins.
 *
 * A thread that has already run reaches release_leaving by returning from
 * arch_context_switch inside sched_switch. A thread running for the first time
 * never returns from anything -- it is switched *to* and starts here -- so
 * without this the processor it started on would carry an unreleased
 * predecessor, which would stay unrunnable until that processor happened to
 * switch again.
 *
 * In C rather than in each architecture's trampoline, deliberately: the two
 * assembly stubs would have to agree about it, and an agreement between two
 * assembly files is the kind that drifts. */
static void first_run(void *arg)
{
	struct thread *t = arg;

	sched_thread_first_run();

	t->entry(t->entry_arg);
}

struct thread *thread_create_stopped(const char *name, void (*entry)(void *),
				     void *arg)
{
	struct thread *t = kzalloc(sizeof(*t));
	paddr_t stack;

	if (!t)
		return 0;

	/* Whole pages, from the page allocator rather than the heap: a stack
	 * that shares a page with something else turns a stack overflow into a
	 * corrupted neighbour instead of a fault, and a fault is far easier to
	 * find. */
	stack = pmm_alloc_pages(THREAD_STACK_PAGES);
	if (!stack) {
		kfree(t);
		return 0;
	}

	t->stack_base = phys_to_virt(stack);
	t->stack_pages = THREAD_STACK_PAGES;
	t->id = next_id++;
	t->vector_guard = VECTOR_GUARD;
	t->state = THREAD_READY;
	t->slice_left = SCHED_SLICE_TICKS;
	kstrlcpy(t->name, name, sizeof(t->name));

	t->cpu = -1;		/* not running anywhere */
	t->idle_for = -1;	/* and not anybody's idle thread */
	t->pinned_to = -1;	/* and free to run anywhere */
	t->off_cpu = true;	/* it has never been on one */

	/* Its vector registers start as a *captured* image rather than as
	 * zeros. An all-zero FXSAVE image sets MXCSR to zero, which unmasks
	 * every floating-point exception, and some zero patterns in its
	 * reserved bits make the restore itself fault. Copying the state this
	 * processor is in -- which was initialised when the unit was enabled --
	 * gives a new thread a defined and legal starting point. */
	arch_vector_save(t->vector_state);

	t->entry = entry;
	t->entry_arg = arg;

	/* Started in a wrapper, not at `entry`. See first_run below. */
	t->stack_pointer = arch_thread_stack_init(
		(u8 *)t->stack_base + THREAD_STACK_PAGES * PAGE_SIZE,
		first_run, t);

	return t;
}

/* Makes a thread the scheduler's. Separate from creating it, and that
 * separation is a bug fix rather than tidiness. (BG-148)
 *
 * `thread_create` used to end by putting the thread in the ring, which makes it
 * runnable *immediately* -- on this processor and on every other one. A caller
 * that then set something on the thread was racing every other processor to do
 * it, and losing that race meant the thread ran with the field unset.
 *
 * The field that mattered was `process`. A user program is created, put in the
 * ring, and only then attached to the process that owns it; a processor that
 * picked it up in between ran it with no process, so the scheduler found no
 * address space for it and left the kernel's loaded -- and the program took an
 * instruction fault on its own first instruction, because its code is mapped in
 * a space that was not the one running.
 *
 * It was invisible until 10 September because until then every program lived in
 * the one address space there was, so running with no process still had the
 * program's code mapped. Address spaces did not introduce this; they removed
 * what was hiding it.
 */
void thread_start(struct thread *t)
{
	u64 flags;

	if (!t)
		return;

	flags = spin_lock_irq(&ring_lock);
	ring_insert(t);
	spin_unlock_irq(&ring_lock, flags);
}

/* The next thread willing to run *here*. The ring lock must be held.
 *
 * THREAD_READY is the only state that can be taken. A thread marked RUNNING is
 * running on some processor -- possibly this one, possibly another -- and
 * scheduling it a second time would put one thread on two processors with one
 * stack between them, which corrupts both within a few instructions and looks
 * like nothing in particular afterwards.
 *
 * That check is the single line on which the whole of this checkpoint's
 * safety rests.
 */
/* Hands back the thread that gave up this processor at the last switch.
 *
 * This is the moment -- and the only moment -- at which it is true that the
 * outgoing thread has stopped using its stack: the switch has completed and
 * some other thread is running here. Everything sched_switch used to do to
 * `prev` before the switch happens here instead, after it.
 *
 * The ring lock is held.
 */
static void release_leaving(struct cpu_local *cpu)
{
	struct thread *gone = cpu->leaving;

	if (!gone)
		return;

	cpu->leaving = 0;

	/* Only a thread that was still RUNNING becomes runnable again. One
	 * that blocked is BLOCKED and its waker decides; one that finished is
	 * FINISHED and the reaper decides. Both still need the flag, because
	 * both are things another processor may act on. */
	if (gone->state == THREAD_RUNNING) {
		gone->state = THREAD_READY;
		gone->cpu = -1;
	}

	__atomic_store_n(&gone->off_cpu, true, __ATOMIC_RELEASE);
}

/* Called by a thread that has just started for the first time. Every other
 * thread reaches release_leaving by returning from arch_context_switch inside
 * sched_switch; a thread that has never run has no such return point, and
 * without this the processor it started on would carry an unreleased
 * predecessor until its next switch. */
void sched_thread_first_run(void)
{
	u64 flags = spin_lock_irq(&ring_lock);

	release_leaving(this_cpu());

	spin_unlock_irq(&ring_lock, flags);
}

static struct thread *pick_next(struct thread *cur, unsigned me)
{
	struct thread *t = cur->next;
	struct thread *fallback = 0;

	while (t != cur) {
		/* Somebody else's idle thread is not work. Taking it leaves the
		 * processor it belongs to with nothing it is allowed to run,
		 * which is how a machine with four processors and two threads
		 * panics while three of them are idle. */
		/* off_cpu, and not merely READY. A thread that has been
		 * marked runnable but whose processor has not finished
		 * leaving it would be resumed from a stack pointer that has
		 * not been written yet. */
		if (t->state == THREAD_READY && t->off_cpu &&
		    (t->idle_for < 0 || t->idle_for == (int)me) &&
		    (t->pinned_to < 0 || t->pinned_to == (int)me)) {
			/* AN IDLE THREAD IS THE LAST RESORT, NOT A TURN IN THE
			 * ROUND.
			 *
			 * It used to be neither, because this ring never contained
			 * an idle thread for the processor doing the picking: the
			 * boot processor had none, and a secondary spent its whole
			 * life in one. Giving processor 0 a real idle thread made
			 * this line wrong without changing it, which is the same
			 * shape as the TLB shootdown and BG-148.
			 *
			 * And it was expensive rather than incorrect, which is why
			 * every self-test still passed. idle_loop waits for an
			 * interrupt, so a slice handed to it is not a short slice --
			 * it is the rest of the tick, doing nothing. Alternating
			 * between the boot thread and an idle thread roughly halved
			 * the machine: the seven paths of the verification run that
			 * write to a disk all timed out, having written correct data
			 * too slowly. */
			if (t->idle_for >= 0) {
				if (!fallback)
					fallback = t;
			} else {
				return t;
			}
		}

		t = t->next;
	}

	/* Nothing else wanted this processor. If the caller is still
	 * runnable it keeps it; otherwise the idle thread is what is left,
	 * which is exactly what it exists for. */
	if (cur->state == THREAD_RUNNING)
		return cur;

	return fallback;
}

void sched_switch(void)
{
	struct cpu_local *me = this_cpu();
	struct thread *prev, *next;
	u64 flags;

	flags = spin_lock_irq(&ring_lock);

	prev = me->current;
	next = pick_next(prev, me->id);

	if (!next) {
		/* Nothing here can run, including the caller.
		 *
		 * On one processor that means everything has finished, and
		 * there is nothing to do but say so. On several it usually
		 * means every other thread is running elsewhere -- which is
		 * ordinary, happens constantly, and is what this processor's
		 * idle thread exists for.
		 *
		 * The comment that used to sit here said exactly that, and the
		 * line under it panicked anyway. It was written before there
		 * was a second processor to make it true, and it stayed while
		 * 9b made it true on aarch64 -- where nothing hit it, because
		 * that architecture's processors happened never to run out at
		 * the same moment. x86_64 hit it on the first four-processor
		 * boot, six times out of six. */
		next = me->idle;

		if (!next || next->state == THREAD_FINISHED) {
			spin_unlock_irq(&ring_lock, flags);
			panic("sched: nothing left to run");
		}
	}

	if (next == prev) {
		prev->slice_left = SCHED_SLICE_TICKS;
		spin_unlock_irq(&ring_lock, flags);
		return;
	}

	/* prev is NOT marked READY here, which is the change BG-159 is.
	 *
	 * It used to be, and that made the thread available to every other
	 * processor while this one was still executing on its stack and had
	 * not yet written its saved stack pointer. It is released instead by
	 * whoever runs here next, which is the first instant at which it has
	 * genuinely stopped. */
	me->leaving = prev;

	/* The invariant, checked where it can be violated. Only on a switch to
	 * an idle thread, which on a busy machine is rare and on an idle one is
	 * a walk of a short ring. */
	if (next->idle_for >= 0) {
		struct thread *t = next->next;

		while (t != next) {
			if (t->state == THREAD_READY && t->idle_for < 0 &&
			    (t->pinned_to < 0 || t->pinned_to == (int)me->id)) {
				idle_over_work++;
				break;
			}
			t = t->next;
		}
	}

	next->state = THREAD_RUNNING;
	next->off_cpu = false;
	next->cpu = (int)me->id;
	next->slice_left = SCHED_SLICE_TICKS;
	me->current = next;
	me->switches++;
	switches++;

	/* The lock is released *before* the switch, not after.
	 *
	 * After the switch this code is running as a different thread, on a
	 * different stack, and `flags` holds that thread's saved interrupt state
	 * from whenever it last switched away -- not ours. Releasing afterwards
	 * would unlock on behalf of somebody else and restore the wrong
	 * interrupt state, and would do it correctly often enough to look fine.
	 *
	 * Releasing here is safe because both threads' states are already
	 * consistent: prev is READY and owned by nobody, next is RUNNING and
	 * owned by this processor. Another processor that looks at the ring now
	 * sees the truth. It cannot take `next`, because `next` is already
	 * marked RUNNING. */
	/* The vector unit, saved out of the outgoing thread and loaded into the
	 * incoming one, both by the processor that is leaving.
	 *
	 * Eagerly rather than lazily. The lazy trick -- disable the unit, catch
	 * the first use, swap then -- saves real work on threads that never
	 * touch it, and it is a second state machine that is wrong only when
	 * two processors race on the same thread. This is two instructions.
	 * When there is a reason to make it lazy there will also be a
	 * measurement saying so. */
	/* Everything the machine has to be told about *which thread* is about
	 * to run here -- the stack a trap from user mode should land on, and
	 * whatever else the architecture keeps per processor on a thread's
	 * behalf. Before the switch, because it is this processor being told
	 * about its next occupant. */
	arch_thread_switched_in(next);

	arch_vector_save(prev->vector_state);

	/* And immediately: a save that wrote past its area has just corrupted
	 * the next field of this thread, and the sooner that is a number
	 * somebody can read the less it looks like an unrelated bug three
	 * subsystems away. This is how BG-146 presented -- a user program
	 * whose process identifier became zero while it ran. */
	if (prev->vector_guard != VECTOR_GUARD) {
		vector_overruns++;
		prev->vector_guard = VECTOR_GUARD;
	}

	arch_vector_restore(next->vector_state);

	spin_unlock(&ring_lock);

	/* And the map the incoming thread runs in, before the switch rather
	 * than after: after the switch this code is the other thread, and a
	 * thread that has already started running in the previous program's
	 * address space has already been able to read it.
	 *
	 * A thread with no process, or a process the kernel made for itself,
	 * gets the kernel's own map. That is not a detail: an idle thread
	 * left pointing at the last program's tables would keep them alive
	 * and reachable on a processor with nothing to run.
	 *
	 * Doing it here and not in the architecture's switch keeps one rule
	 * in one place. The cost when nothing changes is a comparison --
	 * addrspace_activate returns immediately when the space is already
	 * the active one, which is every switch between two threads of one
	 * program and every switch between kernel threads.
	 */
	{
		struct process *np = process_of(next);

		addrspace_activate(np ? np->space : NULL);
	}

	arch_context_switch(&prev->stack_pointer, next->stack_pointer);

	/* Reached as the *incoming* thread, whenever it is next scheduled.
	 *
	 * `this_cpu()` is recomputed rather than reusing `me`: a thread
	 * resumes on whichever processor picked it, which need not be the one
	 * it left. `me` is the other processor's block, saved in this frame
	 * when this thread switched away, and using it here would release a
	 * thread on somebody else's behalf.
	 *
	 * Interrupts are still off -- they were off on the way in and the
	 * switch preserved that -- so the lock is taken without touching
	 * them. */
	spin_lock(&ring_lock);
	release_leaving(this_cpu());
	spin_unlock(&ring_lock);

	arch_irq_restore(flags);
}

void sched_yield(void)
{
	sched_switch();
}

bool sched_tick(void)
{
	struct cpu_local *me = this_cpu();
	struct thread *cur = me->current;

	me->ticks++;

	if (!cur)
		return false;

	cur->ran_ticks++;

	if (cur->slice_left > 0)
		cur->slice_left--;

	/* True means "switch on the way out of this interrupt". The switch is
	 * not done here because the caller is the architecture's interrupt path,
	 * and it may have work to finish -- acknowledging the interrupt
	 * controller, for one -- that must happen before the stack changes. */
	if (cur->slice_left == 0) {
		__atomic_add_fetch(&preemptions, 1, __ATOMIC_RELAXED);
		return true;
	}

	return false;
}

/* Defined below, beside the walk it does. */
static void reap(void);

static u64 reaped_threads;

u64 sched_threads_reaped(void)
{
	return reaped_threads;
}

static struct work reaper_work;

static void run_the_reaper(void *arg)
{
	(void)arg;
	reap();
}

void thread_exit(void)
{
	struct thread *dead = this_cpu()->current;

	dead->state = THREAD_FINISHED;
	dead->cpu = -1;

	/* Its process, if it has one, is told before the switch -- because
	 * after the switch this code is not running and there is no later. */
	process_thread_ended(dead, 0);

	/* The stack cannot be freed here: this code is standing on it. It is
	 * left for the reaper below, run from another thread on another stack.
	 *
	 * **And something has to ask it to run.** That sentence used to end
	 * "left for whoever notices the thread is finished", and nobody
	 * noticed: `reap` had exactly one caller in the whole kernel and it was
	 * a self-test. Every thread stack and every ended process was held for
	 * the life of the machine. (BG-183)
	 *
	 * Deferred rather than done here, because here is the one place it
	 * cannot be done. The worker refuses a second queueing while the first
	 * is still pending, which is exactly right: one pending reap collects
	 * however many threads have died by the time it runs. */
	work_schedule(&reaper_work);

	sched_switch();

	panic("sched: a finished thread was scheduled again");
}

/* Frees the stacks of threads that have finished. Called from a running thread,
 * so it is never standing on the memory it frees.
 *
 * One pass per removal, restarting each time, rather than one walk that removes
 * as it goes. That is deliberate and it is a correction: the first version tried
 * to restart the walk with a `continue` inside a do-while, which jumps to the
 * loop *condition* -- and the condition had just been made true by the restart,
 * so it reaped exactly one thread and stopped. The summary showed two finished
 * threads still in the ring, which is the only reason it was noticed.
 *
 * Restarting the whole scan is O(n^2) in the worst case and that is fine: it
 * runs when a thread ends, not in a loop, and n is the number of threads.
 */
static bool is_boot_thread(const struct thread *t)
{
	for (unsigned i = 0; i < MAX_CPUS; i++)
		if (t == &boot_threads[i])
			return true;
	return false;
}

static void reap(void)
{
	for (;;) {
		struct thread *victim = NULL;
		struct thread *t, *start;
		u64 flags;

		/* Under the ring lock, which it did not used to take.
		 *
		 * That was safe for exactly as long as this only ran from a
		 * test, on a quiet machine, with nothing else touching the
		 * ring. Running it from the worker thread -- which is the fix
		 * for BG-183 -- makes it concurrent with every scheduling
		 * decision on every processor, and an unlocked walk of a list
		 * somebody else is splicing is a pointer into freed memory. */
		flags = spin_lock_irq(&ring_lock);

		t = ring;
		start = ring;

		if (!t) {
			spin_unlock_irq(&ring_lock, flags);
			return;
		}

		do {
			/* off_cpu, and not merely finished. thread_exit marks a
			 * thread FINISHED and then switches away, and it is standing
			 * on the stack about to be freed for the whole of that gap.
			 * `t != this_cpu()->current` does not cover it, because the
			 * thread finishing is current on a *different* processor.
			 *
			 * That is how this was found: a page freed here was handed
			 * straight to the allocator's own concurrency test, which
			 * wrote its marker over a live return address. The panic
			 * reported a link register of 0xacce5501. */
			if (t->state == THREAD_FINISHED && t->off_cpu &&
			    t != this_cpu()->current && !is_boot_thread(t)) {
				ring_remove(t);
				victim = t;
				break;
			}
			t = t->next;
		} while (t != start);

		spin_unlock_irq(&ring_lock, flags);

		if (!victim)
			return;

		/* Freed outside the lock. Releasing a thread can release the
		 * last reference to an address space, which walks and frees
		 * page tables -- and this lock is taken on every scheduling
		 * decision the machine makes. */
		process_thread_reaped(victim);

		pmm_free_pages(virt_to_phys(victim->stack_base),
			       victim->stack_pages);
		kfree(victim);

		reaped_threads++;
	}
}

void sched_reaper_init(void)
{
	work_init(&reaper_work, run_the_reaper, NULL);
}

void sched_print_summary(void)
{
	struct thread *t = ring;

	kprintf("\nScheduler\n");
	kprintf("  slice        : %u ticks (%u ms)\n",
		SCHED_SLICE_TICKS, SCHED_SLICE_TICKS * 10);
	kprintf("  switches     : %lu, of which %lu were preemptions\n",
		switches, preemptions);

	if (!t)
		return;

	do {
		kprintf("  thread %lu     : %s, %s, %lu ticks\n", t->id, t->name,
			t->state == THREAD_RUNNING ? "running" :
			t->state == THREAD_READY   ? "ready" : "finished",
			t->ran_ticks);
		t = t->next;
	} while (t != ring);
}

/* --- The test ------------------------------------------------------------
 *
 * Three threads that never yield, running for longer than a slice. If they all
 * make progress, execution was taken away from them -- which is the claim of
 * this checkpoint and is not something a cooperative scheduler could pass.
 *
 * THE FIRST VERSION OF THIS TEST PASSED WITHOUT TESTING ANYTHING, and the way
 * it failed is worth keeping. It ran each thread for a fixed count of four
 * million increments, then asserted that the number of context switches had
 * gone up. But each thread finished inside its own slice, so it was never
 * preempted -- it ran to completion and called thread_exit(), which switches
 * *voluntarily*. Four switches for three threads, exactly what pure cooperation
 * would produce, and a green "pass" for preemption that had not happened once.
 *
 * Two things were wrong and both had to be fixed. The threads now run against
 * the clock rather than a count, so they cannot finish inside a slice however
 * fast the machine is. And preemptions are counted separately from voluntary
 * switches, so the assertion is about the thing being claimed rather than about
 * a number that both would move.
 */

static volatile u64 counter[3];
static volatile unsigned finished;
static volatile u64 test_deadline_ns;

static void counting_thread(void *arg)
{
	unsigned which = (unsigned)(uintptr_t)arg;

	/* No yield anywhere in here, deliberately, and no bound but the clock.
	 * A thread that stops on its own proves nothing about preemption. */
	while (time_monotonic_ns() < test_deadline_ns)
		counter[which]++;

	/* Atomically, and the reason is the deadline above.
	 *
	 * `volatile` stops the compiler keeping this in a register. It does
	 * nothing whatever about two processors incrementing it at once, which
	 * is a read, an add and a write that another processor can land in the
	 * middle of -- and then one of the two increments never happened.
	 *
	 * All three threads stop at the *same* deadline, so on a machine with
	 * three free processors they arrive here together by construction. The
	 * collision is not a rare interleaving this test might hit; it is the
	 * expected one. The waiter then never sees three and yields for ever.
	 *
	 * Written when there was one processor, where it was correct. */
	__atomic_add_fetch(&finished, 1, __ATOMIC_RELEASE);
}

bool sched_self_test(void)
{
	bool ok = true;
	u64 preemptions_before = preemptions;
	u64 idle_over_work_before = idle_over_work;

	counter[0] = counter[1] = counter[2] = 0;
	finished = 0;

	/* Long enough that three threads sharing it cannot each fit inside one
	 * fifty-millisecond slice, on any machine. */
	test_deadline_ns = time_monotonic_ns() + 400000000ULL;

	for (unsigned i = 0; i < 3; i++) {
		char name[THREAD_NAME_MAX] = "counter-0";

		name[8] = (char)('0' + i);
		if (!thread_create(name, counting_thread, (void *)(uintptr_t)i)) {
			kputs("  sched: could not create a thread\n");
			return false;
		}
	}

	/* Wait by yielding rather than spinning: this thread has nothing to do,
	 * and a scheduler test that burns a slice proving it is scheduled is
	 * measuring the wrong thing.
	 *
	 * Bounded, because an unbounded wait for a count that never arrives is
	 * a machine that stops with nothing printed -- the least informative
	 * failure a kernel can have, and the one this project has already
	 * decided to convert into a failed test wherever it appears. The bound
	 * is generous: the threads run against a deadline 400 ms out. */
	{
		u64 give_up = time_monotonic_ns() + 5000000000ULL;	/* 5 s */

		while (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) < 3 &&
		       time_monotonic_ns() < give_up)
			sched_yield();
	}

	if (__atomic_load_n(&finished, __ATOMIC_ACQUIRE) < 3) {
		kprintf("  sched: only %u of 3 threads reported finishing\n",
			__atomic_load_n(&finished, __ATOMIC_ACQUIRE));
		ok = false;
	}

	if (preemptions <= preemptions_before) {
		kputs("  sched: no thread was ever preempted, so the tick is not "
		      "taking execution away\n");
		ok = false;
	}

	for (unsigned i = 0; i < 3; i++)
		if (counter[i] == 0) {
			kprintf("  sched: thread %u never ran\n", i);
			ok = false;
		}

	/* And every finished thread's stack came back. A scheduler that leaks a
	 * stack per thread is a machine that dies after a few thousand of them.
	 *
	 * **Waited for, with a bound, rather than demanded at once** -- and the
	 * difference is BG-164, which turned the verification run red on a
	 * kernel that was behaving correctly.
	 *
	 * `reap` will not free a thread until `off_cpu` says the processor it
	 * was running on has finished with it, which is BG-159's fix and is
	 * what stops a stack being handed to the page allocator while another
	 * processor is still standing on it. That flag is set by *that*
	 * processor, afterwards. So a thread can be finished and not yet
	 * reapable, and calling reap once and asserting immediately is asking
	 * one processor to have already done something another one owes it.
	 *
	 * On an idle machine the gap is too short to see. At eight processors
	 * under load it opened often enough to fail two runs in twelve, on a
	 * kernel where nothing was wrong.
	 *
	 * The assertion still means what it meant: a stack that genuinely
	 * leaks is never reaped, so it is still here when the deadline passes.
	 * What changed is that "not yet" and "not ever" stopped being the same
	 * answer. */
	{
		u64 deadline = time_monotonic_ns() + 500000000ull;
		bool waiting = true;

		while (waiting) {
			struct thread *t;

			reap();

			waiting = false;
			t = ring;

			if (t) do {
				if (t->state == THREAD_FINISHED) {
					waiting = true;
					break;
				}
				t = t->next;
			} while (t != ring);

			if (!waiting)
				break;

			if (time_monotonic_ns() > deadline) {
				kputs("  sched: a finished thread was still "
				      "not reaped half a second later\n");
				ok = false;
				break;
			}

			/* Yielded rather than spun: what is being waited for is
			 * another processor reaching the end of a context
			 * switch, and spinning here is a processor refusing to
			 * let that happen on its own. */
			sched_yield();
		}
	}

	/* And nothing wrote past its vector area while all that was running.
	 *
	 * Asserted rather than printed. This was BG-146: sixteen bytes past the
	 * end of a 512-byte save area, into the next field of the same
	 * structure, on one architecture only -- and it had been there since
	 * the vector unit was enabled. Nothing failed until a field that
	 * mattered happened to be sitting there. */
	if (vector_overruns) {
		kprintf("  sched: %lu context switches wrote past a thread's "
			"vector save area\n", (unsigned long)vector_overruns);
		ok = false;
	}

	/* And that no processor sat in its idle thread while there was work.
	 *
	 * Three threads have just been run to a deadline, so for the whole of
	 * that window there was something ready on this processor. An idle
	 * thread scheduled during it is a slice thrown away -- and because
	 * idle_loop waits for an interrupt, it is not a short slice.
	 *
	 * This is here because exactly that happened and every test in this
	 * file passed anyway: the machine was correct and about half as fast,
	 * and the only thing that noticed was seven disk paths in the
	 * verification run timing out. See BG-158. */
	if (idle_over_work != idle_over_work_before) {
		kprintf("  sched: an idle thread was scheduled %lu time(s) while another thread was ready to run\n",
			idle_over_work - idle_over_work_before);
		ok = false;
	}
	return ok;
}
