#include <recon/kernel/user.h>
#include <recon/kernel/process.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/cpu.h>
#include <recon/kernel/random.h>
#include <recon/kernel/rootfs.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/smp.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/time.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/console.h>
#include <recon/kernel/kstring.h>

static volatile u64 exits;
static volatile i64 last_exit_code;
static volatile u64 calls_served;
static volatile u64 faults;
static volatile u64 refusals;

bool user_range_ok(u64 addr, u64 len)
{
	u64 end;

	/* Zero length is fine and touches nothing. */
	if (len == 0)
		return true;

	/* Overflow first, and this is the check people forget: a program that
	 * passes a pointer near the top and a huge length would otherwise
	 * produce an end address that wrapped below the start, and every
	 * subsequent range test would pass. */
	end = addr + len;
	if (end < addr)
		return false;

	/* The whole range has to be in the user half, and not in the first page
	 * -- which is never mapped, so that a null pointer faults in a user
	 * program exactly as it does in the kernel. */
	if (addr < PAGE_SIZE)
		return false;

	return end <= USER_LIMIT;
}

/* --- The calls ------------------------------------------------------------ */

static i64 sys_exit(u64 code, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	last_exit_code = (i64)code;
	exits++;

	/* Ends the thread, which is a kernel object; the user program simply
	 * stops existing. Does not return. */
	thread_exit();
	return 0;
}

static i64 sys_write(u64 fd, u64 buf, u64 len, u64 a3, u64 a4, u64 a5)
{
	const char *p;

	/* One "file" so far, and it is the console. A real descriptor table
	 * needs files, which need a filesystem. */
	if (fd != 1 && fd != 2)
		return SYS_EINVAL;

	/* THE CHECK THAT MATTERS. Without it a user program hands the kernel any
	 * address it likes and the kernel obligingly reads it -- which is every
	 * secret in the machine, retrieved by asking politely. */
	if (!user_range_ok(buf, len)) {
		refusals++;
		return SYS_EFAULT;
	}

	p = (const char *)(uintptr_t)buf;

	for (u64 i = 0; i < len; i++)
		kputc(p[i]);

	return (i64)len;
}

static i64 sys_getpid(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct thread *t = sched_current();

	/* The *process*, which is what this call has always meant. A thread
	 * identifier answered here would be a different number for each thread
	 * of one program, and every caller of getpid wants the program. A
	 * kernel thread belongs to no process and gets its thread number, which
	 * is the only honest answer available. */
	{
		struct process *p = process_of(t);

		if (p)
			return (i64)p->id;
	}

	return t ? (i64)t->id : -1;
}

static i64 sys_time(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	return (i64)time_monotonic_ns();
}

static i64 sys_yield(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	sched_yield();
	return SYS_OK;
}

/* Random bytes, for a program that has to make a key.
 *
 * The length is capped rather than unbounded. Not for safety -- the range check
 * below is what makes it safe -- but because generating is work, this kernel
 * cannot yet be interrupted part-way through a system call, and a program
 * asking for a gigabyte would hold its processor for the whole of it. A cap
 * that returns a short count is something a caller loops on; there is no
 * version of that request worth honouring in one go.
 */
#define RANDOM_MAX_AT_ONCE 4096

static i64 sys_random(u64 buf, u64 len, u64 a2, u64 a3, u64 a4, u64 a5)
{
	if (len == 0)
		return 0;

	if (len > RANDOM_MAX_AT_ONCE)
		len = RANDOM_MAX_AT_ONCE;

	if (!user_range_ok(buf, len)) {
		refusals++;
		return SYS_EFAULT;
	}

	/* Asked *before* writing, so that a refusal leaves the caller's buffer
	 * exactly as it was. A generator that half-fills a buffer and then
	 * reports failure hands a caller who ignores the return value a key
	 * made of whatever was already there. */
	if (!random_ready())
		return SYS_EAGAIN;

	if (!random_bytes((void *)(uintptr_t)buf, (size_t)len))
		return SYS_EAGAIN;

	return (i64)len;
}

