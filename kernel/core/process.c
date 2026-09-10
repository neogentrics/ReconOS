/* The process table. See process.h for what this is and what it stops short of.
 *
 * A fixed table rather than a list, for the same reason the block layer and the
 * processor list use one: the cap is a number somebody can see, running out of
 * them is a refusal with a message rather than an allocation failure three
 * layers down, and nothing here is on a path where thirty-two is limiting.
 */
#include <recon/kernel/process.h>
#include <recon/kernel/vfs.h>

#include <recon/kernel/addrspace.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/user.h>

static struct process table[PROCESS_MAX];
static struct spinlock table_lock = SPINLOCK_INIT("process");

/* Never reused, and never zero.
 *
 * Zero means "no process" everywhere in this file, so a real process must not
 * have it. And a number that is reused is a number that identifies two things
 * over a machine's life -- so a message sent to a process that has since ended
 * would reach whatever took its place. */
static u32 next_id = 1;

void process_init(void)
{
	unsigned i;

	spin_init(&table_lock, "process");

	for (i = 0; i < PROCESS_MAX; i++)
		table[i].state = PROCESS_FREE;
}

struct process *process_create(const char *name, u32 parent, u32 uid, u32 gid)
{
	struct process *p = NULL;
	unsigned i;
	u64 flags = spin_lock_irq(&table_lock);

	for (i = 0; i < PROCESS_MAX; i++)
		if (table[i].state == PROCESS_FREE) {
			p = &table[i];
			break;
		}

	if (p) {
		kmemset(p, 0, sizeof(*p));

		p->state  = PROCESS_RUNNING;
		p->id     = next_id++;
		p->parent = parent;
		p->uid    = uid;
		p->gid    = gid;
		p->personality = &personality_recon;

		kstrlcpy(p->name, name ? name : "unnamed", sizeof(p->name));
	}

	spin_unlock_irq(&table_lock, flags);

	/* Its first three descriptors, outside the table lock.
	  *
	  * Outside because opening a file takes the descriptor lock and may take
	  * the heap, and holding the process table across either of those puts
	  * two locks in an order nothing else in the kernel uses -- which is how
	  * a deadlock gets built out of two correct pieces. The process is
	  * already RUNNING and visible here, and that is safe: a process with no
	  * descriptors yet is a process whose threads have not started. */
	if (p)
		fd_open_standard(p);

	return p;
}

struct process *process_by_id(u32 id)
{
	unsigned i;

	if (!id)
		return NULL;

	for (i = 0; i < PROCESS_MAX; i++)
		if (table[i].state != PROCESS_FREE && table[i].id == id)
			return &table[i];

	return NULL;
}

struct process *process_of(const struct thread *t)
{
	if (!t || !t->process)
		return NULL;

	return process_by_id(t->process);
}

void process_attach(struct process *p, struct thread *t)
{
	u64 flags;

	if (!p || !t)
		return;

	flags = spin_lock_irq(&table_lock);

	t->process = p->id;
	p->threads++;

	spin_unlock_irq(&table_lock, flags);
}

void process_thread_ended(struct thread *t, i64 code)
{
	struct process *p;
	u64 flags;

	if (!t || !t->process)
		return;

	flags = spin_lock_irq(&table_lock);

	p = NULL;
	{
		unsigned i;

		for (i = 0; i < PROCESS_MAX; i++)
			if (table[i].state == PROCESS_RUNNING &&
			    table[i].id == t->process) {
				p = &table[i];
				break;
			}
	}

	if (p) {
		/* The *first* exit code is kept, not the last.
		 *
		 * A program that fails and then tears down its remaining
		 * threads would otherwise report whatever the last one happened
		 * to return, which is usually success -- and a failure reported
		 * as success is worse than no report. */
		if (p->exit_code == 0)
			p->exit_code = code;

		if (p->threads)
			p->threads--;

		/* Ended when the last thread is, not when any thread is. A
		 * program with a worker that finishes early has not ended. */
		if (p->threads == 0)
			p->state = PROCESS_ENDED;
	}

	t->process = 0;

	/* Whether this was the last thread, decided while the lock is held and
	  * acted on after it is dropped. Closing a descriptor commits to a disk;
	  * doing that with the process table held would stop every other
	  * processor in the machine from creating a thread for the length of a
	  * write to storage. */
	{
		struct process *ended = (p && p->state == PROCESS_ENDED) ? p : NULL;

		spin_unlock_irq(&table_lock, flags);

		if (ended)
			fd_close_all(ended);
	}
}

void process_set_space(struct process *p, struct addrspace *as)
{
	u64 flags;

	if (!p)
		return;

	flags = spin_lock_irq(&table_lock);
	p->space = addrspace_hold(as);
	spin_unlock_irq(&table_lock, flags);
}

