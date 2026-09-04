/*
 * The ReconOS desktop.
 *
 * What sits on the wallpaper: the contents of /Users/Desktop, drawn as icons.
 * It is a view of a real folder rather than a separate store, so anything that
 * puts a file there -- the terminal, the file explorer, an application --
 * puts it on the desktop, with nothing needing to be told.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

#include <wlr/types/wlr_scene.h>

#include "ReconOS.h"
#include "recon_desktop.h"
#include "recon_fs.h"
#include "recon_registry.h"
#include "recon_icons.h"
#include "recon_shell.h"
#include "recon_server.h"
#include "recon_theme.h"
#include "recon_ui.h"

#define ICON_WIDTH 88
#define ICON_HEIGHT 82
#define ICON_IMAGE 40
#define ICON_MARGIN 16
#define LABEL_GAP 4

#define ITEMS_MAX 128

#define COLOR_LABEL THEME(DESKTOP_LABEL)
#define COLOR_LABEL_SHADOW THEME(DESKTOP_LABEL_SHADOW)
#define COLOR_SELECTED THEME(DESKTOP_SELECTION)
#define COLOR_FOLDER RECON_RGB(0xE0, 0xC0, 0x60)
#define COLOR_FOLDER_TAB RECON_RGB(0xC8, 0xA8, 0x48)
#define COLOR_FILE RECON_RGB(0xF4, 0xF4, 0xF4)
#define COLOR_FILE_FOLD RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_APP THEME(ACCENT)
#define COLOR_EDGE THEME(WINDOW_EDGE)

/* A desktop item is one of these. Shortcuts are files ending in .app whose
 * contents name a built-in application. */
enum item_kind {
    ITEM_FOLDER,
    ITEM_FILE,
    ITEM_SHORTCUT,
    /*
     * The recycle bin. Not a file on the desktop at all -- it is a view of the
     * user's bin, put here because a system where deleting has nowhere visible
     * to put things is a system people do not trust. It cannot be renamed or
     * deleted, which is why it needs a kind of its own rather than being a
     * shortcut that happens to point at the bin.
     */
    ITEM_TRASH,
};

struct desktop_item {
    char name[RECON_NAME_MAX];   /* the file on disk */
    char label[RECON_NAME_MAX];  /* what is shown, without any extension */
    char target[RECON_NAME_MAX]; /* for shortcuts: the application named */
    enum item_kind kind;
    int x, y;
    /* True when somebody put it here, as against the grid having. Only these
     * are written down: saving every position would fill the layout with
     * places nobody chose. */
    bool placed;
};

struct recon_desktop {
    struct recon_server *server;
    struct recon_font *font;
    struct recon_panel *panel;

    struct desktop_item items[ITEMS_MAX];
    int item_count;
    int selected;

    /*
     * The icon being dragged, or -1.
     *
     * `moved` is what separates a drag from a click: pressing an icon and
     * letting go without going anywhere has to keep meaning "select", and
     * pressing an already-selected icon has to keep meaning "open". Without
     * it, every click would end as a drag of zero distance and nothing would
     * ever open.
     */
    int dragging;
    int drag_offset_x, drag_offset_y;
    bool moved;

    /* The item whose label is being edited, or -1. */
    int renaming;
    struct recon_edit rename_edit;

    int width, height;
};

/* The desktop folder, as a path. Written out often enough to be worth a
 * name of its own. */
static void desktop_path(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s/%s", recon_fs_user_dir("Desktop"), name);
}

/* --- Loading --- */

/* Read a shortcut's target: the first non-empty line of the file. */
static bool read_shortcut(const char *name, char *target, size_t size) {
    char path[RECON_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", recon_fs_user_dir("Desktop"), name);

    size_t length = 0;
    char *data = recon_fs_read("/", path, &length);
    if (data == NULL) {
        return false;
    }

    char *line = data;
    char *newline = strchr(data, '\n');
    if (newline != NULL) {
        *newline = '\0';
    }

    /* Trim trailing whitespace, which a text editor is likely to leave. */
    size_t end = strlen(line);
    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\r')) {
        line[--end] = '\0';
    }

    snprintf(target, size, "%s", line);
    free(data);
    return target[0] != '\0';
}

/*
 * --- Where the icons are ---
 *
 * A desktop somebody has arranged is a thing they made, and it used to be
 * thrown away on every start: the icons were laid out in a fixed grid every
 * time, and there was no way to move one anyway.
 *
 * The whole arrangement is one registry value rather than a key per icon,
 * because a key per icon would have to be named after the file, and registry
 * keys cannot contain spaces -- so "My Notes" and "My-Notes" would have
 * collided on the one thing a layout must not get wrong, which is which icon
 * goes where.
 *
 * Positions are stored as a grid column and row, not as pixels. A layout
 * saved on one screen and restored on a smaller one would otherwise put half
 * the icons off the edge; a column and row can be clamped into whatever
 * space there is and still keep the arrangement's shape.
 */
