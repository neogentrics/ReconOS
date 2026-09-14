/* Suspend: the registry, the refusal, and the unwinding.
 *
 * See suspend.h for why this is a registry at all. What is here is the ordering
 * and the failure path, which are the two things a suspend framework is for --
 * the sleep write itself is one instruction and is the easy part.
 */

#include <recon/kernel/suspend.h>

#include <recon/kernel/acpi.h>
#include <recon/kernel/aml.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/kstring.h>
#include <recon/kernel/lock.h>
#include <recon/kernel/console.h>

struct entry {
	const char *name;
	const struct suspend_ops *ops;
	bool quiesced;
};

static struct entry table[SUSPEND_MAX];
static unsigned count;
static unsigned refused_declares;

/* Taken around declaration and around a suspend. A bus walk that finds two
 * devices on two processors would otherwise write the same slot twice. */
static struct spinlock lock = SPINLOCK_INIT("suspend");

void suspend_declare(const char *name, const struct suspend_ops *ops)
{
	u64 flags = spin_lock_irq(&lock);

	if (count >= SUSPEND_MAX) {
		/* Counted rather than silently dropped. A device missing from
		 * this table is a device nobody will notice cannot come back,
		 * which is the one failure this file exists to prevent -- so
		 * the cap being hit has to be visible in the summary. */
		refused_declares++;
		spin_unlock_irq(&lock, flags);
		return;
	}

	table[count].name     = name;
	table[count].ops      = ops;
	table[count].quiesced = false;
	count++;

	spin_unlock_irq(&lock, flags);
}

bool suspend_ready(char *why, u64 len)
{
	unsigned i;
	u64 used = 0;
	bool ok = true;

	if (why && len)
		why[0] = '\0';

	for (i = 0; i < count; i++) {
		if (table[i].ops && table[i].ops->quiesce && table[i].ops->resume)
			continue;

		ok = false;

		/* The names, joined, up to whatever room there is. A truncated
		 * list is still worth more than a number: the first two names
		 * are usually the whole answer. */
		if (why && len) {
			const char *n = table[i].name ? table[i].name : "?";

			if (used && used + 2 < len) {
				why[used++] = ',';
				why[used++] = ' ';
				why[used] = '\0';
			}

			while (*n && used + 1 < len)
				why[used++] = *n++;

			why[used] = '\0';
		}
	}

	/* A cap that was hit means there are devices this table never heard
	 * about, and it cannot name them -- but it must not answer "ready". */
	if (refused_declares)
		ok = false;

	return ok;
}

/* Stop everything, newest first.
 *
 * **And put back what was stopped if one of them refuses.** That unwinding is
 * the reason this is a loop with state rather than a loop: a caller told "no"
 * whose disk controller is still stopped has been handed a worse machine than
 * the one it asked about.
 */
static bool quiesce_all(struct entry *t, unsigned n, const char **who)
{
	unsigned i;

	*who = 0;

	for (i = n; i > 0; i--) {
		struct entry *e = &t[i - 1];

		if (!e->ops || !e->ops->quiesce)
			continue;

		if (e->ops->quiesce()) {
			e->quiesced = true;
			continue;
		}

		*who = e->name;

		/* Back up over the ones already stopped, in the order they
		 * would have been resumed in. */
		for (; i <= n; i++) {
			struct entry *back = &t[i - 1];

			if (!back->quiesced)
				continue;

			if (back->ops->resume)
				back->ops->resume();

			back->quiesced = false;
		}

		return false;
	}

	return true;
}

/* Bring everything back, oldest first, and say what did not come.
 *
 * Nothing is abandoned part way. A device that fails to resume is reported and
 * the rest are still resumed: the machine is awake either way, and stopping
 * here would lose the devices after it as well for no gain.
 */
static unsigned resume_all(struct entry *t, unsigned n)
{
	unsigned i;
	unsigned lost = 0;

	for (i = 0; i < n; i++) {
		struct entry *e = &t[i];

		if (!e->quiesced)
			continue;

		if (e->ops->resume && e->ops->resume()) {
			e->quiesced = false;
			continue;
		}

		kprintf("  suspend: %s did not come back\n",
			e->name ? e->name : "a device");
		e->quiesced = false;
		lost++;
	}

	return lost;
}

