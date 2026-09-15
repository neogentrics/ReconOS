#include <recon/kernel/fbcon.h>
#include <recon/kernel/fbdev.h>
#include <recon/kernel/user.h>

#include <recon/kernel/power.h>
#include <recon/kernel/signal.h>
#include <recon/kernel/identity.h>
#include <recon/kernel/process.h>
#include <recon/kernel/arch.h>
#include <recon/kernel/cpu.h>
#include <recon/kernel/random.h>
#include <recon/kernel/rootfs.h>
#include <recon/kernel/vfs.h>
#include <recon/kernel/net.h>
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
/* --- sockets ---------------------------------------------------------------
 *
 * The socket layer has existed since 12 September with no caller outside its
 * own self-test. These five calls are the doorway; everything they reach was
 * already built and already tested.
 *
 * A socket is a `struct file`, so there is nothing here for sending, receiving
 * or closing -- `SYS_WRITE`, `SYS_READ` and `SYS_CLOSE` do those, on the same
 * descriptor, with the same calls a program uses for a file.
 */
static struct socket *socket_for_fd(struct process *p, u64 fd)
{
	struct file *f = fd_get(p, (int)fd);

	/* `file_socket` returns null for a descriptor that names something
	 * else, rather than reading a pipe's private pointer as a socket. The
	 * type is decided by the ops table, which is the thing that actually
	 * governs behaviour. */
	return f ? file_socket(f) : NULL;
}

static i64 sys_socket(u64 type, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct process *p = caller();
	struct socket *s;
	struct file *f;
	int fd;

	if (type != SOCK_STREAM && type != SOCK_DGRAM)
		return SYS_EINVAL;

	s = socket_create((int)type);
	if (!s)
		return SYS_ENOSPC;

	f = socket_file_create(s);
	if (!f) {
		socket_close(s);
		return SYS_ENOMEM;
	}

	fd = fd_install(p, f);

	/* Our own reference goes either way: the descriptor holds one now, and
	 * on failure dropping it is what closes the socket nobody got. */
	file_release(f);

	if (fd < 0)
		return SYS_EMFILE;

	return fd;
}

static i64 sys_bind(u64 fd, u64 addr, u64 port, u64 a3, u64 a4, u64 a5)
{
	struct socket *s = socket_for_fd(caller(), fd);

	if (!s)
		return SYS_EBADF;

	if (port > 0xFFFF)
		return SYS_EINVAL;

	return socket_bind(s, (ipv4_addr)addr, (u16)port) ? SYS_OK : SYS_EINVAL;
}

static i64 sys_listen(u64 fd, u64 backlog, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct socket *s = socket_for_fd(caller(), fd);

	if (!s)
		return SYS_EBADF;

	return socket_listen(s, (unsigned)backlog) ? SYS_OK : SYS_EINVAL;
}

/* Takes the next waiting connection, or says there is none.
 *
 * **EAGAIN rather than blocking.** `socket_accept` returns null when nobody is
 * waiting, and a call that blocked instead would need a wait queue on the
 * listener and a way to be interrupted -- neither of which exists yet. A
 * server polls. That is honest about what this can do, and a program written
 * against it keeps working when blocking arrives.
 */
static i64 sys_accept(u64 fd, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct process *p = caller();
	struct socket *s = socket_for_fd(p, fd);
	struct socket *c;
	struct file *f;
	int nfd;

	if (!s)
		return SYS_EBADF;

	c = socket_accept(s);
	if (!c)
		return SYS_EAGAIN;

	f = socket_file_create(c);
	if (!f) {
		socket_close(c);
		return SYS_ENOMEM;
	}

	nfd = fd_install(p, f);
	file_release(f);

	if (nfd < 0)
		return SYS_EMFILE;

	return nfd;
}

static i64 sys_connect(u64 fd, u64 addr, u64 port, u64 a3, u64 a4, u64 a5)
{
	struct socket *s = socket_for_fd(caller(), fd);

	if (!s)
		return SYS_EBADF;

	if (port > 0xFFFF)
		return SYS_EINVAL;

	return socket_connect(s, (ipv4_addr)addr, (u16)port)
		? SYS_OK : SYS_EIO;
}

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

