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
#include "recon_taskmgr.h"
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

/* Hit-region ids. Window buttons use TASK_BASE + index. */
#define HIT_APPS_BUTTON 1
#define HIT_TASK_BASE 100
#define HIT_MENU_BASE 200
#define HIT_SEC_BASE 300

/* --- Apps menu contents --- */

enum recon_app_action {
    RECON_APP_LAUNCH,
    RECON_APP_TASKMGR,
    RECON_APP_QUIT,
};

struct recon_app_entry {
    const char *label;
    const char *command; /* NULL means the configured terminal */
    enum recon_app_action action;
};

/*
 * Hardcoded for now. Reading .desktop files off the system would work, but
 * ReconOS is meant to present its own applications rather than inherit
 * whatever a host distribution happens to have installed.
 */
static const struct recon_app_entry APPS[] = {
    { "Terminal", NULL, RECON_APP_LAUNCH },
    { "Task Manager", NULL, RECON_APP_TASKMGR },
    { "Shut Down", NULL, RECON_APP_QUIT },
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

    struct recon_panel *taskbar;
    struct recon_panel *menu;
    bool menu_open;

    struct recon_panel *security;
    bool security_open;
    struct recon_taskmgr *taskmgr;

    int screen_width, screen_height;

    /*
     * Windows in the order their buttons are drawn. The taskbar hit region
     * carries an index into this, so a click resolves to a window without
     * depending on focus order, which changes as soon as the click lands.
     */
    struct recon_toplevel *buttons[32];
    int button_count;
};

static int menu_height(void) {
    return APP_COUNT * MENU_ITEM_HEIGHT + MENU_PADDING * 2;
}

static int security_height(void) {
    return SEC_TITLE_HEIGHT + SEC_PADDING * 3 +
        SEC_COUNT * (SEC_BUTTON_HEIGHT + SEC_PADDING) + 20;
}

