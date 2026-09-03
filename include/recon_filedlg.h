/*
 * A file dialog: choosing something to open, or somewhere to save.
 *
 * It is drawn inside the window that asked for it rather than as a window of
 * its own. A separate window could be dragged away from, or left behind its
 * own parent, and would need focus rules of its own to stop that; drawn
 * in place it is always where the question was asked, and the application
 * routes input to it for exactly as long as it is up.
 *
 * The struct is public so an application can hold one by value. Nothing else
 * needs to allocate.
 */

#ifndef RECON_FILEDLG_H
#define RECON_FILEDLG_H

#include <stdbool.h>
#include <stdint.h>

#include <xkbcommon/xkbcommon.h>

#include "recon_fs.h"
#include "recon_ui.h"

struct recon_panel;
struct recon_font;

/*
 * Hit ids the dialog claims while it is open. An application must keep its own
 * ids below this; there is room for five hundred, which is four hundred and
 * ninety more than any of them use.
 */
#define RECON_FILEDLG_HIT_BASE (1000 + 500)

#define RECON_FILEDLG_ENTRIES_MAX 256

enum recon_filedlg_mode {
    RECON_FILEDLG_OPEN,
    RECON_FILEDLG_SAVE,
};

/* What the last click or key press did to the dialog. */
enum recon_filedlg_result {
    RECON_FILEDLG_NOTHING,   /* Still open; redraw. */
    RECON_FILEDLG_ACCEPTED,  /* A path was chosen; read it with _path(). */
    RECON_FILEDLG_CANCELLED, /* The user backed out. */
    RECON_FILEDLG_UNHANDLED, /* Not the dialog's input after all. */
};

struct recon_filedlg {
    bool open;
    enum recon_filedlg_mode mode;
    char title[64];

    char cwd[RECON_PATH_MAX];
    struct recon_dirent entries[RECON_FILEDLG_ENTRIES_MAX];
    int entry_count;
    int selected;
    int scroll;
    int rows_visible;

    /* The filename being typed, for saving. Always shown, so an Open dialog
     * can also be typed into rather than only clicked through. */
    struct recon_edit name;

    /* Filled in when the result is ACCEPTED. */
    char result[RECON_PATH_MAX];

    char message[160];
};

/*
 * Put the dialog up.
 *
 * `start_dir` may be NULL for the user's own folder. `suggested_name` is the
 * name offered for saving, and may be NULL.
 */
void recon_filedlg_open(struct recon_filedlg *dialog, enum recon_filedlg_mode mode,
    const char *title, const char *start_dir, const char *suggested_name);

void recon_filedlg_close(struct recon_filedlg *dialog);
bool recon_filedlg_is_open(const struct recon_filedlg *dialog);

/* The chosen path, valid after a result of ACCEPTED. */
const char *recon_filedlg_path(const struct recon_filedlg *dialog);

/*
 * Draw over the rectangle given, which should be the whole content area: the
 * dialog dims what is behind it so it is obvious that the question wants
 * answering before anything else happens.
 */
void recon_filedlg_draw(struct recon_filedlg *dialog, struct recon_panel *panel,
    struct recon_font *font, int x, int y, int w, int h);

enum recon_filedlg_result recon_filedlg_click(struct recon_filedlg *dialog,
    uint32_t hit_id);
enum recon_filedlg_result recon_filedlg_key(struct recon_filedlg *dialog,
    xkb_keysym_t sym, uint32_t modifiers);

void recon_filedlg_scroll(struct recon_filedlg *dialog, double delta);

#endif
