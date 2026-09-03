/*
 * ReconOS File Explorer.
 *
 * A native application: it ships with the system and is part of it, rather
 * than something installed into it. It browses the ReconOS filesystem only --
 * there is no path it can be given that reaches the host, because there is no
 * such path.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_appwin.h"
#include "recon_explorer.h"
#include "recon_fs.h"
#include "recon_ui.h"

#define TOOLBAR_HEIGHT 30
#define PATHBAR_HEIGHT 24
#define HEADER_HEIGHT 22
#define ROW_HEIGHT 18
#define STATUS_HEIGHT 24
#define PADDING 6
#define BUTTON_HEIGHT 22
#define BUTTON_GAP 4

#define COL_NAME 8
#define COL_TYPE 300
#define COL_SIZE 400

#define ENTRIES_MAX 512

#define COLOR_BG RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_LIST_BG RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_HEADER RECON_RGB(0xD4, 0xD4, 0xD4)
#define COLOR_TEXT RECON_RGB(0x10, 0x10, 0x10)
#define COLOR_DIR RECON_RGB(0x1A, 0x3A, 0x8B)
#define COLOR_ROW_ALT RECON_RGB(0xF2, 0xF2, 0xF6)
#define COLOR_SELECTED RECON_RGB(0x30, 0x50, 0x90)
#define COLOR_SELECTED_TEXT RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_BUTTON RECON_RGB(0xC8, 0xC8, 0xC8)
#define COLOR_PATH_BG RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_STATUS RECON_RGB(0x30, 0x30, 0x30)
#define COLOR_WARNING RECON_RGB(0x8B, 0x1A, 0x1A)

#define HIT_UP (RECON_APPWIN_HIT_USER + 1)
#define HIT_HOME (RECON_APPWIN_HIT_USER + 2)
#define HIT_NEWFOLDER (RECON_APPWIN_HIT_USER + 3)
#define HIT_DELETE (RECON_APPWIN_HIT_USER + 4)
#define HIT_REFRESH (RECON_APPWIN_HIT_USER + 5)
#define HIT_ROW_BASE (RECON_APPWIN_HIT_USER + 100)

struct recon_explorer {
    struct recon_font *font;
    struct recon_appwin *win;

    char cwd[RECON_PATH_MAX];
    struct recon_dirent entries[ENTRIES_MAX];
    int entry_count;

    int selected;
    int scroll;
    int rows_visible;

    /*
     * Deleting is confirmed by asking again rather than by a dialog. The
     * armed state is cleared by anything else the user does, so a stray second
     * click cannot delete something they have since moved on from.
     */
    bool delete_armed;

    char status[192];
    bool status_is_warning;

    int new_folder_counter;
};

/* --- Navigation --- */

static void set_status(struct recon_explorer *ex, bool warning, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_status(struct recon_explorer *ex, bool warning, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(ex->status, sizeof(ex->status), fmt, args);
    va_end(args);
    ex->status_is_warning = warning;
}

static void reload(struct recon_explorer *ex) {
    ex->entry_count = recon_fs_list(ex->cwd, ".", ex->entries, ENTRIES_MAX);
    if (ex->entry_count < 0) {
        ex->entry_count = 0;
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }
    if (ex->entry_count > ENTRIES_MAX) {
        ex->entry_count = ENTRIES_MAX;
    }

    int dirs = 0;
    size_t bytes = 0;
    for (int i = 0; i < ex->entry_count; i++) {
        if (ex->entries[i].kind == RECON_FILE_DIRECTORY) {
            dirs++;
        } else {
            bytes += ex->entries[i].size;
        }
    }

    set_status(ex, false, "%d item%s   %d folder%s   %zu bytes",
        ex->entry_count, ex->entry_count == 1 ? "" : "s",
        dirs, dirs == 1 ? "" : "s", bytes);
}

static void navigate(struct recon_explorer *ex, const char *path) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];

    if (!recon_fs_resolve(ex->cwd, path, host, sizeof(host),
            canonical, sizeof(canonical))) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    struct recon_dirent info;
    if (!recon_fs_stat(ex->cwd, path, &info) || info.kind != RECON_FILE_DIRECTORY) {
        set_status(ex, true, "'%s' is not a folder", canonical);
        return;
    }

    snprintf(ex->cwd, sizeof(ex->cwd), "%s", canonical);
    ex->selected = -1;
    ex->scroll = 0;
    ex->delete_armed = false;
    reload(ex);
}

