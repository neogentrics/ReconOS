/*
 * The rest of what a headless browser needs, on top of `appwin_stubs.c`.
 *
 * --- What is stubbed and what is not ---
 *
 * The suite links the browser's **real** dependencies wherever they are
 * logic: `recon_html.c`, `recon_css.c`, `recon_cookie.c`, `recon_form.c`,
 * `recon_http.c`, `recon_fs.c` and `recon_appwin.c` are all the shipping
 * files. What is stubbed here is the part of the desktop that puts pixels on
 * a screen, asks the host for a font, or opens a socket -- and each of those
 * is stubbed for a different reason:
 *
 *  - **Drawing** because the suite makes no claim about pixels. `recon_web.c`
 *    is 6,433 lines and almost none of them are a `memcpy` into a buffer; the
 *    part worth holding is what it decides.
 *  - **Fonts** because a font comes off the host's disk, and a browser whose
 *    suite fails on a machine with no fonts installed is a suite about the
 *    machine.
 *  - **The network** because a suite that opens sockets is a suite that fails
 *    on a train. `recon_web_open_path` reads a file through `recon_fs`, which
 *    is real here, so every page in this suite is a real parse of real bytes
 *    -- it is only the *fetching* that is absent.
 *
 * --- The font is NULL and that is deliberate ---
 *
 * `recon_font_system` answers NULL, which is what a machine with no font
 * installed hands back, and every drawing call here ignores it. Measuring a
 * string would otherwise need a real face, and the width of a word is not
 * something this suite has an opinion about.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ReconOS.h"
#include "recon_filedlg.h"
#include "recon_media.h"
#include "recon_net.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_widget.h"

/* --- colour ------------------------------------------------------------- */

/* --- drawing ------------------------------------------------------------ */

void recon_draw_image_clipped(struct recon_panel *panel, int x, int y, int w,
        int h, const unsigned char *rgba, int image_width, int image_height,
        int top, int bottom) {
    (void)panel; (void)x; (void)y; (void)w; (void)h; (void)rgba;
    (void)image_width; (void)image_height; (void)top; (void)bottom;
}

/* --- fonts -------------------------------------------------------------- */

/*
 * No face, at any size.
 *
 * `recon_web.c` checks for NULL everywhere it asks for one, because a machine
 * with no font installed is a real state -- v0.4.67 measured the desktop
 * program's exit code for exactly that. So this is not a convenience; it is
 * one of the states the browser is written to survive.
 */
struct recon_font *recon_font_system(int pixel_height) {
    (void)pixel_height;
    return NULL;
}

struct recon_font *recon_font_bold(int pixel_height) {
    (void)pixel_height;
    return NULL;
}

struct recon_font *recon_font_monospace(int pixel_height) {
    (void)pixel_height;
    return NULL;
}

/* --- the text field ----------------------------------------------------- */

/*
 * The URL bar, which does not type.
 *
 * `recon_edit` lives in `recon_ui.c` with the drawing, so linking the real one
 * would bring a compositor back with it. What that costs is the ability to
 * test typing an address -- and it costs nothing else, because this suite
 * navigates with `recon_web_open_path`, which is the same door the Explorer
 * uses when somebody double-clicks an HTML file.
 *
 * `RECON_EDIT_IGNORED` is the honest answer: no key is one the editor used.
 * A stub answering `COMMIT` would have the browser navigating on every
 * keystroke, which is not a browser anybody would recognise.
 */
void recon_edit_begin(struct recon_edit *edit, const char *initial,
        bool select_stem) {
    (void)select_stem;
    if (edit != NULL) {
        memset(edit, 0, sizeof(*edit));
        edit->anchor = -1;
        if (initial != NULL) {
            snprintf(edit->text, sizeof(edit->text), "%s", initial);
            edit->length = (int)strlen(edit->text);
            edit->caret = edit->length;
        }
    }
}

