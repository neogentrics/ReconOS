/*
 * ReconOS desktop shell: taskbar and apps menu. See include/recon_shell.h.
 *
 * Both are panels the compositor draws into directly, so the whole shell costs
 * two textures regardless of how many buttons are on it, and nothing is
 * redrawn unless something actually changed.
 */

#define _POSIX_C_SOURCE 200112L

#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "recon_server.h"
#include "recon_shell.h"
#include "recon_appwin.h"
#include "recon_calc.h"
#include "recon_desktop.h"
#include "recon_explorer.h"
#include "recon_notepad.h"
#include "recon_terminal.h"
#include "recon_taskmgr.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_ui.h"

/* --- Look --- */

#define TASKBAR_HEIGHT 34
#define TASKBAR_PADDING 3
#define BUTTON_HEIGHT (TASKBAR_HEIGHT - TASKBAR_PADDING * 2)
#define APPS_BUTTON_WIDTH 74
#define TASK_BUTTON_MAX_WIDTH 180
#define TASK_BUTTON_MIN_WIDTH 60
#define TEXT_INSET 8

#define MENU_ITEM_HEIGHT 28
#define MENU_WIDTH 190
#define MENU_PADDING 4

#define FONT_HEIGHT 14

/* The Ctrl+Alt+Del box. */
#define SEC_WIDTH 300
#define SEC_TITLE_HEIGHT 24
#define SEC_ITEM_HEIGHT 30
#define SEC_PADDING 8
#define SEC_BUTTON_HEIGHT 26

/*
 * The one place the shell's colours are defined. A skin is a different set of
 * these, which is why nothing below uses a literal.
 */
#define COLOR_BAR RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_TEXT RECON_RGB(0x10, 0x10, 0x10)
#define COLOR_TEXT_DIM RECON_RGB(0x40, 0x40, 0x40)
#define COLOR_BUTTON RECON_RGB(0xC8, 0xC8, 0xC8)
#define COLOR_BUTTON_ACTIVE RECON_RGB(0xA8, 0xA8, 0xB4)
#define COLOR_MENU RECON_RGB(0xC8, 0xC8, 0xC8)
#define COLOR_MENU_BORDER RECON_RGB(0x30, 0x30, 0x30)
#define COLOR_ACCENT RECON_RGB(0x8B, 0x1A, 0x1A)
#define COLOR_DIALOG_TITLE RECON_RGB(0x20, 0x2A, 0x44)
#define COLOR_DIALOG_TITLE_TEXT RECON_RGB(0xF0, 0xF0, 0xF0)
/* What the pointer is over, in a menu. */
#define COLOR_MENU_HILITE RECON_RGB(0x30, 0x50, 0x90)
#define COLOR_MENU_HILITE_TEXT RECON_RGB(0xFF, 0xFF, 0xFF)
/* Half-transparent black. Alpha is what makes the desktop show through. */
#define COLOR_DIM RECON_RGBA(0x00, 0x00, 0x00, 0x99)

/* Hit-region ids. Window buttons use TASK_BASE + index. */
#define HIT_APPS_BUTTON 1
#define HIT_TASK_BASE 100
#define HIT_MENU_BASE 200
#define HIT_SEC_BASE 300
#define HIT_CONTEXT_BASE 400

/* What a context menu entry does. */
enum context_action {
    CTX_OPEN,
    CTX_DELETE,
    CTX_NEW_FOLDER,
    CTX_NEW_SHORTCUT,
    CTX_REFRESH,
    CTX_RESTORE,
    CTX_MINIMIZE,
    CTX_MAXIMIZE,
    CTX_CLOSE,
    CTX_PROPERTIES,
};

/* The context menu. */
#define CONTEXT_ITEM_HEIGHT 24
#define CONTEXT_WIDTH 200
#define CONTEXT_PADDING 3
#define CONTEXT_ITEMS_MAX 10

/* --- Apps menu contents --- */

enum recon_app_action {
    RECON_APP_LAUNCH,
    RECON_APP_TASKMGR,
    RECON_APP_CALC,
    RECON_APP_NOTEPAD,
    RECON_APP_TERMINAL,
    RECON_APP_EXPLORER,
    RECON_APP_QUIT,
};

struct recon_app_entry {
    const char *label;
    const char *command; /* NULL means the configured terminal */
    enum recon_app_action action;
    const char *icon;
};

/*
 * Hardcoded for now. Reading .desktop files off the system would work, but
 * ReconOS is meant to present its own applications rather than inherit
 * whatever a host distribution happens to have installed.
 */
static const struct recon_app_entry APPS[] = {
    { "File Explorer", NULL, RECON_APP_EXPLORER, RECON_ICON_EXPLORER },
    { "Terminal", NULL, RECON_APP_TERMINAL, RECON_ICON_TERMINAL },
    { "Notepad", NULL, RECON_APP_NOTEPAD, RECON_ICON_NOTEPAD },
    { "Calculator", NULL, RECON_APP_CALC, RECON_ICON_CALCULATOR },
    { "Task Manager", NULL, RECON_APP_TASKMGR, RECON_ICON_TASKMGR },
    { "Shut Down", NULL, RECON_APP_QUIT, "shutdown" },
};

/* What the Ctrl+Alt+Del box offers, in the order it offers it. */
enum sec_action {
    SEC_TASKMGR,
    SEC_SHUTDOWN,
    SEC_CANCEL,
};

static const char *const SEC_ITEMS[] = {
    "Task Manager",
    "Shut Down",
    "Cancel",
};

#define SEC_COUNT ((int)(sizeof(SEC_ITEMS) / sizeof(SEC_ITEMS[0])))

#define APP_COUNT ((int)(sizeof(APPS) / sizeof(APPS[0])))

/* --- Shell --- */

struct recon_shell {
    struct recon_server *server;
    struct recon_font *font;

    struct recon_desktop *desktop;
    struct recon_panel *taskbar;
    struct recon_panel *menu;
    bool menu_open;
    /*
     * The entry the pointer is over, or -1. A menu that does not show what is
     * about to be chosen makes the user aim and hope; this is what turns a
     * list into something you can read before committing to it.
     */
    int menu_hover;
    int context_hover;
    int security_hover;

    /* Built-in windows. Listed on the taskbar beside client windows, and
     * offered input in front-to-back order. */
    struct recon_appwin *apps[8];
    int app_count;
    /*
     * Indices into apps[], front-most first. Input must be offered in the
     * order things are drawn, not the order they were created, or a window
     * underneath answers for the one on top of it.
     */
    int app_order[8];
    /*
     * Which built-in window holds focus, or -1 for none. Exactly one window
     * may hold it, which is a fact about all of them and so cannot be left to
     * each one separately: every window believing itself focused is how they
     * all drew active title bars and all claimed the keyboard.
     */
    int focused_app;

