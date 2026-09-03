/*
 * The ReconOS file dialog. See include/recon_filedlg.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "recon_filedlg.h"
#include "recon_icons.h"
#include "recon_theme.h"

#define DIALOG_MARGIN 18
/* An upper bound, so a big window does not get a dialog the size of a big
 * window. A file picker is a fixed-size thing everywhere else for the same
 * reason. */
#define DIALOG_MAX_WIDTH 460
#define DIALOG_MAX_HEIGHT 340
#define TITLE_HEIGHT 24
#define PATH_HEIGHT 22
#define ROW_HEIGHT 18
#define NAME_HEIGHT 24
#define BUTTON_HEIGHT 24
#define BUTTON_WIDTH 84
#define PADDING 8

/* Enough to say the window behind is not usable, not so much that it
 * looks switched off. */
#define COLOR_DIM THEME(DIM)
#define COLOR_BG THEME(DIALOG)
#define COLOR_TITLE THEME(DIALOG_TITLE)
#define COLOR_TITLE_TEXT THEME(DIALOG_TITLE_TEXT)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_DIR THEME(DIRECTORY)
#define COLOR_LIST_BG THEME(SURFACE)
#define COLOR_SELECTED THEME(SELECTION)
#define COLOR_SELECTED_TEXT THEME(SELECTION_TEXT)
#define COLOR_BUTTON THEME(BUTTON)
#define COLOR_BORDER THEME(MENU_BORDER)
#define COLOR_WARNING THEME(WARNING)

#define HIT_UP (RECON_FILEDLG_HIT_BASE + 1)
#define HIT_HOME (RECON_FILEDLG_HIT_BASE + 2)
#define HIT_ACCEPT (RECON_FILEDLG_HIT_BASE + 3)
#define HIT_CANCEL (RECON_FILEDLG_HIT_BASE + 4)
#define HIT_NAME (RECON_FILEDLG_HIT_BASE + 5)
#define HIT_ROW_BASE (RECON_FILEDLG_HIT_BASE + 100)

/* --- Listing --- */

static void reload(struct recon_filedlg *dialog) {
    dialog->entry_count = recon_fs_list(dialog->cwd, ".", dialog->entries,
        RECON_FILEDLG_ENTRIES_MAX);
    if (dialog->entry_count < 0) {
        dialog->entry_count = 0;
        snprintf(dialog->message, sizeof(dialog->message), "%s",
            recon_fs_last_error());
    }
    if (dialog->entry_count > RECON_FILEDLG_ENTRIES_MAX) {
        dialog->entry_count = RECON_FILEDLG_ENTRIES_MAX;
    }
    dialog->selected = -1;
    dialog->scroll = 0;
}

static void go_to(struct recon_filedlg *dialog, const char *path) {
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];

    if (!recon_fs_resolve(dialog->cwd, path, host, sizeof(host),
            canonical, sizeof(canonical))) {
        snprintf(dialog->message, sizeof(dialog->message), "%s",
            recon_fs_last_error());
        return;
    }

    struct recon_dirent info;
    if (!recon_fs_stat(dialog->cwd, path, &info) ||
            info.kind != RECON_FILE_DIRECTORY) {
        snprintf(dialog->message, sizeof(dialog->message),
            "'%s' is not a folder", canonical);
        return;
    }

    snprintf(dialog->cwd, sizeof(dialog->cwd), "%s", canonical);
    dialog->message[0] = '\0';
    reload(dialog);
}

/* --- Lifecycle --- */

void recon_filedlg_open(struct recon_filedlg *dialog, enum recon_filedlg_mode mode,
        const char *title, const char *start_dir, const char *suggested_name) {
    if (dialog == NULL) {
        return;
    }

    memset(dialog, 0, sizeof(*dialog));
    dialog->open = true;
    dialog->mode = mode;
    dialog->selected = -1;
    snprintf(dialog->title, sizeof(dialog->title), "%s",
        title != NULL ? title : (mode == RECON_FILEDLG_SAVE ? "Save As" : "Open"));

    const char *start = (start_dir != NULL && *start_dir != '\0')
        ? start_dir : recon_fs_user_dir("Documents");
    snprintf(dialog->cwd, sizeof(dialog->cwd), "%s", start);

    /* An unreachable starting folder should not leave an empty dialog with no
     * way out; fall back to somewhere that certainly exists. */
    struct recon_dirent info;
    if (!recon_fs_stat("/", dialog->cwd, &info) ||
            info.kind != RECON_FILE_DIRECTORY) {
        snprintf(dialog->cwd, sizeof(dialog->cwd), "%s", recon_fs_user_dir(NULL));
    }

    reload(dialog);

    /* The caret sits before the extension, so typing replaces the name and
     * leaves ".txt" alone. */
    recon_edit_begin(&dialog->name, suggested_name != NULL ? suggested_name : "",
        true);
}