bool process_reap(u32 id, i64 *code)
{
	unsigned i;
	bool found = false;
	struct addrspace *space = NULL;
	u64 flags = spin_lock_irq(&table_lock);

	for (i = 0; i < PROCESS_MAX; i++)
		if (table[i].state == PROCESS_ENDED && table[i].id == id) {
			if (code)
				*code = table[i].exit_code;

			/* The address space goes when the status is collected,
			 * not when the last thread ended: a thread that has just
			 * called into the kernel to exit is still running on a
			 * stack reached through this map. */
			space = table[i].space;
			table[i].space = NULL;

			table[i].state = PROCESS_FREE;
			found = true;
			break;
		}

	spin_unlock_irq(&table_lock, flags);

	/* Outside the lock: releasing the last reference walks and frees page
	 * tables, and this lock is taken on the scheduler's path. */
	addrspace_release(space);

	return found;
}

unsigned process_count(void)
{
	unsigned i, n = 0;

	for (i = 0; i < PROCESS_MAX; i++)
		if (table[i].state != PROCESS_FREE)
			n++;

	return n;
}

struct process *process_at(unsigned index)
{
	unsigned i, n = 0;

	for (i = 0; i < PROCESS_MAX; i++)
		if (table[i].state != PROCESS_FREE) {
			if (n == index)
				return &table[i];
			n++;
		}

	return NULL;
}

void process_print_summary(void)
{
	unsigned i, n = process_count();

	kprintf("\nProcesses\n");
	kprintf("  table        : %u of %u in use\n", n, (unsigned)PROCESS_MAX);

	for (i = 0; i < n; i++) {
		struct process *p = process_at(i);

		if (!p)
			continue;

		kprintf("  %-12s : id %u, parent %u, uid %u, %u thread%s, %s\n",
			p->name, p->id, p->parent, p->uid, p->threads,
			p->threads == 1 ? "" : "s",
			p->state == PROCESS_ENDED ? "ended" : "running");
	}

	/* Said plainly, beside the table, so that nobody reads a list of
	 * processes and concludes they are isolated from each other. */
	{
		unsigned own = 0;

		for (i = 0; i < PROCESS_MAX; i++)
			if (table[i].state != PROCESS_FREE && table[i].space)
				own++;

		/* Said plainly beside the table. It read "shared -- processes do
		 * not have address spaces of their own yet" until they did, and a
		 * line like that is worth changing on the same day rather than
		 * being found later by somebody who believed it. */
		kprintf("  memory       : %u with an address space of its own, "
			"%u sharing the kernel's\n", own, n - own);
	}
}

/* --- the self-test --------------------------------------------------------
 *
 * The interesting property is not that a table can hold entries. It is that a
 * process ends when its *last* thread does and not before, because that is the
 * rule everything above will depend on and the one an obvious implementation
 * gets wrong by ending the process with the first thread that exits.
 */
static volatile unsigned test_started;

static void quiet_thread(void *arg)
{
	(void)arg;
	__atomic_add_fetch(&test_started, 1, __ATOMIC_RELEASE);
}

bool process_self_test(void)
{
	struct process *p = process_create("test", 0, UID_NOBODY, UID_NOBODY);
	struct thread *a, *b;
	i64 code = 0;
	bool ok = true;

	if (!p) {
		kputs("  process: the table had no room\n");
		return false;
	}

	if (!p->id) {
		kputs("  process: a process was given identifier zero, which "
		      "means 'none' everywhere else\n");
		return false;
	}

	test_started = 0;

	a = thread_create("proc-a", quiet_thread, 0);
	b = thread_create("proc-b", quiet_thread, 0);

	if (!a || !b) {
		kputs("  process: could not create its threads\n");
		return false;
	}

	process_attach(p, a);
	process_attach(p, b);

	if (p->threads != 2) {
		kprintf("  process: two threads attached and it counted %u\n",
			p->threads);
		ok = false;
	}

	/* One thread ends. The process must not. */
	process_thread_ended(a, 0);

	if (p->state != PROCESS_RUNNING) {
		kputs("  process: it ended when its first thread did, not its "
		      "last\n");
		ok = false;
	}

	process_thread_ended(b, 7);

	if (p->state != PROCESS_ENDED) {
		kputs("  process: its last thread ended and it did not\n");
		ok = false;
	}

	/* And the status survives until somebody collects it. */
	if (!process_reap(p->id, &code)) {
		kputs("  process: an ended process could not be collected\n");
		ok = false;
	} else if (code != 7) {
		kprintf("  process: it exited with 7 and reported %ld\n",
			(long)code);
		ok = false;
	}

	/* Collected twice is refused: a status is delivered once, and a second
	 * caller must not be told a process ended that it has already reaped. */
	if (process_reap(p->id, &code)) {
		kputs("  process: it was collected twice\n");
		ok = false;
	}

	return ok;
}