#define DESKTOP_LAYOUT_KEY "desktop/icons"

/* Where an icon sits, in columns and rows from the top left. */
static void cell_of(const struct recon_desktop *desktop, int x, int y,
        int *column, int *row) {
    int step_x = ICON_WIDTH + ICON_MARGIN;
    int step_y = ICON_HEIGHT + ICON_MARGIN;
    (void)desktop;

    *column = (x - ICON_MARGIN + step_x / 2) / step_x;
    *row = (y - ICON_MARGIN + step_y / 2) / step_y;
    if (*column < 0) { *column = 0; }
    if (*row < 0) { *row = 0; }
}

static void place_at_cell(struct recon_desktop *desktop, int index,
        int column, int row) {
    int columns = (desktop->width - ICON_MARGIN) / (ICON_WIDTH + ICON_MARGIN);
    int rows = (desktop->height - ICON_MARGIN) / (ICON_HEIGHT + ICON_MARGIN);
    if (columns < 1) { columns = 1; }
    if (rows < 1) { rows = 1; }

    /* Clamped rather than refused: a screen that shrank should bring its
     * icons in, not lose them off the edge. */
    if (column > columns - 1) { column = columns - 1; }
    if (row > rows - 1) { row = rows - 1; }
    if (column < 0) { column = 0; }
    if (row < 0) { row = 0; }

    desktop->items[index].x = ICON_MARGIN + column * (ICON_WIDTH + ICON_MARGIN);
    desktop->items[index].y = ICON_MARGIN + row * (ICON_HEIGHT + ICON_MARGIN);
}

/*
 * Write the arrangement down.
 *
 * Only the icons that have been moved. Saving every one would fill the value
 * with positions nobody chose, and then a new icon arriving would have
 * nowhere to go that was not already claimed by a position the grid had
 * picked for something else.
 */
static void save_layout(struct recon_desktop *desktop) {
    char value[RECON_REGISTRY_VALUE_MAX];
    size_t used = 0;
    value[0] = '\0';

    for (int i = 0; i < desktop->item_count; i++) {
        if (!desktop->items[i].placed) {
            continue;
        }

        int column, row;
        cell_of(desktop, desktop->items[i].x, desktop->items[i].y,
            &column, &row);

        /*
         * A name with an equals or a semicolon in it would break the format
         * it is being written into, so such a name is left out rather than
         * corrupting everything after it. Not a real limitation today --
         * ReconOS refuses those in file names -- and the check costs nothing.
         */
        if (strchr(desktop->items[i].name, '=') != NULL ||
                strchr(desktop->items[i].name, ';') != NULL) {
            continue;
        }

        int written = snprintf(value + used, sizeof(value) - used,
            "%s%s=%d,%d", used > 0 ? ";" : "",
            desktop->items[i].name, column, row);
        if (written < 0 || (size_t)written >= sizeof(value) - used) {
            /* Out of room. What fits is kept; the rest keep the grid, which
             * is better than losing the whole arrangement to one long name. */
            value[used] = '\0';
            break;
        }
        used += (size_t)written;
    }

    recon_registry_set(RECON_REG_USER, DESKTOP_LAYOUT_KEY, value);
}

/* Read the arrangement back, if there is one for this icon. */
static bool saved_cell(const char *name, int *column, int *row) {
    const char *value = recon_registry_get(RECON_REG_USER,
        DESKTOP_LAYOUT_KEY, "");
    if (value == NULL || *value == '\0' || name == NULL) {
        return false;
    }

    size_t name_len = strlen(name);
    for (const char *entry = value; entry != NULL && *entry != '\0';) {
        const char *end = strchr(entry, ';');
        size_t length = (end != NULL) ? (size_t)(end - entry) : strlen(entry);

        if (length > name_len && entry[name_len] == '=' &&
                strncmp(entry, name, name_len) == 0) {
            int c = 0, r = 0;
            if (sscanf(entry + name_len + 1, "%d,%d", &c, &r) == 2) {
                *column = c;
                *row = r;
                return true;
            }
            return false;
        }

        entry = (end != NULL) ? end + 1 : NULL;
    }
    return false;
}

