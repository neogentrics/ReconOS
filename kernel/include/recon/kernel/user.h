/* User mode, and the boundary the whole system rests on.
 *
 * Everything before this checkpoint ran at one privilege level. A bug anywhere
 * could write anywhere; "this account may not do that" was a decision code made
 * about itself, and any program could have decided otherwise. After this, the
 * processor enforces it.
 *
 * That is the difference between the desktop's accounts being a convention and
 * being true. Today ReconOS declines to install a program for a standard
 * account because its Control Panel declines to; nothing stops a program that
 * simply does not ask.
 *
 * --- Personality, and why the table is a pointer ---
 *
 * A system call arrives as a number. What that number *means* is a property of
 * the program making it: 1 is `write` to a Linux binary and something else
 * entirely to a Windows one. So the table is per-process rather than compiled
 * in, and a process is created with a personality that selects it.
 *
 * Nothing needs that yet -- there is one table and every process uses it. It is
 * built this way now because it costs one pointer now and a rewrite later, and
 * because it is the single thing the kernel owes the whole
 * run-other-systems'-programs ambition. Linux calls this `binfmt`; Windows NT
 * called them subsystems.
 */
#ifndef RECON_KERNEL_USER_H
#define RECON_KERNEL_USER_H

#include <recon/kernel/types.h>
#include <recon/kernel/compiler.h>

/* The calls themselves. Deliberately few: each one exists because something
 * calls it, and a system call invented before its first caller gets its
 * arguments wrong in a way that is expensive to change afterwards. */
enum {
	SYS_EXIT = 0,		/* (code) -- does not return */
	SYS_WRITE,		/* (fd, buffer, length) -> bytes written */
	SYS_GETPID,		/* () -> id */
	SYS_TIME,		/* () -> nanoseconds since boot */
	SYS_YIELD,		/* () -- give up the rest of this slice */

	/* The three that exist because docs/KERNEL-WANTS.md asks for them: the
	 * desktop scrapes all of this out of the host's /proc today, parsed
	 * with sscanf, and it will not survive a machine that has no /proc. */
	SYS_RANDOM,		/* (buffer, length) -> bytes written */
	SYS_MACHINE,		/* (buffer, length) -> bytes the kernel would write */
	SYS_WALLTIME,		/* () -> nanoseconds since 1970 */

	/* (path, path_len, mode, data, len) -> bytes written.
	 *
	 * One call rather than create-then-chmod, which is the whole reason it
	 * exists: the file becomes visible already carrying its permissions. */
	SYS_CREATE,

	SYS_MAX
};

/* Negative returns are errors, which is the convention every system that has
 * to report one through a single register arrives at. */
#define SYS_OK          0
#define SYS_ENOSYS    (-1)	/* no such call in this personality */
#define SYS_EFAULT    (-2)	/* the caller passed an address it does not own */
#define SYS_EINVAL    (-3)

/* Not now, and not never.
 *
 * Its own code rather than EINVAL, because the difference decides what a
 * caller does: a bad argument means the program is wrong and should stop, and
 * this means the machine has not gathered enough entropy yet and the same call
 * may work later. A program told EINVAL by a key generator would report itself
 * broken; a program told this can wait, or explain. */
#define SYS_EAGAIN    (-4)

/* The rest, each because a caller does something different about it. Collapsing
 * them into one failure is how "this machine has no filesystem" and "you asked
 * for a name that is already taken" become the same error message. */
#define SYS_ENODEV    (-5)	/* no filesystem is mounted */
#define SYS_EEXIST    (-6)	/* the name is taken, and nothing was replaced */
#define SYS_ENOENT    (-7)	/* a directory in the path does not exist */
#define SYS_ENOSPC    (-8)
#define SYS_EIO       (-9)

/* What the machine is, for a program that cannot read the host's /proc because
 * there is no host.
 *
 * The size is passed *in* by the caller and the number of bytes the kernel
 * would have written is returned, so that a program built against an older
 * kernel and one built against a newer one both work: the kernel writes no
 * more than it was given room for, and the caller can see it was given a short
 * answer. Growing this structure is therefore allowed; reordering it is not.
 */
struct recon_machine {
	u32 size;		/* what the caller had room for */
	u32 version;		/* 1 */

	u32 processors_found;	/* what the machine has */
	u32 processors_online;	/* what this kernel is using -- not the same */

	u64 memory_bytes;
	u64 memory_free_bytes;

	u32 entropy_bits;	/* zero means keys will be refused */
	u32 page_size;

	char architecture[16];
	char cpu_vendor[16];
	char cpu_model[64];
};

typedef i64 (*syscall_fn)(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

struct personality {
	const char *name;
	syscall_fn table[SYS_MAX];
};

/* The one personality there is. ReconOS's own. */
extern const struct personality personality_recon;

/* Called by each architecture from its system-call entry path. Looks the number
 * up in the *calling thread's* personality, not in a global. */
i64 syscall_dispatch(u64 number, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);

/* Whether an address range is one the calling user program is allowed to have
 * handed us. Every system call that takes a pointer must ask, before touching
 * it -- a kernel that dereferences a user pointer without checking is a kernel
 * any user program can make read or write anything. */
bool user_range_ok(u64 addr, u64 len);

/* Starts a thread that runs the named test program. Both programs live in the
 * architecture's assembly, because there is nowhere else for a user program to
 * live until there is a filesystem. */

void user_init(void);

/* Starts a thread that runs in user mode. Returns 0 if there is no memory.
 * The entry point and stack are user addresses in the low half. */
struct thread *user_thread_create(const char *name, const void *code,
				  size_t code_len);

bool user_self_test(void);

/* Runs a ring-3 program that calls SYS_RANDOM, SYS_MACHINE and SYS_WALLTIME and
 * checks its own answers. Separate from user_self_test because that one is
 * about whether user mode works at all, and this one is about whether these
 * particular calls do. */
bool user_facts_test(void);

/* The other half of the same claim, and the more important half: a user program
 * that reaches where it should not is ended, and the kernel is not. */
bool user_boundary_test(void);

/* Called by each architecture's fault handler when it ends a user program.
 * Portable code counts them; only the architecture can recognise one. */
void user_note_fault(void);

void user_print_summary(void);

/* --- What the architecture provides -------------------------------------- */

/* Sets up whatever the processor needs before it can run anything at a lower
 * privilege: the descriptors, the system-call entry point, the stack it
 * switches to on the way back in. Called on every processor. */
void arch_user_init(void);

/* Drops to user mode at `entry` with stack `stack_top`. Does not return -- the
 * thread comes back only through a system call or a fault. */
RK_NORETURN void arch_enter_user(u64 entry, u64 stack_top);

/* Where the user half of the address space lives. Below the kernel, and above
 * the never-mapped first page, so that a null pointer in a user program faults
 * exactly as it does in the kernel. */
#define USER_BASE       0x0000000000400000ULL
#define USER_STACK_TOP  0x0000000000800000ULL
/* How far the stack may grow downwards from USER_STACK_TOP. Reserved, not
 * allocated: the pages that exist are the ones the program has actually
 * touched, which is what makes a generous number cheap. Generous is the point
 * -- a stack limit that is tight is a program that dies for a reason nobody
 * can see from the fault. */
#define USER_STACK_MAX  (64 * 1024)

#define USER_LIMIT      0x0000800000000000ULL

#endif /* RECON_KERNEL_USER_H */
