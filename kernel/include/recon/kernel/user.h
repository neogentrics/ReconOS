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

#include <recon/kernel/elf.h>
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

	/* Descriptors. Appended rather than inserted: a call number is a
	 * promise to every program already built against it, and renumbering
	 * SYS_WRITE to make room for SYS_OPEN would silently turn every
	 * existing write into something else. */
	SYS_OPEN,		/* (path, path_len, flags, mode) -> fd */
	SYS_CLOSE,		/* (fd) -> 0, or why not */
	SYS_READ,		/* (fd, buffer, length) -> bytes read */
	SYS_SEEK,		/* (fd, offset, from) -> new position */
	SYS_PIPE,		/* (int fds[2]) -> 0; fds[0] reads, fds[1] writes */

	/* Who is asking, and what it is still allowed to do.
	 *
	 * These exist because the kernel started enforcing permissions and a
	 * program had no way to find out *as whom*. A process refused a file
	 * could not tell "I am the wrong user" from "the file is not there",
	 * which is the difference between asking somebody to log in and
	 * reporting a bug. */
	SYS_GETUID,		/* () -> the user this process runs as */
	SYS_GETGID,		/* () -> its group */

	/* And the half without which capabilities are inert.
	 *
	 * `capability_drop` had exactly one caller in this kernel -- its own
	 * self-test. The entire argument for having capabilities is that a
	 * program holds a power for the window it needs it and gives it up
	 * afterwards, and no program could, because there was no way to ask.
	 *
	 * DROPCAP returns what is *still held*, which is what makes it
	 * testable: the effect of the call is visible in the call's own answer
	 * rather than only in a later one. It can never widen a set -- there is
	 * no grant, and adding one later would undo the only property this is
	 * for. */
	SYS_GETCAPS,		/* () -> the privileged things it may still do */
	SYS_DROPCAP,		/* (caps) -> what is still held afterwards */

	/* (path, path_len, buffer, length) -> bytes a whole listing needs.
	 *
	 * The names come back NUL-terminated and back to back, and the answer
	 * is the size of the *whole* listing whether or not it fitted -- so a
	 * caller compares it with what it offered, and a bigger number means
	 * nothing was written and says how much to come back with. Asking with
	 * a length of zero is how a program finds out the size.
	 *
	 * Whole or nothing, rather than as much as fits: a caller handed the
	 * first half of a directory alongside a success has no way to know. */
	SYS_LIST,

	/* (process, signal) -> whether it was recorded.
	 *
	 * Recorded, not delivered. The target acts on it when one of its
	 * threads next returns to user mode, which may be immediately or may
	 * be after it has finished what it was doing. A caller that needs to
	 * know it *arrived* is asking a different question than this answers. */
	SYS_KILL,

	/* (signal, what, handler, restorer) -> whether it was accepted.
	 *
	 * `restorer` is required for a handler and is where the handler
	 * returns to -- a few instructions that invoke SYS_SIGRETURN. Refused
	 * without one, because a handler with nowhere to return to runs once
	 * and then executes whatever follows it in memory. */
	SYS_SIGACTION,

	/* (mask) -> what the mask was before.
	 *
	 * SIGKILL is never blocked whatever is asked for, and asking is not an
	 * error: a program masking everything is doing something reasonable. */
	SYS_SIGMASK,

	/* Restores what a handler interrupted. Made by the restorer, never by
	 * a program directly -- the frame it reads carries a marker this
	 * kernel wrote, and one without it is refused. */
	SYS_SIGRETURN,

	/* (fd, length) -> the address it was mapped at, or why not.
	 *
	 * Puts the memory a file *is* into the caller's address space. Only
	 * /dev/fb0 answers today, and the whole reason this exists is that a
	 * framebuffer reached through `write` is a screen's worth of copying per
	 * frame, through a system call, to reach memory the program could have
	 * been storing to directly.
	 *
	 * **The program does not choose the address.** One that picked its own
	 * would have to know what else is mapped, and the kernel is the only
	 * thing that does. The address comes back instead.
	 *
	 * Refused rather than clamped for a length larger than the file has:
	 * a program given half a screen and told it succeeded draws off the end
	 * of what it got.
	 */
	SYS_MAP,

	/* (buffer, length) -> bytes a whole description needs.
	 *
	 * What the screen is: width, height, **pitch**, format, and how many
	 * bytes of it exist. Pitch is the one a program cannot work out and the
	 * one it cannot do without -- adapters pad a row to whatever suits them,
	 * and `width * 4` draws a picture that shears a pixel further left on
	 * every line.
	 *
	 * Same shape as SYS_MACHINE: asking with a length of zero is how a
	 * program finds out the size, and a bigger answer than was offered means
	 * nothing was written.
	 */
	SYS_SCREEN,

	/* (action) -- does not return on a machine that obeys.
	 *
	 * The desktop's Shut Down and Restart exist, are drawn, and have had
	 * nothing to call: there were twenty-six system calls and none of them
	 * was about power, while `power_off()` had been reachable from the
	 * kernel command line and nowhere else.
	 *
	 * Behind CAP_SHUTDOWN, which was already defined, already tracked, and
	 * already listed in the boot report -- the permission half of this was
	 * built before the thing it guards.
	 *
	 * **It refuses with a reason.** A program told only "no" cannot tell
	 * *this machine has no ACPI* from *you are not allowed*, and those need
	 * different words on a screen. The reasons map to distinct errors
	 * rather than collapsing into one failure.
	 *
	 * Suspend is deliberately not here. Off and restart are a request the
	 * firmware honours or does not; suspend is a contract with every driver
	 * about state, and putting it in this call would be asking for the hard
	 * one in order to get the easy one. */
	SYS_POWER,

	/* (path, path_len, mode) -> 0, or why not.
	 *
	 * The desktop has had a layout since v0.1.0 -- /System, /Programs,
	 * /Users -- and no way to build it. `reconfs_create` has taken a type
	 * since the format was written; what was missing was a way to ask for
	 * one.
	 *
	 * **A call of its own rather than a bit in SYS_CREATE's mode**, and
	 * the reason is that a mode comes straight from a program. A high bit
	 * meaning "directory" is one an uninitialised variable or a shifted
	 * constant can set, and the program would be told it succeeded. A
	 * number cannot be reached by getting a permission wrong.
	 *
	 * The parent must exist: making /a/b/c where /a/b does not is ENOENT
	 * rather than three directories nobody asked for. */
	SYS_MKDIR,

	SYS_MAX
};

