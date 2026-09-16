/*
 * The first frame of the ReconOS desktop, as a function of facts.
 *
 * --- What this is, and what it is not ---
 *
 * It is the desktop's `screen.c`: the same split, one layer up. Everything
 * here takes a panel and a description and draws; **nothing here makes a
 * system call or knows one exists.** So the host can render exactly what a
 * machine will render, into memory, and the suite can check the pixels and
 * write out a picture somebody can look at -- instead of finding out what the
 * first desktop frame looks like by writing a stick and walking to a machine.
 *
 * It is **not** the shell. `src/recon_shell.c` and `src/recon_desktop.c` are
 * the real thing and both are still behind wlroots -- they are in the
 * twenty-seven sources `scripts/check-userland.sh` cannot build. What this
 * draws is the frame those will eventually draw *into*: a wallpaper, a task
 * bar, and one window with something in it.
 *
 * --- Why it draws with the real layer rather than its own primitives ---
 *
 * `userland/init/screen.c` writes pixels itself, deliberately: it has to work
 * before there is a volume, so it cannot read a theme or a font off one.
 *
 * This is the other case. It runs after the volume is mounted, so it uses
 * `recon_theme_color`, `recon_font_system` and `recon_fill_rect` -- the same
 * code the desktop draws with on Linux today. That is the point of it: if this
 * frame comes up on a machine, what came up is **the desktop's own drawing
 * layer**, not a picture of one.
 */

#ifndef RECON_DESKTOP_SHELL_FRAME_H
#define RECON_DESKTOP_SHELL_FRAME_H

struct recon_panel;

/*
 * What the frame has to say.
 *
 * Every field is a string, for the reason `screen.h` gives: every field is
 * going to be drawn, and formatting a number is the caller's job -- it has
 * `snprintf`, and this has no business knowing whether memory is counted in
 * bytes or gibibytes.
 *
 * A NULL field is drawn as nothing rather than as "(null)", because a machine
 * that could not answer one question should still show the answers to the
 * others.
 */
struct recon_shell_facts {
    const char *version;    /* "ReconOS 0.4.54" */
    const char *machine;    /* "x86_64, 8 processors" */
    const char *display;    /* "1920 x 1080, 7680 bytes a row" */
    const char *volume;     /* "System volume, 11 folders" */
    const char *note;       /* one line under the rest, or NULL */
};

/*
 * Draw it.
 *
 * `width` and `height` are the panel's, passed rather than asked for: a panel
 * knows its own size, and taking it here means the suite can draw the same
 * frame at a dozen sizes without making a dozen panels.
 *
 * Does not commit. The caller decides when the screen changes, which on a
 * framebuffer is the difference between one frame and a half-drawn one.
 */
void recon_shell_first_frame(struct recon_panel *panel, int width, int height,
    const struct recon_shell_facts *facts);

/*
 * How tall the task bar is at a given screen height.
 *
 * Exposed because the frame is not the only thing that needs it -- whatever
 * eventually puts a window on this has to know what part of the screen is
 * already spoken for, and two answers to that question is how a window ends up
 * with its bottom edge under the bar.
 */
int recon_shell_taskbar_height(int screen_height);

#endif /* RECON_DESKTOP_SHELL_FRAME_H */
