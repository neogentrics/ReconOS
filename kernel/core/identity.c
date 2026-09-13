/* See identity.h, particularly the part about the first matching class. */
#include <recon/kernel/identity.h>
#include <recon/kernel/process.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/smp.h>
#include <recon/kernel/console.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/user.h>
#include <recon/kernel/time.h>
#include <recon/kernel/rootfs.h>

static u64 allowed, refused;

/* How many capabilities have been given up. Here with the other counters
 * rather than beside the code that increments it, because the summary that
 * prints it comes earlier in this file than that code does. */
static u64 drops;

static struct process *asking(void)
{
	struct thread *t = this_cpu()->current;

	return t ? process_of(t) : NULL;
}

u32 identity_uid(void)
{
	struct process *p = asking();

	/* No process means the kernel itself. See the header: it is allowed
	 * everything, and giving it an identity of its own would be a second
	 * account that can do everything. */
	return p ? p->uid : UID_KERNEL;
}

u32 identity_gid(void)
{
	struct process *p = asking();

	return p ? p->gid : UID_KERNEL;
}

bool identity_may_as(u32 uid, u32 gid, u32 mode, u32 owner, u32 group,
		     enum access_want want)
{
	u32 bits;

	if (!want)
		return true;		/* asking for nothing is always fine */

	/* The kernel, and root. Before the class check rather than as a fourth
	 * class, because "may do anything" is not a set of permission bits. */
	if (uid == UID_KERNEL)
		return true;

	/* --- the first matching class, and only it ----------------------
	 *
	 * An `if/else if/else`, deliberately, and not three tests or-ed
	 * together. Mode 0004 on a file you own means the owner may *not* read
	 * it, and an or would let them. See the header.
	 */
	if (uid == owner)
		bits = (mode >> 6) & 7;
	else if (gid == group)
		bits = (mode >> 3) & 7;
	else
		bits = mode & 7;

	/* Every bit that was asked for, not any of them. A request to open for
	 * reading and writing is refused unless both are granted -- a caller
	 * handed a descriptor that can do half of what it asked for would find
	 * out at the first write, with the file already open. */
	if ((want & ACCESS_READ)  && !(bits & 4))
		return false;
	if ((want & ACCESS_WRITE) && !(bits & 2))
		return false;
	if ((want & ACCESS_EXEC)  && !(bits & 1))
		return false;

	return true;
}

bool identity_may(u32 mode, u32 owner, u32 group, enum access_want want)
{
	/* The override is a *capability* here, where identity_may_as has it as
	 * a uid. That is the difference between the two: the pure one answers
	 * "would this identity be allowed by these bits", and this one answers
	 * "may whoever is running do it" -- which a process holding
	 * CAP_FILE_OVERRIDE may, and a process that has dropped it may not,
	 * whatever its uid. */
	bool ok = capable(CAP_FILE_OVERRIDE) ||
		  identity_may_as(identity_uid(), identity_gid(), mode, owner,
				  group, want);

	if (ok)
		allowed++;
	else
		refused++;

	return ok;
}

enum access_want identity_want_from_open(unsigned open_flags)
{
	unsigned want = 0;

	if (open_flags & OPEN_READ)
		want |= ACCESS_READ;

	if (open_flags & OPEN_WRITE)
		want |= ACCESS_WRITE;

	return (enum access_want)want;
}

void identity_print_summary(void)
{
	kprintf("\nPermissions\n");

	/* Printed even at zero, because zero is the interesting number: it
	 * means nothing has been checked, which is what this row said for
	 * months. */
	kprintf("  checked      : %llu allowed, %llu refused\n",
		(unsigned long long)allowed, (unsigned long long)refused);

	kprintf("  running as   : uid %u, gid %u\n", identity_uid(),
		identity_gid());

	{
		u64 held = capability_held();
		static const u64 all[] = { CAP_FILE_OVERRIDE, CAP_RAW_DISK,
					   CAP_SHUTDOWN };
		unsigned i;

		kputs("  holding      :");

		for (i = 0; i < sizeof(all) / sizeof(all[0]); i++)
			if (held & all[i])
				kprintf(" %s", capability_name(all[i]));

		if (!held)
			kputs(" nothing");

		kprintf("  (%llu dropped so far)\n",
			(unsigned long long)drops);
	}

	/* The gap, stated where somebody reading the summary will see it. */
	kputs("  not checked  : the directories above a path, and set-user-id"
	      "\n");
}

