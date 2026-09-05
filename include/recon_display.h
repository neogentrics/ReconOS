/*
 * The screens, and what sizes they can be.
 *
 * --- Why this file exists at all ---
 *
 * The Control Panel needs to answer "how many pixels is the display, and what
 * else could it be". Today that answer comes from wlroots, which asks DRM,
 * which asks the Linux kernel. Tomorrow it comes from ReconOS's own kernel
 * asking its own driver.
 *
 * Nothing above this file knows which of those happened. The Display Settings
 * page calls `recon_display_*` and has no idea that wlroots exists, in the
 * same way the Storage page calls `recon_volume_*` and has no idea that its
 * three "volumes" are currently three directories. That is the same trick
 * twice, deliberately: write the interface the system wants, implement it
 * against whatever is underneath today, and swapping the underneath is one
 * file rather than every page that ever asked a question.
 *
 * The alternative -- Display Settings calling `wlr_output_set_custom_mode`
 * directly -- would work today and would have to be found and unpicked later,
 * in a page that by then has other things in it.
 *
 * --- What is deliberately not here ---
 *
 * Nothing about drawing. The scene graph, the frame loop and the buffers stay
 * wlroots' job, because that is the compositor foundation and Phase 2 replaces
 * it wholesale rather than routing around it. This file is only for the
 * questions whose answers a kernel eventually owns: what displays are there,
 * what can they do, and change one.
 */

#ifndef RECON_DISPLAY_H
#define RECON_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>

struct recon_server;

#define RECON_DISPLAY_NAME_MAX 32
#define RECON_DISPLAY_MODES_MAX 32

/*
 * A size a display can be shown at.
 *
 * `refresh` is in millihertz, which is what the hardware reports and what
 * avoids 59.94 becoming 59. Divided for display, never for comparison.
 */
struct recon_display_mode {
    int width;
    int height;
    int refresh;

    /* The one it is set to now. */
    bool current;
    /* The one it prefers -- usually its native resolution. */
    bool preferred;
};

struct recon_display {
    char name[RECON_DISPLAY_NAME_MAX];

    int width;
    int height;
    int refresh;

    /*
     * False when the size cannot be changed.
     *
     * True of every nested and headless backend: ReconOS running in a window
     * on another compositor is whatever size that window is, and asking it to
     * be something else is asking the wrong thing. The page says so rather
     * than offering a control that would silently do nothing.
     */
    bool can_change;

    /* How many modes it offers. Zero is normal for a display that cannot be
     * changed, and is not an error. */
    int mode_count;
};

/* Remember which server to ask. Called once, when the server is up. */
void recon_display_init(struct recon_server *server);

int recon_display_count(void);
bool recon_display_at(int index, struct recon_display *out);

/* The modes one display offers, newest-and-largest first. */
bool recon_display_mode_at(int display, int index,
    struct recon_display_mode *out);

/*
 * Put a display into one of its modes.
 *
 * False with `why_out` saying which of the several reasons it was: no such
 * display, no such mode, a backend that cannot change, or the change being
 * refused by whatever is underneath. Those are different problems and a page
 * that said only "failed" would be hiding which.
 */
bool recon_display_set_mode(int display, int mode, char *why_out,
    size_t why_size);

const char *recon_display_last_error(void);

#endif /* RECON_DISPLAY_H */