/* What this machine is.
 *
 * Every number here is one the desktop currently scrapes out of /proc and
 * parses with sscanf. The kernel has known all of them since checkpoint 3; what
 * was missing was any way to ask.
 */
static i64 sys_machine(u64 buf, u64 len, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct recon_machine m;
	struct cpu_caps caps;
	u64 copy;

	if (!user_range_ok(buf, len)) {
		refusals++;
		return SYS_EFAULT;
	}

	kmemset(&m, 0, sizeof(m));
	arch_cpu_caps(&caps);

	m.size    = (u32)sizeof(m);
	m.version = 1;

	/* Both counts, because they answer different questions and a machine
	 * where they differ is a machine with something wrong with it. One
	 * number would hide exactly that case. */
	m.processors_found  = smp_cpu_count();
	m.processors_online = smp_cpus_online();

	m.memory_bytes      = (u64)pmm_total_pages() * PAGE_SIZE;
	m.memory_free_bytes = (u64)pmm_free_page_count() * PAGE_SIZE;

	m.entropy_bits = random_entropy_bits();
	m.page_size    = (u32)PAGE_SIZE;

	kstrlcpy(m.architecture, arch_name(), sizeof(m.architecture));
	kstrlcpy(m.cpu_vendor, caps.vendor, sizeof(m.cpu_vendor));
	kstrlcpy(m.cpu_model, caps.brand, sizeof(m.cpu_model));

	/* No more than the caller had room for, and the *full* size returned
	 * regardless -- so a program built against an older kernel gets a valid
	 * prefix and can see that it was given one. Writing sizeof(m) into a
	 * buffer the caller sized for an earlier version is the bug this shape
	 * exists to prevent, and it is a bug that only appears once there are
	 * two versions, which is to say after it is too late to notice. */
	copy = len < sizeof(m) ? len : sizeof(m);
	kmemcpy((void *)(uintptr_t)buf, &m, (size_t)copy);

	return (i64)sizeof(m);
}

/* The wall clock, which is a different question from SYS_TIME.
 *
 * SYS_TIME is monotonic: it never goes backwards and means nothing outside this
 * boot. This one is the date, and it can jump. A caller measuring how long
 * something took must use the first; a caller stamping a file must use this.
 * Collapsing them into one call is how a duration comes out negative. */
static i64 sys_walltime(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	return (i64)time_wall_ns();
}

/* Creates a file with its permissions already set, in one call.
 *
 * One call rather than open-then-chmod, and that is the entire point: see
 * rootfs.h. Under copy-on-write the create, the contents and the mode are one
 * transaction, so the file becomes visible only when it is finished, already
 * carrying the permissions it was asked for. There is no window, not even
 * across a power cut.
 *
 * The path is copied out of user memory before anything is done with it. A
 * kernel that walked a filesystem through a pointer the caller can still write
 * to would be re-reading a path that changed after it was checked -- the oldest
 * shape of privilege bug there is, and one that costs nothing to avoid here
 * because the path is short.
 */
#define CREATE_PATH_MAX 256
#define CREATE_MAX_BYTES 65536