/* --- Actions --- */

static void do_new_folder(struct recon_explorer *ex) {
    /* Without a way to type a name yet, pick an unused one. */
    char name[64];
    do {
        snprintf(name, sizeof(name), "New Folder %d", ++ex->new_folder_counter);
    } while (recon_fs_exists(ex->cwd, name) && ex->new_folder_counter < 1000);

    if (!recon_fs_mkdir(ex->cwd, name)) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }
    reload(ex);
    set_status(ex, false, "Created '%s'", name);
}

static void do_delete(struct recon_explorer *ex) {
    if (ex->selected < 0 || ex->selected >= ex->entry_count) {
        set_status(ex, true, "Select something to delete first");
        return;
    }

    const struct recon_dirent *entry = &ex->entries[ex->selected];

    if (!ex->delete_armed) {
        /* Ask before doing it. Deleting is not undoable here, so it should
         * take a deliberate second action rather than one stray click. */
        ex->delete_armed = true;
        set_status(ex, true, "Delete '%s'? Click Delete again to confirm.", entry->name);
        return;
    }

    ex->delete_armed = false;
    if (!recon_fs_remove(ex->cwd, entry->name)) {
        set_status(ex, true, "%s", recon_fs_last_error());
        return;
    }

    char removed[RECON_NAME_MAX];
    snprintf(removed, sizeof(removed), "%s", entry->name);
    ex->selected = -1;
    reload(ex);
    set_status(ex, false, "Deleted '%s'", removed);
}

/* --- Drawing --- */

static int draw_button(struct recon_explorer *ex, struct recon_panel *p,
        int x, int y, const char *label, uint32_t id, bool warning) {
    int ascent = recon_font_ascent(ex->font);
    int width = recon_text_width(ex->font, label) + 22;

    recon_fill_rect(p, x, y, width, BUTTON_HEIGHT, COLOR_BUTTON);
    recon_draw_bevel(p, x, y, width, BUTTON_HEIGHT, false);
    recon_draw_text(p, ex->font, x + 11, y + (BUTTON_HEIGHT + ascent) / 2 - 2,
        width - 16, label, warning ? COLOR_WARNING : COLOR_TEXT);
    recon_hit_add(p, x, y, width, BUTTON_HEIGHT, id);

    return x + width + BUTTON_GAP;
}

