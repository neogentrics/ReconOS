/*
 * The theme, over Wayland.
 *
 * Two functions, because that is the whole of what the rest of the system needs
 * to know: bring the global up, and say when the palette changed.
 *
 * Separate from recon_theme.h because that header is included by the skin tests,
 * which link recon_theme.c on its own and have no compositor -- deliberately, so
 * a palette can be measured without one. A wayland type in that header would
 * end that.
 *
 * See protocol/recon-theme.xml for what is actually sent and why it is shaped
 * the way it is.
 */

#ifndef RECON_THEMEWL_H
#define RECON_THEMEWL_H

#include <stdbool.h>

struct wl_display;

/*
 * Offer the theme to clients. False when the global cannot be created, which is
 * not fatal: the desktop works, and clients that arrive later look like
 * themselves rather than like the desktop.
 */
bool recon_themewl_init(struct wl_display *display);

/*
 * The palette changed; tell everyone listening.
 *
 * Called from wherever a skin is applied, and safe to call before init or when
 * nobody is listening. Cheap enough not to need a guard at the call site --
 * which matters, because the call site is a restyle and there are several.
 */
void recon_themewl_changed(void);

#endif /* RECON_THEMEWL_H */
