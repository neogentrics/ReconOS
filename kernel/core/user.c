#include <recon/kernel/user.h>
#include <recon/kernel/signal.h>
#include <recon/kernel/identity.h>
#include <recon/kernel/process.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/cpu.h>
#include <recon/kernel/random.h>
#include <recon/kernel/rootfs.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/sched.h>
#include <recon/kernel/smp.h>
#include <recon/kernel/vm.h>
#include <recon/kernel/pmm.h>
#include <recon/kernel/heap.h>
#include <recon/kernel/time.h>
#include <recon/kernel/addrspace.h>
#include <recon/kernel/console.h>
#include <recon/kernel/elf.h>
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


/* --- signals -------------------------------------------------------------- */

static i64 sys_kill(u64 process, u64 sig, u64 a2, u64 a3, u64 a4, u64 a5)
{
	return signal_send((u32)process, (unsigned)sig) ? SYS_OK : SYS_EINVAL;
}

static i64 sys_sigaction(u64 sig, u64 what, u64 handler, u64 restorer,
			 u64 a4, u64 a5)
{
	return signal_set_action((unsigned)sig, (unsigned)what, handler,
				 restorer) ? SYS_OK : SYS_EINVAL;
}

static i64 sys_sigmask(u64 mask, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	return (i64)signal_set_mask((u32)mask);
}

static i64 sys_sigreturn(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct thread *t = sched_current();

	if (!t)
		return SYS_EPERM;

	/* Recorded rather than done. The hook on the way back to user mode has
	 * the saved registers; this does not. See the field's own comment.
	 *
	 * The value returned here is discarded: the hook overwrites every
	 * register the return path is about to read, including the one this
	 * would have landed in. */
	t->signal_restoring = true;

	return SYS_OK;
}

static i64 sys_exit(u64 code, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	last_exit_code = (i64)code;
	exits++;

	/* Ends the thread, which is a kernel object; the user program simply
	 * stops existing. Does not return. */
	thread_exit();
	return 0;
}

/* The process making this call, or null for a thread that has none. */
static struct process *caller(void)
{
	return process_of(sched_current());
}

/* THE CHECK THAT MATTERS, in one place for all four descriptor calls.
 *
 * Without it a user program hands the kernel any address it likes and the
 * kernel obligingly reads or writes it -- which is every secret in the machine,
 * retrieved by asking politely. */
static bool user_buffer_ok(u64 buf, u64 len)
{
	if (user_range_ok(buf, len))
		return true;

	refusals++;
	return false;
}

/* Reads a path out of the caller and terminates it here.
 *
 * Terminated by us, at the length we were given, so a path with an embedded
 * zero is short rather than a way to smuggle one string past a check and have a
 * different one used. */
static i64 copy_path(u64 path, u64 path_len, char *out, size_t max)
{
	if (path_len == 0 || path_len >= max)
		return SYS_EINVAL;

	if (!user_buffer_ok(path, path_len))
		return SYS_EFAULT;

	kmemcpy(out, (const void *)(uintptr_t)path, (size_t)path_len);
	out[path_len] = '\0';
	return SYS_OK;
}

/* Every one of the four below has the same shape, and that is the point.
 *
 * None of them contains an `if` about what kind of thing the descriptor names.
 * A console, a file on the mounted volume and a pipe reach exactly the same
 * lines here -- which is the difference between a virtual filesystem and one
 * filesystem with a switch statement in front of it. */
static i64 sys_write(u64 fd, u64 buf, u64 len, u64 a3, u64 a4, u64 a5)
{
	struct file *f;
	i64 n;

	if (!user_buffer_ok(buf, len))
		return SYS_EFAULT;

	f = fd_get(caller(), (int)fd);
	if (!f)
		return SYS_EBADF;

	/* "Cannot be written" is answered once, here, rather than by a stub in
	 * every implementation that cannot -- and it is a different answer from
	 * a write that was attempted and failed. */
	if (!f->ops->write) {
		file_release(f);
		return SYS_EPERM;
	}

	n = f->ops->write(f, (const void *)(uintptr_t)buf, len);
	vfs_note_write();
	file_release(f);
	return n;
}