/* Memory a program can have because it asked for it, with no file behind it.
 *
 * `SYS_MAP` began as a way to put the framebuffer where a program could draw on
 * it, and a descriptor of -1 is how every other system spells "no file, just
 * memory". `docs/KERNEL-WANTS.md` asked for it in those words -- *"two calls
 * over machinery that exists"* -- and the machinery really did exist: this adds
 * a reservation and the fault handler that has always served the stack serves
 * the heap without being told about it.
 *
 * So there is no allocation here. A reservation costs no physical memory at all
 * and a page appears when the program writes to it, which is what makes an
 * allocator that asks for a megabyte at a time affordable on a machine that has
 * far less than a megabyte to spare per program.
 *
 * **-1 rather than a flag or a number of its own.** The library asked with -1
 * before this existed, because that is the spelling `KERNEL-WANTS` proposed and
 * the one every other system uses; a number of ours would have been a second
 * thing to know and a third thing to keep in step. Nothing in `userland/` is
 * rebuilt or relinked by this landing -- `mem_recon.c` sends exactly the call it
 * has always sent, and now gets an address back.
 *
 * Zeroed, and that is a promise rather than an accident of the fault handler:
 * `malloc` does not clear what it hands out, so a program reading an
 * uninitialised allocation must not be reading whatever the last program left in
 * that page. Untouched pages read from the one shared page of zeroes and a write
 * is given a fresh page copied from it, so the guarantee holds in both halves.
 */
static i64 map_anonymous(struct process *p, u64 length)
{
	u64 va, size;

	if (length == 0)
		return SYS_EINVAL;

	size = (length + PAGE_SIZE - 1) & ~((u64)PAGE_SIZE - 1);

	/* A length within a page of the top rounds up to zero, and a zero size
	 * would reserve nothing and report an address for it. */
	if (size < length)
		return SYS_EINVAL;

	va = p->space->map_next ? p->space->map_next : USER_MAP_BASE;

	/* Non-wrapping, like every other range check here. */
	if (size > USER_MAP_END - va)
		return SYS_ENOMEM;

	/* `_more`, so a heap that grows costs one region however far it grows.
	 * A device mapped in between two heap requests breaks the run and the
	 * next request starts a second one, which is correct rather than
	 * unfortunate: the addresses really are not adjacent. */
	if (!addrspace_reserve_more(p->space, (vaddr_t)va, size,
				    VM_READ | VM_WRITE | VM_USER))
		return SYS_ENOMEM;

	p->space->map_next = va + size;
	return (i64)va;
}

/* Put a file's memory in the caller's map.
 *
 * The mapping outlives the descriptor on purpose, and that is worth saying
 * because a file-backed mapping does the opposite -- it holds a reference,
 * because a page faulted in later has to be read from somewhere. There is
 * nothing to read here. The pages are a device at a fixed physical address, so
 * once they are in the tables the file has no further part in it and a program
 * can close the descriptor and keep drawing.
 *
 * What that does *not* survive is the mode changing underneath it: a program
 * holding a mapping of a framebuffer that has since been re-sized is drawing
 * into memory that is no longer the screen. Nothing re-sizes after boot today,
 * and when something does this is the line it has to answer for.
 */