static i64 sys_create(u64 path, u64 path_len, u64 mode, u64 data, u64 len,
		      u64 a5)
{
	char kpath[CREATE_PATH_MAX];
	enum reconfs_status st;

	if (path_len == 0 || path_len >= sizeof(kpath))
		return SYS_EINVAL;

	if (len > CREATE_MAX_BYTES)
		return SYS_EINVAL;

	if (!user_range_ok(path, path_len) ||
	    (len && !user_range_ok(data, len))) {
		refusals++;
		return SYS_EFAULT;
	}

	kmemcpy(kpath, (const void *)(uintptr_t)path, (size_t)path_len);
	kpath[path_len] = '\0';

	/* Terminated by us, at the length we were given, so a path with an
	 * embedded zero is short rather than a way to smuggle one string past a
	 * check and have a different one used. */

	st = rootfs_create_file(kpath, (u32)mode,
				len ? (const void *)(uintptr_t)data : NULL,
				(u32)len);

	switch (st) {
	case RECONFS_OK:
		return (i64)len;

	/* Each of these means something a caller would act on differently, so
	 * they are not collapsed into one failure. */
	case RECONFS_ERR_NOT_MOUNTED:
		return SYS_ENODEV;
	case RECONFS_ERR_EXISTS:
		return SYS_EEXIST;
	case RECONFS_ERR_NOT_FOUND:
	case RECONFS_ERR_NOT_DIR:
		return SYS_ENOENT;
	case RECONFS_ERR_NAME:
	case RECONFS_ERR_TOO_DEEP:
		return SYS_EINVAL;
	case RECONFS_ERR_NOSPACE:
	case RECONFS_ERR_TOO_LARGE:
		return SYS_ENOSPC;
	default:
		return SYS_EIO;
	}
}

const struct personality personality_recon = {
	.name = "ReconOS",
	.table = {
		[SYS_EXIT]   = sys_exit,
		[SYS_WRITE]  = sys_write,
		[SYS_GETPID] = sys_getpid,
		[SYS_TIME]   = sys_time,
		[SYS_YIELD]  = sys_yield,
		[SYS_RANDOM]   = sys_random,
		[SYS_MACHINE]  = sys_machine,
		[SYS_WALLTIME] = sys_walltime,
		[SYS_CREATE]   = sys_create,
	},
};

i64 syscall_dispatch(u64 number, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct thread *t = sched_current();
	const struct personality *p = (t && t->personality)
		? t->personality : &personality_recon;

	calls_served++;

	/* An unknown number is answered, not punished. A program built for a
	 * system this kernel does not implement should learn that from a return
	 * value rather than from being killed -- that is how a compatibility
	 * layer finds out what it has to provide. */
	if (number >= SYS_MAX || !p->table[number])
		return SYS_ENOSYS;

	return p->table[number](a0, a1, a2, a3, a4, a5);
}

void user_note_fault(void)
{
	faults++;
}

void user_init(void)
{
	exits = 0;
	calls_served = 0;
	faults = 0;
	refusals = 0;
	arch_user_init();
}

/* --- Making one ---------------------------------------------------------- */

/* The trampoline a user thread starts on. It runs in the kernel just long
 * enough to drop out of it. */
static void user_thread_start(void *arg)
{
	u64 entry = (u64)(uintptr_t)arg;

	arch_enter_user(entry, USER_STACK_TOP);
}

