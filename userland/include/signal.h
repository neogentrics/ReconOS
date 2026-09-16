/*
 * <signal.h>.
 *
 * What a program can do about a signal, and what it can send one with.
 *
 * --- what this is for -----------------------------------------------------
 *
 * Two files in the desktop want it and they want different halves.
 * `src/recon_procinfo.c` sends: End Task is `kill(pid, SIGTERM)` and the
 * second press is `SIGKILL`. `src/recon_error.c` catches: it installs a
 * handler for the four faults that mean the system stopped, writes a code
 * somebody can read to `/System/Logs`, and lets the default handler finish the
 * job.
 *
 * Both are real on this kernel. It has had `SYS_KILL`, `SYS_SIGACTION`,
 * `SYS_SIGMASK` and `SYS_SIGRETURN` since before the desktop could compile for
 * it at all.
 *
 * --- the numbers ----------------------------------------------------------
 *
 * POSIX's, because they are the kernel's: `kernel/include/recon/kernel/
 * signal.h` numbers them the same way and there is no reason for this side to
 * disagree with the side that delivers them.
 *
 * **`SIGBUS` is here and this kernel never raises it.** POSIX puts it at 10
 * and the kernel's list goes straight from `SIGKILL` at 9 to `SIGSEGV` at 11.
 * Declaring it is not a promise that it arrives -- it is a reservation, so
 * that a program which handles it compiles, and so that nothing else is ever
 * given the number. `recon_error.c` handles it because it also runs on Linux,
 * where a bus error is a real thing a machine does.
 *
 * --- what is deliberately absent ------------------------------------------
 *
 * `sigaction`, `sigprocmask`, `sigsuspend`, `sigwait`, real-time signals, and
 * `siginfo_t`. Nothing in the desktop asks for any of them, and a signal
 * interface is the wrong place to guess: every one of those has a contract
 * about what is delivered and in what order, and a version of it that is
 * *nearly* right is worse than none, because the failure is a program that
 * stops at a moment nobody can reproduce.
 *
 * The kernel has `SYS_SIGMASK` and this does not expose it for the same
 * reason. When something needs to block a signal it can be added with the
 * caller's requirement in front of it, which is how every other part of this
 * library was written.
 */

#ifndef RECON_SIGNAL_H
#define RECON_SIGNAL_H

#include <sys/types.h>

/*
 * The numbers, as the kernel has them.
 *
 * `SIG_MAX` there is 16, so 1 to 15 is the whole range a program can be sent.
 */
#define SIGHUP     1   /* the thing on the other end went away */
#define SIGINT     2   /* somebody asked it to stop */
#define SIGQUIT    3
#define SIGILL     4   /* it executed something that is not an instruction */
#define SIGABRT    6   /* it gave up on itself */
#define SIGBUS    10   /* reserved; this kernel does not raise it */
#define SIGFPE     8
#define SIGKILL    9   /* cannot be caught, blocked or ignored */
#define SIGSEGV   11   /* it touched memory that is not there */
#define SIGPIPE   13   /* it wrote to a pipe nobody is reading */
#define SIGALRM   14
#define SIGTERM   15   /* please stop */

/*
 * A handler.
 *
 * `sighandler_t` rather than the bare pointer type in every prototype,
 * because `void (*signal(int, void (*)(int)))(int)` is a declaration almost
 * nobody reads correctly and the mistake it invites is silent.
 */
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)   /* whatever the kernel would have done */
#define SIG_IGN ((sighandler_t)1)   /* nothing at all */
#define SIG_ERR ((sighandler_t)-1)  /* signal() failed */

/*
 * Say what to do about a signal, and get back what was being done before.
 *
 * SIG_ERR and `errno` on failure -- `EINVAL` for a signal number this system
 * does not have, and for SIGKILL, which cannot be caught whatever is asked.
 *
 * **The previous handler is what this system last installed**, not what the
 * kernel holds. The kernel's `SYS_SIGACTION` answers whether it accepted the
 * change and not what was there before, so the library remembers. That is a
 * real difference from the system this replaces and it matters in one case:
 * a handler installed by something other than this library -- which on
 * ReconOS is nothing, and on the host would be a program mixing two libraries.
 */
sighandler_t signal(int signal_number, sighandler_t handler);

/* Send a signal to this process. Zero, or -1 with `errno`. */
int raise(int signal_number);

/* Send a signal to a process. Zero, or -1 with `errno` -- `ESRCH` when there
 * is no such process, `EPERM` when it is not this program's to signal. */
int kill(pid_t pid, int signal_number);

#endif /* RECON_SIGNAL_H */
