/*
 * The entry point `scripts/link-desktop.sh` links against.
 *
 * --- Why this is five lines and not a program ---
 *
 * It used to be the subject: a hand-written stand-in that drew a wallpaper, a
 * window and some text. That measured something real and it measured the wrong
 * thing -- a probe somebody wrote to be linkable is a probe that gets adjusted
 * whenever it will not link, and the number it reports slowly becomes a number
 * about the probe.
 *
 * The subject is `userland/desktop/desktop.c` now: **the actual program**, the one
 * that will run from the volume. This only supplies an entry point, because
 * the C runtime that calls `main` is built on the kernel side -- `recon_init`
 * is linked by `kernel/`'s makefile, not this tree's -- and the question here
 * is about symbols rather than about startup.
 *
 * So what the script reports is what the real desktop is missing, and there is
 * nothing in between to adjust.
 */

extern int main(void);

void _start(void)
{
    main();

    /*
     * `main` does not return -- it ends in a loop, because a desktop that
     * returned would leave whatever the kernel shows next on the screen and
     * make a frame that drew correctly look like one that crashed. This is
     * here so that the *linker* is not entitled to assume otherwise.
     */
    for (;;) {
    }
}