static i64 sys_map(u64 fd, u64 length, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct process *p = caller();
	struct file *f;
	paddr_t pa;
	u64 have, va, size;
	unsigned flags;

	(void)a2; (void)a3; (void)a4; (void)a5;

	if (!p || !p->space)
		return SYS_EPERM;

	/* No file. Checked before `fd_get`, because -1 is not a descriptor that
	 * happens to be closed -- it is the caller saying there is no file, and
	 * answering EBADF for it is what this call used to do. */
	if ((int)fd == -1)
		return map_anonymous(p, length);

	f = fd_get(p, (int)fd);
	if (!f)
		return SYS_EBADF;

	/* A file that is a stream of bytes is not a window onto memory, and
	 * that is a different answer from a mapping that failed. */
	if (!f->ops->map) {
		file_release(f);
		return SYS_EPERM;
	}

	if (!f->ops->map(f, &pa, &have, &flags)) {
		file_release(f);
		return SYS_ENODEV;
	}

	file_release(f);

	/* Refused, not clamped. A program that asked for a screen's worth, was
	 * handed half of one, and was told it succeeded draws off the end of
	 * what it got -- and the fault lands nowhere near the mistake. */
	if (length == 0 || length > have)
		return SYS_EINVAL;

	/* Whole pages, because that is the unit the tables work in. Rounding up
	 * gives the program a little more than it asked for rather than a little
	 * less, and the extra is still inside the device. */
	size = (length + PAGE_SIZE - 1) & ~((u64)PAGE_SIZE - 1);

	/* **Zero is the starting value, on purpose.**
	 *
	 * `addrspace_create` clears the whole slot rather than naming the fields
	 * it knows about -- that is KF-147's fix, and what it was for was a
	 * process inheriting the last occupant's regions. A cursor that had to be
	 * set to USER_MAP_BASE there would be a field that must be non-zero in a
	 * structure deliberately zeroed wholesale: a second place to keep in
	 * step, and the next field added would be wrong the same way.
	 *
	 * So an untouched space reads zero and means it. Using it directly would
	 * map a framebuffer at address zero -- the one page kept unmapped so that
	 * a null pointer faults. */
	va = p->space->map_next ? p->space->map_next : USER_MAP_BASE;

	/* Non-wrapping, like every other range check here: `va + size` past the
	 * end of the area would compare as comfortably inside. */
	if (size > USER_MAP_END - va)
		return SYS_ENOMEM;

	/* VM_USER is what makes it reachable from ring 3 at all, and it is added
	 * here rather than asked of the file -- a file deciding whether its
	 * memory is reachable from user mode would be a file deciding who may
	 * touch a device. */
	if (!addrspace_map(p->space, (vaddr_t)va, pa, size, flags | VM_USER))
		return SYS_ENOMEM;

	p->space->map_next = va + size;
	return (i64)va;
}

