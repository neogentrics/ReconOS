/*
 * What the desktop asks of the machine underneath it.
 *
 * --- Why this exists ---
 *
 * Every other file in `userland/desktop/` can be run on the host.
 * `shell_frame.c` takes a panel and draws; `keyboard.c` takes events and gives
 * back keystrokes. Both are checked in a millisecond by a suite, and both were
 * written that way on purpose.
 *
 * `main.c` was the exception, and it is the **worst** file to have as the
 * exception: it is the one that decides whether a machine shows a desktop or a
 * black screen. Eight calls, each with its own exit code, and not one of them
 * had ever been executed anywhere.
 *
 * It could not be shimmed the way the C library is. `libc/` reaches the kernel
 * through `recon_sys_open` and friends, so `userland/tests/hostsys.c` replaces
 * that layer and the whole library runs on Linux. But `recon_screen` and
 * `recon_map` are **inline** in `<recon.h>` and expand to a raw `syscall`
 * instruction -- there is no function to replace, and running them on Linux
 * would issue a Linux system call with a ReconOS number.
 *
 * So the calls go behind a table of function pointers, which is the same seam
 * this project has now drawn three times: `recon_memory_source` for the
 * allocator, `recon_panel_present` for the screen, and this. The argument is
 * the same each time and has held each time -- **the ReconOS implementation is
 * one small file, and a second implementation is what proves the seam is in
 * the right place.**
 *
 * --- What is deliberately not here ---
 *
 * Drawing. The panel this hands back is the drawing layer's, and everything
 * after that goes through `recon_ui.h` as it always did. This is only the
 * boundary where the machine is asked for things: a screen, a mapping, a file,
 * some bytes, and the time it takes to wait.
 */

#ifndef RECON_DESKTOP_MACHINE_H
#define RECON_DESKTOP_MACHINE_H

#include <stddef.h>

struct recon_screen;
struct recon_machine;

/*
 * The machine, as six questions.
 *
 * Every one returns what the ReconOS call returns: a negative number is a
 * failure and its value is the reason. That is a deliberate choice over
 * inventing an error type -- the real implementation is then a pass-through
 * with nothing to get wrong, and a fake one has to answer in the same
 * vocabulary rather than a friendlier one that hides a case.
 */
struct recon_desktop_machine {
	/* How the screen is arranged. Fills `into`, answers its size. */
	long long (*screen)(struct recon_screen *into, unsigned long long size);

	/* Open something by name. Flags are ReconOS's, not POSIX's. */
	long long (*open)(const char *path, unsigned long long flags);

	/* Map an open file into memory. Answers the address, or negative. */
	long long (*map)(int fd, unsigned long long length);

	/* Read bytes. Answers how many, 0 for none, negative for a failure. */
	long long (*read)(int fd, void *into, unsigned long long length);

	/*
	 * What the machine is. May be NULL, and the desktop copes: a machine
	 * that cannot describe itself still gets a desktop, because one that
	 * refuses to appear for want of a processor count is a black screen,
	 * and a black screen is the outcome nobody can diagnose.
	 */
	long long (*facts)(struct recon_machine *into, unsigned long long size);

	/*
	 * Give up the processor, and say whether to carry on.
	 *
	 * The real machine always says yes and this loop never ends, which is
	 * right: a desktop that returned would leave whatever the kernel shows
	 * next on the screen, and a frame that drew correctly would look like
	 * one that crashed.
	 *
	 * A test says no, eventually. That is the whole of what makes this
	 * program runnable off a kernel -- and it is an honest hook rather
	 * than a test-only one, because "should this keep running" is a real
	 * question that a real system happens to always answer the same way.
	 */
	int (*carry_on)(void);
};

/*
 * The one the ReconOS kernel provides. In `machine_recon.c`, which is the only
 * file in this directory that a host cannot run.
 */
const struct recon_desktop_machine *recon_desktop_machine_reconos(void);

/*
 * Run the desktop.
 *
 * Returns an exit code: 0 if it was asked to stop, and one of the numbers in
 * `main.c` if it could not start. It does not return while `carry_on` says to
 * keep going.
 */
int recon_desktop_run(const struct recon_desktop_machine *machine);

#endif /* RECON_DESKTOP_MACHINE_H */