static void layout_items(struct recon_desktop *desktop) {
    int columns = (desktop->width - ICON_MARGIN) / (ICON_WIDTH + ICON_MARGIN);
    if (columns < 1) {
        columns = 1;
    }
    int rows = (desktop->height - ICON_MARGIN) / (ICON_HEIGHT + ICON_MARGIN);
    if (rows < 1) {
        rows = 1;
    }

    (void)columns;

    /*
     * Anything somebody put somewhere goes back there first, so the icons
     * that have not been moved can fill in around them.
     */
    for (int i = 0; i < desktop->item_count; i++) {
        int column, row;
        desktop->items[i].placed = saved_cell(desktop->items[i].name,
            &column, &row);
        if (desktop->items[i].placed) {
            place_at_cell(desktop, i, column, row);
        }
    }

    /*
     * Fill down a column before starting the next, which is how desktops
     * arrange icons and keeps them out of the middle of the screen. Cells
     * already claimed are stepped over, so an unplaced icon never lands on
     * top of one somebody arranged.
     */
    int next = 0;
    for (int i = 0; i < desktop->item_count; i++) {
        if (desktop->items[i].placed) {
            continue;
        }

        for (;;) {
            int column = next / rows;
            int row = next % rows;
            next++;

            bool taken = false;
            for (int other = 0; other < desktop->item_count && !taken; other++) {
                if (!desktop->items[other].placed) {
                    continue;
                }
                int oc, orow;
                cell_of(desktop, desktop->items[other].x,
                    desktop->items[other].y, &oc, &orow);
                taken = (oc == column && orow == row);
            }
            if (taken && next < desktop->item_count * 2 + rows) {
                continue;
            }

            place_at_cell(desktop, i, column, row);
            break;
        }
    }
}

static int index_of(struct recon_desktop *desktop, const char *name);

bool recon_desktop_is_trash_item(const char *name) {
    return name != NULL && strcmp(name, RECON_DESKTOP_TRASH_NAME) == 0;
}

int recon_desktop_trash_count(struct recon_desktop *desktop) {
    (void)desktop;
    return recon_fs_trash_count();
}

void recon_desktop_reload(struct recon_desktop *desktop) {
    /*
     * A rename in progress is held as an index, and the indices are about to
     * change. Remember which name is being edited and find it again below.
     *
     * Simply cancelling here looked safe and was not: every context menu
     * action ends with a shell refresh, which reloads the desktop, so a
     * rename was destroyed in the same breath that started it. "Rename does
     * nothing" was this.
     */
    char renaming_name[RECON_NAME_MAX];
    bool was_renaming = false;
    if (desktop != NULL && desktop->renaming >= 0 &&
            desktop->renaming < desktop->item_count) {
        snprintf(renaming_name, sizeof(renaming_name), "%s",
            desktop->items[desktop->renaming].name);
        was_renaming = true;
    }
    if (desktop == NULL) {
        return;
    }

    struct recon_dirent entries[ITEMS_MAX];
    int count = recon_fs_list("/", recon_fs_user_dir("Desktop"), entries, ITEMS_MAX);
    if (count < 0) {
        count = 0;
    }
    if (count > ITEMS_MAX) {
        count = ITEMS_MAX;
    }

    desktop->item_count = 0;

    /*
     * The recycle bin comes first and always. It is not a file on the desktop
     * -- it is a view of the user's bin -- so it is added here rather than
     * found, and cannot be removed by deleting something.
     */
    {
        struct desktop_item *bin = &desktop->items[desktop->item_count++];
        snprintf(bin->name, sizeof(bin->name), "%s", RECON_DESKTOP_TRASH_NAME);
        snprintf(bin->label, sizeof(bin->label), "%s", RECON_DESKTOP_TRASH_NAME);
        bin->target[0] = '\0';
        bin->kind = ITEM_TRASH;
    }

    for (int i = 0; i < count; i++) {
        /* Nothing on disk may take the bin's name and shadow it. */
        if (recon_desktop_is_trash_item(entries[i].name)) {
            continue;
        }

        struct desktop_item *item = &desktop->items[desktop->item_count];

        snprintf(item->name, sizeof(item->name), "%s", entries[i].name);
        snprintf(item->label, sizeof(item->label), "%s", entries[i].name);
        item->target[0] = '\0';

        if (entries[i].kind == RECON_FILE_DIRECTORY) {
            item->kind = ITEM_FOLDER;
        } else {
            size_t length = strlen(item->name);
            bool is_shortcut = length > 4 &&
                strcasecmp(item->name + length - 4, ".app") == 0;

            if (is_shortcut && read_shortcut(item->name, item->target,
                    sizeof(item->target))) {
                item->kind = ITEM_SHORTCUT;
                /* Shown without the extension, the way a shortcut should be. */
                snprintf(item->label, sizeof(item->label), "%.*s",
                    (int)(length - 4), item->name);
            } else {
                item->kind = ITEM_FILE;
            }
        }

        desktop->item_count++;
    }

    layout_items(desktop);

    /* Put the rename back on the same file, wherever it now sits. If it has
     * gone -- deleted from underneath, or renamed already -- the edit goes
     * with it rather than pointing at whatever took its place. */
    if (was_renaming) {
        desktop->renaming = index_of(desktop, renaming_name);
        if (desktop->renaming < 0) {
            recon_edit_end(&desktop->rename_edit);
        }
    }

    recon_desktop_refresh(desktop);
}