/* --- Drawing --- */

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

    /* A small mark so the button reads as the system menu, not a window. */
    recon_fill_rect(bar, TASKBAR_PADDING + TEXT_INSET, TASKBAR_PADDING + 9, 10, 10,
        COLOR_ACCENT);
    recon_draw_text(bar, shell->font, TASKBAR_PADDING + TEXT_INSET + 16, baseline,
        APPS_BUTTON_WIDTH - TEXT_INSET - 20, "Apps", COLOR_TEXT);
    recon_hit_add(bar, TASKBAR_PADDING, TASKBAR_PADDING,
        APPS_BUTTON_WIDTH, BUTTON_HEIGHT, HIT_APPS_BUTTON);

    /* One button per open window, sharing the remaining width. */
    shell->button_count = 0;
    int x = TASKBAR_PADDING * 2 + APPS_BUTTON_WIDTH;
    int available = width - x - TASKBAR_PADDING;

    int window_count = wl_list_length(&shell->server->toplevels);
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

    /* The head of the list is the focused window. */
    struct recon_toplevel *focused = NULL;
    if (!wl_list_empty(&shell->server->toplevels)) {
        focused = wl_container_of(shell->server->toplevels.next, focused, link);
    }

    struct recon_toplevel *toplevel;
    wl_list_for_each(toplevel, &shell->server->toplevels, link) {
        if (shell->button_count >= (int)(sizeof(shell->buttons) / sizeof(shell->buttons[0]))) {
            break;
        }
        if (x + button_width > width - TASKBAR_PADDING) {
            break;
        }

        bool active = (toplevel == focused);
        recon_fill_rect(bar, x, TASKBAR_PADDING, button_width, BUTTON_HEIGHT,
            active ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
        recon_draw_bevel(bar, x, TASKBAR_PADDING, button_width, BUTTON_HEIGHT, active);

        const char *title = toplevel->xdg_toplevel->title;
        recon_draw_text(bar, shell->font, x + TEXT_INSET, baseline,
            button_width - TEXT_INSET * 2,
            title != NULL ? title : "Untitled",
            active ? COLOR_TEXT : COLOR_TEXT_DIM);

        recon_hit_add(bar, x, TASKBAR_PADDING, button_width, BUTTON_HEIGHT,
            HIT_TASK_BASE + shell->button_count);
        shell->buttons[shell->button_count++] = toplevel;

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

        recon_draw_text(menu, shell->font, MENU_PADDING + TEXT_INSET, baseline,
            width - MENU_PADDING * 2 - TEXT_INSET * 2, APPS[i].label, COLOR_TEXT);
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
        recon_fill_rect(p, SEC_PADDING, y, bw, SEC_BUTTON_HEIGHT, COLOR_BUTTON);
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
    if (shell->security != NULL) {
        /* Centred: it is a question that interrupts, not a corner notice. */
        recon_panel_set_position(shell->security,
            (shell->screen_width - SEC_WIDTH) / 2,
            (shell->screen_height - security_height()) / 2);
    }
    recon_taskmgr_center(shell->taskmgr, shell->screen_width, shell->screen_height);
}

/* --- Lifecycle --- */

struct recon_shell *recon_shell_create(struct recon_server *server,
        int screen_width, int screen_height) {
    struct recon_shell *shell = calloc(1, sizeof(*shell));
    if (shell == NULL) {
        return NULL;
    }

    shell->server = server;
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

    shell->security = recon_panel_create(&server->scene->tree,
        SEC_WIDTH, security_height());
    if (shell->security != NULL) {
        recon_panel_set_enabled(shell->security, false);
        draw_security(shell);
    }

    shell->taskmgr = recon_taskmgr_create(server, shell->font);

    layout(shell);
    draw_taskbar(shell);

    wlr_log(WLR_INFO, "ReconOS: shell up, taskbar %dx%d",
        screen_width, TASKBAR_HEIGHT);
    return shell;
}

void recon_shell_destroy(struct recon_shell *shell) {
    if (shell == NULL) {
        return;
    }
    recon_taskmgr_destroy(shell->taskmgr);
    recon_panel_destroy(shell->security);
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
    recon_panel_raise_to_top(shell->taskbar);
    if (shell->menu_open) {
        recon_panel_raise_to_top(shell->menu);
    }
    /* The task manager and the security box sit above everything, including
     * the taskbar: they are how you regain control when something else has
     * taken over the screen. */
    recon_taskmgr_raise(shell->taskmgr);
    if (shell->security_open) {
        recon_panel_raise_to_top(shell->security);
    }
}

void recon_shell_open_taskmgr(struct recon_shell *shell) {
    if (shell == NULL) {
        return;
    }
    recon_shell_close_menu(shell);
    recon_taskmgr_show(shell->taskmgr);
}

bool recon_shell_security_open(struct recon_shell *shell) {
    return shell != NULL && shell->security_open;
}

void recon_shell_toggle_security(struct recon_shell *shell) {
    if (shell == NULL || shell->security == NULL) {
        return;
    }
    shell->security_open = !shell->security_open;
    recon_panel_set_enabled(shell->security, shell->security_open);
    if (shell->security_open) {
        recon_shell_close_menu(shell);
        recon_panel_raise_to_top(shell->security);
    }
    recon_damage_all(shell->server);
}

void recon_shell_handle_motion(struct recon_shell *shell, double lx, double ly) {
    if (shell != NULL) {
        recon_taskmgr_handle_motion(shell->taskmgr, lx, ly);
    }
}

bool recon_shell_handle_scroll(struct recon_shell *shell, double lx, double ly,
        double delta) {
    if (shell == NULL) {
        return false;
    }
    if (recon_taskmgr_contains_point(shell->taskmgr, lx, ly)) {
        recon_taskmgr_handle_scroll(shell->taskmgr, delta);
        return true;
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
    if (recon_taskmgr_contains_point(shell->taskmgr, lx, ly)) {
        return true;
    }
    int px, py;
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
                    recon_taskmgr_show(shell->taskmgr);
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

    /* The task manager floats above the desktop and handles its own chrome. */
    if (recon_taskmgr_handle_click(shell->taskmgr, lx, ly, pressed)) {
        return true;
    }

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
                    recon_taskmgr_show(shell->taskmgr);
                } else {
                    recon_spawn(shell->server, APPS[index].command);
                }
                return true;
            }
        }
        recon_shell_close_menu(shell);
        return true;
    }

    if (shell->taskbar != NULL && point_in_panel(shell->taskbar, lx, ly, &px, &py)) {
        if (!pressed) {
            return true;
        }
        uint32_t hit = recon_hit_test(shell->taskbar, px, py);

        if (hit == HIT_APPS_BUTTON) {
            toggle_menu(shell);
        } else if (hit >= HIT_TASK_BASE && hit < HIT_MENU_BASE) {
            int index = (int)(hit - HIT_TASK_BASE);
            if (index >= 0 && index < shell->button_count) {
                recon_focus_toplevel(shell->buttons[index]);
            }
            recon_shell_close_menu(shell);
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

    return false;
}
