/*
 * The ReconOS desktop: what sits on the wallpaper.
 *
 * It shows the contents of /Users/Desktop as icons. Being a view of a real
 * folder rather than a separate store means anything that writes a file there
 * -- the terminal, the file explorer, an application -- puts it on the
 * desktop, with nothing needing to be told about it.
 *
 * A shortcut is a file ending in .app whose first line names a built-in
 * application. Deliberately plain text, so it can be made with the tools
 * ReconOS already has rather than needing a dedicated one.
 */

#ifndef RECON_DESKTOP_H
#define RECON_DESKTOP_H

#include <stdbool.h>

#include "recon_fs.h"

struct recon_server;
struct recon_font;
struct recon_desktop;
struct wlr_scene_node;

#define RECON_DESKTOP_DIR "/Users/Desktop"
#define RECON_DESKTOP_HIT_BASE 100

enum recon_desktop_action_kind {
    RECON_DESKTOP_ACTION_NONE,
    RECON_DESKTOP_ACTION_OPEN_APP,  /* target names a built-in application */
    RECON_DESKTOP_ACTION_OPEN_PATH, /* target is a ReconOS path */
};

/*
 * What a click asked for. The desktop reports rather than acts, because
 * opening an application or a folder is the shell's business, not the
 * backdrop's.
 */
struct recon_desktop_action {
    enum recon_desktop_action_kind kind;
    char target[RECON_PATH_MAX];
};

struct recon_desktop *recon_desktop_create(struct recon_server *server,
    struct recon_font *font, int width, int height);
void recon_desktop_destroy(struct recon_desktop *desktop);

void recon_desktop_resize(struct recon_desktop *desktop, int width, int height);

/* Re-read the folder. Call when its contents may have changed. */
void recon_desktop_reload(struct recon_desktop *desktop);
void recon_desktop_refresh(struct recon_desktop *desktop);

/* Keep the desktop above the wallpaper and below everything else. */
void recon_desktop_lower(struct recon_desktop *desktop,
    struct wlr_scene_node *background);

struct wlr_scene_node *recon_desktop_node(struct recon_desktop *desktop);

bool recon_desktop_handle_click(struct recon_desktop *desktop, double lx, double ly,
    bool pressed, struct recon_desktop_action *action);

#endif