/* --- Drawing --- */

static void draw_icon(struct recon_desktop *desktop, struct recon_panel *p,
        const struct desktop_item *item) {
    int cx = item->x + (ICON_WIDTH - ICON_IMAGE) / 2;
    int cy = item->y + 6;

    /*
     * A shortcut shows the icon of what it points at, asked for by name rather
     * than derived from the title: "File Explorer" is not "fileexplorer", and
     * guessing only works while the two happen to line up.
     */
    if (item->kind == ITEM_SHORTCUT && item->target[0] != '\0') {
        const char *icon = recon_shell_icon_for_app(desktop->server->shell,
            item->target);
        if (icon != NULL && recon_icon_draw(p, icon, cx, cy, ICON_IMAGE)) {
            return;
        }
    }

    /* A full bin looks different from an empty one, which is most of the
     * reason for having it in sight. */
    if (item->kind == ITEM_TRASH) {
        const char *icon = recon_fs_trash_count() > 0
            ? RECON_ICON_TRASH_FULL : RECON_ICON_TRASH;
        if (recon_icon_draw(p, icon, cx, cy, ICON_IMAGE)) {
            return;
        }
    }

    const char *generic =
        item->kind == ITEM_FOLDER ? RECON_ICON_FOLDER :
        item->kind == ITEM_SHORTCUT ? RECON_ICON_APP :
        item->kind == ITEM_TRASH ? RECON_ICON_TRASH : RECON_ICON_FILE;
    if (recon_icon_draw(p, generic, cx, cy, ICON_IMAGE)) {
        return;
    }

    /* No icon file: draw one, so the desktop is complete without any. */
    switch (item->kind) {
    case ITEM_FOLDER:
        /* A folder: a tab along the top of a body. */
        recon_fill_rect(p, cx, cy + 4, ICON_IMAGE, ICON_IMAGE - 10, COLOR_FOLDER);
        recon_fill_rect(p, cx, cy, ICON_IMAGE / 2, 6, COLOR_FOLDER_TAB);
        recon_stroke_rect(p, cx, cy + 4, ICON_IMAGE, ICON_IMAGE - 10, COLOR_EDGE);
        break;

    case ITEM_FILE:
        /* A page with a folded corner. */
        recon_fill_rect(p, cx + 4, cy, ICON_IMAGE - 8, ICON_IMAGE - 4, COLOR_FILE);
        recon_stroke_rect(p, cx + 4, cy, ICON_IMAGE - 8, ICON_IMAGE - 4, COLOR_EDGE);
        recon_fill_rect(p, cx + ICON_IMAGE - 14, cy, 10, 10, COLOR_FILE_FOLD);
        recon_stroke_rect(p, cx + ICON_IMAGE - 14, cy, 10, 10, COLOR_EDGE);
        /* Lines of text. */
        for (int i = 0; i < 4; i++) {
            recon_fill_rect(p, cx + 9, cy + 16 + i * 5, ICON_IMAGE - 20, 2,
                RECON_RGB(0x90, 0x90, 0x90));
        }
        break;

    case ITEM_SHORTCUT:
        /* An application: a solid tile in the accent colour. */
        recon_fill_rect(p, cx + 2, cy + 2, ICON_IMAGE - 4, ICON_IMAGE - 8, COLOR_APP);
        recon_draw_bevel(p, cx + 2, cy + 2, ICON_IMAGE - 4, ICON_IMAGE - 8, false);
        recon_stroke_rect(p, cx + 2, cy + 2, ICON_IMAGE - 4, ICON_IMAGE - 8, COLOR_EDGE);
        break;

    case ITEM_TRASH:
        /* A tapered bin, if no icon file could be drawn. */
        recon_fill_rect(p, cx + 5, cy + 6, ICON_IMAGE - 10, ICON_IMAGE - 12,
            RECON_RGB(0x9A, 0xA4, 0xB0));
        recon_stroke_rect(p, cx + 5, cy + 6, ICON_IMAGE - 10, ICON_IMAGE - 12,
            COLOR_EDGE);
        recon_fill_rect(p, cx + 3, cy + 3, ICON_IMAGE - 6, 3,
            RECON_RGB(0xB8, 0xC0, 0xCA));
        recon_stroke_rect(p, cx + 3, cy + 3, ICON_IMAGE - 6, 3, COLOR_EDGE);
        break;
    }
}