/* What the screen is. See SYS_SCREEN in user.h for why pitch is the point. */
static i64 sys_screen(u64 buf, u64 len, u64 a2, u64 a3, u64 a4, u64 a5)
{
	struct fb_info info;
	int err;
	u64 copy;

	(void)a2; (void)a3; (void)a4; (void)a5;

	if (!user_range_ok(buf, len)) {
		refusals++;
		return SYS_EFAULT;
	}

	err = fbdev_describe(&info);
	if (err != SYS_OK)
		return err;

	/* Whole or nothing, and the size comes back either way -- so asking with
	 * a length of zero is how a program finds out how much to offer, and a
	 * bigger answer than was offered means nothing was written. A caller
	 * handed the first half of a struct alongside a success has no way to
	 * know which half it got. */
	copy = sizeof(info);
	if (len >= copy)
		kmemcpy((void *)(uintptr_t)buf, &info, (size_t)copy);

	return (i64)copy;
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

	/* The memory, not the span it is scattered across. A program asking a
	 * machine how much memory it has does not want the size of the hole in
	 * the middle of it (KF-224). */
	m.memory_bytes      = (u64)pmm_usable_pages() * PAGE_SIZE;
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

/* One directory, and its parents must already be there.
 *
 * The same path handling as sys_create and for the same reasons: copied into
 * the kernel's own buffer at the length the caller gave, and terminated here
 * -- so a path with a zero byte inside it is *short* rather than a way to get
 * one string past a check and have a different one used.
 */
static i64 sys_mkdir(u64 path, u64 path_len, u64 mode, u64 a3, u64 a4, u64 a5)
{
	char kpath[CREATE_PATH_MAX];
	enum reconfs_status st;

	if (path_len == 0 || path_len >= sizeof(kpath))
		return SYS_EINVAL;

	if (!user_range_ok(path, path_len)) {
		refusals++;
		return SYS_EFAULT;
	}

	kmemcpy(kpath, (const void *)(uintptr_t)path, (size_t)path_len);
	kpath[path_len] = '\0';

	st = rootfs_create_directory(kpath, (u32)mode);
	if (st == RECONFS_OK)
		return SYS_OK;

	return user_status_from_reconfs(st);
}

/* Stopping or restarting the machine, which the desktop has been unable to
 * ask for since it had a Shut Down button.
 *
 * Everything under this is already built: `power_off` and `power_restart` both
 * check CAP_SHUTDOWN themselves and both return an `enum power_result` saying
 * which of four things went wrong. This function's whole job is to refuse an
 * action it does not recognise and to carry those reasons out to user mode
 * without flattening them.
 *
 * **Flattening them is the thing this must not do.** A desktop that shows
 * "could not shut down" for a machine with no ACPI, for a program that was not
 * given the capability, and for an architecture this kernel cannot stop is a
 * desktop nobody can act on. Three sentences, three numbers.
 *
 * An unknown action is EINVAL rather than a default to "off". A program asking
 * for something this kernel has not heard of is a program from a later version
 * than this one, and turning the machine off because it asked for something
 * else is the worst possible reading of it.
 */
static i64 sys_power(u64 action, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	enum power_result r;

	(void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

	switch (action) {
	case POWER_ACTION_OFF:
		r = power_off();
		break;
	case POWER_ACTION_RESTART:
		r = power_restart();
		break;
	default:
		return SYS_EINVAL;
	}

	/* Reached only where it did not happen. */
	switch (r) {
	case POWER_REFUSED:       return SYS_EPERM;
	case POWER_NO_REGISTER:   return SYS_ENOPOWER;
	case POWER_NO_SLEEP_STATE: return SYS_ENOSTATE;
	case POWER_UNSUPPORTED:   return SYS_ENOMECH;
	case POWER_OK:
	default:                  return SYS_EIO;
	}
}

/* Can a program reach SYS_POWER, and is an action this kernel does not know
 * refused rather than obeyed?
 *
 * Driven through `syscall_dispatch` with the number, not by calling
 * `sys_power`. That is the point rather than a detail: `sys_power` is static,
 * and what is being asserted is that the number arrives there. Calling the
 * function directly would prove the function works and say nothing about
 * whether any program can reach it -- BG-055 exactly, *code written, compiled,
 * shipped and never once run*.
 *
 * **An unknown action must not default to "off".** A program built against a
 * later kernel, asking for something this one has never heard of, would stop
 * the machine. EINVAL is the only safe reading of a request that cannot be
 * understood.
 *
 * --- what is deliberately not here, and what it would have cost -------------
 *
 * The other half -- *a program without CAP_SHUTDOWN is refused* -- is the
 * safety-critical one and cannot be asked from here. The first draft of this
 * test dropped the capability and called SYS_POWER with a real action,
 * expecting EPERM. It would have **turned the machine off on every boot**:
 * `identity.c` states that a kernel thread's capabilities are the absence of a
 * process rather than a field, so `capability_drop` is a documented no-op here
 * and `capable(CAP_SHUTDOWN)` is true. The refusal would not have come; the
 * obedience would have.
 *
 * Testing it needs a real process, which needs a user program, which is
 * assembly in two architectures. Owed rather than skipped quietly.
 */
bool user_power_test(void)
{
	extern const unsigned char user_test_power[];
	extern const u64 user_test_power_len;

	u64 exits_before = exits;
	struct thread *t;
	u64 deadline;
	i64 r;

	/* From here first, where it is safe to ask: an action this kernel does
	 * not know must be refused rather than defaulted to "off". */
	r = syscall_dispatch(SYS_POWER, 0xBEEF, 0, 0, 0, 0, 0);
	if (r != SYS_EINVAL) {
		kprintf("  power: an unknown action answered %ld, wanted "
			"EINVAL\n", (long)r);
		return false;
	}

	/* And then the half that needs a process, which is why it is a
	 * program: a kernel thread holds every capability by construction and
	 * would be obeyed rather than refused.
	 *
	 * What it proves is *a program that does not hold CAP_SHUTDOWN is
	 * refused*. It asks to drop the capability first, which is a no-op
	 * today -- `process_create` starts a process with none -- and then
	 * reads back what it still holds and asks nothing of the machine
	 * unless the capability is genuinely gone. That guard cannot fire
	 * today. It is there because this runs on every path in the matrix,
	 * and the day a process starts with capabilities is not a day to find
	 * out that this program powers off every guest. */
	t = user_thread_create("no-shutdown", user_test_power,
			       user_test_power_len);
	if (!t) {
		kputs("  power: could not start the program that asks\n");
		return false;
	}

	deadline = time_monotonic_ns() + 2000000000ULL;
	while (exits == exits_before && time_monotonic_ns() < deadline)
		sched_yield();

	if (exits == exits_before) {
		kputs("  power: the program never reached its exit call\n");
		return false;
	}

	switch (last_exit_code) {
	case 60:
		return true;
	case 2:
		/* Unreachable while a process starts with no capabilities,
		 * which is today. Reported as a failure rather than a pass
		 * because if it ever does happen, the refusal went untested --
		 * and the program stopping rather than asking is the only
		 * reason the machine is still here to print this. */
		kputs("  power: CAP_SHUTDOWN was held after being dropped, so "
		      "the refusal went untested -- and the program was right "
		      "to stop rather than ask\n");
		return false;
	case 1:
		kputs("  power: a program without CAP_SHUTDOWN was not "
		      "refused\n");
		return false;
	default:
		kprintf("  power: the program exited with %ld, which is not "
			"one of its own codes\n", (long)last_exit_code);
		return false;
	}
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
		[SYS_MKDIR]    = sys_mkdir,
		[SYS_GETUID]   = sys_getuid,
		[SYS_GETGID]   = sys_getgid,
		[SYS_GETCAPS]  = sys_getcaps,
		[SYS_DROPCAP]  = sys_dropcap,
		[SYS_LIST]     = sys_list,
		[SYS_MAP]      = sys_map,
		[SYS_SCREEN]   = sys_screen,
		[SYS_POWER]    = sys_power,
		[SYS_SOCKET]   = sys_socket,
		[SYS_BIND]     = sys_bind,
		[SYS_LISTEN]   = sys_listen,
		[SYS_ACCEPT]   = sys_accept,
		[SYS_CONNECT]  = sys_connect,
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
	 * first instruction. (KF-148) */
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
	 * first instruction. (KF-148) */
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
 * in the log the whole time and nothing in the message pointed at it. (KF-150)
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

/*
 * --- A program written in C ---
 *
 * The desktop is ninety thousand lines of C, and until this test every program
 * that had ever run in ring 3 on this kernel was assembly. That is not a
 * difference of degree: assembly can be written against a system call by hand,
 * and C needs a stack that is aligned the way the ABI says, a data segment its
 * globals actually live in, and a compiler that has not replaced a loop with a
 * call to something that is not there.
 *
 * So this runs `user/paint.c`, built by the same compiler as the kernel with
 * the kernel's flags removed, linked against nothing. It asks the screen's
 * size, refuses a kernel whose description is a different size from the
 * structure it was built against, maps /dev/fb0, fills it, and reads back four
 * markers placed through `pitch`.
 *
 * **Every marker is placed through pitch and never through width times four.**
 * On this emulator the two are equal, which is exactly why a program that got
 * it wrong would pass here and shear on a laptop -- so the two markers on the
 * right-hand edge are the ones that would move, and they are read back.
 */
/*
 * --- The first program that is not a test ---
 *
 * `hello.S` proved the loader. `paint.c` proved that C compiled against these
 * headers runs here at all. Both exit with a code this kernel reads, which is
 * what makes them tests.
 *
 * `recon_init` is what somebody sees. It asks SYS_MACHINE what the machine is,
 * SYS_SCREEN how the display is arranged, SYS_LIST what is on the volume, and
 * draws a screen a person standing in front of a computer that has just
 * started can read. Then it yields forever, because there is nothing to hand
 * the screen to.
 *
 * **It is not waited for.** Every other user program here is created, waited
 * on and read; waiting for this one would be waiting for the machine to be
 * switched off.
 */
bool user_start_first_screen(void)
{
	enum elf_result r = ELF_OK;
	struct thread *t;
	struct fb_info info;
	i64 err = SYS_OK;

	/*
	 * No screen is not a failure. A serial-only boot is a real
	 * configuration this kernel supports, and a machine with no display
	 * has nowhere to put a first-boot screen -- so it says so and the
	 * boot carries on.
	 */
	if (fbdev_describe(&info) != SYS_OK) {
		kputs("no screen on this machine, so nothing to draw on\n");
		return false;
	}

	/* --- the volume first ----------------------------------------------
	 *
	 * The copy inside the kernel image is a fallback, not the system. A
	 * machine that has been installed onto has its own program on its own
	 * disk, and the point of preferring it is that the two can then be
	 * built and replaced separately -- which is the difference between a
	 * kernel that *contains* a system and a kernel that *starts* one.
	 *
	 * Any refusal falls through to the built-in copy rather than failing
	 * the boot: a volume with no program on it is every machine that has
	 * not been installed onto yet, including every disk in the test rig.
	 */
	t = user_exec_path("recon-init", RECON_SYSTEM_INIT, &r, &err);
	if (t) {
		kprintf("the system: %s, %s\n", RECON_SYSTEM_INIT,
			"from the volume");
		return true;
	}

	/* Said, and said with the reason, because "it used the built-in copy"
	 * has two causes that want different actions: there is no program on
	 * the volume, or there is one and it would not load. The second is a
	 * broken install and the first is an ordinary machine. */
	kprintf("the system: no %s (%ld%s%s), using the copy inside the "
		"kernel\n", RECON_SYSTEM_INIT, (long)err,
		r == ELF_OK ? "" : ", ",
		r == ELF_OK ? "" : elf_why(r));

	t = user_elf_create("recon-init", init_elf_image, init_elf_image_len,
			    &r);
	if (!t) {
		kprintf("the first screen could not be started%s%s\n",
			r == ELF_OK ? "" : " -- ",
			r == ELF_OK ? "" : elf_why(r));
		return false;
	}

	return true;
}

bool user_c_program_test(void)
{
	static const char *const why[] = {
		"it exited 0, which is not one of its codes -- so it never"
		" reached its own exit call",			/* 0 */
	};
	static const struct {
		i64 code;
		const char *what;
	} named[] = {
		{ 60, "the kernel's screen description is a different size"
		      " from the one the program was built against" },
		{ 61, "SYS_SCREEN refused" },
		{ 62, "/dev/fb0 would not open" },
		{ 63, "SYS_MAP refused" },
		{ 64, "the screen's numbers do not describe a screen" },
		{ 65, "a pixel did not read back -- the mapping is not the"
		      " screen, or pitch was not honoured" },
	};

	u64 exits_before  = exits;
	u64 calls_before  = calls_served;
	u64 faults_before = faults;
	enum elf_result r = ELF_OK;
	struct thread *t;
	u64 deadline;
	size_t i;

	/*
	 * No screen is not a failure. A serial-only boot is a real
	 * configuration this kernel supports, and a test that failed there
	 * would make every headless verification run red for a reason that is
	 * not a fault.
	 */
	{
		struct fb_info info;

		if (fbdev_describe(&info) != SYS_OK) {
			kputs("  a C program: no screen on this machine, so"
			      " nothing to draw on\n");
			return true;
		}
	}

	t = user_elf_create("paint-c", paint_elf_image, paint_elf_image_len,
			    &r);
	if (!t) {
		kprintf("  a C program: could not start it%s%s\n",
			r == ELF_OK ? "" : " -- ",
			r == ELF_OK ? "" : elf_why(r));
		return false;
	}

	deadline = time_monotonic_ns() + 2000000000ULL;
	while (exits == exits_before && time_monotonic_ns() < deadline)
		sched_yield();

	if (exits == exits_before) {
		kputs("  a C program: it never reached its exit call\n");
		say_how_far_it_got(t, calls_before, faults_before);
		return false;
	}

	if (last_exit_code == 55) {
		kprintf("  a C program: compiled from C, loaded, and its"
			" pixels are on the screen\n");
		return true;
	}

	for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
		if (last_exit_code == named[i].code) {
			kprintf("  a C program: %s\n", named[i].what);
			return false;
		}
	}

	if (last_exit_code == 0)
		kprintf("  a C program: %s\n", why[0]);
	else
		kprintf("  a C program: exited with %ld, which is not one of"
			" its own codes\n", (long)last_exit_code);
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

/* Checkpoint 21's second half, end to end.
 *
 * A program opened /dev/fb0, was told the screen's shape, mapped the whole of
 * it, and stored two words. This checks the words are *on the screen* -- read
 * back through the direct map at the physical address the device reported,
 * which is a second route to the same memory and the only one that can
 * disagree with the first.
 *
 * The exit code alone would not do. A mapping that handed the program a fresh
 * anonymous page would satisfy every step it can see: the stores land, the
 * read-back matches, and it exits 21 having drawn on nothing at all. That is
 * KF-141's shape -- a driver that records what it asked for passes any test
 * that asks what it recorded -- and the answer is the same one: ask the
 * hardware.
 */
bool user_framebuffer_test(void)
{
	extern const unsigned char user_fb_program[];
	extern const u64 user_fb_program_len;

	struct fb_info info;
	u64 exits_before = exits;
	struct thread *t;
	u64 deadline;
	const volatile u32 *pixels;
	u32 at_origin, at_row1;

	/* No screen is not a failure. Most of the verification matrix boots
	 * with none -- every aarch64 path, because AAVMF provides no
	 * framebuffer -- and a test that reported FAIL there would be reporting
	 * on the emulator's choices rather than on this kernel. */
	if (fbdev_describe(&info) != SYS_OK) {
		kputs("  framebuffer: no screen on this machine, so there is "
		      "nothing for a program to map\n");
		return true;
	}

	/* Two rows, because the second marker goes at the start of row 1. A
	 * screen one row tall is not one this can say anything about. */
	if (info.height < 2 || info.pitch < 4) {
		kprintf("  framebuffer: the screen is %ux%u with a pitch of "
			"%u, which is too small to check a stride against\n",
			info.width, info.height, info.pitch);
		return false;
	}

	t = user_thread_create("fb", user_fb_program, user_fb_program_len);
	if (!t) {
		kputs("  framebuffer: could not create a user thread\n");
		return false;
	}

	deadline = time_monotonic_ns() + 2000000000ULL;
	while (exits == exits_before && time_monotonic_ns() < deadline)
		sched_yield();

	if (exits == exits_before) {
		kputs("  framebuffer: the program never reached its exit "
		      "call\n");
		return false;
	}

	/* Every step has its own number, so a failure says how far it got
	 * rather than only that it stopped. */
	switch (last_exit_code) {
	case 21:
		break;
	case 1:
		kputs("  framebuffer: the program could not open /dev/fb0\n");
		return false;
	case 2:
		kputs("  framebuffer: SYS_SCREEN did not describe the screen "
		      "-- a program built against a different kernel would "
		      "look exactly like this\n");
		return false;
	case 3:
		kputs("  framebuffer: SYS_MAP refused\n");
		return false;
	case 4:
		kputs("  framebuffer: the mapping took a store and could not "
		      "give it back, so it is not memory\n");
		return false;
	default:
		kprintf("  framebuffer: the program exited with %ld, which is "
			"not a number it can produce\n", (long)last_exit_code);
		return false;
	}

	/* And now the part the program cannot check for itself. */
	{
		const struct framebuffer *fb = fbcon_framebuffer();

		if (!fb)
			return false;

		pixels = (const volatile u32 *)phys_to_virt(fb->base);
	}

	at_origin = pixels[0];
	at_row1   = pixels[info.pitch / 4];

	if (at_origin != 0x5245434FU) {
		kprintf("  framebuffer: the program wrote 0x5245434F at the "
			"origin and the screen holds 0x%08X -- it was given a "
			"mapping of something that is not the framebuffer\n",
			(unsigned)at_origin);
		return false;
	}

	/* The stride, which is the half that catches a plausible lie.
	 *
	 * If SYS_SCREEN had reported `width * 4` on an adapter that pads its
	 * rows, this store landed somewhere inside row 0 and row 1 still holds
	 * whatever the console last drew there. */
	if (at_row1 != 0x214E534FU) {
		kprintf("  framebuffer: the second marker is missing from the "
			"start of row 1 (%u bytes in), so the pitch the "
			"program was given is not the screen's\n",
			(unsigned)info.pitch);
		return false;
	}

	kprintf("  framebuffer: a program mapped %ux%u, pitch %u, and both "
		"markers are on the screen\n",
		info.width, info.height, info.pitch);
	return true;
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

	rootfs_clear_before_test(path);

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