    /*
     * The right-click menu. One panel serves every use of it -- desktop,
     * explorer, taskbar -- because a context menu is the same thing wherever
     * it appears, differing only in what it offers.
     */
    struct recon_panel *context;
    bool context_open;
    struct {
        char label[48];
        uint32_t id;
        bool enabled;
        bool separator_after;
    } context_items[CONTEXT_ITEMS_MAX];
    int context_item_count;
    enum recon_context_kind context_kind;
    char context_target[RECON_PATH_MAX];

    struct recon_panel *security;
    /* Dims the desktop behind the security box, so it is obvious that the
     * question wants answering before anything else happens. */
    struct recon_panel *dim;
    bool security_open;
    struct recon_appwin *taskmgr;

    int screen_width, screen_height;

    /*
     * Windows in the order their buttons are drawn. The taskbar hit region
     * carries an index into this, so a click resolves to a window without
     * depending on focus order, which changes as soon as the click lands.
     */
    struct taskbar_button {
        struct recon_toplevel *toplevel; /* one of these is set */
        struct recon_appwin *appwin;
    } buttons[32];
    int button_count;
    /* Which button a taskbar context menu was opened on. */
    int context_button;
    /* Which built-in window a window context menu was opened on. */
    int context_app;
};

static int menu_height(void) {
    return APP_COUNT * MENU_ITEM_HEIGHT + MENU_PADDING * 2;
}

static int security_height(void) {
    return SEC_TITLE_HEIGHT + SEC_PADDING * 3 +
        SEC_COUNT * (SEC_BUTTON_HEIGHT + SEC_PADDING) + 20;
}

/*
 * Give focus to one built-in window, taking it from every other.
 *
 * Pass -1 when focus goes elsewhere -- to a client window, or nowhere.
 */
static void set_focused_app(struct recon_shell *shell, int index) {
    shell->focused_app = index;
    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_set_focused(shell->apps[i], i == index);
    }
}

/* --- Context menu --- */

/* Defined with the other input helpers, further down. */
static bool point_in_panel(struct recon_panel *panel, double lx, double ly,
    int *px, int *py);
static void draw_menu(struct recon_shell *shell);
static void draw_security(struct recon_shell *shell);

static int context_height(struct recon_shell *shell) {
    int height = CONTEXT_PADDING * 2;
    for (int i = 0; i < shell->context_item_count; i++) {
        height += CONTEXT_ITEM_HEIGHT;
        if (shell->context_items[i].separator_after) {
            height += 5;
        }
    }
    return height;
}

static void draw_context(struct recon_shell *shell) {
    struct recon_panel *p = shell->context;
    if (p == NULL) {
        return;
    }

    int width = recon_panel_width(p);
    int height = recon_panel_height(p);
    int ascent = recon_font_ascent(shell->font);

    recon_fill(p, COLOR_MENU);
    recon_hit_clear(p);

    int y = CONTEXT_PADDING;
    for (int i = 0; i < shell->context_item_count; i++) {
        /* Only entries that can be chosen highlight: showing a disabled one
         * as selectable would promise something the click will not do. */
        bool hovered = (i == shell->context_hover) && shell->context_items[i].enabled;

        if (hovered) {
            recon_fill_rect(p, CONTEXT_PADDING, y, width - CONTEXT_PADDING * 2,
                CONTEXT_ITEM_HEIGHT, COLOR_MENU_HILITE);
        }

        recon_draw_text(p, shell->font, 14, y + (CONTEXT_ITEM_HEIGHT + ascent) / 2 - 2,
            width - 24, shell->context_items[i].label,
            !shell->context_items[i].enabled ? COLOR_TEXT_DIM :
            hovered ? COLOR_MENU_HILITE_TEXT : COLOR_TEXT);

        /* Disabled entries are shown rather than hidden, so the menu keeps the
         * same shape and what is unavailable is visible. */
        if (shell->context_items[i].enabled) {
            recon_hit_add(p, CONTEXT_PADDING, y, width - CONTEXT_PADDING * 2,
                CONTEXT_ITEM_HEIGHT, HIT_CONTEXT_BASE + i);
        }
        y += CONTEXT_ITEM_HEIGHT;

        if (shell->context_items[i].separator_after) {
            recon_fill_rect(p, 6, y + 2, width - 12, 1, RECON_RGB(0x90, 0x90, 0x90));
            y += 5;
        }
    }

    recon_draw_bevel(p, 0, 0, width, height, false);
    recon_stroke_rect(p, 0, 0, width, height, COLOR_MENU_BORDER);
    recon_panel_commit(p);
}

static void context_add(struct recon_shell *shell, const char *label,
        enum context_action action, bool enabled, bool separator) {
    if (shell->context_item_count >= CONTEXT_ITEMS_MAX) {
        return;
    }
    int i = shell->context_item_count++;
    snprintf(shell->context_items[i].label, sizeof(shell->context_items[i].label),
        "%s", label);
    shell->context_items[i].id = (uint32_t)action;
    shell->context_items[i].enabled = enabled;
    shell->context_items[i].separator_after = separator;
}

/* Show the menu at a point, kept on screen if it would run off an edge. */
static void context_show(struct recon_shell *shell, double lx, double ly) {
    if (shell->context == NULL || shell->context_item_count == 0) {
        return;
    }

    int height = context_height(shell);
    recon_panel_resize(shell->context, CONTEXT_WIDTH, height);

    int x = (int)lx;
    int y = (int)ly;
    if (x + CONTEXT_WIDTH > shell->screen_width) {
        x = shell->screen_width - CONTEXT_WIDTH;
    }
    if (y + height > shell->screen_height - TASKBAR_HEIGHT) {
        /* Above the pointer rather than below, so it neither runs off the
         * bottom nor hides behind the taskbar. */
        y -= height;
    }
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }

    recon_panel_set_position(shell->context, x, y);
    draw_context(shell);
    recon_panel_set_enabled(shell->context, true);
    recon_panel_raise_to_top(shell->context);
    shell->context_hover = -1;
    shell->context_open = true;
    recon_damage_all(shell->server);
}

void recon_shell_close_context(struct recon_shell *shell) {
    if (shell == NULL || !shell->context_open) {
        return;
    }
    shell->context_open = false;
    recon_panel_set_enabled(shell->context, false);
    recon_damage_all(shell->server);
}

/* Move a window to the front of the input order. */
static void raise_app_order(struct recon_shell *shell, int index) {
    int position = -1;
    for (int i = 0; i < shell->app_count; i++) {
        if (shell->app_order[i] == index) {
            position = i;
            break;
        }
    }
    if (position <= 0) {
        return;
    }
    for (int i = position; i > 0; i--) {
        shell->app_order[i] = shell->app_order[i - 1];
    }
    shell->app_order[0] = index;
}

/*
 * The topmost scene node at this point.
 *
 * The scene graph is the only thing that knows what is actually drawn on top.
 * Asking whether a point falls inside a window is not the same question: a
 * maximized window contains every point on screen, so it would claim clicks
 * meant for windows stacked above it.
 */
