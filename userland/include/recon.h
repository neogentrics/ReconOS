/*
 * The ReconOS system call interface, for programs written in C.
 *
 * Everything that has run in user mode on this kernel so far has been
 * assembly, and assembly is not what the desktop is: it is ninety thousand
 * lines of C. So the first thing the desktop side needs from this kernel is
 * not another system call -- it is the ability to *call* the ones that exist
 * from the language the rest of the system is written in.
 *
 * That is the whole of this file, and it is deliberately not a C library.
 * There is no malloc, no printf, no string.h. Those are a libc and a libc is a
 * later, larger decision; what is here is the boundary itself, one inline
 * function per call, so that the next thing written for this kernel can be a
 * `.c` file.
 *
 * --- Why inline assembly and not a stub library ---
 *
 * A call has to place its arguments in named registers and execute one
 * instruction. A function that did that would itself need a calling
 * convention, a stack frame and a return -- which is three things that can be
 * wrong between a program and a trap that has none of them. Inline is also
 * what makes this header the only file: there is nothing to link against, so a
 * program is `gcc -ffreestanding prog.c crt0.S` and nothing else.
 *
 * --- The register convention is Linux's, deliberately ---
 *
 * Not because anything here is Linux, but because the kernel chose it so that
 * a compatibility layer would have less to translate. It is written down here
 * as well as there because a program that gets it wrong does not fail at the
 * call -- it makes a *different* call, with whatever happened to be in the
 * register, and that is the kind of fault that looks like the kernel
 * misbehaving.
 */

#ifndef RECON_USER_H
#define RECON_USER_H

typedef unsigned char       u8;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef long long           i64;

/*
 * The numbers, in the order the kernel defines them.
 *
 * Copied rather than included, because `recon/kernel/user.h` is a kernel
 * header: it pulls in kernel types and declares kernel functions, and a user
 * program that included it would compile against half a kernel. **A number
 * here that disagrees with the kernel's is a program that calls the wrong
 * thing and is told it succeeded**, so the enumeration is written in the same
 * order with the same names, and the one test that matters checks a call whose
 * answer this program could not invent.
 */
enum {
	SYS_EXIT = 0,
	SYS_WRITE,
	SYS_GETPID,
	SYS_TIME,
	SYS_YIELD,
	SYS_RANDOM,
	SYS_MACHINE,
	SYS_WALLTIME,
	SYS_CREATE,
	SYS_OPEN,
	SYS_CLOSE,
	SYS_READ,
	SYS_SEEK,
	SYS_PIPE,
	SYS_GETUID,
	SYS_GETGID,
	SYS_GETCAPS,
	SYS_DROPCAP,
	SYS_LIST,
	SYS_KILL,
	SYS_SIGACTION,
	SYS_SIGMASK,
	SYS_SIGRETURN,
	SYS_MAP,
	SYS_SCREEN,
	SYS_POWER,
	SYS_MKDIR
};

/* Negative is why not. The names the kernel uses, so a program reporting a
 * failure and a kernel explaining one say the same word. */
#define SYS_OK          0
#define SYS_ENOSYS    (-1)
#define SYS_EFAULT    (-2)
#define SYS_EINVAL    (-3)
#define SYS_EAGAIN    (-4)
#define SYS_ENODEV    (-5)
#define SYS_EEXIST    (-6)
#define SYS_ENOENT    (-7)
#define SYS_ENOSPC    (-8)
#define SYS_EIO       (-9)
#define SYS_EMFILE   (-10)
#define SYS_EBADF    (-11)
#define SYS_EPERM    (-12)
#define SYS_EPIPE    (-13)
#define SYS_ENOMEM   (-14)
#define SYS_ENOPOWER (-15)
#define SYS_ENOSTATE (-16)
#define SYS_ENOMECH  (-17)

/*
 * What a file is opened for.
 *
 * **Zero is not a default.** A program that passes no flags has asked to open
 * a file for neither reading nor writing, and the kernel refuses it -- which is
 * correct, and is a refusal that reads as "the device would not open" if you
 * do not know the flags exist. That cost the first C program written for this
 * kernel one boot, so the constants are here rather than left to be found in
 * `vfs.h`, which is a kernel header a program cannot include.
 */