static void explorer_draw(void *user, struct recon_panel *p,
        int x, int y, int w, int h) {
    struct recon_explorer *ex = user;
    int ascent = recon_font_ascent(ex->font);

    recon_fill_rect(p, x, y, w, h, COLOR_BG);

    /* Toolbar. */
    int bx = x + PADDING;
    int by = y + (TOOLBAR_HEIGHT - BUTTON_HEIGHT) / 2;
    bx = draw_button(ex, p, bx, by, "Up", HIT_UP, false);
    bx = draw_button(ex, p, bx, by, "Home", HIT_HOME, false);
    bx = draw_button(ex, p, bx, by, "Refresh", HIT_REFRESH, false);
    bx = draw_button(ex, p, bx, by, "New Folder", HIT_NEWFOLDER, false);
    draw_button(ex, p, bx, by, ex->delete_armed ? "Confirm Delete" : "Delete",
        HIT_DELETE, ex->delete_armed);

    /* Path bar. */
    int py = y + TOOLBAR_HEIGHT;
    recon_fill_rect(p, x + PADDING, py, w - PADDING * 2, PATHBAR_HEIGHT, COLOR_PATH_BG);
    recon_draw_bevel(p, x + PADDING, py, w - PADDING * 2, PATHBAR_HEIGHT, true);
    recon_draw_text(p, ex->font, x + PADDING + 6,
        py + (PATHBAR_HEIGHT + ascent) / 2 - 2, w - PADDING * 2 - 12,
        ex->cwd, COLOR_TEXT);

    /* Column headings. */
    int hy = py + PATHBAR_HEIGHT + 2;
    recon_fill_rect(p, x, hy, w, HEADER_HEIGHT, COLOR_HEADER);
    recon_fill_rect(p, x, hy + HEADER_HEIGHT - 1, w, 1, RECON_RGB(0x80, 0x80, 0x80));

    int hbase = hy + (HEADER_HEIGHT + ascent) / 2 - 2;
    recon_draw_text(p, ex->font, x + COL_NAME, hbase, 260, "Name", COLOR_TEXT);
    recon_draw_text(p, ex->font, x + COL_TYPE, hbase, 90, "Type", COLOR_TEXT);
    recon_draw_text(p, ex->font, x + COL_SIZE, hbase, 90, "Size", COLOR_TEXT);

    /* Listing. */
    int ly = hy + HEADER_HEIGHT;
    int list_h = h - (ly - y) - STATUS_HEIGHT;
    ex->rows_visible = list_h > 0 ? list_h / ROW_HEIGHT : 0;

    recon_fill_rect(p, x, ly, w, list_h, COLOR_LIST_BG);

    for (int row = 0; row < ex->rows_visible; row++) {
        int index = ex->scroll + row;
        if (index >= ex->entry_count) {
            break;
        }

        const struct recon_dirent *entry = &ex->entries[index];
        int ry = ly + row * ROW_HEIGHT;
        int baseline = ry + (ROW_HEIGHT + ascent) / 2 - 2;
        bool selected = (index == ex->selected);
        bool is_dir = (entry->kind == RECON_FILE_DIRECTORY);

        if (selected) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_SELECTED);
        } else if (row % 2 == 1) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        recon_color text = selected ? COLOR_SELECTED_TEXT
            : (is_dir ? COLOR_DIR : COLOR_TEXT);

        recon_draw_text(p, ex->font, x + COL_NAME, baseline,
            COL_TYPE - COL_NAME - 10, entry->name, text);
        recon_draw_text(p, ex->font, x + COL_TYPE, baseline, 90,
            is_dir ? "Folder" : "File", text);

        if (!is_dir) {
            char size[32];
            if (entry->size >= 1024 * 1024) {
                snprintf(size, sizeof(size), "%.1f MB", entry->size / (1024.0 * 1024.0));
            } else if (entry->size >= 1024) {
                snprintf(size, sizeof(size), "%.1f KB", entry->size / 1024.0);
            } else {
                snprintf(size, sizeof(size), "%zu B", entry->size);
            }
            recon_draw_text(p, ex->font, x + COL_SIZE, baseline, 90, size, text);
        }

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + row);
    }

    if (ex->entry_count == 0) {
        recon_draw_text(p, ex->font, x + COL_NAME, ly + ROW_HEIGHT,
            w - COL_NAME, "This folder is empty", RECON_RGB(0x80, 0x80, 0x80));
    }

    /* Status. */
    int sy = y + h - STATUS_HEIGHT;
    recon_fill_rect(p, x, sy, w, STATUS_HEIGHT, COLOR_BG);
    recon_draw_text(p, ex->font, x + PADDING, sy + (STATUS_HEIGHT + ascent) / 2 - 2,
        w - PADDING * 2, ex->status,
        ex->status_is_warning ? COLOR_WARNING : COLOR_STATUS);
}

/* --- Input --- */