static struct wlr_scene_node *topmost_node(struct recon_shell *shell,
        double lx, double ly) {
    double sx, sy;
    return wlr_scene_node_at(&shell->server->scene->tree.node, lx, ly, &sx, &sy);
}

/* The built-in window that owns a node, if any. */
static int appwin_index_for_node(struct recon_shell *shell,
        struct wlr_scene_node *node) {
    if (node == NULL) {
        return -1;
    }
    for (int i = 0; i < shell->app_count; i++) {
        if (recon_appwin_node(shell->apps[i]) == node) {
            return i;
        }
    }
    return -1;
}

/* --- Drawing --- */

/*
 * One taskbar button. A minimized window is drawn recessed and its label
 * dimmed, so the bar shows at a glance what is on screen and what is put away.
 */
static void draw_task_button(struct recon_shell *shell, struct recon_panel *bar,
        int x, int w, int baseline, const char *title, const char *icon,
        bool active, bool minimized) {
    recon_color fill = active ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
    if (minimized) {
        fill = COLOR_BAR;
    }

    recon_fill_rect(bar, x, TASKBAR_PADDING, w, BUTTON_HEIGHT, fill);
    recon_draw_bevel(bar, x, TASKBAR_PADDING, w, BUTTON_HEIGHT, active || minimized);

    /* The icon comes first, so a bar of buttons can be read at a glance
     * without depending on the titles fitting. */
    int text_x = x + TEXT_INSET;
    int icon_size = BUTTON_HEIGHT - 10;
    if (icon != NULL &&
            recon_icon_draw(bar, icon, x + 5, TASKBAR_PADDING + 5, icon_size)) {
        text_x = x + 5 + icon_size + 5;
    }

    recon_draw_text(bar, shell->font, text_x, baseline,
        w - (text_x - x) - TEXT_INSET, title,
        active ? COLOR_TEXT : COLOR_TEXT_DIM);
}