/* --- Capabilities ---------------------------------------------------------
 *
 * See identity.h: dropped, never gained, and the kernel holds everything.
 */
u64 capability_held(void)
{
	struct process *p = asking();

	/* No process is the kernel, which holds everything. Same rule as the
	 * identity above it and for the same reason: it is the thing doing the
	 * enforcing. */
	return p ? p->caps : CAP_ALL;
}

bool capable(u64 caps)
{
	/* Every one, not any. `(held & caps) != 0` is the tempting form and it
	 * grants a caller that asked for two powers on the strength of one. */
	return (capability_held() & caps) == caps;
}

void capability_drop(u64 caps)
{
	struct process *p = asking();

	/* The kernel cannot drop anything, because there is nowhere to record
	 * it: its capabilities are the absence of a process, not a field. That
	 * is worth knowing rather than silently doing nothing -- a kernel
	 * thread that tried this would believe it had given something up. */
	if (!p)
		return;

	p->caps &= ~caps;
	drops++;
}

const char *capability_name(u64 cap)
{
	switch (cap) {
	case CAP_FILE_OVERRIDE: return "file-override";
	case CAP_RAW_DISK:      return "raw-disk";
	case CAP_SHUTDOWN:      return "shutdown";
	default:                return 0;
	}
}

/* --- the capability self-test ---------------------------------------------
 *
 * Run in a process of its own, because the question "what do I hold" is asked
 * of whoever is running and cannot be asked on somebody else's behalf -- the
 * same reason the enforcement check needs a thread.
 *
 * The two claims worth testing are the two that make a capability worth having:
 *
 *   - **dropping one leaves the others.** A drop that cleared the set would
 *     pass any test that only checked the dropped power was gone;
 *   - **a drop is permanent.** There is no grant, so the test that matters is
 *     that nothing puts it back -- including creating a child, which is how a
 *     dropped power usually comes back in systems that get this wrong.
 */
static volatile int cap_answer;

static void capability_thread(void *arg)
{
	volatile int *out = arg;
	int result = 1;		/* 1 is a pass; anything else names the fault */

	/* This process was made with uid 0, so it starts with everything. */
	if (!capable(CAP_RAW_DISK) || !capable(CAP_SHUTDOWN) ||
	    !capable(CAP_FILE_OVERRIDE))
		result = 2;

	/* Two at once must need both. */
	if (result == 1 && !capable(CAP_RAW_DISK | CAP_SHUTDOWN))
		result = 3;

	capability_drop(CAP_RAW_DISK);

	if (result == 1 && capable(CAP_RAW_DISK))
		result = 4;

	/* The others are untouched. */
	if (result == 1 && (!capable(CAP_SHUTDOWN) ||
			    !capable(CAP_FILE_OVERRIDE)))
		result = 5;

	/* And asking for the pair now fails, because one half is gone --
	 * which is the "every one, not any" rule doing its job. */
	if (result == 1 && capable(CAP_RAW_DISK | CAP_SHUTDOWN))
		result = 6;

	/* Dropping something already dropped is not an error and does not
	 * bring it back. */
	capability_drop(CAP_RAW_DISK);

	if (result == 1 && capable(CAP_RAW_DISK))
		result = 7;

	/* --- and the same answers through the system-call layer -----------
	 *
	 * Which is the only way a program can reach any of this. Until these
	 * calls existed `capability_drop` had exactly one caller in the whole
	 * kernel -- the lines above -- so every property proved here was a
	 * property of a mechanism nothing could use.
	 *
	 * Each answer is compared against the kernel call beside it rather than
	 * against a constant. A constant would prove this file agrees with
	 * itself; what is worth proving is that the two layers agree, because
	 * the layer a program sees is the one that has to be right.
	 *
	 * Dispatched directly rather than through a trap from ring 3. These
	 * four take no pointers, so there is no user address to get wrong, and
	 * the entry path is a table lookup that does not know one number from
	 * another -- which the programs in user_entry.S already exercise. Said
	 * out loud because "tested" and "tested from user mode" are different
	 * claims and this is the weaker one.
	 */
	if (result == 1 &&
	    syscall_dispatch(SYS_GETCAPS, 0, 0, 0, 0, 0, 0) !=
	    (i64)capability_held())
		result = 8;

	if (result == 1 &&
	    syscall_dispatch(SYS_GETUID, 0, 0, 0, 0, 0, 0) !=
	    (i64)identity_uid())
		result = 9;

	if (result == 1 &&
	    syscall_dispatch(SYS_GETGID, 0, 0, 0, 0, 0, 0) !=
	    (i64)identity_gid())
		result = 10;

	/* A drop through the call must narrow, and must answer with what is
	 * left -- the return is the whole reason a program can check that it
	 * worked without a second call. */
	{
		i64 left = syscall_dispatch(SYS_DROPCAP, CAP_SHUTDOWN,
					    0, 0, 0, 0, 0);

		if (result == 1 && left != (i64)capability_held())
			result = 11;

		if (result == 1 && capable(CAP_SHUTDOWN))
			result = 12;
	}

	/* And it cannot be made to give anything back. Dropping nothing is the
	 * closest thing to a grant the interface has, and it must be a
	 * no-op -- if this ever answers with more than it was asked, the one
	 * property capabilities exist for is gone. */
	if (result == 1 &&
	    syscall_dispatch(SYS_DROPCAP, 0, 0, 0, 0, 0, 0) !=
	    (i64)capability_held())
		result = 13;

	{
		u64 before = capability_held();

		if (result == 1 &&
		    (u64)syscall_dispatch(SYS_DROPCAP, 0, 0, 0, 0, 0, 0)
		    & ~before)
			result = 14;
	}

	*out = result;
}