void recon_filedlg_close(struct recon_filedlg *dialog) {
    if (dialog == NULL) {
        return;
    }
    dialog->open = false;
    recon_edit_end(&dialog->name);
}

bool recon_filedlg_is_open(const struct recon_filedlg *dialog) {
    return dialog != NULL && dialog->open;
}

const char *recon_filedlg_path(const struct recon_filedlg *dialog) {
    return dialog != NULL ? dialog->result : "";
}

/* --- Deciding --- */

/*
 * Turn what is typed into a path, or say why it cannot be one.
 *
 * A name with a slash in it is refused rather than resolved: the dialog shows
 * one folder, and a name that quietly saved somewhere else would be saving
 * somewhere the user was not looking.
 */
static enum recon_filedlg_result accept(struct recon_filedlg *dialog) {
    const char *name = dialog->name.text;

    while (*name == ' ') {
        name++;
    }
    if (*name == '\0') {
        snprintf(dialog->message, sizeof(dialog->message),
            dialog->mode == RECON_FILEDLG_SAVE
                ? "Type a name to save as" : "Choose a file");
        return RECON_FILEDLG_NOTHING;
    }
    if (strchr(name, '/') != NULL) {
        snprintf(dialog->message, sizeof(dialog->message),
            "A name cannot contain '/'");
        return RECON_FILEDLG_NOTHING;
    }

    char trimmed[RECON_NAME_MAX];
    snprintf(trimmed, sizeof(trimmed), "%s", name);
    char *end = trimmed + strlen(trimmed);
    while (end > trimmed && end[-1] == ' ') {
        *--end = '\0';
    }

    /* Typing a folder's name goes into it, rather than trying to save over
     * it. */
    struct recon_dirent info;
    if (recon_fs_stat(dialog->cwd, trimmed, &info) &&
            info.kind == RECON_FILE_DIRECTORY) {
        go_to(dialog, trimmed);
        recon_edit_begin(&dialog->name, "", false);
        return RECON_FILEDLG_NOTHING;
    }

    if (dialog->mode == RECON_FILEDLG_OPEN &&
            !recon_fs_exists(dialog->cwd, trimmed)) {
        snprintf(dialog->message, sizeof(dialog->message),
            "'%s' does not exist", trimmed);
        return RECON_FILEDLG_NOTHING;
    }

    if (strcmp(dialog->cwd, "/") == 0) {
        snprintf(dialog->result, sizeof(dialog->result), "/%s", trimmed);
    } else {
        snprintf(dialog->result, sizeof(dialog->result), "%s/%s",
            dialog->cwd, trimmed);
    }

    dialog->open = false;
    recon_edit_end(&dialog->name);
    return RECON_FILEDLG_ACCEPTED;
}

/* --- Drawing --- */

/* Where the dialog sits inside the content area. */
static void dialog_rect(int x, int y, int w, int h,
        int *dx, int *dy, int *dw, int *dh) {
    int width = w - DIALOG_MARGIN * 2;
    int height = h - DIALOG_MARGIN * 2;

    /*
     * Capped, so some of the window is still visible around it. Filling the
     * whole content area made the window underneath look like it had vanished
     * and left its frame behind -- which is what it looked like to a user, and
     * a dialog should read as being *over* something rather than as having
     * replaced it.
     */
    if (width > DIALOG_MAX_WIDTH) {
        width = DIALOG_MAX_WIDTH;
    }
    if (height > DIALOG_MAX_HEIGHT) {
        height = DIALOG_MAX_HEIGHT;
    }

    /* Small windows still get a usable dialog: it fills what there is rather
     * than being clipped down to nothing. */
    if (width < 220) {
        width = w > 8 ? w - 8 : w;
    }
    if (height < 160) {
        height = h > 8 ? h - 8 : h;
    }

    *dw = width;
    *dh = height;
    *dx = x + (w - width) / 2;
    *dy = y + (h - height) / 2;
}