#define OPEN_READ   (1u << 0)
#define OPEN_WRITE  (1u << 1)

/* What the screen is. Laid out to match the kernel's `struct fb_info` field
 * for field -- the kernel's header says growing it is allowed and reordering
 * it is not, and this is the other side of that promise. */
struct recon_screen {
	u32 width;
	u32 height;
	u32 pitch;		/* bytes per row, padded by the adapter */
	u32 format;
	u64 bytes;		/* pitch * height */
};

/* --- The trap --- */

#if defined(__x86_64__)

/*
 * `syscall` destroys rcx and r11 -- the processor puts the return address in
 * one and the flags in the other -- so both are named as clobbered. Leaving
 * them out is the classic way to write a wrapper that works until the compiler
 * decides to keep something in rcx across the call.
 */
static inline i64 recon_call6(u64 n, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
			      u64 a5)
{
	i64 ret;
	register u64 r10 __asm__("r10") = a3;
	register u64 r8  __asm__("r8")  = a4;
	register u64 r9  __asm__("r9")  = a5;

	__asm__ volatile("syscall"
			 : "=a"(ret)
			 : "a"(n), "D"(a0), "S"(a1), "d"(a2),
			   "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return ret;
}

#elif defined(__aarch64__)

static inline i64 recon_call6(u64 n, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4,
			      u64 a5)
{
	register u64 x8 __asm__("x8") = n;
	register u64 x0 __asm__("x0") = a0;
	register u64 x1 __asm__("x1") = a1;
	register u64 x2 __asm__("x2") = a2;
	register u64 x3 __asm__("x3") = a3;
	register u64 x4 __asm__("x4") = a4;
	register u64 x5 __asm__("x5") = a5;

	__asm__ volatile("svc #0"
			 : "+r"(x0)
			 : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4),
			   "r"(x5)
			 : "memory");
	return (i64)x0;
}

#else
#error "ReconOS user programs are built for x86_64 or aarch64"
#endif

#define RECON_CALL0(n)                recon_call6((n), 0, 0, 0, 0, 0, 0)
#define RECON_CALL1(n, a)             recon_call6((n), (u64)(a), 0, 0, 0, 0, 0)
#define RECON_CALL2(n, a, b)          recon_call6((n), (u64)(a), (u64)(b), 0, 0, 0, 0)
#define RECON_CALL3(n, a, b, c)       recon_call6((n), (u64)(a), (u64)(b), (u64)(c), 0, 0, 0)
#define RECON_CALL4(n, a, b, c, d)    recon_call6((n), (u64)(a), (u64)(b), (u64)(c), (u64)(d), 0, 0)

/* --- The calls --- */

static inline void recon_exit(int code)
{
	RECON_CALL1(SYS_EXIT, code);
	/* Not reached. The loop is here because falling off the end of a
	 * function whose call does not return executes whatever follows it. */
	for (;;) {
	}
}

static inline i64 recon_write(int fd, const void *buffer, u64 length)
{
	return RECON_CALL3(SYS_WRITE, fd, buffer, length);
}

static inline i64 recon_read(int fd, void *buffer, u64 length)
{
	return RECON_CALL3(SYS_READ, fd, buffer, length);
}

static inline i64 recon_open(const char *path, u64 path_len, u64 flags,
			     u64 mode)
{
	return RECON_CALL4(SYS_OPEN, path, path_len, flags, mode);
}

static inline i64 recon_close(int fd)
{
	return RECON_CALL1(SYS_CLOSE, fd);
}

static inline i64 recon_getpid(void)
{
	return RECON_CALL0(SYS_GETPID);
}

/*
 * Nanoseconds since this machine started, and it never goes backwards.
 *
 * Means nothing outside this boot. A caller measuring how long something took
 * uses this one.
 */
static inline i64 recon_time(void)
{
	return RECON_CALL0(SYS_TIME);
}

/*
 * Nanoseconds since 1970, and it can jump.
 *
 * The date. A caller stamping a file uses this one. Collapsing the two into a
 * single call is how a duration comes out negative, which is why the kernel
 * offers two -- see the comment on `sys_walltime` in kernel/core/user.c.
 */
static inline i64 recon_walltime(void)
{
	return RECON_CALL0(SYS_WALLTIME);
}