static i64 sys_read(u64 fd, u64 buf, u64 len, u64 a3, u64 a4, u64 a5)
{
	struct file *f;
	i64 n;

	if (!user_buffer_ok(buf, len))
		return SYS_EFAULT;

	f = fd_get(caller(), (int)fd);
	if (!f)
		return SYS_EBADF;

	if (!f->ops->read) {
		file_release(f);
		return SYS_EPERM;
	}

	n = f->ops->read(f, (void *)(uintptr_t)buf, len);
	vfs_note_read();
	file_release(f);
	return n;
}

static i64 sys_open(u64 path, u64 path_len, u64 flags, u64 mode, u64 a4,
		   u64 a5)
{
	char kpath[VFS_PATH_MAX];
	struct file *f;
	i64 st = copy_path(path, path_len, kpath, sizeof(kpath));
	int fd;

	if (st != SYS_OK)
		return st;

	/* Neither readable nor writable is refused rather than given some
	 * default. A program that asked for nothing gets to find out it asked
	 * for nothing. */
	if (!(flags & (OPEN_READ | OPEN_WRITE)))
		return SYS_EINVAL;

	f = file_open_path(kpath, (unsigned)flags, (u32)mode, &st);
	if (!f)
		return st;

	fd = fd_install(caller(), f);

	/* The install took a reference of its own, so this one goes either way.
	 * On failure that is the last one and the file is closed here -- which
	 * for a created file means it is never written, which is right: it was
	 * never handed to anybody. */
	file_release(f);
	return fd;
}

static i64 sys_close(u64 fd, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	/* The result is returned rather than discarded, and that is not a
	 * formality: a file written to the mounted volume is committed here,
	 * so this is the only place its failure can be reported. */
	return fd_close(caller(), (int)fd);
}

/* Both ends at once, because a pipe with one end is not a pipe and a program
 * that had to ask twice could be left holding half of one. */
static i64 sys_pipe(u64 out, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct file *r = NULL, *w = NULL;
	struct process *p = caller();
	int fds[2];

	if (!user_buffer_ok(out, sizeof(fds)))
		return SYS_EFAULT;

	if (!pipe_create(&r, &w))
		return SYS_ENOSPC;

	fds[0] = fd_install(p, r);
	fds[1] = fd_install(p, w);

	/* Our own references go either way: the descriptors hold them now, and
	 * on failure dropping them is what closes the end nobody got. */
	file_release(r);
	file_release(w);

	if (fds[0] < 0 || fds[1] < 0) {
		/* Half a pipe is worse than none: a program handed one end and
		 * an error has to guess whether to close it. */
		if (fds[0] >= 0)
			fd_close(p, fds[0]);
		if (fds[1] >= 0)
			fd_close(p, fds[1]);
		return SYS_EMFILE;
	}

	kmemcpy((void *)(uintptr_t)out, fds, sizeof(fds));
	return SYS_OK;
}