void recon_desktop_refresh(struct recon_desktop *desktop) {
    if (desktop == NULL || desktop->panel == NULL) {
        return;
    }

    struct recon_panel *p = desktop->panel;
    int ascent = recon_font_ascent(desktop->font);

    /* Fully transparent: the wallpaper shows through everywhere an icon is
     * not. */
    recon_fill(p, RECON_RGBA(0, 0, 0, 0));
    recon_hit_clear(p);

    for (int i = 0; i < desktop->item_count; i++) {
        const struct desktop_item *item = &desktop->items[i];

        if (i == desktop->selected) {
            recon_fill_rect(p, item->x, item->y, ICON_WIDTH, ICON_HEIGHT,
                COLOR_SELECTED);
        }

        draw_icon(desktop, p, item);

        /* The label, with a shadow so it stays readable over any wallpaper. */
        int label_w = recon_text_width(desktop->font, item->label);
        int lx = item->x + (ICON_WIDTH - label_w) / 2;
        if (lx < item->x + 2) {
            lx = item->x + 2;
        }
        int ly = item->y + 6 + ICON_IMAGE + LABEL_GAP + ascent;

        if (i == desktop->renaming && desktop->rename_edit.active) {
            /* Editing the name where the name already is leaves no doubt
             * about which icon is being renamed. */
            recon_edit_draw(p, desktop->font, item->x + 2,
                item->y + 6 + ICON_IMAGE + LABEL_GAP - 2,
                ICON_WIDTH - 4, ascent + 8, &desktop->rename_edit);
        } else {
            recon_draw_text(p, desktop->font, lx + 1, ly + 1, ICON_WIDTH - 4,
                item->label, COLOR_LABEL_SHADOW);
            recon_draw_text(p, desktop->font, lx, ly, ICON_WIDTH - 4,
                item->label, COLOR_LABEL);
        }

        recon_hit_add(p, item->x, item->y, ICON_WIDTH, ICON_HEIGHT,
            RECON_DESKTOP_HIT_BASE + i);
    }

    recon_panel_commit(p);
    recon_damage_all(desktop->server);
}

/* --- Lifecycle --- */

struct recon_desktop *recon_desktop_create(struct recon_server *server,
        struct recon_font *font, int width, int height) {
    struct recon_desktop *desktop = calloc(1, sizeof(*desktop));
    if (desktop == NULL) {
        return NULL;
    }

    desktop->server = server;
    desktop->font = font;
    desktop->width = width;
    desktop->height = height;
    desktop->selected = -1;
    /* Nothing is being dragged yet. Zero would mean the first icon. */
    desktop->dragging = -1;

    desktop->panel = recon_panel_create(&server->scene->tree, width, height);
    if (desktop->panel == NULL) {
        free(desktop);
        return NULL;
    }
    recon_panel_set_position(desktop->panel, 0, 0);

    recon_desktop_reload(desktop);
    return desktop;
}

void recon_desktop_destroy(struct recon_desktop *desktop) {
    if (desktop == NULL) {
        return;
    }
    recon_panel_destroy(desktop->panel);
    free(desktop);
}

void recon_desktop_resize(struct recon_desktop *desktop, int width, int height) {
    if (desktop == NULL) {
        return;
    }
    desktop->width = width;
    desktop->height = height;
    recon_panel_resize(desktop->panel, width, height);
    recon_panel_set_position(desktop->panel, 0, 0);
    layout_items(desktop);
    recon_desktop_refresh(desktop);
}

/*
 * Keep the desktop directly above the wallpaper and below everything else.
 * It is the backdrop: nothing should ever be behind it, and nothing except
 * the wallpaper should be under it.
 */
void recon_desktop_lower(struct recon_desktop *desktop,
        struct wlr_scene_node *background) {
    if (desktop == NULL || desktop->panel == NULL) {
        return;
    }
    struct wlr_scene_node *node = recon_panel_node(desktop->panel);
    if (node != NULL) {
        wlr_scene_node_lower_to_bottom(node);
    }
    if (background != NULL) {
        wlr_scene_node_lower_to_bottom(background);
    }
}

struct wlr_scene_node *recon_desktop_node(struct recon_desktop *desktop) {
    if (desktop == NULL || desktop->panel == NULL) {
        return NULL;
    }
    return recon_panel_node(desktop->panel);
}

/* --- Operations --- */

/* The item at a point, by name, or NULL for empty desktop. */
const char *recon_desktop_item_at(struct recon_desktop *desktop, double lx, double ly) {
    if (desktop == NULL) {
        return NULL;
    }

    int px = (int)lx;
    int py = (int)ly;
    uint32_t hit = recon_hit_test(desktop->panel, px, py);
    if (hit < RECON_DESKTOP_HIT_BASE) {
        return NULL;
    }

    int index = (int)(hit - RECON_DESKTOP_HIT_BASE);
    if (index < 0 || index >= desktop->item_count) {
        return NULL;
    }

    /* Select it too: a right click should act on what it points at, and show
     * which that is. */
    desktop->selected = index;
    recon_desktop_refresh(desktop);
    return desktop->items[index].name;
}