static void draw_taskbar(struct recon_shell *shell) {
    struct recon_panel *bar = shell->taskbar;
    if (bar == NULL) {
        return;
    }

    int width = recon_panel_width(bar);
    int baseline = TASKBAR_PADDING + (BUTTON_HEIGHT + recon_font_ascent(shell->font)) / 2 - 2;

    recon_fill(bar, COLOR_BAR);
    /* A highlight along the top edge lifts the bar off the wallpaper. */
    recon_fill_rect(bar, 0, 0, width, 1, RECON_RGB(0xE8, 0xE8, 0xE8));

    recon_hit_clear(bar);

    /* Apps button. */
    recon_fill_rect(bar, TASKBAR_PADDING, TASKBAR_PADDING,
        APPS_BUTTON_WIDTH, BUTTON_HEIGHT,
        shell->menu_open ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
    recon_draw_bevel(bar, TASKBAR_PADDING, TASKBAR_PADDING,
        APPS_BUTTON_WIDTH, BUTTON_HEIGHT, shell->menu_open);

    /* A mark so the button reads as the system menu rather than a window. */
    int mark_size = BUTTON_HEIGHT - 10;
    if (!recon_icon_draw(bar, "system", TASKBAR_PADDING + 6, TASKBAR_PADDING + 5,
            mark_size)) {
        recon_fill_rect(bar, TASKBAR_PADDING + TEXT_INSET, TASKBAR_PADDING + 9,
            10, 10, COLOR_ACCENT);
    }
    recon_draw_text(bar, shell->font, TASKBAR_PADDING + 6 + mark_size + 6, baseline,
        APPS_BUTTON_WIDTH - mark_size - 18, "Apps", COLOR_TEXT);
    recon_hit_add(bar, TASKBAR_PADDING, TASKBAR_PADDING,
        APPS_BUTTON_WIDTH, BUTTON_HEIGHT, HIT_APPS_BUTTON);

    /* One button per window, client or built-in, sharing the remaining
     * width. Both kinds appear here, because from the user's side there is no
     * difference between them. */
    shell->button_count = 0;
    int x = TASKBAR_PADDING * 2 + APPS_BUTTON_WIDTH;
    int available = width - x - TASKBAR_PADDING;

    int window_count = wl_list_length(&shell->server->toplevels);
    for (int i = 0; i < shell->app_count; i++) {
        if (recon_appwin_is_open(shell->apps[i])) {
            window_count++;
        }
    }

    if (window_count <= 0 || available < TASK_BUTTON_MIN_WIDTH) {
        recon_panel_commit(bar);
        return;
    }

    int button_width = (available / window_count) - TASKBAR_PADDING;
    if (button_width > TASK_BUTTON_MAX_WIDTH) {
        button_width = TASK_BUTTON_MAX_WIDTH;
    }
    if (button_width < TASK_BUTTON_MIN_WIDTH) {
        button_width = TASK_BUTTON_MIN_WIDTH;
    }

    int max_buttons = (int)(sizeof(shell->buttons) / sizeof(shell->buttons[0]));

    /* Built-in windows first, so their position does not shift as client
     * windows come and go. */
    for (int i = 0; i < shell->app_count; i++) {
        struct recon_appwin *win = shell->apps[i];
        if (!recon_appwin_is_open(win) || shell->button_count >= max_buttons) {
            continue;
        }
        if (x + button_width > width - TASKBAR_PADDING) {
            break;
        }

        bool active = recon_appwin_is_focused(win);
        draw_task_button(shell, bar, x, button_width, baseline,
            recon_appwin_title(win), recon_appwin_icon(win),
            active, recon_appwin_is_minimized(win));

        recon_hit_add(bar, x, TASKBAR_PADDING, button_width, BUTTON_HEIGHT,
            HIT_TASK_BASE + shell->button_count);
        shell->buttons[shell->button_count].toplevel = NULL;
        shell->buttons[shell->button_count].appwin = win;
        shell->button_count++;
        x += button_width + TASKBAR_PADDING;
    }

    struct recon_toplevel *toplevel;
    wl_list_for_each(toplevel, &shell->server->toplevels, link) {
        if (shell->button_count >= max_buttons) {
            break;
        }
        if (x + button_width > width - TASKBAR_PADDING) {
            break;
        }

        /* A client window has no icon of its own, so it gets the generic
         * application one rather than nothing. */
        draw_task_button(shell, bar, x, button_width, baseline,
            recon_toplevel_title(toplevel), RECON_ICON_APP,
            recon_toplevel_is_focused(toplevel),
            recon_toplevel_is_minimized(toplevel));

        recon_hit_add(bar, x, TASKBAR_PADDING, button_width, BUTTON_HEIGHT,
            HIT_TASK_BASE + shell->button_count);
        shell->buttons[shell->button_count].toplevel = toplevel;
        shell->buttons[shell->button_count].appwin = NULL;
        shell->button_count++;
        x += button_width + TASKBAR_PADDING;
    }

    recon_panel_commit(bar);
}

static void draw_menu(struct recon_shell *shell) {
    struct recon_panel *menu = shell->menu;
    if (menu == NULL) {
        return;
    }

    int width = recon_panel_width(menu);
    int height = recon_panel_height(menu);

    recon_fill(menu, COLOR_MENU);
    recon_stroke_rect(menu, 0, 0, width, height, COLOR_MENU_BORDER);
    recon_draw_bevel(menu, 1, 1, width - 2, height - 2, false);

    recon_hit_clear(menu);

    int ascent = recon_font_ascent(shell->font);
    for (int i = 0; i < APP_COUNT; i++) {
        int y = MENU_PADDING + i * MENU_ITEM_HEIGHT;
        int baseline = y + (MENU_ITEM_HEIGHT + ascent) / 2 - 2;
        bool hovered = (i == shell->menu_hover);

        if (hovered) {
            recon_fill_rect(menu, MENU_PADDING, y, width - MENU_PADDING * 2,
                MENU_ITEM_HEIGHT, COLOR_MENU_HILITE);
        }

        int label_x = MENU_PADDING + TEXT_INSET;
        int icon_size = MENU_ITEM_HEIGHT - 8;
        if (recon_icon_draw(menu, APPS[i].icon, MENU_PADDING + 6, y + 4, icon_size)) {
            label_x = MENU_PADDING + 6 + icon_size + 8;
        }
        recon_draw_text(menu, shell->font, label_x, baseline,
            width - label_x - MENU_PADDING, APPS[i].label,
            hovered ? COLOR_MENU_HILITE_TEXT : COLOR_TEXT);
        recon_hit_add(menu, MENU_PADDING, y, width - MENU_PADDING * 2,
            MENU_ITEM_HEIGHT, HIT_MENU_BASE + i);
    }

    recon_panel_commit(menu);
}

static void draw_security(struct recon_shell *shell) {
    struct recon_panel *p = shell->security;
    if (p == NULL) {
        return;
    }

    int width = recon_panel_width(p);
    int height = recon_panel_height(p);
    int ascent = recon_font_ascent(shell->font);

    recon_fill(p, COLOR_MENU);
    recon_hit_clear(p);

    recon_fill_rect(p, 0, 0, width, SEC_TITLE_HEIGHT, COLOR_DIALOG_TITLE);
    recon_draw_text(p, shell->font, SEC_PADDING,
        (SEC_TITLE_HEIGHT + ascent) / 2 - 1, width - SEC_PADDING * 2,
        "ReconOS Security", COLOR_DIALOG_TITLE_TEXT);

    recon_draw_text(p, shell->font, SEC_PADDING,
        SEC_TITLE_HEIGHT + SEC_PADDING + ascent,
        width - SEC_PADDING * 2, "What would you like to do?", COLOR_TEXT);

    int y = SEC_TITLE_HEIGHT + SEC_PADDING * 2 + 20;
    for (int i = 0; i < SEC_COUNT; i++) {
        int bw = width - SEC_PADDING * 2;
        bool hovered = (i == shell->security_hover);

        recon_fill_rect(p, SEC_PADDING, y, bw, SEC_BUTTON_HEIGHT,
            hovered ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
        recon_draw_bevel(p, SEC_PADDING, y, bw, SEC_BUTTON_HEIGHT, false);
        recon_draw_text(p, shell->font, SEC_PADDING + 12,
            y + (SEC_BUTTON_HEIGHT + ascent) / 2 - 2, bw - 24,
            SEC_ITEMS[i], COLOR_TEXT);
        recon_hit_add(p, SEC_PADDING, y, bw, SEC_BUTTON_HEIGHT, HIT_SEC_BASE + i);
        y += SEC_BUTTON_HEIGHT + SEC_PADDING;
    }

    recon_draw_bevel(p, 0, 0, width, height, false);
    recon_stroke_rect(p, 0, 0, width, height, COLOR_MENU_BORDER);
    recon_panel_commit(p);
}

/* --- Layout --- */

static void layout(struct recon_shell *shell) {
    if (shell->taskbar != NULL) {
        recon_panel_resize(shell->taskbar, shell->screen_width, TASKBAR_HEIGHT);
        recon_panel_set_position(shell->taskbar, 0,
            shell->screen_height - TASKBAR_HEIGHT);
    }
    if (shell->menu != NULL) {
        recon_panel_set_position(shell->menu, TASKBAR_PADDING,
            shell->screen_height - TASKBAR_HEIGHT - menu_height());
    }
    if (shell->dim != NULL) {
        recon_panel_resize(shell->dim, shell->screen_width, shell->screen_height);
        recon_panel_set_position(shell->dim, 0, 0);
        recon_fill(shell->dim, COLOR_DIM);
        recon_panel_commit(shell->dim);
    }
    if (shell->security != NULL) {
        /* Centred: it is a question that interrupts, not a corner notice. */
        recon_panel_set_position(shell->security,
            (shell->screen_width - SEC_WIDTH) / 2,
            (shell->screen_height - security_height()) / 2);
    }
    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_screen_changed(shell->apps[i], shell->screen_width,
            shell->screen_height, TASKBAR_HEIGHT);
    }
    recon_desktop_resize(shell->desktop, shell->screen_width,
        shell->screen_height - TASKBAR_HEIGHT);
}

/* --- Lifecycle --- */

struct recon_shell *recon_shell_create(struct recon_server *server,
        int screen_width, int screen_height) {
    struct recon_shell *shell = calloc(1, sizeof(*shell));
    if (shell == NULL) {
        return NULL;
    }

    shell->server = server;
    shell->focused_app = -1;
    shell->screen_width = screen_width;
    shell->screen_height = screen_height;

    /* No font means no labels, but the taskbar still works as buttons. */
    shell->font = recon_font_load(getenv("RECONOS_FONT"), FONT_HEIGHT);

    shell->taskbar = recon_panel_create(&server->scene->tree,
        screen_width, TASKBAR_HEIGHT);
    if (shell->taskbar == NULL) {
        wlr_log(WLR_ERROR, "ReconOS: could not create the taskbar");
        recon_font_destroy(shell->font);
        free(shell);
        return NULL;
    }

    shell->menu = recon_panel_create(&server->scene->tree, MENU_WIDTH, menu_height());
    if (shell->menu != NULL) {
        recon_panel_set_enabled(shell->menu, false);
        draw_menu(shell);
    }

    shell->dim = recon_panel_create(&server->scene->tree,
        screen_width, screen_height);
    if (shell->dim != NULL) {
        recon_panel_set_enabled(shell->dim, false);
    }

    shell->context = recon_panel_create(&server->scene->tree,
        CONTEXT_WIDTH, CONTEXT_ITEM_HEIGHT * CONTEXT_ITEMS_MAX);
    if (shell->context != NULL) {
        recon_panel_set_enabled(shell->context, false);
    }

    shell->security = recon_panel_create(&server->scene->tree,
        SEC_WIDTH, security_height());
    if (shell->security != NULL) {
        recon_panel_set_enabled(shell->security, false);
        draw_security(shell);
    }

    shell->taskmgr = recon_taskmgr_create(server, shell->font);
    if (shell->taskmgr != NULL) {
        shell->app_order[shell->app_count] = shell->app_count;
        shell->apps[shell->app_count++] = shell->taskmgr;
    }

    struct recon_appwin *calc = recon_calc_create(server, shell->font);
    if (calc != NULL) {
        shell->app_order[shell->app_count] = shell->app_count;
        shell->apps[shell->app_count++] = calc;
    }

    struct recon_appwin *notepad = recon_notepad_create(server, shell->font);
    if (notepad != NULL) {
        shell->app_order[shell->app_count] = shell->app_count;
        shell->apps[shell->app_count++] = notepad;
    }

    struct recon_appwin *terminal = recon_terminal_create(server, shell->font);
    if (terminal != NULL) {
        shell->app_order[shell->app_count] = shell->app_count;
        shell->apps[shell->app_count++] = terminal;
    }

    struct recon_appwin *explorer = recon_explorer_create(server, shell->font);
    if (explorer != NULL) {
        shell->app_order[shell->app_count] = shell->app_count;
        shell->apps[shell->app_count++] = explorer;
    }

    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_screen_changed(shell->apps[i], screen_width, screen_height,
            TASKBAR_HEIGHT);
    }

    /* Shortcuts for the native applications, written on first run only, so
     * removing one stays removed. */
    static const struct { const char *file; const char *target; } DEFAULTS[] = {
        { "File Explorer.app", "File Explorer" },
        { "Terminal.app", "ReconOS Terminal" },
        { "Notepad.app", "Notepad" },
    };
    char marker[RECON_PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/.desktop-set-up", recon_fs_user_dir(NULL));
    if (!recon_fs_exists("/", marker)) {
        for (size_t i = 0; i < sizeof(DEFAULTS) / sizeof(DEFAULTS[0]); i++) {
            char path[RECON_PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", recon_fs_user_dir("Desktop"),
                DEFAULTS[i].file);
            char body[RECON_NAME_MAX + 2];
            int length = snprintf(body, sizeof(body), "%s\n", DEFAULTS[i].target);
            recon_fs_write("/", path, body, (size_t)length);
        }
        recon_fs_write("/", RECON_DIR_SYSTEM_CONFIG "/desktop-initialized", "1\n", 2);
    }

    shell->context_button = -1;
    shell->context_app = -1;
    shell->menu_hover = -1;
    shell->context_hover = -1;
    shell->security_hover = -1;
    shell->desktop = recon_desktop_create(server, shell->font,
        screen_width, screen_height - TASKBAR_HEIGHT);

    layout(shell);
    draw_taskbar(shell);

    /* Debug aid: open and maximize a window at startup, so the state that
     * shows the rendering fault can be reached without a person clicking. */
    const char *autostart = getenv("RECONOS_DEBUG_AUTOSTART");
    if (autostart != NULL && strcmp(autostart, "taskmgr-max") == 0 &&
            shell->taskmgr != NULL) {
        recon_appwin_show(shell->taskmgr);
        recon_appwin_focus(shell->taskmgr);
        recon_appwin_set_maximized(shell->taskmgr, true);
    }

    wlr_log(WLR_INFO, "ReconOS: shell up, taskbar %dx%d",
        screen_width, TASKBAR_HEIGHT);
    return shell;
}

void recon_shell_destroy(struct recon_shell *shell) {
    if (shell == NULL) {
        return;
    }
    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_destroy(shell->apps[i]);
    }
    recon_desktop_destroy(shell->desktop);
    recon_panel_destroy(shell->context);
    recon_panel_destroy(shell->security);
    recon_panel_destroy(shell->dim);
    recon_panel_destroy(shell->menu);
    recon_panel_destroy(shell->taskbar);
    recon_font_destroy(shell->font);
    free(shell);
}

