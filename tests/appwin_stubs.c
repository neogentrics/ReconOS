/*
 * The forty-odd things `recon_appwin.c` calls, answered plausibly.
 *
 * --- What this is for ---
 *
 * `tests/test_appwin.c` runs the real window transitions -- show, hide,
 * minimize, restore -- to find out which of an application's callbacks each
 * one fires. That is decided entirely inside `recon_appwin.c`, and the only
 * reason it could not be checked before was that the file would not link
 * without a compositor under it.
 *
 * It does not need one. `struct recon_server` is opaque to it and is only ever
 * tested against NULL; `struct recon_panel` is made by one call here and
 * handed back to others. Nothing in that file looks inside either. So this
 * file is the link, and it is deliberately dull.
 *
 * --- Why none of these fails when it is called ---
 *
 * A tripwire would be the stronger stub if these paths were unreachable, and
 * they are not: showing a window draws it, so every drawing primitive is
 * reached on the ordinary path. A stub that failed on being called would be
 * testing that a window does not draw, which is not true and not the question.
 *
 * **Nothing in the suite asserts anything about this file.** Every check there
 * is about which callbacks on the application's own impl were reached, and in
 * what order. These answers only have to be harmless enough to let the real
 * code get there.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ReconOS.h"
#include "recon_error.h"
#include "recon_help.h"
#include "recon_icons.h"
#include "recon_registry.h"
#include "recon_server_facts.h"
#include "recon_shell.h"
#include "recon_theme.h"
#include "recon_titlebar.h"
#include "recon_ui.h"
#include "recon_widget.h"

/*
 * The panel.
 *
 * One address, handed out and never looked at -- by this file or by the one
 * under test. It has to be non-NULL, because `apply_visibility` returns early
 * on a window without a panel and a test whose transitions were all early
 * returns would pass while measuring nothing.
 */
static int g_panel_storage;
#define FAKE_PANEL ((struct recon_panel *)&g_panel_storage)

struct recon_panel *recon_server_window_panel(struct recon_server *server,
        int width, int height) {
    (void)server; (void)width; (void)height;
    return FAKE_PANEL;
}

struct recon_shell *recon_server_shell(struct recon_server *server) {
    (void)server;
    return NULL;
}

void recon_damage_all(struct recon_server *server) { (void)server; }

/* --- the panel ---------------------------------------------------------- */

void recon_panel_destroy(struct recon_panel *panel) { (void)panel; }
void recon_panel_commit(struct recon_panel *panel) { (void)panel; }
void recon_panel_raise_to_top(struct recon_panel *panel) { (void)panel; }

void recon_panel_set_enabled(struct recon_panel *panel, bool enabled) {
    (void)panel; (void)enabled;
}

void recon_panel_set_position(struct recon_panel *panel, int x, int y) {
    (void)panel; (void)x; (void)y;
}

bool recon_panel_resize(struct recon_panel *panel, int width, int height) {
    (void)panel; (void)width; (void)height;
    return true;
}

void recon_panel_fade(struct recon_panel *panel, int x, int y, int w, int h,
        uint8_t alpha) {
    (void)panel; (void)x; (void)y; (void)w; (void)h; (void)alpha;
}

bool recon_panel_tip_at(struct recon_panel *panel, double lx, double ly,
        char *out, size_t size) {
    (void)panel; (void)lx; (void)ly;
    if (out != NULL && size > 0) {
        out[0] = '\0';
    }
    return false;
}

/*
 * A screen with room in it.
 *
 * These two are what `center()` divides by, so zero would put every window at
 * a negative offset and the arithmetic under test would be running on a
 * machine with no display. 1280x720 is the size the look harness photographs
 * at, which makes a failure here readable against a picture.
 */
int recon_panel_width(const struct recon_panel *panel) {
    (void)panel;
    return 1280;
}

int recon_panel_height(const struct recon_panel *panel) {
    (void)panel;
    return 720;
}

/* --- drawing ------------------------------------------------------------ */

void recon_fill(struct recon_panel *panel, recon_color color) {
    (void)panel; (void)color;
}

void recon_fill_role(struct recon_panel *panel, int x, int y, int w, int h,
        enum recon_theme_role role) {
    (void)panel; (void)x; (void)y; (void)w; (void)h; (void)role;
}

void recon_stroke_rect(struct recon_panel *panel, int x, int y, int w, int h,
        recon_color color) {
    (void)panel; (void)x; (void)y; (void)w; (void)h; (void)color;
}

void recon_draw_bevel(struct recon_panel *panel, int x, int y, int w, int h,
        bool pressed) {
    (void)panel; (void)x; (void)y; (void)w; (void)h; (void)pressed;
}