/* Negative returns are errors, which is the convention every system that has
 * to report one through a single register arrives at. */
/* What SYS_POWER is being asked for. Named rather than 0 and 1, because a
 * call site reading `sys_power(1)` is one nobody can check, and the two
 * outcomes differ by whether the machine comes back. */
#define POWER_ACTION_OFF     0
#define POWER_ACTION_RESTART 1

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
#define SYS_EMFILE   (-10)	/* this process holds as many files as it may */
#define SYS_EBADF    (-11)	/* that descriptor names nothing */
#define SYS_EPERM    (-12)	/* the file is open, and not for that */
#define SYS_EPIPE    (-13)	/* the other end is gone */

/* The kernel could not get the memory to do it.
 *
 * Its own code for the same reason EAGAIN has one: a program told EINVAL
 * concludes the request was wrong and stops, and this one may well work a
 * moment later. Collapsing them turns a busy machine into a broken
 * program. */
#define SYS_ENOMEM   (-14)

/* Why a machine would not stop. Separate numbers because they send whoever
 * reads them somewhere completely different: one is a machine that cannot,
 * one is a program that may not, and one is a kernel that has not been taught
 * this architecture. A single failure would make a desktop say "could not shut
 * down" to all three. */
#define SYS_ENOPOWER (-15)	/* this machine gave no way to do it */
#define SYS_ENOSTATE (-16)	/* its description names no such state */
/* Not SYS_ENOSYS, which is taken and means something else entirely: "no
 * such call in this personality". This is a call that exists, on a
 * machine this kernel has no way to stop. The collision was caught by
 * -Werror on the first build, which is where a name that means two
 * things should be caught. */
#define SYS_ENOMECH  (-17)	/* this kernel cannot reach it here */

enum reconfs_status;

/* One filesystem status as one system-call status. In one place because which
 * distinctions a program is allowed to see is a decision, and a decision made
 * in three places is three decisions. */
i64 user_status_from_reconfs(enum reconfs_status st);	/* the other end is gone */

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

/* What the screen is, for a program that is about to draw on it.
 *
 * Here beside `recon_machine` and not in fbdev.h, for the reason that one is
 * here: it crosses into user mode, and a structure a program lays out has to
 * be described in the one place both sides read.
 *
 * **`pitch` is the field that cannot be derived and cannot be done without.**
 * Bytes per row is *not* `width * 4` on real hardware -- adapters pad a row to
 * whatever suits them -- and a program that assumes it draws a picture that
 * shears one pixel further left on every line. Making a program guess would be
 * withholding the one fact it cannot recover from the pixels.
 *
 * `bytes` is `pitch * height`: what exists, not the size of the memory window,
 * which is often larger and sometimes the whole BAR. A program told the screen
 * is bigger than it is draws off the end of it.
 *
 * Same rule as `recon_machine`: growing this is allowed, reordering it is not.
 */