void recon_shell_resize(struct recon_shell *shell, int screen_width, int screen_height) {
    if (shell == NULL) {
        return;
    }
    shell->screen_width = screen_width;
    shell->screen_height = screen_height;
    layout(shell);
    draw_taskbar(shell);
}

void recon_shell_refresh(struct recon_shell *shell) {
    if (shell != NULL) {
        recon_desktop_reload(shell->desktop);
        draw_taskbar(shell);
        recon_damage_all(shell->server);
    }
}

int recon_shell_reserved_bottom(struct recon_shell *shell) {
    return shell != NULL ? TASKBAR_HEIGHT : 0;
}

void recon_shell_raise(struct recon_shell *shell) {
    if (shell == NULL) {
        return;
    }
    /* The desktop stays at the bottom, above only the wallpaper. */
    recon_desktop_lower(shell->desktop, shell->server->background_buffer != NULL
        ? &shell->server->background_buffer->node
        : (shell->server->background_rect != NULL
            ? &shell->server->background_rect->node : NULL));
    recon_panel_raise_to_top(shell->taskbar);
    if (shell->menu_open) {
        recon_panel_raise_to_top(shell->menu);
    }
    /* The task manager and the security box sit above everything, including
     * the taskbar: they are how you regain control when something else has
     * taken over the screen. */
    /*
     * Built-in windows are deliberately not raised here. They are ordinary
     * windows and should stack by use like any other; raising them whenever
     * the shell was raised pinned them above everything, so a client window
     * could never be brought in front of the task manager.
     */
    if (shell->security_open) {
        recon_panel_raise_to_top(shell->dim);
        recon_panel_raise_to_top(shell->security);
    }
}

void recon_shell_open_taskmgr(struct recon_shell *shell) {
    if (shell == NULL) {
        return;
    }
    recon_shell_close_menu(shell);
    raise_app_order(shell, 0);
    set_focused_app(shell, 0);
    recon_appwin_show(shell->taskmgr);
    recon_appwin_focus(shell->taskmgr);
    recon_shell_refresh(shell);
}

int recon_shell_app_count(struct recon_shell *shell) {
    return shell != NULL ? shell->app_count : 0;
}

struct recon_appwin *recon_shell_app_at(struct recon_shell *shell, int index) {
    if (shell == NULL || index < 0 || index >= shell->app_count) {
        return NULL;
    }
    return shell->apps[index];
}

const char *recon_shell_icon_for_app(struct recon_shell *shell, const char *title) {
    if (shell == NULL || title == NULL) {
        return NULL;
    }
    for (int i = 0; i < shell->app_count; i++) {
        if (strcmp(recon_appwin_title(shell->apps[i]), title) == 0) {
            return recon_appwin_icon(shell->apps[i]);
        }
    }
    return NULL;
}

void recon_shell_open_app(struct recon_shell *shell, int index) {
    if (shell == NULL || index < 0 || index >= shell->app_count) {
        return;
    }
    recon_shell_close_menu(shell);
    raise_app_order(shell, index);
    set_focused_app(shell, index);
    recon_appwin_show(shell->apps[index]);
    recon_appwin_focus(shell->apps[index]);
    recon_shell_refresh(shell);
}