void recon_filedlg_draw(struct recon_filedlg *dialog, struct recon_panel *panel,
        struct recon_font *font, int x, int y, int w, int h) {
    if (dialog == NULL || !dialog->open || panel == NULL || font == NULL) {
        return;
    }

    int ascent = recon_font_ascent(font);

    /* Dim what is behind, so it reads as a question rather than as another
     * part of the window. */
    recon_fill_rect(panel, x, y, w, h, COLOR_DIM);

    int dx, dy, dw, dh;
    dialog_rect(x, y, w, h, &dx, &dy, &dw, &dh);

    recon_fill_rect(panel, dx, dy, dw, dh, COLOR_BG);
    recon_stroke_rect(panel, dx, dy, dw, dh, COLOR_BORDER);

    /* Title bar. */
    recon_fill_rect(panel, dx, dy, dw, TITLE_HEIGHT, COLOR_TITLE);
    recon_draw_text(panel, font, dx + 8, dy + (TITLE_HEIGHT + ascent) / 2 - 1,
        dw - 16, dialog->title, COLOR_TITLE_TEXT);

    /* Where we are, with the two ways out of it. */
    int py = dy + TITLE_HEIGHT + PADDING / 2;
    int nav_w = 34;

    recon_fill_rect(panel, dx + PADDING, py, nav_w, PATH_HEIGHT, COLOR_BUTTON);
    recon_draw_bevel(panel, dx + PADDING, py, nav_w, PATH_HEIGHT, false);
    recon_draw_text(panel, font, dx + PADDING + 11,
        py + (PATH_HEIGHT + ascent) / 2 - 2, nav_w, "Up", COLOR_TEXT);
    recon_hit_add(panel, dx + PADDING, py, nav_w, PATH_HEIGHT, HIT_UP);

    int hx = dx + PADDING + nav_w + 4;
    recon_fill_rect(panel, hx, py, nav_w + 12, PATH_HEIGHT, COLOR_BUTTON);
    recon_draw_bevel(panel, hx, py, nav_w + 12, PATH_HEIGHT, false);
    recon_draw_text(panel, font, hx + 8, py + (PATH_HEIGHT + ascent) / 2 - 2,
        nav_w + 12, "Home", COLOR_TEXT);
    recon_hit_add(panel, hx, py, nav_w + 12, PATH_HEIGHT, HIT_HOME);

    int path_x = hx + nav_w + 18;
    int path_w = dx + dw - PADDING - path_x;
    if (path_w > 0) {
        recon_fill_rect(panel, path_x, py, path_w, PATH_HEIGHT, COLOR_LIST_BG);
        recon_draw_bevel(panel, path_x, py, path_w, PATH_HEIGHT, true);
        recon_draw_text(panel, font, path_x + 6,
            py + (PATH_HEIGHT + ascent) / 2 - 2, path_w - 12,
            dialog->cwd, COLOR_TEXT);
    }

    /* The listing fills whatever is left between the path bar and the
     * controls along the bottom. */
    int list_y = py + PATH_HEIGHT + 4;
    int bottom = dy + dh - PADDING - BUTTON_HEIGHT - 4 - NAME_HEIGHT - 4;
    int list_h = bottom - list_y;
    int list_x = dx + PADDING;
    int list_w = dw - PADDING * 2;

    if (list_h < ROW_HEIGHT) {
        list_h = ROW_HEIGHT;
    }
    dialog->rows_visible = list_h / ROW_HEIGHT;

    recon_fill_rect(panel, list_x, list_y, list_w, list_h, COLOR_LIST_BG);
    recon_draw_bevel(panel, list_x, list_y, list_w, list_h, true);

    for (int row = 0; row < dialog->rows_visible; row++) {
        int index = dialog->scroll + row;
        if (index >= dialog->entry_count) {
            break;
        }

        const struct recon_dirent *entry = &dialog->entries[index];
        bool is_dir = (entry->kind == RECON_FILE_DIRECTORY);
        int ry = list_y + 2 + row * ROW_HEIGHT;
        if (ry + ROW_HEIGHT > list_y + list_h) {
            break;
        }

        bool selected = (index == dialog->selected);
        if (selected) {
            recon_fill_rect(panel, list_x + 2, ry, list_w - 4, ROW_HEIGHT,
                COLOR_SELECTED);
        }

        int name_x = list_x + 8;
        if (recon_icon_draw(panel, is_dir ? RECON_ICON_FOLDER : RECON_ICON_FILE,
                list_x + 4, ry + 2, ROW_HEIGHT - 4)) {
            name_x = list_x + 4 + (ROW_HEIGHT - 4) + 6;
        }

        recon_draw_text(panel, font, name_x, ry + (ROW_HEIGHT + ascent) / 2 - 2,
            list_w - (name_x - list_x) - 8, entry->name,
            selected ? COLOR_SELECTED_TEXT : (is_dir ? COLOR_DIR : COLOR_TEXT));

        recon_hit_add(panel, list_x + 2, ry, list_w - 4, ROW_HEIGHT,
            HIT_ROW_BASE + row);
    }

    if (dialog->entry_count == 0) {
        recon_draw_text(panel, font, list_x + 8, list_y + ROW_HEIGHT,
            list_w - 16, "This folder is empty", RECON_RGB(0x80, 0x80, 0x80));
    }

    /* The name being typed. */
    int ny = list_y + list_h + 4;
    int label_w = recon_text_width(font, "Name:") + 8;
    recon_draw_text(panel, font, dx + PADDING, ny + (NAME_HEIGHT + ascent) / 2 - 2,
        label_w, "Name:", COLOR_TEXT);
    recon_edit_draw(panel, font, dx + PADDING + label_w, ny,
        dw - PADDING * 2 - label_w, NAME_HEIGHT, &dialog->name);
    recon_hit_add(panel, dx + PADDING + label_w, ny,
        dw - PADDING * 2 - label_w, NAME_HEIGHT, HIT_NAME);

    /* Buttons, and whatever went wrong beside them. */
    int by = ny + NAME_HEIGHT + 4;
    int cancel_x = dx + dw - PADDING - BUTTON_WIDTH;
    int accept_x = cancel_x - BUTTON_WIDTH - 6;

    const char *accept_label =
        dialog->mode == RECON_FILEDLG_SAVE ? "Save" : "Open";

    recon_fill_rect(panel, accept_x, by, BUTTON_WIDTH, BUTTON_HEIGHT, COLOR_BUTTON);
    recon_draw_bevel(panel, accept_x, by, BUTTON_WIDTH, BUTTON_HEIGHT, false);
    recon_draw_text(panel, font,
        accept_x + (BUTTON_WIDTH - recon_text_width(font, accept_label)) / 2,
        by + (BUTTON_HEIGHT + ascent) / 2 - 2, BUTTON_WIDTH, accept_label,
        COLOR_TEXT);
    recon_hit_add(panel, accept_x, by, BUTTON_WIDTH, BUTTON_HEIGHT, HIT_ACCEPT);

    recon_fill_rect(panel, cancel_x, by, BUTTON_WIDTH, BUTTON_HEIGHT, COLOR_BUTTON);
    recon_draw_bevel(panel, cancel_x, by, BUTTON_WIDTH, BUTTON_HEIGHT, false);
    recon_draw_text(panel, font,
        cancel_x + (BUTTON_WIDTH - recon_text_width(font, "Cancel")) / 2,
        by + (BUTTON_HEIGHT + ascent) / 2 - 2, BUTTON_WIDTH, "Cancel", COLOR_TEXT);
    recon_hit_add(panel, cancel_x, by, BUTTON_WIDTH, BUTTON_HEIGHT, HIT_CANCEL);

    if (dialog->message[0] != '\0') {
        recon_draw_text(panel, font, dx + PADDING,
            by + (BUTTON_HEIGHT + ascent) / 2 - 2,
            accept_x - dx - PADDING - 8, dialog->message, COLOR_WARNING);
    }
}

