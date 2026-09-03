/*
 * The ReconOS desktop.
 *
 * What sits on the wallpaper: the contents of /Users/Desktop, drawn as icons.
 * It is a view of a real folder rather than a separate store, so anything that
 * puts a file there -- the terminal, the file explorer, an application --
 * puts it on the desktop, with nothing needing to be told.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_scene.h>

#include "recon_desktop.h"
#include "recon_fs.h"
#include "recon_server.h"
#include "recon_ui.h"

#define ICON_WIDTH 88
#define ICON_HEIGHT 82
#define ICON_IMAGE 40
#define ICON_MARGIN 16
#define LABEL_GAP 4

#define ITEMS_MAX 128

#define COLOR_LABEL RECON_RGB(0xF0, 0xF0, 0xF0)
#define COLOR_LABEL_SHADOW RECON_RGBA(0x00, 0x00, 0x00, 0xC0)
#define COLOR_SELECTED RECON_RGBA(0x30, 0x50, 0x90, 0xA0)
#define COLOR_FOLDER RECON_RGB(0xE0, 0xC0, 0x60)
#define COLOR_FOLDER_TAB RECON_RGB(0xC8, 0xA8, 0x48)
#define COLOR_FILE RECON_RGB(0xF4, 0xF4, 0xF4)
#define COLOR_FILE_FOLD RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_APP RECON_RGB(0x8B, 0x1A, 0x1A)
#define COLOR_EDGE RECON_RGB(0x30, 0x30, 0x30)

/* A desktop item is one of these. Shortcuts are files ending in .app whose
 * contents name a built-in application. */
enum item_kind {
    ITEM_FOLDER,
    ITEM_FILE,
    ITEM_SHORTCUT,
};

struct desktop_item {
    char name[RECON_NAME_MAX];   /* the file on disk */
    char label[RECON_NAME_MAX];  /* what is shown, without any extension */
    char target[RECON_NAME_MAX]; /* for shortcuts: the application named */
    enum item_kind kind;
    int x, y;
};

struct recon_desktop {
    struct recon_server *server;
    struct recon_font *font;
    struct recon_panel *panel;

    struct desktop_item items[ITEMS_MAX];
    int item_count;
    int selected;

    int width, height;
};

/* --- Loading --- */

/* Read a shortcut's target: the first non-empty line of the file. */
static bool read_shortcut(const char *name, char *target, size_t size) {
    char path[RECON_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", RECON_DESKTOP_DIR, name);

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

static void layout_items(struct recon_desktop *desktop) {
    int columns = (desktop->width - ICON_MARGIN) / (ICON_WIDTH + ICON_MARGIN);
    if (columns < 1) {
        columns = 1;
    }
    int rows = (desktop->height - ICON_MARGIN) / (ICON_HEIGHT + ICON_MARGIN);
    if (rows < 1) {
        rows = 1;
    }

    /* Fill down a column before starting the next, which is how desktops
     * arrange icons and keeps them out of the middle of the screen. */
    for (int i = 0; i < desktop->item_count; i++) {
        int column = i / rows;
        int row = i % rows;
        desktop->items[i].x = ICON_MARGIN + column * (ICON_WIDTH + ICON_MARGIN);
        desktop->items[i].y = ICON_MARGIN + row * (ICON_HEIGHT + ICON_MARGIN);
    }
}

void recon_desktop_reload(struct recon_desktop *desktop) {
    if (desktop == NULL) {
        return;
    }

    struct recon_dirent entries[ITEMS_MAX];
    int count = recon_fs_list("/", RECON_DESKTOP_DIR, entries, ITEMS_MAX);
    if (count < 0) {
        count = 0;
    }
    if (count > ITEMS_MAX) {
        count = ITEMS_MAX;
    }

    desktop->item_count = 0;
    for (int i = 0; i < count; i++) {
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
    recon_desktop_refresh(desktop);
}

/* --- Drawing --- */

static void draw_icon(struct recon_desktop *desktop, struct recon_panel *p,
        const struct desktop_item *item) {
    int cx = item->x + (ICON_WIDTH - ICON_IMAGE) / 2;
    int cy = item->y + 6;

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

        recon_draw_text(p, desktop->font, lx + 1, ly + 1, ICON_WIDTH - 4,
            item->label, COLOR_LABEL_SHADOW);
        recon_draw_text(p, desktop->font, lx, ly, ICON_WIDTH - 4,
            item->label, COLOR_LABEL);

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

/* --- Input --- */

bool recon_desktop_handle_click(struct recon_desktop *desktop, double lx, double ly,
        bool pressed, struct recon_desktop_action *action) {
    if (desktop == NULL || !pressed) {
        return false;
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

    if (desktop->selected == index) {
        /* Clicking an already-selected item opens it, which is a double click
         * without needing to measure the gap between two. */
        switch (item->kind) {
        case ITEM_SHORTCUT:
            action->kind = RECON_DESKTOP_ACTION_OPEN_APP;
            snprintf(action->target, sizeof(action->target), "%s", item->target);
            break;
        case ITEM_FOLDER:
            action->kind = RECON_DESKTOP_ACTION_OPEN_PATH;
            snprintf(action->target, sizeof(action->target), "%s/%s",
                RECON_DESKTOP_DIR, item->name);
            break;
        case ITEM_FILE:
            action->kind = RECON_DESKTOP_ACTION_OPEN_PATH;
            snprintf(action->target, sizeof(action->target), "%s", RECON_DESKTOP_DIR);
            break;
        }
        return true;
    }

    desktop->selected = index;
    recon_desktop_refresh(desktop);
    return true;
}