/* Open whatever a desktop item points at. */
static void open_desktop_item(struct recon_shell *shell, const char *name) {
    struct recon_desktop_action action;
    if (!recon_desktop_action_for(shell->desktop, name, &action)) {
        return;
    }

    if (action.kind == RECON_DESKTOP_ACTION_OPEN_APP) {
        for (int i = 0; i < shell->app_count; i++) {
            if (strcmp(recon_appwin_title(shell->apps[i]), action.target) == 0) {
                recon_shell_open_app(shell, i);
                return;
            }
        }
    } else if (action.kind == RECON_DESKTOP_ACTION_OPEN_PATH) {
        recon_shell_open_app(shell, 4); /* the file explorer */
    }
}

/* Carry out what a context menu entry asked for. */
static void context_activate(struct recon_shell *shell, enum context_action action) {
    switch (shell->context_kind) {
    case RECON_CONTEXT_TASKBAR_WINDOW: {
        if (shell->context_button < 0 || shell->context_button >= shell->button_count) {
            return;
        }
        struct taskbar_button *button = &shell->buttons[shell->context_button];

        if (button->appwin != NULL) {
            switch (action) {
            case CTX_RESTORE:
                recon_appwin_restore(button->appwin);
                break;
            case CTX_MINIMIZE:
                recon_appwin_minimize(button->appwin);
                break;
            case CTX_MAXIMIZE:
                recon_appwin_set_maximized(button->appwin,
                    !recon_appwin_is_maximized(button->appwin));
                break;
            case CTX_CLOSE:
                recon_appwin_hide(button->appwin);
                break;
            default:
                break;
            }
        } else if (button->toplevel != NULL) {
            switch (action) {
            case CTX_RESTORE:
                recon_toplevel_restore(button->toplevel);
                break;
            case CTX_MINIMIZE:
                recon_toplevel_minimize(button->toplevel);
                break;
            case CTX_MAXIMIZE:
                recon_toplevel_toggle_maximized(button->toplevel);
                break;
            case CTX_CLOSE:
                recon_toplevel_close(button->toplevel);
                break;
            default:
                break;
            }
        }
        break;
    }

    case RECON_CONTEXT_WINDOW: {
        if (shell->context_app < 0 || shell->context_app >= shell->app_count) {
            return;
        }
        struct recon_appwin *win = shell->apps[shell->context_app];
        switch (action) {
        case CTX_RESTORE:
            recon_appwin_set_maximized(win, false);
            break;
        case CTX_MINIMIZE:
            recon_appwin_minimize(win);
            break;
        case CTX_MAXIMIZE:
            recon_appwin_set_maximized(win, true);
            break;
        case CTX_CLOSE:
            recon_appwin_hide(win);
            break;
        default:
            break;
        }
        break;
    }

    case RECON_CONTEXT_DESKTOP_ITEM:
        if (action == CTX_OPEN) {
            open_desktop_item(shell, shell->context_target);
        } else if (action == CTX_DELETE) {
            recon_desktop_delete(shell->desktop, shell->context_target);
        }
        break;

    case RECON_CONTEXT_DESKTOP:
        if (action == CTX_NEW_FOLDER) {
            recon_desktop_new_folder(shell->desktop);
        } else if (action == CTX_NEW_SHORTCUT) {
            recon_desktop_new_shortcut(shell->desktop);
        } else if (action == CTX_REFRESH) {
            recon_desktop_reload(shell->desktop);
        }
        break;
    }

    recon_shell_refresh(shell);
}

bool recon_shell_handle_right_click(struct recon_shell *shell, double lx, double ly) {
    if (shell == NULL) {
        return false;
    }

    recon_shell_close_menu(shell);
    recon_shell_close_context(shell);
    shell->context_item_count = 0;
    shell->context_target[0] = 0;

    int px, py;

    /* A window button on the taskbar. */
    if (shell->taskbar != NULL && point_in_panel(shell->taskbar, lx, ly, &px, &py)) {
        uint32_t hit = recon_hit_test(shell->taskbar, px, py);
        if (hit < HIT_TASK_BASE || hit >= HIT_MENU_BASE) {
            return false;
        }

        int index = (int)(hit - HIT_TASK_BASE);
        if (index < 0 || index >= shell->button_count) {
            return false;
        }

        shell->context_kind = RECON_CONTEXT_TASKBAR_WINDOW;
        shell->context_button = index;

        struct taskbar_button *button = &shell->buttons[index];
        bool builtin = (button->appwin != NULL);
        bool minimized = builtin
            ? recon_appwin_is_minimized(button->appwin)
            : recon_toplevel_is_minimized(button->toplevel);

        context_add(shell, "Restore", CTX_RESTORE, minimized, false);
        context_add(shell, "Minimize", CTX_MINIMIZE, !minimized, false);
        context_add(shell, "Maximize", CTX_MAXIMIZE, true, true);
        context_add(shell, "Close", CTX_CLOSE, true, false);
        context_show(shell, lx, ly);
        return true;
    }

    struct wlr_scene_node *node = topmost_node(shell, lx, ly);

    /*
     * A window under the pointer offers what can be done to the window. Right
     * click should answer everywhere rather than only in the two places that
     * happen to have something interesting to say.
     */
    int app_index = appwin_index_for_node(shell, node);
    if (app_index >= 0) {
        struct recon_appwin *win = shell->apps[app_index];
        shell->context_kind = RECON_CONTEXT_WINDOW;
        shell->context_app = app_index;

        context_add(shell, "Restore", CTX_RESTORE,
            recon_appwin_is_maximized(win), false);
        context_add(shell, "Minimize", CTX_MINIMIZE, true, false);
        context_add(shell, "Maximize", CTX_MAXIMIZE,
            !recon_appwin_is_maximized(win), true);
        context_add(shell, "Close", CTX_CLOSE, true, false);
        context_show(shell, lx, ly);
        return true;
    }

    /* The desktop, on an icon or on empty space. */
    if (node == recon_desktop_node(shell->desktop)) {
        const char *name = recon_desktop_item_at(shell->desktop, lx, ly);
        if (name != NULL) {
            shell->context_kind = RECON_CONTEXT_DESKTOP_ITEM;
            snprintf(shell->context_target, sizeof(shell->context_target), "%s", name);
            context_add(shell, "Open", CTX_OPEN, true, true);
            context_add(shell, "Delete", CTX_DELETE, true, true);
            /* Shown but unavailable: there is no properties view yet, and
             * hiding it would suggest there never will be. */
            context_add(shell, "Properties", CTX_PROPERTIES, false, false);
        } else {
            shell->context_kind = RECON_CONTEXT_DESKTOP;
            context_add(shell, "New Folder", CTX_NEW_FOLDER, true, false);
            context_add(shell, "New Shortcut", CTX_NEW_SHORTCUT, true, true);
            context_add(shell, "Refresh", CTX_REFRESH, true, false);
        }
        context_show(shell, lx, ly);
        return true;
    }

    return false;
}

