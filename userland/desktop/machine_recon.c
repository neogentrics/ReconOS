/*
 * The machine, when the machine is a ReconOS kernel. See machine.h.
 *
 * **This is the only file in `userland/desktop/` that a host cannot run**, and
 * it is six pass-throughs. That is the measure of whether the seam is in the
 * right place: everything with a decision in it is on the other side of it,
 * and what is left here has nothing to get wrong.
 *
 * The calls it wraps are `static inline` in `<recon.h>` and expand to a raw
 * `syscall` instruction, which is why they could not be replaced the way
 * `libc/`'s are -- there was no function to replace. There is one now, six
 * times over.
 */

#include <recon.h>
#include <recon_machine.h>

#include "machine.h"

static long long screen_of(struct recon_screen *into, unsigned long long size)
{
	return recon_screen(into, size);
}

static long long open_of(const char *path, unsigned long long flags)
{
	return recon_open_path(path, flags);
}

static long long map_of(int fd, unsigned long long length)
{
	return recon_map(fd, length);
}

static long long read_of(int fd, void *into, unsigned long long length)
{
	return recon_read(fd, into, length);
}

static long long facts_of(struct recon_machine *into, unsigned long long size)
{
	return recon_machine_facts(into, size);
}

static int carry_on(void)
{
	/*
	 * Always. A desktop that returned would end the process and leave
	 * whatever the kernel draws next on the screen, which makes a frame
	 * that drew correctly look like one that crashed.
	 *
	 * The yield is here rather than in the caller because it is the part
	 * that is about this machine: on a kernel with one processor, a loop
	 * that never yields is a machine that never runs anything else.
	 */
	recon_yield();
	return 1;
}

static const struct recon_desktop_machine RECONOS = {
	.screen = screen_of,
	.open = open_of,
	.map = map_of,
	.read = read_of,
	.facts = facts_of,
	.carry_on = carry_on,
};

const struct recon_desktop_machine *recon_desktop_machine_reconos(void)
{
	return &RECONOS;
}

/*
 * The entry point.
 *
 * Here rather than beside the logic, because it is the line that *chooses* a
 * machine and this is the file that has one. With it in `desktop.c` the logic
 * could not be linked into anything else: a suite would bring a second `main`
 * and an undefined reference to the one file it is deliberately not linking.
 */
int main(void)
{
	return recon_desktop_run(recon_desktop_machine_reconos());
}