struct thread *user_thread_create(const char *name, const void *code,
				  size_t code_len)
{
	struct thread *t;
	struct process *p;
	struct addrspace *as;
	paddr_t code_page, stack_page;

	if (code_len > PAGE_SIZE)
		return 0;

	code_page  = pmm_alloc_page();
	stack_page = pmm_alloc_page();
	if (!code_page || !stack_page)
		return 0;

	kmemcpy(phys_to_virt(code_page), code, code_len);

	/* THE PROCESS AND ITS ADDRESS SPACE COME FIRST, AND THAT ORDER IS THE
	 * WHOLE OF WHAT CHANGED.
	 *
	 * This used to map the program's pages and then create a process to own
	 * the thread. There was one map, so both programs' pages landed at the
	 * same addresses in the same tables and the second one loaded sat on top
	 * of the first -- which is why the process table had to say out loud
	 * that one program could be live at a time. The mappings now go into a
	 * space that belongs to this program, so they cannot be that.
	 *
	 * Created as `nobody` rather than as the kernel, because a program that
	 * runs with the most privileged identity by default is the wrong
	 * direction to fail in -- and nothing yet checks, so the value recorded
	 * now is the one enforcement will find when it arrives. */
	p = process_create(name, 0, UID_NOBODY, UID_NOBODY);
	if (!p) {
		/* Refused rather than silently unowned: a program with no
		 * process has no identity, and everything above this line
		 * assumes one. */
		kprintf("  user: no room in the process table for %s\n", name);
		return 0;
	}

	p->personality = &personality_recon;

	as = addrspace_create();
	if (!as) {
		kprintf("  user: no address space for %s\n", name);
		return 0;
	}

	/* The process holds the reference from here; this one is handed over
	 * rather than kept, so that the space lives exactly as long as the
	 * process does and not one caller longer. */
	process_set_space(p, as);
	addrspace_release(as);

	/* The program's own pages, and the only two mapped for it.
	 *
	 * VM_USER is what makes them reachable at all from user mode -- and its
	 * absence everywhere else is what makes the rest of the machine
	 * unreachable. The kernel's own mappings do not carry it, so a user
	 * program that dereferences a kernel address faults rather than reads.
	 *
	 * The code is executable and not writable; the stack is writable and not
	 * executable. Neither is both, which costs nothing here and is the whole
	 * of what stops a program being talked into running its own input. */
	if (!addrspace_map(as, USER_BASE, code_page, PAGE_SIZE,
			   VM_READ | VM_EXEC | VM_USER))
		return 0;

	/* THE STACK IS A PROMISE RATHER THAN A MAPPING, AND THAT IS WHY EVERY
	 * BOOT TESTS DEMAND PAGING.
	 *
	 * It used to be one page, mapped before the program ran. Now the whole
	 * stack region is reserved and nothing is mapped: the program's first
	 * push faults, the handler sees an address the program was promised,
	 * gives it a page, and the instruction runs again.
	 *
	 * Deliberately not a separate test. A demand-paging path that only some
	 * test exercises is a path that rots; this one is between every user
	 * program and its first instruction, so if it breaks, "user mode: pass"
	 * stops being printed rather than a test nobody ran going quietly red.
	 *
	 * Reserved downwards from the top of the user stack, which is what makes
	 * this stack growth as well as demand paging -- the region is the size
	 * the stack may reach, and the pages that exist are the ones it has
	 * actually reached. */
	if (!addrspace_reserve(as, USER_STACK_TOP - USER_STACK_MAX,
			       USER_STACK_MAX,
			       VM_READ | VM_WRITE | VM_USER)) {
		kprintf("  user: no room to reserve a stack for %s\n", name);
		return 0;
	}

	/* The page that was going to be the stack is not needed after all. */
	pmm_free_page(stack_page);
	stack_page = 0;

	t = thread_create(name, user_thread_start, (void *)(uintptr_t)USER_BASE);
	if (!t)
		return 0;

	t->personality = &personality_recon;
	process_attach(p, t);

	return t;
}

/* The three calls the desktop asked for, exercised from ring 3.
 *
 * From ring 3 specifically, and not by calling the handlers directly, because
 * the thing most likely to be wrong is not the handler. It is the boundary: an
 * argument that arrives in the wrong register, a pointer check that rejects a
 * legitimate buffer, a structure the two sides disagree about the shape of.
 * None of those is reachable from a kernel-side call.
 *
 * The program reports which step failed through its exit code, so a failure
 * here names the call rather than saying that something went wrong. */