bool recon_shell_handle_key(struct recon_shell *shell, uint32_t sym,
        uint32_t modifiers) {
    if (shell == NULL || shell->focused_app < 0) {
        return false;
    }
    /* Only the focused window, so a calculator sitting open in the background
     * cannot swallow digits meant for the notepad. */
    return recon_appwin_handle_key(shell->apps[shell->focused_app], sym, modifiers);
}

void recon_shell_clear_app_focus(struct recon_shell *shell) {
    if (shell != NULL) {
        set_focused_app(shell, -1);
    }
}

bool recon_shell_security_open(struct recon_shell *shell) {
    return shell != NULL && shell->security_open;
}

void recon_shell_toggle_security(struct recon_shell *shell) {
    if (shell == NULL || shell->security == NULL) {
        return;
    }
    shell->security_open = !shell->security_open;
    recon_panel_set_enabled(shell->dim, shell->security_open);
    recon_panel_set_enabled(shell->security, shell->security_open);
    if (shell->security_open) {
        recon_shell_close_menu(shell);
        /* Dim first, then the box on top of it. */
        recon_panel_raise_to_top(shell->dim);
        recon_panel_raise_to_top(shell->security);
    }
    recon_damage_all(shell->server);
}

/*
 * Follow the pointer across whichever menu is open, redrawing only when the
 * highlighted entry actually changes -- moving within one entry should not
 * cost a repaint.
 */
static void update_hover(struct recon_shell *shell, double lx, double ly) {
    int px, py;

    int menu = -1;
    int context = -1;
    int security = -1;

    if (shell->context_open && shell->context != NULL &&
            point_in_panel(shell->context, lx, ly, &px, &py)) {
        uint32_t hit = recon_hit_test(shell->context, px, py);
        if (hit >= HIT_CONTEXT_BASE) {
            context = (int)(hit - HIT_CONTEXT_BASE);
        }
    } else if (shell->menu_open && shell->menu != NULL &&
            point_in_panel(shell->menu, lx, ly, &px, &py)) {
        uint32_t hit = recon_hit_test(shell->menu, px, py);
        if (hit >= HIT_MENU_BASE) {
            menu = (int)(hit - HIT_MENU_BASE);
        }
    } else if (shell->security_open && shell->security != NULL &&
            point_in_panel(shell->security, lx, ly, &px, &py)) {
        uint32_t hit = recon_hit_test(shell->security, px, py);
        if (hit >= HIT_SEC_BASE) {
            security = (int)(hit - HIT_SEC_BASE);
        }
    }

    if (context != shell->context_hover) {
        shell->context_hover = context;
        if (shell->context_open) {
            draw_context(shell);
            recon_damage_all(shell->server);
        }
    }
    if (menu != shell->menu_hover) {
        shell->menu_hover = menu;
        if (shell->menu_open) {
            draw_menu(shell);
            recon_damage_all(shell->server);
        }
    }
    if (security != shell->security_hover) {
        shell->security_hover = security;
        if (shell->security_open) {
            draw_security(shell);
            recon_damage_all(shell->server);
        }
    }
}

void recon_shell_handle_motion(struct recon_shell *shell, double lx, double ly) {
    if (shell == NULL) {
        return;
    }
    update_hover(shell, lx, ly);
    for (int i = 0; i < shell->app_count; i++) {
        recon_appwin_handle_motion(shell->apps[i], lx, ly);
    }
}

const char *recon_shell_cursor_at(struct recon_shell *shell, double lx, double ly) {
    if (shell == NULL) {
        return NULL;
    }
    /* Ask in stacking order, so an edge hidden behind another window does not
     * claim the cursor. */
    for (int i = 0; i < shell->app_count; i++) {
        const char *name =
            recon_appwin_cursor_at(shell->apps[shell->app_order[i]], lx, ly);
        if (name != NULL) {
            return name;
        }
    }
    return NULL;
}

bool recon_shell_handle_scroll(struct recon_shell *shell, double lx, double ly,
        double delta) {
    if (shell == NULL) {
        return false;
    }
    for (int i = 0; i < shell->app_count; i++) {
        if (recon_appwin_handle_scroll(shell->apps[shell->app_order[i]], lx, ly, delta)) {
            return true;
        }
    }
    return false;
}

void recon_shell_close_menu(struct recon_shell *shell) {
    if (shell == NULL || !shell->menu_open) {
        return;
    }
    shell->menu_open = false;
    recon_panel_set_enabled(shell->menu, false);
    draw_taskbar(shell);
    recon_damage_all(shell->server);
}

static void toggle_menu(struct recon_shell *shell) {
    if (shell->menu == NULL) {
        return;
    }
    shell->menu_open = !shell->menu_open;
    recon_panel_set_enabled(shell->menu, shell->menu_open);
    if (shell->menu_open) {
        recon_panel_raise_to_top(shell->menu);
    }
    draw_taskbar(shell);
    recon_damage_all(shell->server);
}

/* --- Input --- */

/* Convert layout coordinates to panel-local, returning false if outside. */
static bool point_in_panel(struct recon_panel *panel, double lx, double ly,
        int *px, int *py) {
    struct wlr_scene_node *node = recon_panel_node(panel);
    if (node == NULL || !node->enabled) {
        return false;
    }

    int local_x = (int)lx - node->x;
    int local_y = (int)ly - node->y;
    if (local_x < 0 || local_y < 0 ||
            local_x >= recon_panel_width(panel) ||
            local_y >= recon_panel_height(panel)) {
        return false;
    }

    *px = local_x;
    *py = local_y;
    return true;
}

bool recon_shell_contains_point(struct recon_shell *shell, double lx, double ly) {
    if (shell == NULL) {
        return false;
    }
    int px, py;
    if (shell->menu_open && shell->menu != NULL &&
            point_in_panel(shell->menu, lx, ly, &px, &py)) {
        return true;
    }
    if (appwin_index_for_node(shell, topmost_node(shell, lx, ly)) >= 0) {
        return true;
    }
    if (shell->security_open && shell->security != NULL &&
            point_in_panel(shell->security, lx, ly, &px, &py)) {
        return true;
    }
    if (shell->menu_open && shell->menu != NULL &&
            point_in_panel(shell->menu, lx, ly, &px, &py)) {
        return true;
    }
    return shell->taskbar != NULL && point_in_panel(shell->taskbar, lx, ly, &px, &py);
}