static bool explorer_click(void *user, uint32_t hit_id, int cx, int cy, bool pressed) {
    struct recon_explorer *ex = user;
    if (!pressed) {
        return false;
    }

    /* Anything other than pressing Delete again cancels a pending delete. */
    if (hit_id != HIT_DELETE) {
        ex->delete_armed = false;
    }

    switch (hit_id) {
    case HIT_UP:
        navigate(ex, "..");
        return true;
    case HIT_HOME:
        navigate(ex, "/");
        return true;
    case HIT_REFRESH:
        reload(ex);
        return true;
    case HIT_NEWFOLDER:
        do_new_folder(ex);
        return true;
    case HIT_DELETE:
        do_delete(ex);
        return true;
    default:
        break;
    }

    if (hit_id >= HIT_ROW_BASE) {
        int index = ex->scroll + (int)(hit_id - HIT_ROW_BASE);
        if (index < 0 || index >= ex->entry_count) {
            return true;
        }

        if (ex->selected == index &&
                ex->entries[index].kind == RECON_FILE_DIRECTORY) {
            /* A second click on an already-selected folder opens it, which is
             * how a double click behaves without needing to time one. */
            navigate(ex, ex->entries[index].name);
            return true;
        }

        ex->selected = index;
        const struct recon_dirent *entry = &ex->entries[index];
        if (entry->kind == RECON_FILE_DIRECTORY) {
            set_status(ex, false, "'%s' - click again to open", entry->name);
        } else {
            set_status(ex, false, "'%s' - %zu bytes", entry->name, entry->size);
        }
        return true;
    }

    return false;
}

static bool explorer_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_explorer *ex = user;

    switch (sym) {
    case XKB_KEY_Up:
        if (ex->selected > 0) {
            ex->selected--;
        }
        if (ex->selected < ex->scroll) {
            ex->scroll = ex->selected;
        }
        return true;

    case XKB_KEY_Down:
        if (ex->selected + 1 < ex->entry_count) {
            ex->selected++;
        }
        if (ex->rows_visible > 0 && ex->selected >= ex->scroll + ex->rows_visible) {
            ex->scroll = ex->selected - ex->rows_visible + 1;
        }
        return true;

    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (ex->selected >= 0 && ex->selected < ex->entry_count &&
                ex->entries[ex->selected].kind == RECON_FILE_DIRECTORY) {
            navigate(ex, ex->entries[ex->selected].name);
        }
        return true;

    case XKB_KEY_BackSpace:
        navigate(ex, "..");
        return true;

    case XKB_KEY_F5:
        reload(ex);
        return true;

    case XKB_KEY_Escape:
        ex->delete_armed = false;
        ex->selected = -1;
        return true;

    default:
        return false;
    }
}

static void explorer_scroll(void *user, double delta) {
    struct recon_explorer *ex = user;
    int max = ex->entry_count - ex->rows_visible;
    if (max < 0) {
        max = 0;
    }

    ex->scroll += (delta > 0) ? 3 : -3;
    if (ex->scroll > max) {
        ex->scroll = max;
    }
    if (ex->scroll < 0) {
        ex->scroll = 0;
    }
}

/* The listing may have changed while the window was closed. */
static void explorer_visibility(void *user, bool visible) {
    if (visible) {
        reload(user);
    }
}

static void explorer_destroy(void *user) {
    free(user);
}

static const struct recon_appwin_impl EXPLORER_IMPL = {
    .title = "File Explorer",
    .default_width = 560,
    .default_height = 420,
    .min_width = 340,
    .min_height = 240,
    .draw = explorer_draw,
    .click = explorer_click,
    .key = explorer_key,
    .scroll = explorer_scroll,
    .visibility = explorer_visibility,
    .destroy = explorer_destroy,
};

struct recon_appwin *recon_explorer_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_explorer *ex = calloc(1, sizeof(*ex));
    if (ex == NULL) {
        return NULL;
    }

    ex->font = font;
    ex->selected = -1;
    snprintf(ex->cwd, sizeof(ex->cwd), "/");
    reload(ex);

    ex->win = recon_appwin_create(server, font, &EXPLORER_IMPL, ex);
    if (ex->win == NULL) {
        free(ex);
        return NULL;
    }
    return ex->win;
}