/* --- Input --- */

enum recon_filedlg_result recon_filedlg_click(struct recon_filedlg *dialog,
        uint32_t hit_id) {
    if (dialog == NULL || !dialog->open) {
        return RECON_FILEDLG_UNHANDLED;
    }

    dialog->message[0] = '\0';

    if (hit_id >= HIT_ROW_BASE) {
        int index = dialog->scroll + (int)(hit_id - HIT_ROW_BASE);
        if (index < 0 || index >= dialog->entry_count) {
            return RECON_FILEDLG_NOTHING;
        }

        const struct recon_dirent *entry = &dialog->entries[index];

        /* A second click on an already-selected row commits it, which is how
         * a double click behaves without needing to time one. */
        if (dialog->selected == index) {
            if (entry->kind == RECON_FILE_DIRECTORY) {
                char name[RECON_NAME_MAX];
                snprintf(name, sizeof(name), "%s", entry->name);
                go_to(dialog, name);
                return RECON_FILEDLG_NOTHING;
            }
            return accept(dialog);
        }

        dialog->selected = index;
        /* Selecting a file puts its name in the box, so Save writes over the
         * file you pointed at rather than a name you have to retype. */
        if (entry->kind != RECON_FILE_DIRECTORY) {
            recon_edit_begin(&dialog->name, entry->name, true);
        }
        return RECON_FILEDLG_NOTHING;
    }

    switch (hit_id) {
    case HIT_UP:
        go_to(dialog, "..");
        return RECON_FILEDLG_NOTHING;

    case HIT_HOME:
        go_to(dialog, recon_fs_user_dir(NULL));
        return RECON_FILEDLG_NOTHING;

    case HIT_NAME:
        return RECON_FILEDLG_NOTHING; /* Already where the typing goes. */

    case HIT_ACCEPT:
        return accept(dialog);

    case HIT_CANCEL:
        recon_filedlg_close(dialog);
        return RECON_FILEDLG_CANCELLED;

    default:
        break;
    }

    /*
     * A click anywhere else, including on the dimmed area, is swallowed. The
     * dialog is a question, and the window behind it should not be usable
     * until it is answered.
     */
    return RECON_FILEDLG_NOTHING;
}