void recon_edit_end(struct recon_edit *edit) {
    if (edit != NULL) {
        edit->active = false;
    }
}

void recon_edit_focus(struct recon_edit *edit) {
    if (edit != NULL) {
        edit->active = true;
    }
}

enum recon_edit_result recon_edit_key(struct recon_edit *edit,
        recon_keysym sym, uint32_t modifiers) {
    (void)edit; (void)sym; (void)modifiers;
    return RECON_EDIT_IGNORED;
}

void recon_edit_draw(struct recon_panel *panel, struct recon_font *font,
        int x, int y, int w, int h, const struct recon_edit *edit) {
    (void)panel; (void)font; (void)x; (void)y; (void)w; (void)h; (void)edit;
}

/* --- hit regions and panels --------------------------------------------- */

/* --- the file dialog ---------------------------------------------------- */

/*
 * Never open.
 *
 * The browser asks `recon_filedlg_is_open` before it draws or routes a key,
 * so answering false keeps every test on the ordinary path. A suite that
 * wanted to check Save As would have to link the real one, and that is a
 * different suite about a different file.
 */
void recon_filedlg_open(struct recon_filedlg *dialog,
        enum recon_filedlg_mode mode, const char *title, const char *start_dir,
        const char *suggested_name) {
    (void)dialog; (void)mode; (void)title; (void)start_dir;
    (void)suggested_name;
}

void recon_filedlg_close(struct recon_filedlg *dialog) { (void)dialog; }

bool recon_filedlg_is_open(const struct recon_filedlg *dialog) {
    (void)dialog;
    return false;
}

enum recon_filedlg_result recon_filedlg_click(struct recon_filedlg *dialog,
        uint32_t hit_id) {
    (void)dialog; (void)hit_id;
    return RECON_FILEDLG_NOTHING;
}

enum recon_filedlg_result recon_filedlg_key(struct recon_filedlg *dialog,
        recon_keysym sym, uint32_t modifiers) {
    (void)dialog; (void)sym; (void)modifiers;
    return RECON_FILEDLG_NOTHING;
}

void recon_filedlg_draw(struct recon_filedlg *dialog, struct recon_panel *panel,
        struct recon_font *font, int x, int y, int w, int h) {
    (void)dialog; (void)panel; (void)font; (void)x; (void)y; (void)w; (void)h;
}

const char *recon_filedlg_path(const struct recon_filedlg *dialog) {
    (void)dialog;
    return "";
}

/* --- the network -------------------------------------------------------- */

/*
 * Every connection is refused, and that is the honest answer for a suite.
 *
 * A stub that pretended to connect would need a second implementation of a
 * server to say anything back, and the first place the two disagreed would be
 * the place nobody checked. What this suite drives instead is
 * `recon_web_open_path`, which reads a file through the real `recon_fs` -- so
 * every page here is a real parse of real bytes and only the fetching is gone.
 */
struct recon_net_stream *recon_net_stream_open(const char *application,
        const char *host, int port,
        const struct recon_net_stream_handlers *handlers, void *user) {
    (void)application; (void)host; (void)port; (void)handlers; (void)user;
    return NULL;
}

struct recon_net_stream *recon_net_stream_open_tls(const char *application,
        const char *host, int port,
        const struct recon_net_stream_handlers *handlers, void *user) {
    (void)application; (void)host; (void)port; (void)handlers; (void)user;
    return NULL;
}

void recon_net_stream_close(struct recon_net_stream *stream) { (void)stream; }

bool recon_net_stream_send(struct recon_net_stream *stream, const char *bytes,
        size_t length) {
    (void)stream; (void)bytes; (void)length;
    return false;
}

const char *recon_net_last_error(void) {
    return "there is no network in this suite";
}

const char *recon_net_result_name(enum recon_net_result result) {
    (void)result;
    return "refused";
}

/* --- the rest ----------------------------------------------------------- */

const char *recon_media_type(const char *name) {
    (void)name;
    return "application/octet-stream";
}