void recon_draw_text(struct recon_panel *panel, struct recon_font *font,
        int x, int y, int max_width, const char *text, recon_color color) {
    (void)panel; (void)font; (void)x; (void)y; (void)max_width; (void)text;
    (void)color;
}

void recon_round_top_corners(struct recon_panel *panel, int radius,
        recon_color edge) {
    (void)panel; (void)radius; (void)edge;
}

bool recon_icon_draw_in(struct recon_panel *panel, const char *name, int x,
        int y, int size, recon_color ink) {
    (void)panel; (void)name; (void)x; (void)y; (void)size; (void)ink;
    return false;
}

int recon_font_ascent(struct recon_font *font) {
    (void)font;
    return 12;
}

/* --- hit regions -------------------------------------------------------- */

void recon_hit_clear(struct recon_panel *panel) { (void)panel; }

bool recon_hit_add(struct recon_panel *panel, int x, int y, int w, int h,
        uint32_t id) {
    (void)panel; (void)x; (void)y; (void)w; (void)h; (void)id;
    return true;
}

bool recon_hit_region(const struct recon_panel *panel, size_t index, int *x,
        int *y, int *w, int *h, uint32_t *id) {
    (void)panel; (void)index; (void)x; (void)y; (void)w; (void)h; (void)id;
    return false;
}

uint32_t recon_hit_test(struct recon_panel *panel, int x, int y) {
    (void)panel; (void)x; (void)y;
    return 0;
}

uint32_t recon_hit_test_active(struct recon_panel *panel, int x, int y) {
    (void)panel; (void)x; (void)y;
    return 0;
}

/* --- widgets ------------------------------------------------------------ */

enum recon_widget_state recon_widget_caption_button(struct recon_panel *panel,
        int x, int y, int size, uint32_t id, enum recon_widget_caption glyph,
        recon_color behind, const char *tip) {
    (void)panel; (void)x; (void)y; (void)size; (void)id; (void)glyph;
    (void)behind; (void)tip;
    return RECON_WIDGET_NORMAL;
}

uint32_t recon_widget_held_id(const struct recon_panel *panel) {
    (void)panel;
    return 0;
}

bool recon_widget_hover(struct recon_panel *panel, uint32_t id) {
    (void)panel; (void)id;
    return false;
}

bool recon_widget_press(struct recon_panel *panel, uint32_t id) {
    (void)panel; (void)id;
    return false;
}

bool recon_widget_release(struct recon_panel *panel) {
    (void)panel;
    return false;
}

/* --- the shell ---------------------------------------------------------- */

void recon_shell_refresh(struct recon_shell *shell) { (void)shell; }
void recon_shell_contents_changed(struct recon_shell *shell) { (void)shell; }

void recon_shell_ask(struct recon_shell *shell, const char *title,
        const char *message, const char *const *buttons, int button_count,
        recon_answer_fn answer, void *user) {
    (void)shell; (void)title; (void)message; (void)buttons; (void)button_count;
    (void)answer; (void)user;
}

/* --- the rest ----------------------------------------------------------- */

/*
 * The registry answers with the fallback, which is what an account that has
 * never moved this window would get. So every window here opens at its
 * default size in its default place -- the state a transition test wants,
 * since a remembered geometry is a second thing deciding what happens.
 */
bool recon_registry_get_bool(enum recon_registry_scope scope, const char *key,
        bool fallback) {
    (void)scope; (void)key;
    return fallback;
}

int recon_registry_get_int(enum recon_registry_scope scope, const char *key,
        int fallback) {
    (void)scope; (void)key;
    return fallback;
}

bool recon_registry_set_bool(enum recon_registry_scope scope, const char *key,
        bool value) {
    (void)scope; (void)key; (void)value;
    return true;
}

bool recon_registry_set_int(enum recon_registry_scope scope, const char *key,
        int value) {
    (void)scope; (void)key; (void)value;
    return true;
}

recon_color recon_theme_color(enum recon_theme_role role) {
    (void)role;
    return 0xFF000000u;
}

int recon_theme_metric(enum recon_theme_metric metric) {
    (void)metric;
    return 0;
}

void recon_titlebar_layout(int width, int inset, int gap, bool with_icon,
        struct recon_titlebar_layout *out) {
    (void)width; (void)inset; (void)gap; (void)with_icon;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

/*
 * Every help topic exists.
 *
 * `recon_appwin_create` checks the one an impl declares and raises a fault for
 * one that does not, and a suite whose windows all declared a missing page
 * would be measuring the fault path on every single test.
 */
bool recon_help_topic_exists(const char *title) {
    (void)title;
    return true;
}

void recon_error_raisef(struct recon_server *server,
        enum recon_error_code code, const char *format, ...) {
    (void)server; (void)code; (void)format;
}
