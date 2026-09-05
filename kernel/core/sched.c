#include <recon/kernel/sched.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/console.h>
#include <recon/kernel/time.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/panic.h>

#define THREAD_STACK_PAGES 4	/* 16KB, which is generous for a kernel thread */

static struct thread *current;
static struct thread *ring;	/* any member; the ring is circular */
static u64 next_id;
static u64 switches;

/* Counted apart from `switches`, and the distinction is the whole point of the
 * self-test below: a voluntary switch proves cooperation works, and only a
 * preemption proves the tick is taking execution away from code that did not
 * offer it. A test that counts both together passes on either. */
static u64 preemptions;

/* The thread the kernel was already running as when the scheduler started.
 * It is not created, it is *adopted* -- it has a stack and a context already,
 * and the first switch away from it is what fills in its stack pointer. Without
 * this there would be no thread to switch away from, and the first switch would
 * have nowhere to save the state it was replacing. */
static struct thread boot_thread;

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
	return current;
}

void sched_init(void)
{
	kmemset(&boot_thread, 0, sizeof(boot_thread));
	kstrlcpy(boot_thread.name, "boot", sizeof(boot_thread.name));
	boot_thread.id = 0;
	boot_thread.state = THREAD_RUNNING;
	boot_thread.slice_left = SCHED_SLICE_TICKS;

	ring = 0;
	ring_insert(&boot_thread);
	current = &boot_thread;
	next_id = 1;
	switches = 0;
	preemptions = 0;
}

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg)
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
	t->state = THREAD_READY;
	t->slice_left = SCHED_SLICE_TICKS;
	kstrlcpy(t->name, name, sizeof(t->name));

	t->stack_pointer = arch_thread_stack_init(
		(u8 *)t->stack_base + THREAD_STACK_PAGES * PAGE_SIZE,
		entry, arg);

	ring_insert(t);
	return t;
}

/* The next thread that is willing to run. Walks the ring from the current
 * thread; if nothing else is ready, the current thread keeps going, and if the
 * current thread has finished and nothing else is ready, there is nothing left
 * to run at all. */
static struct thread *pick_next(void)
{
	struct thread *t = current->next;

	while (t != current) {
		if (t->state == THREAD_READY)
			return t;
		t = t->next;
	}

	return (current->state == THREAD_RUNNING) ? current : 0;
}

void sched_switch(void)
{
	struct thread *prev = current;
	struct thread *next = pick_next();

	if (!next) {
		/* Everything has finished. The caller is a thread that is about
		 * to stop existing, so there is nowhere to go. */
		panic("sched: nothing left to run");
	}

	if (next == prev) {
		prev->slice_left = SCHED_SLICE_TICKS;
		return;
	}

	if (prev->state == THREAD_RUNNING)
		prev->state = THREAD_READY;

	next->state = THREAD_RUNNING;
	next->slice_left = SCHED_SLICE_TICKS;
	current = next;
	switches++;

	/* After this returns, the stack is the *other* thread's -- so `prev` is
	 * whichever thread is now resuming, not the one that called. Anything
	 * that has to happen for the outgoing thread must happen above this
	 * line. */
	arch_context_switch(&prev->stack_pointer, next->stack_pointer);
}

void sched_yield(void)
{
	sched_switch();
}

bool sched_tick(void)
{
	if (!current)
		return false;

	current->ran_ticks++;

	if (current->slice_left > 0)
		current->slice_left--;

	/* True means "switch on the way out of this interrupt". The switch is
	 * not done here because the caller is the architecture's interrupt path,
	 * and it may have work to finish -- acknowledging the interrupt
	 * controller, for one -- that must happen before the stack changes. */
	if (current->slice_left == 0) {
		preemptions++;
		return true;
	}

	return false;
}

void thread_exit(void)
{
	struct thread *dead = current;

	dead->state = THREAD_FINISHED;

	/* The stack cannot be freed here: this code is standing on it. It is
	 * left for whoever notices the thread is finished, which is the reaper
	 * below -- run from another thread, on another stack. */
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
static void reap(void)
{
	bool removed;

	do {
		struct thread *t = ring;
		struct thread *start = ring;

		removed = false;

		if (!t)
			return;

		do {
			if (t->state == THREAD_FINISHED &&
			    t != current && t != &boot_thread) {
				ring_remove(t);
				pmm_free_pages(virt_to_phys(t->stack_base),
					       t->stack_pages);
				kfree(t);
				removed = true;
				break;
			}
			t = t->next;
		} while (t != start);
	} while (removed);
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

	finished++;
}

bool sched_self_test(void)
{
	bool ok = true;
	u64 preemptions_before = preemptions;

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
	 * measuring the wrong thing. */
	while (finished < 3)
		sched_yield();

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

	reap();

	/* And every finished thread's stack came back. A scheduler that leaks a
	 * stack per thread is a machine that dies after a few thousand of them. */
	{
		struct thread *t = ring;

		if (t) do {
			if (t->state == THREAD_FINISHED) {
				kputs("  sched: a finished thread was not reaped\n");
				ok = false;
				break;
			}
			t = t->next;
		} while (t != ring);
	}

	return ok;
}