static bool capability_self_test(void)
{
	struct process *p;
	struct thread *t;
	u64 deadline;

	cap_answer = 0;

	p = process_create("caps", 0, UID_KERNEL, UID_KERNEL);
	t = p ? thread_create_stopped("caps", capability_thread,
				      (void *)&cap_answer) : NULL;

	if (!p || !t) {
		kputs("  identity: could not make a process to hold "
		      "capabilities\n");
		return false;
	}

	process_attach(p, t);
	thread_start(t);

	deadline = time_monotonic_ns() + 2000000000ull;

	while (!cap_answer && time_monotonic_ns() < deadline)
		sched_yield();

	switch (cap_answer) {
	case 1:
		break;
	case 0:
		kputs("  identity: the capability thread never answered\n");
		return false;
	case 2:
		kputs("  identity: a process made as the kernel did not start "
		      "with every capability, so existing callers lose powers "
		      "they had\n");
		return false;
	case 3:
		kputs("  identity: asking for two capabilities at once failed "
		      "while holding both\n");
		return false;
	case 4:
		kputs("  identity: a dropped capability was still held -- a "
		      "drop that does not drop is a power nobody can give "
		      "up\n");
		return false;
	case 5:
		kputs("  identity: dropping one capability took the others "
		      "with it\n");
		return false;
	case 8:
		kputs("  identity: SYS_GETCAPS and capability_held disagree, "
		      "so a program is told something the kernel does not "
		      "believe\n");
		return false;
	case 9:
		kputs("  identity: SYS_GETUID does not answer the uid the "
		      "kernel enforces against\n");
		return false;
	case 10:
		kputs("  identity: SYS_GETGID does not answer the gid the "
		      "kernel enforces against\n");
		return false;
	case 11:
		kputs("  identity: SYS_DROPCAP answered with a set that is "
		      "not what the process holds afterwards\n");
		return false;
	case 12:
		kputs("  identity: SYS_DROPCAP returned, and the capability "
		      "was still held -- a drop nothing outside the kernel "
		      "can actually perform\n");
		return false;
	case 13:
		kputs("  identity: SYS_DROPCAP asked to drop nothing did not "
		      "answer with the set that is held\n");
		return false;
	case 14:
		kputs("  identity: SYS_DROPCAP handed back a capability that "
		      "was not held going in, which is a grant\n");
		return false;
	case 6:
		kputs("  identity: asking for two capabilities succeeded while "
		      "holding only one, so the check grants on any rather "
		      "than all\n");
		return false;
	case 7:
		kputs("  identity: dropping a capability twice brought it "
		      "back\n");
		return false;
	default:
		kprintf("  identity: the capability thread said %d\n",
			cap_answer);
		return false;
	}

	/* And the kernel itself still holds everything, which is what keeps
	 * every caller that predates this working. */
	if (!capable(CAP_ALL)) {
		kputs("  identity: the kernel does not hold every capability, "
		      "so it cannot enforce what it cannot do\n");
		return false;
	}

	return true;
}

