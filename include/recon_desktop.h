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

#include <xkbcommon/xkbcommon.h>

#include "recon_fs.h"

struct recon_server;
struct recon_font;
struct recon_desktop;
struct wlr_scene_node;

/* The desktop folder belongs to whoever is logged in; ask the filesystem. */
#define RECON_DESKTOP_HIT_BASE 100

enum recon_desktop_action_kind {
    RECON_DESKTOP_ACTION_NONE,
    RECON_DESKTOP_ACTION_OPEN_APP,  /* target names a built-in application */
    RECON_DESKTOP_ACTION_OPEN_PATH, /* target is a ReconOS path */
};

/* The name the recycle bin appears under. Fixed, because it is the same thing
 * on every desktop and nothing should be able to shadow it. */
#define RECON_DESKTOP_TRASH_NAME "Recycle Bin"

/* True if the named desktop item is the recycle bin, which cannot be renamed,
 * cut, copied or deleted. */
bool recon_desktop_is_trash_item(const char *name);

/* How many items the bin holds, for the label and the icon. */
int recon_desktop_trash_count(struct recon_desktop *desktop);

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

/* --- Operations, for the context menu --- */

/* The item at a point, by name, or NULL over empty desktop. Selects it. */
const char *recon_desktop_item_at(struct recon_desktop *desktop, double lx, double ly);

/* What opening a named item should do. */
bool recon_desktop_action_for(struct recon_desktop *desktop, const char *name,
    struct recon_desktop_action *action);

/* Move a desktop item to the recycle bin. */
void recon_desktop_delete(struct recon_desktop *desktop, const char *name);

/* Destroy it outright. Only after the user has been asked. */
void recon_desktop_purge(struct recon_desktop *desktop, const char *name);
void recon_desktop_new_folder(struct recon_desktop *desktop);
void recon_desktop_new_file(struct recon_desktop *desktop);
void recon_desktop_new_shortcut(struct recon_desktop *desktop);

/* Hold an item for a move or a copy, and drop whatever is held onto the
 * desktop. The clipboard is the system's, so this pastes what was copied in
 * the file explorer just as readily. */
void recon_desktop_clip(struct recon_desktop *desktop, const char *name, bool cut);
void recon_desktop_paste(struct recon_desktop *desktop);

/*
 * Rename in place: the label under the icon becomes a text box.
 *
 * While one is open the desktop takes the keyboard, which is why the shell
 * has to ask -- otherwise Escape and Enter would go somewhere else and the
 * box would never close.
 */
void recon_desktop_begin_rename(struct recon_desktop *desktop, const char *name);
bool recon_desktop_is_renaming(struct recon_desktop *desktop);
bool recon_desktop_handle_key(struct recon_desktop *desktop, xkb_keysym_t sym,
    uint32_t modifiers);

#endif