enum suspend_result suspend_to_ram(void)
{
	const struct aml_state *a = aml();
	struct acpi_fadt_facts fadt;
	char why[128];
	const char *who = 0;
	u64 flags;
	enum suspend_result r;

	/* Asked before anything is stopped, and that order is the point: a
	 * machine that cannot suspend must find out while it is still whole.
	 *
	 * `why` is filled and not printed here -- the caller decides whether a
	 * refusal is worth saying out loud, and the boot summary already names
	 * them on every boot. */
	if (!suspend_ready(why, sizeof(why)))
		return SUSPEND_NOT_DECLARED;

	if (!a || !a->sleep[3].have)
		return SUSPEND_NO_STATE;

	if (!acpi_fadt(&fadt) || !fadt.pm1a_control)
		return SUSPEND_NO_REGISTER;

	/* **Everything that can be known while the machine is whole, first.**
	 *
	 * The architecture is asked whether it could actually sleep and wake
	 * before a single device is stopped. Today it says no on both -- there
	 * is no armed wake source, and a machine told to sleep with nothing to
	 * wake it does sleep, and does not come back, and somebody has to hold
	 * the power button.
	 *
	 * Refusing is strictly better than attempting, and refusing *here* is
	 * better than refusing after the disk controller has been stopped. */
	r = arch_suspend_possible();
	if (r != SUSPEND_OK)
		return r;

	flags = spin_lock_irq(&lock);

	if (!quiesce_all(table, count, &who)) {
		spin_unlock_irq(&lock, flags);
		kprintf("  suspend: %s would not stop, so nothing was\n",
			who ? who : "a device");
		return SUSPEND_REFUSED;
	}

	r = arch_suspend_to_ram((u16)a->sleep[3].typ_a,
				(u16)a->sleep[3].typ_b);

	resume_all(table, count);
	spin_unlock_irq(&lock, flags);

	return r;
}

void suspend_print_summary(void)
{
	char why[128];

	if (!count) {
		kputs("  suspend      : nothing has declared itself\n");
		return;
	}

	if (suspend_ready(why, sizeof(why))) {
		kprintf("  suspend      : %u device(s) can come back\n", count);
		return;
	}

	/* Named, not counted. "Three devices cannot resume" sends nobody
	 * anywhere; the names are the whole of the next piece of work. */
	kprintf("  suspend      : %u device(s) declared, and these cannot come "
		"back yet: %s\n", count, why);

	if (refused_declares)
		kprintf("               : and %u more could not be recorded at "
			"all -- SUSPEND_MAX is too small, so this list is "
			"short as well as long\n", refused_declares);
}

/* --- the self-test ---------------------------------------------------------
 *
 * Mostly about the unwind, because that is where a suspend framework is either
 * right or a machine that hangs with its disk switched off. The ordering is
 * easy to write and easy to check; putting back what was already stopped when
 * the third device refuses is neither.
 *
 * It runs on its own table through the real functions. A copy of the logic
 * written to be tested would pass while the real one was wrong, which is a
 * story this register tells more often than any other.
 */
static unsigned log_at;
static char     log_of[8];

static void note(char c)
{
	if (log_at < sizeof(log_of) - 1)
		log_of[log_at++] = c;
}

static bool refuse_b;

static bool a_quiesce(void) { note('a'); return true; }
static bool a_resume(void)  { note('A'); return true; }
static bool b_quiesce(void) { note('b'); return !refuse_b; }
static bool b_resume(void)  { note('B'); return true; }
static bool c_quiesce(void) { note('c'); return true; }
static bool c_resume(void)  { note('C'); return true; }

static const struct suspend_ops ops_a = { a_quiesce, a_resume };
static const struct suspend_ops ops_b = { b_quiesce, b_resume };
static const struct suspend_ops ops_c = { c_quiesce, c_resume };