enum recon_filedlg_result recon_filedlg_key(struct recon_filedlg *dialog,
        xkb_keysym_t sym, uint32_t modifiers) {
    if (dialog == NULL || !dialog->open) {
        return RECON_FILEDLG_UNHANDLED;
    }

    /* Up and Down move through the listing rather than through the text, since
     * the text is one line and has nowhere vertical to go. */
    if (sym == XKB_KEY_Up || sym == XKB_KEY_Down) {
        int index = dialog->selected;
        index += (sym == XKB_KEY_Down) ? 1 : -1;

        if (index < 0) {
            index = 0;
        }
        if (index >= dialog->entry_count) {
            index = dialog->entry_count - 1;
        }
        if (index < 0) {
            return RECON_FILEDLG_NOTHING;
        }

        dialog->selected = index;
        if (index < dialog->scroll) {
            dialog->scroll = index;
        } else if (dialog->rows_visible > 0 &&
                index >= dialog->scroll + dialog->rows_visible) {
            dialog->scroll = index - dialog->rows_visible + 1;
        }

        if (dialog->entries[index].kind != RECON_FILE_DIRECTORY) {
            recon_edit_begin(&dialog->name, dialog->entries[index].name, true);
        }
        return RECON_FILEDLG_NOTHING;
    }

    dialog->message[0] = '\0';

    switch (recon_edit_key(&dialog->name, sym, modifiers)) {
    case RECON_EDIT_COMMIT:
        /* Enter on a highlighted folder goes into it; otherwise the typed
         * name is the answer. */
        if (dialog->selected >= 0 && dialog->selected < dialog->entry_count &&
                dialog->entries[dialog->selected].kind == RECON_FILE_DIRECTORY &&
                dialog->name.length == 0) {
            char name[RECON_NAME_MAX];
            snprintf(name, sizeof(name), "%s",
                dialog->entries[dialog->selected].name);
            go_to(dialog, name);
            return RECON_FILEDLG_NOTHING;
        }
        return accept(dialog);

    case RECON_EDIT_CANCEL:
        recon_filedlg_close(dialog);
        return RECON_FILEDLG_CANCELLED;

    case RECON_EDIT_CHANGED:
    case RECON_EDIT_IGNORED:
        break;
    }

    /* Everything else is swallowed while the dialog is up. */
    return RECON_FILEDLG_NOTHING;
}

void recon_filedlg_scroll(struct recon_filedlg *dialog, double delta) {
    if (dialog == NULL || !dialog->open) {
        return;
    }

    int max = dialog->entry_count - dialog->rows_visible;
    if (max < 0) {
        max = 0;
    }

    dialog->scroll += (delta > 0) ? 3 : -3;
    if (dialog->scroll > max) {
        dialog->scroll = max;
    }
    if (dialog->scroll < 0) {
        dialog->scroll = 0;
    }
}