struct fb_info {
	u32 width;
	u32 height;
	u32 pitch;		/* bytes per row, padded by the adapter */
	u32 format;
	u64 bytes;		/* pitch * height */
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

/* Checkpoint 21's second half: a program opens /dev/fb0, is told the screen's
 * shape, maps it, and draws -- and the kernel then looks at the framebuffer to
 * see whether the pixels are there. True on a machine with no screen, which is
 * most of the verification matrix and is not a failure. */
bool user_framebuffer_test(void);

/* Whether SYS_POWER is reachable by number and refuses an action it does not
 * know rather than defaulting to stopping the machine.
 *
 * Not whether a program without CAP_SHUTDOWN is refused, which is the more
 * important question and cannot be asked from a kernel thread: a kernel thread
 * holds every capability by construction, so it would be obeyed rather than
 * refused. See the comment on the implementation. */
bool user_power_test(void);

/* A program that signals itself, handles it, and carries on. */
bool user_signal_test(void);

/* Runs a ring-3 program that calls SYS_RANDOM, SYS_MACHINE and SYS_WALLTIME and
 * checks its own answers. Separate from user_self_test because that one is
 * about whether user mode works at all, and this one is about whether these
 * particular calls do. */
bool user_facts_test(void);

/* Loads an ELF into an address space of its own and makes a thread to run it.
 * `why` is filled in when the file is refused, so a caller can say which of the
 * loader's refusals it met rather than only that it failed. */
struct thread *user_elf_create(const char *name, const void *image, u64 len,
			       enum elf_result *why);

/* Runs the program built alongside the kernel, and requires it to report that
 * its own segments arrived correctly. */
bool user_elf_test(void);

/* Runs a program compiled from C -- not assembly -- which asks the screen's
 * size, maps /dev/fb0 and draws through `pitch`. What it proves is not that a
 * program can draw, which checkpoint 21 settled, but that a program written in
 * the language the desktop is written in can be built for this kernel and run
 * on it. Returns true on a machine with no screen, which is the honest answer
 * on a serial-only boot rather than a failure. */
bool user_c_program_test(void);

/*
 * Start the first program that is not a test, and **do not wait for it**.
 *
 * Every other user program this kernel runs is a self-test: it is created, it
 * is waited on, its exit code is read, and the boot carries on. This one is
 * the opposite in every respect -- it draws a screen and stays up, so waiting
 * for it would be waiting for the machine to be switched off.
 *
 * Returns false when there is nothing to draw on or the program would not
 * load. Neither is fatal: a serial-only boot is a configuration this kernel
 * supports, and a machine with no screen has nowhere to put a first-boot
 * screen.
 */
bool user_start_first_screen(void);

/* The largest program that can be loaded from a path. Bounded because the
 * image is read into the kernel's memory before it is parsed, and a file that
 * does not fit is refused rather than loaded from its first bytes. */
#define USER_EXEC_MAX (1024u * 1024u)

/* Loads and runs a program named by a path, through the file interface.
 *
 * Nothing here knows which filesystem the path names. `*why` says what the ELF
 * loader objected to, `*error` what the file layer did, and they are separate
 * because "there is no such file" and "that file is not a program" send whoever
 * reads them to different places. */
struct thread *user_exec_path(const char *name, const char *path,
			      enum elf_result *why, i64 *error);
bool user_exec_path_test(void);

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

/* Where a mapped device lands.
 *
 * Above the stack and a long way clear of it. It cannot go in the space
 * between USER_BASE and USER_STACK_TOP, which is four megabytes in total: a
 * 2560x1440 framebuffer is fourteen, and an 8K one is a hundred and thirty.
 *
 * A range rather than a single address because a program may map more than one
 * thing, and each address space carries a cursor through it -- so two mappings
 * in one program cannot land on each other, and the same program run twice
 * gets the same answer. */
#define USER_MAP_BASE   0x0000000010000000ULL	/* 256 MB */
#define USER_MAP_END    0x0000000080000000ULL	/* 2 GB -- room for several */

#define USER_LIMIT      0x0000800000000000ULL

#endif /* RECON_KERNEL_USER_H */