/* --- the self-test --------------------------------------------------------
 *
 * Every case here is a pure question -- a mode, an owner, a group, and somebody
 * asking -- so none of it needs a file, a process or a disk. That is the reason
 * the policy is a separate function from the places that call it: a permission
 * check tested only through `open` is a permission check tested on whatever
 * modes the test files happen to have.
 *
 * The case that matters most is the one nobody writes by accident:
 *
 *   **mode 0004, asked by the owner, must be refused.**
 *
 * The owner's bits say no and the other bits say yes. An implementation that
 * or-s the classes together says yes, passes every ordinary test -- 0644, 0600,
 * 0666 all behave -- and is wrong exactly when somebody has gone to the trouble
 * of hiding a file from its owner.
 */
/* Opens a file it must not be allowed to open, and says what happened.
 *
 * A separate function because it has to run *as* the process -- the identity
 * comes from the running thread, so the question cannot be asked on its
 * behalf by the test. */
static void try_to_open(void *arg)
{
	volatile int *answer = arg;
	i64 err = SYS_OK;
	struct file *f = file_open_path("/permission-test", OPEN_READ, 0, &err);

	if (f) {
		file_release(f);
		*answer = 2;
		return;
	}

	*answer = 1;
}

bool identity_self_test(void)
{
	bool ok = true;

	/* A file owned by uid 1000, group 1000. */
	const u32 OWNER = 1000, GROUP = 1000;

	/* --- the ordinary ones, which any implementation gets right ----- */
	if (!identity_may_as(OWNER, GROUP, 0644, OWNER, GROUP, ACCESS_READ)) {
		kputs("  identity: 0644 was not readable by its owner\n");
		ok = false;
	}

	if (!identity_may_as(OWNER, GROUP, 0644, OWNER, GROUP, ACCESS_WRITE)) {
		kputs("  identity: 0644 was not writable by its owner\n");
		ok = false;
	}

	if (identity_may_as(2000, 2000, 0644, OWNER, GROUP, ACCESS_WRITE)) {
		kputs("  identity: 0644 was writable by a stranger\n");
		ok = false;
	}

	if (!identity_may_as(2000, 2000, 0644, OWNER, GROUP, ACCESS_READ)) {
		kputs("  identity: 0644 was not readable by a stranger\n");
		ok = false;
	}

	/* --- the group class ------------------------------------------- */
	if (!identity_may_as(2000, GROUP, 0640, OWNER, GROUP, ACCESS_READ)) {
		kputs("  identity: 0640 was not readable by its group\n");
		ok = false;
	}

	if (identity_may_as(2000, GROUP, 0640, OWNER, GROUP, ACCESS_WRITE)) {
		kputs("  identity: 0640 was writable by its group\n");
		ok = false;
	}

	if (identity_may_as(2000, 3000, 0640, OWNER, GROUP, ACCESS_READ)) {
		kputs("  identity: 0640 was readable by a stranger\n");
		ok = false;
	}

	/* --- the one an or gets wrong ----------------------------------- */
	if (identity_may_as(OWNER, GROUP, 0004, OWNER, GROUP, ACCESS_READ)) {
		kputs("  identity: mode 0004 was readable by its owner -- the "
		      "owner's bits say no and the other bits say yes, so the "
		      "classes are being or-ed together rather than the first "
		      "matching one deciding\n");
		ok = false;
	}

	/* The same file, read by somebody else, must still work -- otherwise
	 * the fix for the above is "refuse everything", which also passes the
	 * check above and is useless. */
	if (!identity_may_as(2000, 2000, 0004, OWNER, GROUP, ACCESS_READ)) {
		kputs("  identity: mode 0004 was not readable by a stranger, "
		      "so the owner's refusal is being applied to everybody\n");
		ok = false;
	}

	/* And the group form of the same trap. */
	if (identity_may_as(2000, GROUP, 0614, OWNER, GROUP, ACCESS_READ)) {
		kputs("  identity: mode 0614 was readable by its group, whose "
		      "bits say no\n");
		ok = false;
	}

	/* --- both halves of a request must be granted ------------------- */
	if (identity_may_as(2000, 2000, 0644, OWNER, GROUP,
			    (enum access_want)(ACCESS_READ | ACCESS_WRITE))) {
		kputs("  identity: a stranger was given read *and* write on "
		      "0644, which grants only read -- a descriptor that can "
		      "do half of what it asked for fails at the first "
		      "write\n");
		ok = false;
	}

	/* --- the kernel ------------------------------------------------- */
	if (!identity_may_as(UID_KERNEL, UID_KERNEL, 0000, OWNER, GROUP,
			     (enum access_want)(ACCESS_READ | ACCESS_WRITE))) {
		kputs("  identity: the kernel was refused a file with no "
		      "permission bits at all, so one wrong mode would lock "
		      "the machine out of its own filesystem\n");
		ok = false;
	}

	/* --- nothing asked for is not a refusal ------------------------- */
	if (!identity_may_as(2000, 2000, 0000, OWNER, GROUP,
			     (enum access_want)0)) {
		kputs("  identity: asking for nothing was refused\n");
		ok = false;
	}

	/* --- execute is its own bit ------------------------------------- */
	if (identity_may_as(OWNER, GROUP, 0644, OWNER, GROUP, ACCESS_EXEC)) {
		kputs("  identity: 0644 was executable\n");
		ok = false;
	}

	if (!identity_may_as(OWNER, GROUP, 0755, OWNER, GROUP, ACCESS_EXEC)) {
		kputs("  identity: 0755 was not executable by its owner\n");
		ok = false;
	}

	/* --- and what the open flags mean ------------------------------- */
	if (identity_want_from_open(OPEN_READ) != ACCESS_READ) {
		kputs("  identity: opening for reading did not ask to read\n");
		ok = false;
	}

	if (identity_want_from_open(OPEN_READ | OPEN_WRITE) !=
	    (enum access_want)(ACCESS_READ | ACCESS_WRITE)) {
		kputs("  identity: opening for both did not ask for both\n");
		ok = false;
	}

	/* --- the capabilities, in a process of their own ---------------- */
	if (!capability_self_test())
		ok = false;

	/* --- and the kernel really is what is running these tests ------- */
	if (identity_uid() != UID_KERNEL) {
		kprintf("  identity: the self-test is running as uid %u, not "
			"as the kernel\n", identity_uid());
		ok = false;
	}

	return ok;
}