/* What opening a named item means, for the shell to carry out. */
bool recon_desktop_action_for(struct recon_desktop *desktop, const char *name,
        struct recon_desktop_action *action) {
    /* The bin opens where the bin is, rather than being a file to open. */
    if (recon_desktop_is_trash_item(name)) {
        action->kind = RECON_DESKTOP_ACTION_OPEN_PATH;
        snprintf(action->target, sizeof(action->target), "%s",
            recon_fs_trash_dir());
        return true;
    }

    if (desktop == NULL || name == NULL || action == NULL) {
        return false;
    }

    for (int i = 0; i < desktop->item_count; i++) {
        if (strcmp(desktop->items[i].name, name) != 0) {
            continue;
        }

        const struct desktop_item *item = &desktop->items[i];
        if (item->kind == ITEM_SHORTCUT) {
            action->kind = RECON_DESKTOP_ACTION_OPEN_APP;
            snprintf(action->target, sizeof(action->target), "%s", item->target);
        } else if (item->kind == ITEM_FOLDER) {
            action->kind = RECON_DESKTOP_ACTION_OPEN_PATH;
            snprintf(action->target, sizeof(action->target), "%s/%s",
                recon_fs_user_dir("Desktop"), item->name);
        } else {
            action->kind = RECON_DESKTOP_ACTION_OPEN_FILE;
            snprintf(action->target, sizeof(action->target), "%s/%s",
                recon_fs_user_dir("Desktop"), item->name);
        }
        return true;
    }
    return false;
}

void recon_desktop_delete(struct recon_desktop *desktop, const char *name) {
    if (desktop == NULL || name == NULL || recon_desktop_is_trash_item(name)) {
        return;
    }

    char path[RECON_PATH_MAX];
    desktop_path(path, sizeof(path), name);

    /* Into the bin, not gone. Deleting something from a desktop is the most
     * casual delete there is -- a stray keypress on a selected icon -- and it
     * is the one that most needs to be recoverable. */
    recon_fs_trash("/", path);

    desktop->selected = -1;
    recon_desktop_reload(desktop);
}

void recon_desktop_purge(struct recon_desktop *desktop, const char *name) {
    if (desktop == NULL || name == NULL || recon_desktop_is_trash_item(name)) {
        return;
    }

    char path[RECON_PATH_MAX];
    desktop_path(path, sizeof(path), name);

    /* A folder with things in it needs the recursive form. This is only
     * reached after the question has been answered. */
    if (!recon_fs_remove("/", path)) {
        recon_fs_remove_tree("/", path);
    }

    desktop->selected = -1;
    recon_desktop_reload(desktop);
}

