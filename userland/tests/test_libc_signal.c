/*
 * ReconOS's signals, against the host's.
 *
 * `hostsys.c` answers the three primitives with POSIX `kill` and `sigaction`,
 * so a handler installed through ReconOS's `signal()` is installed with the
 * real delivery machinery underneath it: raising the signal has to actually
 * run the handler, in a real process, at a real moment.
 *
 * --- what this cannot reach, said here rather than discovered -------------
 *
 * **The restorer.** `libc/signal.c` builds a couple of instructions for a
 * handler to return through, because `SYS_SIGACTION` refuses a handler without
 * one -- and on a host, glibc has its own and `hostsys.c` drops ours. So every
 * check below passes whether that assembly is right or wrong.
 *
 * What proves it is a program in ring 3 on the machine, the same way the
 * socket claim was settled: install a handler, raise the signal, and see
 * whether the program is still running afterwards. A handler with a broken
 * restorer does not come back.
 *
 * This is written down because a green suite over signals reads like coverage
 * of signals, and this one is coverage of everything except the part that is
 * hardest to get right.
 *
 * Run with: cmake --build build && ./build/recon_libc_signal_tests
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Declared rather than included, because the test file is deliberately not
 * renamed -- `prefix.h` is applied to the library's sources and not to this
 * one, so that the two can be compared. Every ReconOS name here is spelled
 * with its prefix on purpose.
 */
typedef void (*recon_handler)(int);

recon_handler recon_libc_signal(int signal_number, recon_handler handler);
int recon_libc_raise(int signal_number);
int recon_libc_kill(pid_t pid, int signal_number);

extern int recon_errno;

#define RECON_SIG_DFL ((recon_handler)0)
#define RECON_SIG_IGN ((recon_handler)1)
#define RECON_SIG_ERR ((recon_handler)-1)

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* --- what a handler records --- */

static volatile sig_atomic_t g_ran;
static volatile sig_atomic_t g_saw;

static void note(int signal_number) {
    g_ran++;
    g_saw = signal_number;
}

static void note_other(int signal_number) {
    (void)signal_number;
    g_ran += 100;
}

/* --- the tests --- */

static void test_a_handler_runs(void) {
    printf("a handler installed through this library actually runs\n");

    g_ran = 0;
    g_saw = 0;

    recon_handler was = recon_libc_signal(SIGUSR1, note);
    check(was != RECON_SIG_ERR, "SIGUSR1 accepts a handler");

    check(recon_libc_raise(SIGUSR1) == 0, "raise says it sent it");
    check(g_ran == 1, "and the handler ran exactly once");
    check(g_saw == SIGUSR1, "and was told which signal it was");

    /* Again, because a handler that runs once and then does not is the shape
     * a missing re-arm has -- and this library installs a permanent action
     * rather than the one-shot the oldest signal() had. */
    check(recon_libc_raise(SIGUSR1) == 0, "it can be raised again");
    check(g_ran == 2, "and the handler ran a second time");

    recon_libc_signal(SIGUSR1, RECON_SIG_DFL);
}

static void test_the_previous_handler_comes_back(void) {
    printf("what was there before\n");

    /*
     * The kernel answers whether it accepted a change, not what was there
     * before, so this library remembers -- which is a real difference from
     * the system it replaces and is written in the header. These checks are
     * what hold the remembering to being right.
     */
    recon_libc_signal(SIGUSR2, RECON_SIG_DFL);

    recon_handler first = recon_libc_signal(SIGUSR2, note);
    check(first == RECON_SIG_DFL, "a signal starts at the default");

    recon_handler second = recon_libc_signal(SIGUSR2, note_other);
    check(second == note, "and hands back the handler it replaces");

    recon_handler third = recon_libc_signal(SIGUSR2, RECON_SIG_IGN);
    check(third == note_other, "including the second one");

    recon_handler fourth = recon_libc_signal(SIGUSR2, RECON_SIG_DFL);
    check(fourth == RECON_SIG_IGN, "and SIG_IGN is a handler like any other");
}