bool user_facts_test(void)
{
	extern const unsigned char user_test_facts[];
	extern const u64 user_test_facts_len;

	static const char *const why[] = {
		"the program ran and did not report",		/* 0 */
		"SYS_RANDOM did not return the length asked for",
		"two SYS_RANDOM reads came back identical",
		"SYS_MACHINE returned no size",
		"SYS_MACHINE filled in the wrong structure version",
		"SYS_WALLTIME returned nothing positive",
		"SYS_CREATE accepted a kernel address as a path",
	};

	u64 exits_before = exits;
	struct thread *t;
	u64 deadline;

	t = user_thread_create("facts", user_test_facts, user_test_facts_len);
	if (!t) {
		kputs("  user: could not create the facts thread\n");
		return false;
	}

	deadline = time_monotonic_ns() + 2000000000ULL;
	while (exits == exits_before && time_monotonic_ns() < deadline)
		sched_yield();

	if (exits == exits_before) {
		kputs("  user: the facts program never exited\n");
		return false;
	}

	if (last_exit_code == 77)
		return true;

	if (last_exit_code >= 0 &&
	    (u64)last_exit_code < sizeof(why) / sizeof(why[0]))
		kprintf("  user: %s\n", why[last_exit_code]);
	else
		kprintf("  user: the facts program exited with %ld, which is "
			"not one of its own codes\n", last_exit_code);

	return false;
}

bool user_self_test(void)
{
	/* Both are defined in the architecture's user_test.S. The length is a
	 * .quad there, so it is a u64 here -- declaring it `unsigned int` would
	 * read the right value on a little-endian machine and be wrong for a
	 * reason no one would enjoy finding. */
	extern const unsigned char user_test_program[];
	extern const u64 user_test_program_len;

	u64 exits_before = exits;
	u64 calls_before = calls_served;
	struct thread *t;
	u64 deadline;

	t = user_thread_create("hello", user_test_program, user_test_program_len);
	if (!t) {
		kputs("  user: could not create a user thread\n");
		return false;
	}

	deadline = time_monotonic_ns() + 2000000000ULL;
	while (exits == exits_before && time_monotonic_ns() < deadline)
		sched_yield();

	if (exits == exits_before) {
		kputs("  user: the program never reached its exit call\n");
		return false;
	}

	if (calls_served <= calls_before) {
		kputs("  user: it exited without making a system call, which "
		      "means it never really ran\n");
		return false;
	}

	/* The program is written to return this exactly. A different value means
	 * it ran but computed something else -- arguments arriving in the wrong
	 * registers looks precisely like this. */
	if (last_exit_code != 42) {
		kprintf("  user: it exited with %ld, not 42 -- so its arguments "
			"did not arrive where it put them\n", last_exit_code);
		return false;
	}

	return true;
}

bool user_boundary_test(void)
{
	extern const unsigned char user_test_bad[];
	extern const u64 user_test_bad_len;

	u64 faults_before   = faults;
	u64 refusals_before = refusals;
	u64 exits_before    = exits;
	struct thread *t;
	u64 deadline;

	t = user_thread_create("trespass", user_test_bad, user_test_bad_len);
	if (!t) {
		kputs("  user: could not create the second user thread\n");
		return false;
	}

	deadline = time_monotonic_ns() + 2000000000ULL;
	while (faults == faults_before && exits == exits_before
	       && time_monotonic_ns() < deadline)
		sched_yield();

	/* The program first asks the kernel to read a kernel address on its
	 * behalf. If that is refused, it goes and reads one itself, and faults.
	 * So the program exiting *normally* is the failure: it means the first
	 * attempt succeeded, and the kernel handed a user program the contents
	 * of its own memory just because it was asked politely. */
	if (exits != exits_before) {
		kputs("  user: a system call read a kernel address on a user "
		      "program's behalf -- the pointer check did not hold\n");
		return false;
	}

	if (refusals <= refusals_before) {
		kputs("  user: the bad pointer was never refused\n");
		return false;
	}

	if (faults == faults_before) {
		kputs("  user: it never faulted, so it never actually reached "
		      "a kernel address\n");
		return false;
	}

	/* And the whole return on this checkpoint: we are still here to say so. */
	return true;
}

void user_print_summary(void)
{
	kprintf("\nUser mode\n");
	kprintf("  personality  : %s, %u calls defined\n",
		personality_recon.name, (unsigned)SYS_MAX);
	kprintf("  served       : %lu system calls\n", calls_served);
	kprintf("  refused      : %lu bad pointers\n", refusals);
	kprintf("  ended        : %lu programs, for faulting\n", faults);
}