static i64 sys_seek(u64 fd, u64 offset, u64 from, u64 a3, u64 a4, u64 a5)
{
	struct file *f = fd_get(caller(), (int)fd);
	i64 n;

	if (!f)
		return SYS_EBADF;

	/* A thing with no position at all -- a console, a pipe -- is a
	 * different answer from a seek that failed, and the program can tell
	 * the two apart. */
	if (!f->ops->seek) {
		file_release(f);
		return SYS_EPERM;
	}

	n = f->ops->seek(f, (i64)offset, (unsigned)from);
	file_release(f);
	return n;
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

static i64 sys_list(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	char kpath[VFS_PATH_MAX];

	if (a1 == 0 || a1 >= sizeof(kpath))
		return SYS_EINVAL;

	if (!user_range_ok(a0, a1) || (a3 && !user_range_ok(a2, a3)))
		return SYS_EFAULT;

	kmemcpy(kpath, (const void *)(uintptr_t)a0, (size_t)a1);
	kpath[a1] = 0;

	/* Terminated by us, at the length we were given, so a path with an
	 * embedded zero is short rather than a way to smuggle one string past
	 * a check and have a different one used -- the same rule SYS_CREATE
	 * follows, and for the same reason.
	 *
	 * The buffer is checked before anything is written to it, which matters
	 * more than usual here: the answer goes to an address the caller chose,
	 * at a length the caller chose. */
	return file_list_path(kpath, a3 ? (char *)(uintptr_t)a2 : NULL,
			      a3, NULL);
}

static i64 sys_getuid(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	return (i64)identity_uid();
}

static i64 sys_getgid(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	return (i64)identity_gid();
}

/* What this process may still do.
 *
 * Returned as a positive number, which works because there are three
 * capabilities and room for sixty-two. The day there are more than will fit
 * below the sign bit this needs a buffer instead, because a negative return is
 * an error by the convention every call here follows -- said here rather than
 * discovered by whoever adds the sixty-third. */
static i64 sys_getcaps(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	return (i64)capability_held();
}

static i64 sys_dropcap(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	/* Bits this kernel has never heard of are dropped too, and that is
	 * deliberate rather than sloppy: a process cannot hold what does not
	 * exist, so the effect is nothing, and refusing would mean a program
	 * built against a later kernel failing here instead of simply giving up
	 * a power this one does not have.
	 *
	 * There is no argument that makes this grant anything. That is not an
	 * omission to be fixed when somebody needs it -- a set that can be
	 * regained protects nothing. */
	capability_drop(a0);

	return (i64)capability_held();
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

/* One filesystem status, as one system-call status.
 *
 * Here rather than inside each caller, because the mapping is a decision --
 * which distinctions a program is allowed to see -- and a decision made in
 * three places is three decisions. Each of these means something a caller
 * would act on differently, so they are not collapsed into one failure. */
i64 user_status_from_reconfs(enum reconfs_status st)
{
	switch (st) {
	case RECONFS_OK:
		return SYS_OK;
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

	if (st == RECONFS_OK)
		return (i64)len;

	return user_status_from_reconfs(st);
}

const struct personality personality_recon = {
	.name = "ReconOS",
	.table = {
		[SYS_EXIT]   = sys_exit,
		[SYS_KILL]      = sys_kill,
		[SYS_SIGACTION] = sys_sigaction,
		[SYS_SIGMASK]   = sys_sigmask,
		[SYS_SIGRETURN] = sys_sigreturn,
		[SYS_WRITE]  = sys_write,
		[SYS_OPEN]   = sys_open,
		[SYS_CLOSE]  = sys_close,
		[SYS_READ]   = sys_read,
		[SYS_SEEK]   = sys_seek,
		[SYS_PIPE]   = sys_pipe,
		[SYS_GETPID] = sys_getpid,
		[SYS_TIME]   = sys_time,
		[SYS_YIELD]  = sys_yield,
		[SYS_RANDOM]   = sys_random,
		[SYS_MACHINE]  = sys_machine,
		[SYS_WALLTIME] = sys_walltime,
		[SYS_CREATE]   = sys_create,
		[SYS_GETUID]   = sys_getuid,
		[SYS_GETGID]   = sys_getgid,
		[SYS_GETCAPS]  = sys_getcaps,
		[SYS_DROPCAP]  = sys_dropcap,
		[SYS_LIST]     = sys_list,
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
	 * direction to fail in.
	 *
	 * That used to end "and nothing yet checks, so the value recorded now is
	 * the one enforcement will find when it arrives." Enforcement arrived on
	 * 11 September, and this is now the identity a program is actually
	 * refused files as -- and, since a process that is not the kernel starts
	 * with no capabilities, the reason a test program cannot drop one. */
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

	t = thread_create_stopped(name, user_thread_start, (void *)(uintptr_t)USER_BASE);
	if (!t)
		return 0;

	t->personality = &personality_recon;
	process_attach(p, t);

	/* Only now may anything else see it. Started before the attach, a
	 * processor that picked it up would run it with no process and so with
	 * the kernel's address space, and the program would fault on its own
	 * first instruction. (BG-148) */
	thread_start(t);

	return t;
}

/* A program from a file, rather than from a byte array compiled into us.
 *
 * The same shape as `user_thread_create` above and deliberately not folded into
 * it: that one is handed instructions and puts them at a fixed address, this
 * one is handed a *file* and does what the file says. The difference is not the
 * copy, it is who decides the layout -- and merging them would mean one
 * function with a flag meaning "and also, ignore everything you were told".
 *
 * That one will go when the last blob does. It cannot go yet: the blobs test
 * the system-call boundary from ring 3 and this loader would have to be working
 * for them to run at all, which is the wrong order to depend in.
 */
struct thread *user_elf_create(const char *name, const void *image, u64 len,
			       enum elf_result *why)
{
	struct thread *t;
	struct process *p;
	struct addrspace *as;
	enum elf_result r;
	u64 entry = 0;

	if (why)
		*why = ELF_OK;

	p = process_create(name, 0, UID_NOBODY, UID_NOBODY);
	if (!p) {
		kprintf("  user: no room in the process table for %s\n", name);
		return 0;
	}

	p->personality = &personality_recon;

	as = addrspace_create();
	if (!as) {
		kprintf("  user: no address space for %s\n", name);
		return 0;
	}

	process_set_space(p, as);
	addrspace_release(as);

	/* The file decides where its own code and data go, inside the half it
	 * is allowed to name. Nothing is mapped if any of it is refused. */
	r = elf_load(as, image, len, &entry);
	if (r != ELF_OK) {
		if (why)
			*why = r;
		kprintf("  user: %s was refused -- %s\n", name, elf_why(r));
		return 0;
	}

	/* The stack is the kernel's business rather than the file's. A program
	 * that could place its own stack could place it over its own code, and
	 * an ELF has no way to ask for one anyway -- `PT_GNU_STACK` carries
	 * permissions, not an address. Reserved rather than mapped, so the
	 * first push faults it in like any other program's. */
	if (!addrspace_reserve(as, USER_STACK_TOP - USER_STACK_MAX,
			       USER_STACK_MAX,
			       VM_READ | VM_WRITE | VM_USER)) {
		kprintf("  user: no room to reserve a stack for %s\n", name);
		return 0;
	}

	t = thread_create_stopped(name, user_thread_start, (void *)(uintptr_t)entry);
	if (!t)
		return 0;

	t->personality = &personality_recon;
	process_attach(p, t);

	/* Only now may anything else see it. Started before the attach, a
	 * processor that picked it up would run it with no process and so with
	 * the kernel's address space, and the program would fault on its own
	 * first instruction. (BG-148) */
	thread_start(t);

	return t;
}

/* --- and the test that runs one ------------------------------------------
 *
 * The program checks itself and says which check failed through its exit code,
 * for the same reason the facts program does: the thing most likely to be wrong
 * is not reachable from the kernel side. Whether `.bss` arrived zeroed is a
 * question only the program can answer, because from here the page is
 * indistinguishable from any other page the loader mapped.
 *
 * Success is 55, not 0. The exit code is read out of a field that is zero
 * before the program runs and zero if it never reached its exit call, so a
 * test that treats zero as success passes in both of the cases it exists to
 * catch.
 */
/* What to say when a program does not finish.
 *
 * "The program never reached its exit call" is true and useless: it is the same
 * sentence whether the program never ran, ran and faulted, ran and stopped, or
 * is still running. Those are four different bugs.
 *
 * It cost real time on 10 September. A program was dying at its own entry point
 * and the test reported a timeout -- because `exits` is only incremented by
 * sys_exit, and a program killed by a fault never gets there. The fault line was
 * in the log the whole time and nothing in the message pointed at it. (BG-150)
 */
static void say_how_far_it_got(const struct thread *t, u64 calls_before,
			       u64 faults_before)
{
	static const char *const state[] = {
		"ready", "running", "?", "blocked", "?", "finished"
	};

	if (t)
		kprintf("  user: it is %s, on processor %d, after %lu tick%s\n",
			(unsigned)t->state < sizeof(state) / sizeof(state[0])
				? state[t->state] : "in a state with no name",
			t->cpu, (unsigned long)t->ran_ticks,
			t->ran_ticks == 1 ? "" : "s");

	kprintf("  user: %lu system call%s served since it started, and %lu "
		"program%s ended for faulting\n",
		(unsigned long)(calls_served - calls_before),
		calls_served - calls_before == 1 ? "" : "s",
		(unsigned long)(faults - faults_before),
		faults - faults_before == 1 ? "" : "s");
}

bool user_elf_test(void)
{
	static const char *const why[] = {
		"it exited 0, which is not one of its codes -- so it never"
		" reached its own exit call",			/* 0 */
		"unused",					/* 1 */
		"the writable segment did not carry the file's bytes",
		"the .bss it was given was not zero",
		"the .bss it was given did not keep what it wrote",
		"a register arrived holding the kernel's data",
	};

	u64 exits_before  = exits;
	u64 calls_before  = calls_served;
	u64 faults_before = faults;
	enum elf_result r = ELF_OK;
	struct thread *t;
	u64 deadline;

	t = user_elf_create("hello-elf", user_elf_image,
			    user_elf_image_len, &r);
	if (!t) {
		kprintf("  user: could not start the program%s%s\n",
			r == ELF_OK ? "" : " -- ",
			r == ELF_OK ? "" : elf_why(r));
		return false;
	}

	deadline = time_monotonic_ns() + 2000000000ULL;
	while (exits == exits_before && time_monotonic_ns() < deadline)
		sched_yield();

	if (exits == exits_before) {
		kputs("  user: the loaded program never reached its exit "
		      "call\n");
		say_how_far_it_got(t, calls_before, faults_before);
		return false;
	}

	if (last_exit_code == 55)
		return true;

	if (last_exit_code > 0 &&
	    (u64)last_exit_code < sizeof(why) / sizeof(why[0]))
		kprintf("  user: %s\n", why[last_exit_code]);
	else
		kprintf("  user: the loaded program exited with %ld, which is "
			"not one of its own codes\n", (long)last_exit_code);

	return false;
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


/* --- signals, end to end --------------------------------------------------
 *
 * The kernel-side checks live in signal.c and are about decisions. This one is
 * about the boundary: a real program in ring 3 registers a handler, sends
 * itself a signal, and the handler runs on its own stack and returns.
 *
 * It exits with 7, and the number is chosen so the failures are
 * distinguishable rather than merely detectable:
 *
 *   0  the handler never ran
 *   1  sigaction refused the registration
 *   2  the handler ran with the wrong signal number in RDI
 *   7  it ran once, with the right number, and the restore put RBX back
 *  14  it ran twice, which means the pending bit was not cleared
 *
 * RBX carries the count deliberately. It is callee-saved, so what is being
 * checked is that the kernel put the *program's own* registers back -- a
 * scratch register would prove nothing, because nothing promises to keep one
 * across a call.
 */
bool user_signal_test(void)
{
	extern const unsigned char user_signal_program[];
	extern const u64 user_signal_program_len;

	u64 exits_before = exits;
	struct thread *t;
	u64 deadline;

	t = user_thread_create("signals", user_signal_program,
			       user_signal_program_len);

	if (!t) {
		kputs("  signals: could not create a user thread\n");
		return false;
	}

	deadline = time_monotonic_ns() + 2000000000ULL;

	while (exits == exits_before && time_monotonic_ns() < deadline)
		sched_yield();

	if (exits == exits_before) {
		kputs("  signals: the program never reached its exit call -- "
		      "the handler did not return\n");
		return false;
	}

	switch (last_exit_code) {
	case 7:
		return true;

	case 0:
		kputs("  signals: the handler never ran, so nothing was "
		      "delivered on the way back to user mode\n");
		return false;

	case 1:
		kputs("  signals: sigaction refused a handler with a "
		      "restorer\n");
		return false;

	case 2:
		kputs("  signals: the handler ran with the wrong signal "
		      "number\n");
		return false;

	case 14:
		kputs("  signals: the handler ran twice -- the pending bit "
		      "was not cleared before delivery\n");
		return false;

	default:
		kprintf("  signals: the program exited with %ld, which is not "
			"a number it can produce -- so RBX did not survive "
			"the handler\n", (long)last_exit_code);
		return false;
	}
}

bool user_self_test(void)
{
	/* Both are defined in the architecture's user_test.S. The length is a
	 * .quad there, so it is a u64 here -- declaring it `unsigned int` would
	 * read the right value on a little-endian machine and be wrong for a
	 * reason no one would enjoy finding. */
	extern const unsigned char user_test_program[];
	extern const u64 user_test_program_len;

	u64 exits_before  = exits;
	u64 calls_before  = calls_served;
	u64 faults_before = faults;
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
		say_how_far_it_got(t, calls_before, faults_before);
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

/* --- a program from a volume ---------------------------------------------
 *
 * The last piece of "a process is a program". Until now a program came from a
 * byte array the kernel was compiled with -- the ELF loader made it a *file*
 * rather than an array, but the file was still inside the kernel image.
 *
 * A path is what makes it somebody else's program. Nothing here knows what
 * filesystem the path names or how the bytes get read: it opens a descriptor,
 * reads until the end, and hands the result to the loader that already exists.
 * That is the whole point of the VFS being underneath it -- the same function
 * will load a program off a pipe, or out of a devfs, or from whatever comes
 * next, without a line changing.
 */
struct thread *user_exec_path(const char *name, const char *path,
			      enum elf_result *why, i64 *error)
{
	struct file *f;
	u8 *image;
	u64 total = 0;
	struct thread *t;

	*error = SYS_OK;
	*why = ELF_OK;

	f = file_open_path(path, OPEN_READ, 0, error);
	if (!f)
		return NULL;

	if (!f->ops->read) {
		file_release(f);
		*error = SYS_EPERM;
		return NULL;
	}

	image = kmalloc(USER_EXEC_MAX);
	if (!image) {
		file_release(f);
		*error = SYS_ENOSPC;
		return NULL;
	}

	/* Read in a loop rather than in one call, because "read" does not
	 * promise to give everything asked for in one go -- a pipe certainly
	 * will not -- and a loader that assumed otherwise would work on a disk
	 * and truncate everything else. */
	for (;;) {
		i64 n = f->ops->read(f, image + total, USER_EXEC_MAX - total);

		if (n < 0) {
			*error = n;
			kfree(image);
			file_release(f);
			return NULL;
		}

		if (n == 0)
			break;

		total += (u64)n;

		if (total == USER_EXEC_MAX) {
			/* One more byte would not fit, so this file is at
			 * least this big -- and a program loaded from the
			 * first N bytes of itself is the shape of fault this
			 * project keeps refusing to build. */
			*error = SYS_ENOSPC;
			kfree(image);
			file_release(f);
			return NULL;
		}
	}

	file_release(f);

	t = user_elf_create(name, image, total, why);

	/* The loader copies what it needs into the address space, so the image
	 * is the reader's and goes back now rather than being leaked for the
	 * life of the program. */
	kfree(image);
	return t;
}

/* Writes the kernel's own program onto the volume, through a descriptor, and
 * then runs it from there.
 *
 * The loop is the assertion. A program that only ever came from an array proves
 * the loader; a program written out through the file interface, read back
 * through the file interface and then executed proves that the three things fit
 * together -- and it is the first time in this kernel that a file on a disk has
 * become a running process.
 */
bool user_exec_path_test(void)
{
	static const char path[] = "/hello.elf";
	struct file *f;
	i64 err = SYS_OK;
	enum elf_result why = ELF_OK;
	u64 exits_before, calls_before, faults_before;
	struct thread *t;
	u64 deadline;

	if (!rootfs()) {
		kputs("  user: no volume to put a program on\n");
		return true;
	}

	f = file_open_path(path, OPEN_WRITE | OPEN_CREATE, 0755, &err);
	if (!f) {
		kprintf("  user: could not create %s (%ld)\n", path,
			(long)err);
		return false;
	}

	if (f->ops->write(f, user_elf_image, user_elf_image_len) !=
	    (i64)user_elf_image_len) {
		kputs("  user: writing the program to the volume was short\n");
		file_release(f);
		return false;
	}

	err = file_release(f);
	if (err != SYS_OK) {
		kprintf("  user: committing the program failed (%ld)\n",
			(long)err);
		return false;
	}

	exits_before  = exits;
	calls_before  = calls_served;
	faults_before = faults;

	t = user_exec_path("hello-from-disk", path, &why, &err);
	if (!t) {
		kprintf("  user: could not run %s (%ld%s%s)\n", path,
			(long)err,
			why == ELF_OK ? "" : ", ",
			why == ELF_OK ? "" : elf_why(why));
		return false;
	}

	deadline = time_monotonic_ns() + 2000000000ULL;
	while (exits == exits_before && time_monotonic_ns() < deadline)
		sched_yield();

	if (exits == exits_before) {
		kputs("  user: the program from the volume never reached its "
		      "exit call\n");
		say_how_far_it_got(t, calls_before, faults_before);
		return false;
	}

	/* 55 is the program's own success code, chosen because zero is what a
	 * program that never ran also produces. */
	if (last_exit_code != 55) {
		kprintf("  user: the program from the volume exited with %ld "
			"rather than 55\n", (long)last_exit_code);
		return false;
	}

	return true;
}
