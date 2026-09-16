/*
 * Signals, over the four calls the kernel took numbers for.
 *
 * See `userland/include/signal.h` for what is here and what is deliberately
 * not. This file is the two halves the desktop asks for: sending one, and
 * saying what to do about one.
 *
 * --- the restorer, which is the whole of the difficulty ------------------
 *
 * `SYS_SIGACTION` refuses a handler without a **restorer** -- an address the
 * handler returns to, holding a few instructions that invoke `SYS_SIGRETURN`.
 * The kernel's own comment says why: *"a handler with nowhere to return to
 * runs once and then executes whatever follows it in memory."*
 *
 * It is not in this file. It is machine code whose entire content is a system
 * call, so it lives with the system calls in `libc/syscalls.c` -- which is
 * also the only file that can see the enum its number has to be checked
 * against, and the file `hostsys.c` replaces wholesale on a host. That is the
 * layering saying where it goes rather than a preference: `internal.h`
 * deliberately does not pull in `recon.h`, and this file could not see
 * `SYS_SIGRETURN` at all.
 */

#include "internal.h"

#include "../include/errno.h"
#include "../include/signal.h"

/*
 * The kernel's `what` values, from `kernel/include/recon/kernel/signal.h`.
 * Three small numbers with no header of their own on this side.
 */
#define ACTION_DEFAULT 0
#define ACTION_IGNORE  1
#define ACTION_HANDLER 2

/*
 * Where a handler returns to.
 *
 * Built in `libc/syscalls.c`, because its entire content is a system call and
 * that is the file the system calls live in -- and because the number in it
 * has to be held against the enum, which only that file can see. On a host,
 * `hostsys.c` provides one that is never reached.
 */
void recon_sig_restorer(void);

/*
 * What this library last installed, per signal.
 *
 * The kernel answers whether it accepted a change, not what was there before,
 * so `signal()`'s promise to return the previous handler is kept here. Zeroed
 * at start, which is SIG_DFL, which is what a process starts with.
 *
 * Indexed by signal number, so slot 0 is unused and the array is one longer
 * than the highest signal. Sixteen matches the kernel's `SIG_MAX`.
 */
#define SIGNAL_SLOTS 16
static sighandler_t g_installed[SIGNAL_SLOTS];

static int known(int signal_number)
{
    return signal_number > 0 && signal_number < SIGNAL_SLOTS;
}

/* Kernel status to errno, and -1. The same shape socket.c uses. */
static int refused(long status)
{
    errno = recon_errno_from_status((int)status);
    return -1;
}

sighandler_t signal(int signal_number, sighandler_t handler)
{
    unsigned long what;
    unsigned long address;
    unsigned long restorer;
    long status;
    sighandler_t was;

    /*
     * SIGKILL is refused here rather than passed down.
     *
     * The kernel refuses it too -- it *cannot* be caught, blocked or ignored,
     * which is the whole point of having one signal that cannot. Refusing at
     * this end means the caller is told with the same `errno` whichever side
     * says no, and that this library never asks for something it knows the
     * answer to.
     */
    if (!known(signal_number) || signal_number == SIGKILL) {
        errno = EINVAL;
        return SIG_ERR;
    }

    if (handler == SIG_DFL) {
        what = ACTION_DEFAULT;
        address = 0;
        restorer = 0;
    } else if (handler == SIG_IGN) {
        what = ACTION_IGNORE;
        address = 0;
        restorer = 0;
    } else {
        what = ACTION_HANDLER;
        address = (unsigned long)handler;
        restorer = (unsigned long)&recon_sig_restorer;
    }

    status = recon_sys_sigaction((unsigned long)signal_number, what, address,
        restorer);
    if (status < 0) {
        errno = recon_errno_from_status((int)status);
        return SIG_ERR;
    }

    /* Only now, because a refused change must not be recorded as made. */
    was = g_installed[signal_number];
    g_installed[signal_number] = handler;
    return was;
}

int kill(pid_t pid, int signal_number)
{
    long status;

    /*
     * Signal zero is POSIX's "does this process exist" and this does not have
     * it: the kernel takes a signal to deliver, and asking it to deliver
     * nothing is a question it was not built to answer. Refused rather than
     * sent as zero, which the kernel would reject anyway with a message about
     * a signal number instead of about the thing that was meant.
     */
    if (!known(signal_number)) {
        errno = EINVAL;
        return -1;
    }

    status = recon_sys_kill((long)pid, (unsigned long)signal_number);
    if (status < 0) {
        return refused(status);
    }
    return 0;
}

int raise(int signal_number)
{
    /*
     * Through `kill` with this process's own id rather than a call of its
     * own, because that is what it is -- and because the kernel has no
     * separate number for it, so inventing one here would be a second path to
     * the same place that could come to disagree with the first.
     */
    return kill((pid_t)recon_sys_getpid(), signal_number);
}