/* Local, like aml.c's, and for the reason aml.c gives: two call sites in one
 * file is not a reason to grow kstring.h, which has kmemcmp and no string
 * compare. */
static bool same_string(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}

	return *a == *b;
}

static bool said(const char *want, const char *what)
{
	log_of[log_at] = '\0';

	if (same_string(log_of, want))
		return true;

	kprintf("  suspend: %s went '%s' and should have gone '%s'\n",
		what, log_of, want);
	return false;
}

bool suspend_self_test(void)
{
	struct entry t[3];
	const char *who;
	bool ok = true;

	t[0].name = "a"; t[0].ops = &ops_a; t[0].quiesced = false;
	t[1].name = "b"; t[1].ops = &ops_b; t[1].quiesced = false;
	t[2].name = "c"; t[2].ops = &ops_c; t[2].quiesced = false;

	/* 1. Everything stops, newest first, and comes back oldest first.
	 *
	 * Both orders in one string, because a framework that stops in the
	 * right order and resumes in the same one is wrong in a way that only
	 * shows up on the machine where a device sits on another. */
	refuse_b = false;
	log_at = 0;

	if (!quiesce_all(t, 3, &who)) {
		kputs("  suspend: a device refused when none was told to\n");
		return false;
	}

	resume_all(t, 3);

	if (!said("cbaABC", "stopping and starting"))
		ok = false;

	/* 2. **The unwind.** `b` refuses, so `c` -- already stopped -- has to
	 * be put back before the refusal is returned.
	 *
	 * The expected string is the whole assertion: c stops, b is asked and
	 * says no, C comes back. `a` is never reached, and that matters too --
	 * a framework that resumed everything rather than what it stopped
	 * would go 'cbCA' and would be resuming a device that was never
	 * stopped. */
	refuse_b = true;
	log_at = 0;
	t[0].quiesced = t[1].quiesced = t[2].quiesced = false;

	if (quiesce_all(t, 3, &who)) {
		kputs("  suspend: the refusing device was allowed to stop\n");
		ok = false;
	} else if (!said("cbC", "unwinding after a refusal")) {
		ok = false;
	}

	if (who && !same_string(who, "b")) {
		kprintf("  suspend: blamed '%s' and it was 'b'\n", who);
		ok = false;
	}

	/* And nothing is left stopped. A refusal that leaves a device down is
	 * the failure this whole path exists to avoid, and it is checked as
	 * state rather than inferred from the order string. */
	{
		unsigned i;

		for (i = 0; i < 3; i++) {
			if (!t[i].quiesced)
				continue;

			kprintf("  suspend: %s was left stopped after a "
				"refusal\n", t[i].name);
			ok = false;
		}
	}

	/* 3. Readiness and its reason have to agree with each other.
	 *
	 * Checked against the real table, whatever this machine happens to
	 * have declared: ready means the list of names is empty, and not ready
	 * means it is not. A readiness answer that came with no reason would
	 * send whoever read it nowhere. */
	{
		char why[128];
		bool ready = suspend_ready(why, sizeof(why));

		if (ready != (why[0] == '\0')) {
			kprintf("  suspend: says %s and names '%s'\n",
				ready ? "ready" : "not ready", why);
			ok = false;
		}

		/* 4. **And the refusal happens before anything is stopped.**
		 *
		 * Only entered when the machine is already known not to be
		 * ready, so this can never suspend a machine from inside a
		 * self-test -- and that guard is what keeps this correct on the
		 * day the drivers gain ops, rather than a precaution about
		 * today.
		 *
		 * Without this, `suspend_to_ram` had no caller at all and the
		 * order of its four checks was reasoning rather than a result. */
		if (!ready) {
			enum suspend_result r = suspend_to_ram();

			if (r != SUSPEND_NOT_DECLARED) {
				kprintf("  suspend: refused with %d, and a "
					"machine that cannot resume must "
					"refuse with %d -- before it stops "
					"anything\n",
					(int)r, (int)SUSPEND_NOT_DECLARED);
				ok = false;
			}
		}
	}

	return ok;
}