static inline void recon_yield(void)
{
	RECON_CALL0(SYS_YIELD);
}

/*
 * The address the file was mapped at, or a negative reason.
 *
 * Returned as a signed value and converted by the caller, because the kernel
 * reports failure in the same register it reports the address in -- and an
 * address is unsigned, so a wrapper that returned one could not say no.
 */
static inline i64 recon_map(int fd, u64 length)
{
	return RECON_CALL2(SYS_MAP, fd, length);
}

/*
 * What the screen is. Asking with a length of zero returns the size a whole
 * description needs, which is how a program finds out whether the structure it
 * was compiled against is the one the kernel writes.
 */
static inline i64 recon_screen(struct recon_screen *into, u64 length)
{
	return RECON_CALL2(SYS_SCREEN, into, length);
}

/* What to ask the machine to do. The two differ by whether it comes back, so
 * they are named: a call site reading `recon_power(1)` is one nobody can
 * check. */
#define POWER_ACTION_OFF     0
#define POWER_ACTION_RESTART 1

/*
 * Stop the machine, or restart it. Does not return on a machine that obeys.
 *
 * Needs the `shutdown` capability -- `recon_getcaps()` says whether this
 * process still holds it, and `recon_dropcap()` is how a program that will
 * never need it gives it up.
 *
 * **Every failure says which failure it is**, because a person looking at a
 * screen needs different words for each:
 *
 *   SYS_EPERM     this program was not given the capability
 *   SYS_ENOPOWER  the machine's firmware named no way to do it
 *   SYS_ENOSTATE  its description declares no such state
 *   SYS_ENOMECH   this kernel cannot reach it on this architecture
 *   SYS_EINVAL    that is not an action this kernel knows
 *
 * Suspend is not one of the actions, and that is deliberate rather than
 * pending: off and restart are a request the firmware honours or does not,
 * while suspend is a contract with every driver about state.
 */
static inline i64 recon_power(u64 action)
{
	return RECON_CALL1(SYS_POWER, action);
}

/*
 * Make a directory. Returns 0, or why not.
 *
 * The parent has to exist: asking for "/System/Fonts" before "/System" is
 * SYS_ENOENT rather than two directories nobody asked for. `SYS_EEXIST` where
 * the name is taken -- including where what is there is already a directory,
 * because "it exists" and "I made it" are different answers and an installer
 * that cannot tell them apart cannot tell a fresh disk from one it has already
 * written to.
 *
 * A call of its own rather than a flag in `recon_create`'s mode: a mode is a
 * number a program computes, and a high bit meaning "directory" is one a
 * shifted constant can set by accident.
 */
static inline i64 recon_mkdir(const char *path, u64 path_len, u64 mode)
{
	return RECON_CALL3(SYS_MKDIR, path, path_len, mode);
}

/* --- The small amount of C library a freestanding program cannot do without --- */

/*
 * GCC is entitled to turn a loop that copies bytes into a call to `memcpy`
 * even with `-ffreestanding`, and the same for `memset` on a loop that fills.
 * A program with neither then fails to *link*, which is a confusing failure a
 * long way from its cause. These are here so that it does not, and they are
 * the only two: everything else a program wants belongs in a libc that has
 * been decided on rather than accumulated here one function at a time.
 */
void *memset(void *to, int value, unsigned long length);
void *memcpy(void *to, const void *from, unsigned long length);

/* The length of a NUL-terminated string, because a program that opens a file
 * has to say how long the path is and counting it by hand at every call site
 * is how one of them ends up wrong. */
static inline u64 recon_strlen(const char *text)
{
	u64 n = 0;

	while (text[n] != '\0') {
		n++;
	}
	return n;
}

/* Open by a NUL-terminated path, with the length counted here rather than at
 * the call site where it can be wrong. Below `recon_strlen` because it uses
 * it. */
static inline i64 recon_open_path(const char *path, u64 flags)
{
	return recon_open(path, recon_strlen(path), flags, 0);
}

/* Write a string to a descriptor, length worked out rather than passed. */
static inline i64 recon_say(int fd, const char *text)
{
	return recon_write(fd, text, recon_strlen(text));
}

#endif /* RECON_USER_H */