bool recon_shell_handle_click(struct recon_shell *shell, double lx, double ly,
        bool pressed) {
    if (shell == NULL) {
        return false;
    }

    int px, py;

    /* An open context menu takes the next click wherever it lands: either it
     * chose something, or it dismissed the menu. */
    if (shell->context_open) {
        if (!pressed) {
            return true;
        }
        if (point_in_panel(shell->context, lx, ly, &px, &py)) {
            uint32_t hit = recon_hit_test(shell->context, px, py);
            recon_shell_close_context(shell);
            if (hit >= HIT_CONTEXT_BASE) {
                int index = (int)(hit - HIT_CONTEXT_BASE);
                if (index >= 0 && index < shell->context_item_count) {
                    context_activate(shell,
                        (enum context_action)shell->context_items[index].id);
                }
            }
        } else {
            recon_shell_close_context(shell);
        }
        return true;
    }

    /* The security box is modal in spirit: while it is up it takes the click,
     * wherever the click landed. */
    if (shell->security_open && shell->security != NULL) {
        if (!pressed) {
            return true;
        }
        if (point_in_panel(shell->security, lx, ly, &px, &py)) {
            uint32_t hit = recon_hit_test(shell->security, px, py);
            if (hit >= HIT_SEC_BASE) {
                int index = (int)(hit - HIT_SEC_BASE);
                recon_shell_toggle_security(shell); /* closes it */
                if (index == SEC_TASKMGR) {
                    recon_shell_open_taskmgr(shell);
                } else if (index == SEC_SHUTDOWN) {
                    recon_quit(shell->server);
                }
                return true;
            }
        } else {
            /* Clicking outside dismisses it, like Cancel. */
            recon_shell_toggle_security(shell);
        }
        return true;
    }

    /*
     * Order here must match what is drawn on top of what. The apps menu is
     * raised above every window, so it has to be offered the click before
     * them: a maximized window covers the same pixels, and checking windows
     * first let it swallow clicks meant for the menu.
     */
    /* The menu sits above the bar, so it gets first refusal. */
    if (shell->menu_open && shell->menu != NULL &&
            point_in_panel(shell->menu, lx, ly, &px, &py)) {
        if (!pressed) {
            return true;
        }
        uint32_t hit = recon_hit_test(shell->menu, px, py);
        if (hit >= HIT_MENU_BASE) {
            int index = (int)(hit - HIT_MENU_BASE);
            if (index >= 0 && index < APP_COUNT) {
                /* Close the menu first: quitting never returns here. */
                recon_shell_close_menu(shell);
                if (APPS[index].action == RECON_APP_QUIT) {
                    recon_quit(shell->server);
                } else if (APPS[index].action == RECON_APP_TASKMGR) {
                    recon_shell_open_taskmgr(shell);
                } else if (APPS[index].action == RECON_APP_CALC) {
                    recon_shell_open_app(shell, 1);
                } else if (APPS[index].action == RECON_APP_NOTEPAD) {
                    recon_shell_open_app(shell, 2);
                } else if (APPS[index].action == RECON_APP_TERMINAL) {
                    recon_shell_open_app(shell, 3);
                } else if (APPS[index].action == RECON_APP_EXPLORER) {
                    recon_shell_open_app(shell, 4);
                } else {
                    recon_spawn(shell->server, APPS[index].command);
                }
                return true;
            }
        }
        recon_shell_close_menu(shell);
        return true;
    }

    /*
     * Then whichever built-in window the scene graph says is actually on top
     * here -- not merely one whose rectangle covers the point.
     */
    struct wlr_scene_node *node = topmost_node(shell, lx, ly);
    int index = appwin_index_for_node(shell, node);
    if (index >= 0) {
        if (recon_appwin_handle_click(shell->apps[index], lx, ly, pressed)) {
            if (pressed) {
                raise_app_order(shell, index);
                set_focused_app(shell, index);
            }
            recon_shell_refresh(shell);
            return true;
        }
    }

    /* A release still has to reach a window mid-drag, whose pointer may have
     * left it. */
    if (!pressed) {
        for (int i = 0; i < shell->app_count; i++) {
            if (recon_appwin_handle_click(shell->apps[shell->app_order[i]],
                    lx, ly, false)) {
                return true;
            }
        }
    }

    if (shell->taskbar != NULL && point_in_panel(shell->taskbar, lx, ly, &px, &py)) {
        if (!pressed) {
            return true;
        }
        uint32_t hit = recon_hit_test(shell->taskbar, px, py);
        wlr_log(WLR_DEBUG, "ReconOS: taskbar click at %d,%d -> hit %u (%d buttons)",
            px, py, hit, shell->button_count);

        if (hit == HIT_APPS_BUTTON) {
            toggle_menu(shell);
        } else if (hit >= HIT_TASK_BASE && hit < HIT_MENU_BASE) {
            int index = (int)(hit - HIT_TASK_BASE);
            if (index >= 0 && index < shell->button_count) {
                struct taskbar_button *button = &shell->buttons[index];

                /*
                 * Clicking the focused window's button puts it away; clicking
                 * any other brings it forward. That is the behaviour people
                 * already expect from a taskbar.
                 */
                if (button->appwin != NULL) {
                    int app_index = -1;
                    for (int a = 0; a < shell->app_count; a++) {
                        if (shell->apps[a] == button->appwin) {
                            app_index = a;
                            break;
                        }
                    }

                    if (recon_appwin_is_minimized(button->appwin)) {
                        recon_appwin_restore(button->appwin);
                        recon_appwin_focus(button->appwin);
                        set_focused_app(shell, app_index);
                        raise_app_order(shell, app_index);
                    } else if (recon_appwin_is_focused(button->appwin)) {
                        recon_appwin_minimize(button->appwin);
                        set_focused_app(shell, -1);
                    } else {
                        recon_appwin_focus(button->appwin);
                        set_focused_app(shell, app_index);
                        raise_app_order(shell, app_index);
                    }
                } else if (button->toplevel != NULL) {
                    if (recon_toplevel_is_minimized(button->toplevel)) {
                        recon_toplevel_restore(button->toplevel);
                    } else if (recon_toplevel_is_focused(button->toplevel)) {
                        recon_toplevel_minimize(button->toplevel);
                    } else {
                        recon_focus_toplevel(button->toplevel);
                    }
                }
            }
            recon_shell_close_menu(shell);
            recon_shell_refresh(shell);
        } else {
            recon_shell_close_menu(shell);
        }
        return true;
    }

    /* Clicking anywhere else dismisses an open menu, and that click is
     * consumed rather than reaching the window behind it. */
    if (shell->menu_open && pressed) {
        recon_shell_close_menu(shell);
        return true;
    }

    /*
     * The desktop answers last. It is the backdrop, so it only sees clicks
     * nothing in front of it wanted -- and only when the scene agrees nothing
     * is drawn over it there.
     */
    if (node == recon_desktop_node(shell->desktop)) {
        struct recon_desktop_action action;
        if (recon_desktop_handle_click(shell->desktop, lx, ly, pressed, &action)) {
            if (action.kind == RECON_DESKTOP_ACTION_OPEN_APP) {
                for (int i = 0; i < shell->app_count; i++) {
                    if (strcmp(recon_appwin_title(shell->apps[i]), action.target) == 0) {
                        recon_shell_open_app(shell, i);
                        break;
                    }
                }
            } else if (action.kind == RECON_DESKTOP_ACTION_OPEN_PATH) {
                /* Folders open in the file explorer, which is index 4. */
                recon_shell_open_app(shell, 4);
            }
            return true;
        }
    }

    return false;
}
