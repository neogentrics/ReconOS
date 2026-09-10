/* See irq.h, particularly the part about what a handler may do. */
#include <recon/kernel/irq.h>
#include <recon/kernel/console.h>
#include <recon/kernel/arch.h>

struct line {
	void (*fn)(void *);
	void *arg;
	const char *name;

	/* Written by the dispatch, which runs with interrupts off on this
	 * processor but can run on any of them. Not atomic: a lost count is a
	 * wrong number in a summary, and taking a lock on the interrupt path to
	 * protect a statistic would be a far worse trade. Said out loud so
	 * nobody later reads these as exact. */
	u64 taken;
	u64 unclaimed;
};

static struct line lines[IRQ_LINES];

bool irq_register(unsigned line, void (*fn)(void *), void *arg,
		  const char *name)
{
	u64 flags;
	bool ok = false;

	if (line >= IRQ_LINES || !fn)
		return false;

	/* Interrupts off around the check-and-set, because the thing being
	 * guarded against is this line firing between them -- which would run a
	 * handler through a half-installed record. Not a lock: there is nothing
	 * to contend with, since registration happens once at startup and the
	 * only other writer is the interrupt this masks. */
	flags = arch_irq_save();

	if (!lines[line].fn) {
		lines[line].fn = fn;
		lines[line].arg = arg;
		lines[line].name = name;
		ok = true;
	}

	arch_irq_restore(flags);

	if (!ok)
		kprintf("irq: line %u is already %s's, so %s did not get it\n",
			line, lines[line].name ? lines[line].name : "?",
			name ? name : "?");

	return ok;
}

void irq_release(unsigned line)
{
	u64 flags;

	if (line >= IRQ_LINES)
		return;

	flags = arch_irq_save();
	lines[line].fn = 0;
	lines[line].arg = 0;
	lines[line].name = 0;
	arch_irq_restore(flags);
}

bool irq_dispatch(unsigned line)
{
	struct line *l;

	if (line >= IRQ_LINES)
		return false;

	l = &lines[line];
	l->taken++;

	if (!l->fn) {
		l->unclaimed++;
		return false;
	}

	l->fn(l->arg);
	return true;
}

void irq_print_summary(void)
{
	unsigned i;
	bool any = false;

	kprintf("\nInterrupt lines\n");

	for (i = 0; i < IRQ_LINES; i++) {
		struct line *l = &lines[i];

		if (!l->taken && !l->fn)
			continue;

		any = true;
		kprintf("  %-2u %-12s %llu taken", i,
			l->name ? l->name : "(nobody)",
			(unsigned long long)l->taken);

		/* The number worth printing. A line that keeps firing with
		 * nobody to take it is a device nobody turned off, and the
		 * machine cannot get out of that on its own -- it will spend
		 * every cycle entering and leaving the same handler-less
		 * interrupt. It looks exactly like a hang. */
		if (l->unclaimed)
			kprintf(", %llu with nobody to take them",
				(unsigned long long)l->unclaimed);

		kprintf("\n");
	}

	if (!any)
		kputs("  none claimed, none seen\n");
}

/* --- the self-test --------------------------------------------------------
 *
 * What is checked here is the bookkeeping, not the delivery. Making a real ISA
 * line fire on demand means having the device that owns it, and there is no such
 * device on every machine this boots on -- so a test that tried would pass on
 * the rig and report a failure on a machine that simply has no floppy
 * controller.
 *
 * The claim being tested is narrower and is the one that has a wrong answer
 * worth catching: **a second registration must be refused rather than take the
 * line**. Silently replacing the first is how one driver stops working when an
 * unrelated one is added, with nothing anywhere saying so.
 */
static void count_first(void *arg)
{
	(*(unsigned *)arg)++;
}

static void count_second(void *arg)
{
	(*(unsigned *)arg)++;
}

bool irq_self_test(void)
{
	static unsigned first_ran, second_ran;
	bool ok = true;
	unsigned spare = IRQ_LINES - 1;	/* 15: the second ATA channel, which
					 * nothing here drives */

	first_ran = second_ran = 0;

	if (lines[spare].fn) {
		/* Something took it after all. Not a failure of this code, and
		 * saying so beats reporting a failure somebody would go looking
		 * for in the wrong file. */
		kprintf("  irq: line %u is taken by %s, so the test did not "
			"run\n", spare, lines[spare].name);
		return true;
	}

	/* A handler of null must be refused: dispatch would call it. */
	if (irq_register(spare, 0, 0, "null")) {
		kputs("  irq: a null handler was accepted, which dispatch "
		      "would call\n");
		irq_release(spare);
		ok = false;
	}

	if (!irq_register(spare, count_first, &first_ran, "test-first")) {
		kputs("  irq: could not claim a free line\n");
		return false;
	}

	/* The one that matters. */
	if (irq_register(spare, count_second, &second_ran, "test-second")) {
		kputs("  irq: a second handler took a line that was already "
		      "claimed, so adding a driver can silently stop another "
		      "one\n");
		ok = false;
	}

	if (!irq_dispatch(spare)) {
		kputs("  irq: dispatching a claimed line said nobody wanted "
		      "it\n");
		ok = false;
	}

	if (first_ran != 1) {
		kprintf("  irq: the registered handler ran %u times, not "
			"once\n", first_ran);
		ok = false;
	}

	if (second_ran) {
		kputs("  irq: the refused handler ran anyway\n");
		ok = false;
	}

	irq_release(spare);

	/* And a released line is unclaimed again -- which is what makes
	 * dispatch's answer worth anything. */
	if (irq_dispatch(spare)) {
		kputs("  irq: a released line still says somebody wants it\n");
		ok = false;
	}

	if (first_ran != 1) {
		kputs("  irq: a released handler ran\n");
		ok = false;
	}

	/* Out of range, both ends. */
	if (irq_register(IRQ_LINES, count_first, &first_ran, "past-the-end")) {
		kputs("  irq: a line past the end was accepted\n");
		ok = false;
	}

	if (irq_dispatch(IRQ_LINES)) {
		kputs("  irq: dispatching past the end found somebody\n");
		ok = false;
	}

	return ok;
}