static void test_ignoring(void) {
    printf("a signal that is ignored does nothing at all\n");

    g_ran = 0;
    recon_libc_signal(SIGUSR1, RECON_SIG_IGN);

    check(recon_libc_raise(SIGUSR1) == 0, "it is still sent");
    check(g_ran == 0, "and nothing ran");

    recon_libc_signal(SIGUSR1, RECON_SIG_DFL);
}

static void test_kill_and_raise_agree(void) {
    printf("raise is kill, to this process\n");

    g_ran = 0;
    g_saw = 0;
    recon_libc_signal(SIGUSR1, note);

    check(recon_libc_kill(getpid(), SIGUSR1) == 0, "kill to self is accepted");
    check(g_ran == 1, "and the handler ran");

    /*
     * The same thing by the other name. `raise` is written as `kill` with this
     * process's own id rather than as a call of its own, because the kernel
     * has no separate number for it -- so a second path here could come to
     * disagree with the first, and this is the check that says it has not.
     */
    check(recon_libc_raise(SIGUSR1) == 0, "and so is raise");
    check(g_ran == 2, "with the same result");

    recon_libc_signal(SIGUSR1, RECON_SIG_DFL);
}

static void test_what_it_refuses(void) {
    printf("what it will not accept\n");

    /*
     * **SIGKILL cannot be caught**, which is the whole point of having one
     * signal that cannot. Refused here rather than passed down, so the caller
     * is told with the same errno whichever side says no.
     */
    recon_errno = 0;
    check(recon_libc_signal(SIGKILL, note) == RECON_SIG_ERR,
        "SIGKILL cannot be caught");
    check(recon_errno == EINVAL, "and says EINVAL");

    recon_errno = 0;
    check(recon_libc_signal(0, note) == RECON_SIG_ERR,
        "nor is zero a signal");
    check(recon_errno == EINVAL, "and says EINVAL");

    recon_errno = 0;
    check(recon_libc_signal(64, note) == RECON_SIG_ERR,
        "nor is a number past the end of the list");
    check(recon_errno == EINVAL, "and says EINVAL");

    /*
     * Signal zero is POSIX's "does this process exist" and this does not have
     * it: the kernel takes a signal to deliver, and asking it to deliver
     * nothing is a question it was not built to answer.
     */
    recon_errno = 0;
    check(recon_libc_kill(getpid(), 0) == -1,
        "kill with signal zero is refused rather than used as a probe");
    check(recon_errno == EINVAL, "and says EINVAL");

    recon_errno = 0;
    check(recon_libc_raise(99) == -1, "raise refuses a signal that is not one");
    check(recon_errno == EINVAL, "and says EINVAL");
}

static void test_against_the_host(void) {
    printf("and the host agrees about all of it\n");

    /*
     * The differential half. The host's `signal` and this one are given the
     * same sequence and their answers compared -- which is worth more than
     * either set of checks above, because it holds the *shape* of the answers
     * rather than what I expected them to be.
     *
     * SIGKILL is the sharp one: both must refuse it, and glibc is where the
     * expectation that they do comes from.
     */
    check(signal(SIGKILL, note) == SIG_ERR, "the host refuses SIGKILL too");
    check(errno == EINVAL, "with the same errno");

    /* A signal both have, installed and taken away on both sides. */
    void (*host_first)(int) = signal(SIGUSR2, note);
    void (*host_second)(int) = signal(SIGUSR2, SIG_DFL);
    check(host_second == note,
        "the host hands back the handler it replaced, as this does");
    (void)host_first;

    recon_libc_signal(SIGUSR2, RECON_SIG_DFL);
    recon_handler ours_first = recon_libc_signal(SIGUSR2, note);
    recon_handler ours_second = recon_libc_signal(SIGUSR2, RECON_SIG_DFL);
    check(ours_second == note, "and so does this");
    (void)ours_first;
}

int main(void) {
    printf("ReconOS signal tests\n\n");

    test_a_handler_runs();
    test_the_previous_handler_comes_back();
    test_ignoring();
    test_kill_and_raise_agree();
    test_what_it_refuses();
    test_against_the_host();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