/* Select a named item after the desktop has been re-read. */
static int index_of(struct recon_desktop *desktop, const char *name) {
    for (int i = 0; i < desktop->item_count; i++) {
        if (strcmp(desktop->items[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

void recon_desktop_new_folder(struct recon_desktop *desktop) {
    if (desktop == NULL) {
        return;
    }

    char name[RECON_NAME_MAX];
    if (!recon_fs_unique_name("/", recon_fs_user_dir("Desktop"), "New Folder", "",
            name, sizeof(name))) {
        return;
    }

    char path[RECON_PATH_MAX];
    desktop_path(path, sizeof(path), name);
    if (!recon_fs_mkdir("/", path)) {
        return;
    }

    recon_desktop_reload(desktop);
    /* Open the name for editing straight away: a folder the system named is
     * not a folder you named. */
    recon_desktop_begin_rename(desktop, name);
}

void recon_desktop_new_file(struct recon_desktop *desktop) {
    if (desktop == NULL) {
        return;
    }

    char name[RECON_NAME_MAX];
    if (!recon_fs_unique_name("/", recon_fs_user_dir("Desktop"), "New File", ".txt",
            name, sizeof(name))) {
        return;
    }

    char path[RECON_PATH_MAX];
    desktop_path(path, sizeof(path), name);
    if (!recon_fs_write("/", path, "", 0)) {
        return;
    }

    recon_desktop_reload(desktop);
    recon_desktop_begin_rename(desktop, name);
}

void recon_desktop_clip(struct recon_desktop *desktop, const char *name, bool cut) {
    if (desktop == NULL || name == NULL) {
        return;
    }
    char path[RECON_PATH_MAX];
    desktop_path(path, sizeof(path), name);
    recon_fs_clip_set(path, cut);
}

void recon_desktop_paste(struct recon_desktop *desktop) {
    if (desktop == NULL) {
        return;
    }

    char source[RECON_PATH_MAX];
    bool cut = false;
    if (!recon_fs_clip_get(source, sizeof(source), &cut)) {
        return;
    }
    if (!recon_fs_exists("/", source)) {
        recon_fs_clip_clear();
        return;
    }

    const char *leaf = strrchr(source, '/');
    leaf = (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : source;

    /* Split at the extension, so a second copy of "notes.txt" becomes
     * "notes 2.txt" rather than "notes.txt 2". */
    char base[RECON_NAME_MAX];
    char extension[RECON_NAME_MAX] = "";
    recon_text_copy(base, sizeof(base), leaf);
    char *dot = strrchr(base, '.');
    if (dot != NULL && dot != base) {
        snprintf(extension, sizeof(extension), "%s", dot);
        *dot = '\0';
    }

    char name[RECON_NAME_MAX];
    if (!recon_fs_unique_name("/", recon_fs_user_dir("Desktop"), base, extension,
            name, sizeof(name))) {
        return;
    }

    char target[RECON_PATH_MAX];
    desktop_path(target, sizeof(target), name);

    bool ok = cut ? recon_fs_rename("/", source, target)
                  : recon_fs_copy("/", source, target);
    if (!ok) {
        return;
    }
    if (cut) {
        recon_fs_clip_clear(); /* A cut is spent; a copy can be pasted again. */
    }

    recon_desktop_reload(desktop);
    desktop->selected = index_of(desktop, name);
    recon_desktop_refresh(desktop);
}

/* --- Renaming --- */

static void end_rename(struct recon_desktop *desktop) {
    desktop->renaming = -1;
    recon_edit_end(&desktop->rename_edit);
}

void recon_desktop_begin_rename(struct recon_desktop *desktop, const char *name) {
    if (desktop == NULL || name == NULL) {
        return;
    }

    int index = index_of(desktop, name);
    if (index < 0) {
        return;
    }

    desktop->selected = index;
    desktop->renaming = index;
    /*
     * The whole filename is edited, extension included -- ".app" is what makes
     * a shortcut a shortcut, so hiding it would let a rename quietly turn one
     * into an ordinary file. The caret sits before the extension, so typing
     * replaces the name and leaves the ending alone.
     */
    recon_edit_begin(&desktop->rename_edit, desktop->items[index].name,
        desktop->items[index].kind != ITEM_FOLDER);
    recon_desktop_refresh(desktop);
}

bool recon_desktop_is_renaming(struct recon_desktop *desktop) {
    return desktop != NULL && desktop->renaming >= 0 && desktop->rename_edit.active;
}

static void commit_rename(struct recon_desktop *desktop) {
    if (desktop->renaming < 0 || desktop->renaming >= desktop->item_count) {
        end_rename(desktop);
        return;
    }

    char from[RECON_NAME_MAX];
    char to[RECON_NAME_MAX];
    snprintf(from, sizeof(from), "%s", desktop->items[desktop->renaming].name);
    snprintf(to, sizeof(to), "%s", desktop->rename_edit.text);
    end_rename(desktop);

    /* Trim the spaces a name picks up from typing: a file called "notes " is
     * almost never what was meant and is invisible on the desktop. */
    char *end = to + strlen(to);
    while (end > to && end[-1] == ' ') {
        *--end = '\0';
    }
    const char *start = to;
    while (*start == ' ') {
        start++;
    }

    if (*start == '\0' || strchr(start, '/') != NULL ||
            strcmp(start, from) == 0) {
        recon_desktop_refresh(desktop);
        return;
    }

    char from_path[RECON_PATH_MAX];
    char to_path[RECON_PATH_MAX];
    desktop_path(from_path, sizeof(from_path), from);
    desktop_path(to_path, sizeof(to_path), start);

    char renamed[RECON_NAME_MAX];
    snprintf(renamed, sizeof(renamed), "%s", start);

    if (recon_fs_rename("/", from_path, to_path)) {
        recon_desktop_reload(desktop);
        desktop->selected = index_of(desktop, renamed);
    }
    recon_desktop_refresh(desktop);
}

/*
 * Move the icon under the pointer, if one is being dragged.
 *
 * Free while the pointer is down and snapped to a cell when it is let go, so
 * the icon follows the hand rather than jumping between cells under it --
 * and so what ends up saved is a grid position, which survives a change of
 * screen size.
 */
void recon_desktop_handle_motion(struct recon_desktop *desktop,
        double lx, double ly) {
    if (desktop == NULL || desktop->dragging < 0 ||
            desktop->dragging >= desktop->item_count) {
        return;
    }

    int x = (int)lx - desktop->drag_offset_x;
    int y = (int)ly - desktop->drag_offset_y;

    if (!desktop->moved &&
            x == desktop->items[desktop->dragging].x &&
            y == desktop->items[desktop->dragging].y) {
        return;    /* Still exactly where it was; not a drag yet. */
    }

    /* Kept on the desktop. An icon dragged off the edge would be somewhere
     * nobody could reach it to drag it back. */
    if (x < 0) { x = 0; }
    if (y < 0) { y = 0; }
    if (x > desktop->width - ICON_WIDTH) { x = desktop->width - ICON_WIDTH; }
    if (y > desktop->height - ICON_HEIGHT) { y = desktop->height - ICON_HEIGHT; }

    desktop->items[desktop->dragging].x = x;
    desktop->items[desktop->dragging].y = y;
    desktop->moved = true;
    recon_desktop_refresh(desktop);
}

bool recon_desktop_handle_key(struct recon_desktop *desktop, xkb_keysym_t sym,
        uint32_t modifiers) {
    if (!recon_desktop_is_renaming(desktop)) {
        return false;
    }

    switch (recon_edit_key(&desktop->rename_edit, sym, modifiers)) {
    case RECON_EDIT_COMMIT:
        commit_rename(desktop);
        return true;
    case RECON_EDIT_CANCEL:
        end_rename(desktop);
        recon_desktop_refresh(desktop);
        return true;
    case RECON_EDIT_CHANGED:
        recon_desktop_refresh(desktop);
        return true;
    case RECON_EDIT_IGNORED:
        /* Swallowed while the box is open: nothing else should act on it. */
        return true;
    }
    return true;
}

/*
 * A shortcut is a text file naming an application, so creating one needs
 * nothing but a write. It points at the file explorer as a starting point;
 * editing the file changes where it points.
 */
void recon_desktop_new_shortcut(struct recon_desktop *desktop) {
    if (desktop == NULL) {
        return;
    }

    char name[RECON_NAME_MAX];
    if (!recon_fs_unique_name("/", recon_fs_user_dir("Desktop"), "New Shortcut",
            ".app", name, sizeof(name))) {
        return;
    }

    char path[RECON_PATH_MAX];
    desktop_path(path, sizeof(path), name);

    const char *body = "File Explorer\n";
    if (!recon_fs_write("/", path, body, strlen(body))) {
        return;
    }

    recon_desktop_reload(desktop);
    recon_desktop_begin_rename(desktop, name);
}

/* --- Input --- */

bool recon_desktop_handle_click(struct recon_desktop *desktop, double lx, double ly,
        bool pressed, struct recon_desktop_action *action) {
    if (desktop == NULL) {
        return false;
    }

    if (!pressed) {
        /*
         * Letting go. An icon that went somewhere lands in the nearest cell
         * and the arrangement is written down; one that did not is left
         * alone, because that press was a click.
         */
        bool was_dragging = (desktop->dragging >= 0 && desktop->moved);
        int index = desktop->dragging;
        desktop->dragging = -1;
        desktop->moved = false;

        if (!was_dragging) {
            return false;
        }

        int column, row;
        cell_of(desktop, desktop->items[index].x, desktop->items[index].y,
            &column, &row);
        place_at_cell(desktop, index, column, row);
        desktop->items[index].placed = true;

        /* Written when it stops, not while it moves: saving on every pixel
         * would write the file hundreds of times to record one decision. */
        save_layout(desktop);
        recon_desktop_refresh(desktop);
        return true;
    }

    int px = (int)lx;
    int py = (int)ly;
    if (px < 0 || py < 0 || px >= desktop->width || py >= desktop->height) {
        return false;
    }

    action->kind = RECON_DESKTOP_ACTION_NONE;

    uint32_t hit = recon_hit_test(desktop->panel, px, py);
    if (hit < RECON_DESKTOP_HIT_BASE) {
        /* Empty desktop: clear the selection. */
        if (desktop->selected != -1) {
            desktop->selected = -1;
            recon_desktop_refresh(desktop);
        }
        return true;
    }

    int index = (int)(hit - RECON_DESKTOP_HIT_BASE);
    if (index < 0 || index >= desktop->item_count) {
        return true;
    }

    const struct desktop_item *item = &desktop->items[index];

    /* Ready to move it, in case the pointer goes anywhere before it is let
     * go. Nothing has happened yet if it does not. */
    desktop->dragging = index;
    desktop->drag_offset_x = px - item->x;
    desktop->drag_offset_y = py - item->y;
    desktop->moved = false;

    if (desktop->selected == index) {
        /*
         * Clicking an already-selected item opens it, which is a double click
         * without needing to measure the gap between two.
         *
         * Asked of recon_desktop_action_for rather than worked out here. This
         * switch used to repeat that function's logic and had no case for the
         * Recycle Bin, so double-clicking the bin produced no action at all
         * while Open on its right-click menu -- which went through the other
         * copy -- worked. One place decides what opening a thing means.
         */
        recon_desktop_action_for(desktop, item->name, action);
        return true;
    }

    desktop->selected = index;
    recon_desktop_refresh(desktop);
    return true;
}
