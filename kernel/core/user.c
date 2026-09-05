#include <recon/kernel/user.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/smp.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/time.h>
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

const struct personality personality_recon = {
	.name = "ReconOS",
	.table = {
		[SYS_EXIT]   = sys_exit,
		[SYS_WRITE]  = sys_write,
		[SYS_GETPID] = sys_getpid,
		[SYS_TIME]   = sys_time,
		[SYS_YIELD]  = sys_yield,
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
	paddr_t code_page, stack_page;

	if (code_len > PAGE_SIZE)
		return 0;

	code_page  = pmm_alloc_page();
	stack_page = pmm_alloc_page();
	if (!code_page || !stack_page)
		return 0;

	kmemcpy(phys_to_virt(code_page), code, code_len);

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
	if (!vm_map(USER_BASE, code_page, PAGE_SIZE,
		    VM_READ | VM_EXEC | VM_USER))
		return 0;

	if (!vm_map(USER_STACK_TOP - PAGE_SIZE, stack_page, PAGE_SIZE,
		    VM_READ | VM_WRITE | VM_USER))
		return 0;

	t = thread_create(name, user_thread_start, (void *)(uintptr_t)USER_BASE);
	if (!t)
		return 0;

	t->personality = &personality_recon;
	return t;
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