/* The half a self-test cannot do.
 *
 * Everything in identity_self_test asks the policy directly, which proves the
 * policy is right and proves nothing about whether anything *consults* it --
 * the exact sentence this row of the audit carried for months.
 *
 * Proving that needs a real file on a real filesystem, and there is none when
 * the self-tests run: the volume is made later, by reconfs_run. Putting the
 * check there anyway would be the trap swap fell into, where a test reported a
 * pass for having found nothing to test. So it lives here, after the volume
 * exists, and says which of the two happened either way.
 */
void identity_run(void)
{
	static volatile int answer;	/* 0 waiting, 1 refused, 2 let in */
	struct process *p;
	struct thread *t;
	enum reconfs_status made;

	if (!rootfs())
		return;		/* the ordinary machine; the summary says so */

	answer = 0;

	/* Owned by the kernel, readable only by its owner. Anybody else falls
	 * into the "other" class, which has no bits at all. */
	made = rootfs_create_file("/permission-test", 0600, "secret", 6);

	if (made != RECONFS_OK && made != RECONFS_ERR_EXISTS) {
		kputs("  permissions  : could not make a file to be refused\n");
		return;
	}

	p = process_create("not-root", 0, 1000, 1000);
	t = p ? thread_create_stopped("asker", try_to_open, (void *)&answer)
	      : NULL;

	if (!p || !t) {
		kputs("  permissions  : could not make a process to ask as\n");
		return;
	}

	/* Attached before it is runnable, which is the whole reason
	 * thread_create_stopped exists: the scheduler reads the process to
	 * decide the thread's identity, and a thread in the ring before that
	 * field is set runs as nobody. (KF-148, and again as KF-165.) */
	process_attach(p, t);
	thread_start(t);

	{
		u64 deadline = time_monotonic_ns() + 2000000000ull;

		while (!answer && time_monotonic_ns() < deadline)
			sched_yield();
	}

	kputs("\nPermissions, enforced\n");

	if (answer == 0)
		kputs("  asked        : the thread never answered\n");
	else if (answer == 2)
		kputs("  LET IN       : a process running as uid 1000 opened a "
		      "file with mode 0600 owned by the kernel -- the mode is "
		      "stored and nothing consults it\n");
	else
		kputs("  refused      : uid 1000 could not open a 0600 file "
		      "owned by the kernel, which is the check working\n");
}
