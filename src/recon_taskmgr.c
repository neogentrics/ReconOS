/*
 * ReconOS Task Manager. See include/recon_taskmgr.h.
 *
 * Sampling runs on a timer that only exists while the window is open. A task
 * manager that keeps polling after you close it is exactly the kind of idle
 * cost this system is meant not to have.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "recon_procinfo.h"
#include "recon_server.h"
#include "recon_taskmgr.h"
#include "recon_ui.h"

/* --- Layout --- */

#define WINDOW_WIDTH 560
#define WINDOW_HEIGHT 420
#define TITLE_HEIGHT 24
#define MENUBAR_HEIGHT 20
#define DROPDOWN_ITEM_HEIGHT 22
#define DROPDOWN_WIDTH 150
#define BORDER 3
#define HEADER_HEIGHT 22
#define ROW_HEIGHT 18
#define FOOTER_HEIGHT 40
#define BUTTON_WIDTH 96
#define BUTTON_HEIGHT 24
#define PADDING 8

/* Column x offsets inside the list area. */
#define COL_NAME 6
#define COL_PID 250
#define COL_CPU 330
#define COL_MEM 420
#define COL_STATE 510

#define REFRESH_MS 1000

/* --- Colours --- */

#define COLOR_FRAME RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_TITLE RECON_RGB(0x20, 0x2A, 0x44)
#define COLOR_TITLE_TEXT RECON_RGB(0xF0, 0xF0, 0xF0)
#define COLOR_LIST_BG RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_HEADER RECON_RGB(0xD4, 0xD4, 0xD4)
#define COLOR_TEXT RECON_RGB(0x10, 0x10, 0x10)
#define COLOR_ROW_ALT RECON_RGB(0xF2, 0xF2, 0xF6)
#define COLOR_ROW_SELECTED RECON_RGB(0x30, 0x50, 0x90)
#define COLOR_ROW_SELECTED_TEXT RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_BUTTON RECON_RGB(0xC8, 0xC8, 0xC8)
#define COLOR_ACCENT RECON_RGB(0x8B, 0x1A, 0x1A)
#define COLOR_STATUS RECON_RGB(0x30, 0x30, 0x30)
#define COLOR_MENUBAR RECON_RGB(0xC8, 0xC8, 0xC8)
#define COLOR_MENU_HILITE RECON_RGB(0x30, 0x50, 0x90)
#define COLOR_MENU_HILITE_TEXT RECON_RGB(0xFF, 0xFF, 0xFF)

/* Hit-region ids. */
#define HIT_TITLEBAR 1
#define HIT_CLOSE 2
#define HIT_END_TASK 3
#define HIT_SORT_NAME 10
#define HIT_SORT_CPU 11
#define HIT_SORT_MEM 12
#define HIT_MENU_FILE 20
#define HIT_MENU_VIEW 21
#define HIT_DROP_BASE 50
#define HIT_ROW_BASE 100

enum sort_mode {
    SORT_CPU,
    SORT_MEMORY,
    SORT_NAME,
};

/* Which menu bar item, if any, is showing its dropdown. */
enum open_menu {
    MENU_NONE,
    MENU_FILE,
    MENU_VIEW,
};

static const char *const FILE_ITEMS[] = { "New Task", "Exit" };
static const char *const VIEW_ITEMS[] = { "Sort by Name", "Sort by CPU", "Sort by Memory" };

#define FILE_COUNT ((int)(sizeof(FILE_ITEMS) / sizeof(FILE_ITEMS[0])))
#define VIEW_COUNT ((int)(sizeof(VIEW_ITEMS) / sizeof(VIEW_ITEMS[0])))

struct recon_taskmgr {
    struct recon_server *server;
    struct recon_font *font;
    struct recon_panel *panel;
    struct recon_proc_snapshot *snapshot;

    bool visible;
    int x, y;

    enum sort_mode sort;
    enum open_menu menu;
    int scroll;          /* first visible row */
    pid_t selected_pid;  /* survives re-sorting, unlike a row index */

    /* Title bar drag. */
    bool dragging;
    double drag_offset_x, drag_offset_y;

    /* Only exists while the window is open. */
    struct wl_event_source *timer;

    char status[128];
};

static int visible_rows(void) {
    return (WINDOW_HEIGHT - TITLE_HEIGHT - MENUBAR_HEIGHT - BORDER * 2
        - HEADER_HEIGHT - FOOTER_HEIGHT) / ROW_HEIGHT;
}

static int menubar_top(void) {
    return TITLE_HEIGHT;
}

static int header_top(void) {
    return TITLE_HEIGHT + MENUBAR_HEIGHT + BORDER;
}

static int list_top(void) {
    return header_top() + HEADER_HEIGHT;
}

/* Menu bar item extents, so drawing and hit testing agree. */
static int menu_item_x(int index) {
    return PADDING + index * 52;
}

/* --- Drawing --- */

static void draw_title_bar(struct recon_taskmgr *tm) {
    struct recon_panel *p = tm->panel;
    int width = recon_panel_width(p);
    int ascent = recon_font_ascent(tm->font);

    recon_fill_rect(p, 0, 0, width, TITLE_HEIGHT, COLOR_TITLE);
    recon_draw_text(p, tm->font, PADDING, (TITLE_HEIGHT + ascent) / 2 - 1,
        width - 60, "ReconOS Task Manager", COLOR_TITLE_TEXT);
    recon_hit_add(p, 0, 0, width - 30, TITLE_HEIGHT, HIT_TITLEBAR);

    /* Close button. */
    int bx = width - 22;
    int by = 4;
    recon_fill_rect(p, bx, by, 16, 16, COLOR_BUTTON);
    recon_draw_bevel(p, bx, by, 16, 16, false);
    recon_draw_text(p, tm->font, bx + 5, by + 12, 12, "x", COLOR_TEXT);
    recon_hit_add(p, bx, by, 16, 16, HIT_CLOSE);
}

static void draw_menubar(struct recon_taskmgr *tm) {
    struct recon_panel *p = tm->panel;
    int width = recon_panel_width(p);
    int y = menubar_top();
    int ascent = recon_font_ascent(tm->font);
    int baseline = y + (MENUBAR_HEIGHT + ascent) / 2 - 2;

    recon_fill_rect(p, 0, y, width, MENUBAR_HEIGHT, COLOR_MENUBAR);
    recon_fill_rect(p, 0, y + MENUBAR_HEIGHT - 1, width, 1,
        RECON_RGB(0x90, 0x90, 0x90));

    static const char *const LABELS[] = { "File", "View" };
    static const uint32_t IDS[] = { HIT_MENU_FILE, HIT_MENU_VIEW };
    static const enum open_menu MENUS[] = { MENU_FILE, MENU_VIEW };

    for (int i = 0; i < 2; i++) {
        int x = menu_item_x(i);
        int w = recon_text_width(tm->font, LABELS[i]) + 16;
        bool open = (tm->menu == MENUS[i]);

        if (open) {
            recon_fill_rect(p, x - 8, y + 2, w, MENUBAR_HEIGHT - 4, COLOR_MENU_HILITE);
        }
        recon_draw_text(p, tm->font, x, baseline, w, LABELS[i],
            open ? COLOR_MENU_HILITE_TEXT : COLOR_TEXT);
        recon_hit_add(p, x - 8, y, w, MENUBAR_HEIGHT, IDS[i]);
    }
}

/* Dropdowns are drawn last so they sit over the list beneath them. */
static void draw_dropdown(struct recon_taskmgr *tm) {
    if (tm->menu == MENU_NONE) {
        return;
    }

    struct recon_panel *p = tm->panel;
    const char *const *items = tm->menu == MENU_FILE ? FILE_ITEMS : VIEW_ITEMS;
    int count = tm->menu == MENU_FILE ? FILE_COUNT : VIEW_COUNT;
    int x = menu_item_x(tm->menu == MENU_FILE ? 0 : 1) - 8;
    int y = menubar_top() + MENUBAR_HEIGHT;
    int height = count * DROPDOWN_ITEM_HEIGHT + 4;
    int ascent = recon_font_ascent(tm->font);

    recon_fill_rect(p, x, y, DROPDOWN_WIDTH, height, COLOR_MENUBAR);
    recon_draw_bevel(p, x, y, DROPDOWN_WIDTH, height, false);
    recon_stroke_rect(p, x, y, DROPDOWN_WIDTH, height, RECON_RGB(0x40, 0x40, 0x40));

    for (int i = 0; i < count; i++) {
        int iy = y + 2 + i * DROPDOWN_ITEM_HEIGHT;
        recon_draw_text(p, tm->font, x + 12,
            iy + (DROPDOWN_ITEM_HEIGHT + ascent) / 2 - 2,
            DROPDOWN_WIDTH - 24, items[i], COLOR_TEXT);
        recon_hit_add(p, x, iy, DROPDOWN_WIDTH, DROPDOWN_ITEM_HEIGHT,
            HIT_DROP_BASE + i);
    }
}

static void draw_header(struct recon_taskmgr *tm) {
    struct recon_panel *p = tm->panel;
    int width = recon_panel_width(p);
    int y = header_top();
    int baseline = y + (HEADER_HEIGHT + recon_font_ascent(tm->font)) / 2 - 2;
    int inner_w = width - BORDER * 2;

    recon_fill_rect(p, BORDER, y, inner_w, HEADER_HEIGHT, COLOR_HEADER);
    recon_fill_rect(p, BORDER, y + HEADER_HEIGHT - 1, inner_w, 1,
        RECON_RGB(0x80, 0x80, 0x80));

    /* An arrow marks the column the list is sorted by. */
    const char *name_label = tm->sort == SORT_NAME ? "Name  v" : "Name";
    const char *cpu_label = tm->sort == SORT_CPU ? "CPU  v" : "CPU";
    const char *mem_label = tm->sort == SORT_MEMORY ? "Memory  v" : "Memory";

    recon_draw_text(p, tm->font, BORDER + COL_NAME, baseline, 200, name_label, COLOR_TEXT);
    recon_draw_text(p, tm->font, BORDER + COL_PID, baseline, 70, "PID", COLOR_TEXT);
    recon_draw_text(p, tm->font, BORDER + COL_CPU, baseline, 80, cpu_label, COLOR_TEXT);
    recon_draw_text(p, tm->font, BORDER + COL_MEM, baseline, 80, mem_label, COLOR_TEXT);
    recon_draw_text(p, tm->font, BORDER + COL_STATE, baseline, 40, "St", COLOR_TEXT);

    recon_hit_add(p, BORDER + COL_NAME, y, COL_PID - COL_NAME, HEADER_HEIGHT,
        HIT_SORT_NAME);
    recon_hit_add(p, BORDER + COL_CPU, y, COL_MEM - COL_CPU, HEADER_HEIGHT,
        HIT_SORT_CPU);
    recon_hit_add(p, BORDER + COL_MEM, y, COL_STATE - COL_MEM, HEADER_HEIGHT,
        HIT_SORT_MEM);
}

static void draw_rows(struct recon_taskmgr *tm) {
    struct recon_panel *p = tm->panel;
    int width = recon_panel_width(p);
    int inner_w = width - BORDER * 2;
    int top = list_top();
    int rows = visible_rows();
    int ascent = recon_font_ascent(tm->font);

    recon_fill_rect(p, BORDER, top, inner_w, rows * ROW_HEIGHT, COLOR_LIST_BG);

    size_t count = recon_proc_count(tm->snapshot);
    for (int row = 0; row < rows; row++) {
        size_t index = (size_t)(tm->scroll + row);
        if (index >= count) {
            break;
        }

        const struct recon_process *proc = recon_proc_at(tm->snapshot, index);
        if (proc == NULL) {
            break;
        }

        int y = top + row * ROW_HEIGHT;
        int baseline = y + (ROW_HEIGHT + ascent) / 2 - 2;
        bool selected = (proc->pid == tm->selected_pid);

        if (selected) {
            recon_fill_rect(p, BORDER, y, inner_w, ROW_HEIGHT, COLOR_ROW_SELECTED);
        } else if (row % 2 == 1) {
            /* Banding makes a long list easier to read across. */
            recon_fill_rect(p, BORDER, y, inner_w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        recon_color text = selected ? COLOR_ROW_SELECTED_TEXT : COLOR_TEXT;
        char buffer[32];

        recon_draw_text(p, tm->font, BORDER + COL_NAME, baseline,
            COL_PID - COL_NAME - 10, proc->name, text);

        snprintf(buffer, sizeof(buffer), "%d", (int)proc->pid);
        recon_draw_text(p, tm->font, BORDER + COL_PID, baseline, 70, buffer, text);

        snprintf(buffer, sizeof(buffer), "%.1f%%", proc->cpu_percent);
        recon_draw_text(p, tm->font, BORDER + COL_CPU, baseline, 80, buffer, text);

        if (proc->memory_kb >= 1024) {
            snprintf(buffer, sizeof(buffer), "%.1f MB", proc->memory_kb / 1024.0);
        } else {
            snprintf(buffer, sizeof(buffer), "%zu KB", proc->memory_kb);
        }
        recon_draw_text(p, tm->font, BORDER + COL_MEM, baseline, 80, buffer, text);

        snprintf(buffer, sizeof(buffer), "%c", proc->state);
        recon_draw_text(p, tm->font, BORDER + COL_STATE, baseline, 30, buffer, text);

        recon_hit_add(p, BORDER, y, inner_w, ROW_HEIGHT, HIT_ROW_BASE + row);
    }
}

static void draw_footer(struct recon_taskmgr *tm) {
    struct recon_panel *p = tm->panel;
    int width = recon_panel_width(p);
    int height = recon_panel_height(p);
    int y = height - FOOTER_HEIGHT;
    int ascent = recon_font_ascent(tm->font);

    recon_fill_rect(p, BORDER, y, width - BORDER * 2, FOOTER_HEIGHT - BORDER,
        COLOR_FRAME);

    /* End Task, on the right where a confirming action belongs. */
    int bx = width - BORDER - PADDING - BUTTON_WIDTH;
    int by = y + (FOOTER_HEIGHT - BUTTON_HEIGHT) / 2 - 2;
    recon_fill_rect(p, bx, by, BUTTON_WIDTH, BUTTON_HEIGHT, COLOR_BUTTON);
    recon_draw_bevel(p, bx, by, BUTTON_WIDTH, BUTTON_HEIGHT, false);
    recon_draw_text(p, tm->font, bx + 16, by + (BUTTON_HEIGHT + ascent) / 2 - 2,
        BUTTON_WIDTH - 20, "End Task", COLOR_TEXT);
    recon_hit_add(p, bx, by, BUTTON_WIDTH, BUTTON_HEIGHT, HIT_END_TASK);

    recon_draw_text(p, tm->font, BORDER + PADDING,
        y + (FOOTER_HEIGHT + ascent) / 2 - 2,
        bx - BORDER - PADDING * 2, tm->status, COLOR_STATUS);
}

static void redraw(struct recon_taskmgr *tm) {
    if (tm->panel == NULL || !tm->visible) {
        return;
    }

    struct recon_panel *p = tm->panel;
    int width = recon_panel_width(p);
    int height = recon_panel_height(p);

    recon_hit_clear(p);
    recon_fill(p, COLOR_FRAME);

    draw_title_bar(tm);
    draw_menubar(tm);
    draw_header(tm);
    draw_rows(tm);
    draw_footer(tm);
    draw_dropdown(tm);

    /* The window's own frame, since nothing else draws one for it. */
    recon_draw_bevel(p, 0, 0, width, height, false);
    recon_stroke_rect(p, 0, 0, width, height, RECON_RGB(0x30, 0x30, 0x30));
    /* A sunken edge around the list, so it reads as inset. */
    recon_stroke_rect(p, BORDER - 1, header_top() - 1,
        width - BORDER * 2 + 2,
        HEADER_HEIGHT + visible_rows() * ROW_HEIGHT + 2,
        RECON_RGB(0x80, 0x80, 0x80));

    recon_panel_commit(p);
    recon_damage_all(tm->server);
}

/* --- Sampling --- */

static void apply_sort(struct recon_taskmgr *tm) {
    switch (tm->sort) {
    case SORT_CPU:
        recon_proc_sort_by_cpu(tm->snapshot);
        break;
    case SORT_MEMORY:
        recon_proc_sort_by_memory(tm->snapshot);
        break;
    case SORT_NAME:
        recon_proc_sort_by_name(tm->snapshot);
        break;
    }
}

static void sample(struct recon_taskmgr *tm) {
    if (!recon_proc_snapshot_refresh(tm->snapshot)) {
        snprintf(tm->status, sizeof(tm->status), "Could not read process list");
        return;
    }
    apply_sort(tm);

    snprintf(tm->status, sizeof(tm->status),
        "%zu processes   CPU %.0f%%   Memory %zu / %zu MB",
        recon_proc_count(tm->snapshot),
        recon_proc_total_cpu_percent(tm->snapshot),
        recon_proc_used_memory_kb(tm->snapshot) / 1024,
        recon_proc_total_memory_kb(tm->snapshot) / 1024);
}

static int on_timer(void *data) {
    struct recon_taskmgr *tm = data;
    if (!tm->visible) {
        return 0;
    }
    sample(tm);
    redraw(tm);
    wl_event_source_timer_update(tm->timer, REFRESH_MS);
    return 0;
}

/* --- Lifecycle --- */

struct recon_taskmgr *recon_taskmgr_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_taskmgr *tm = calloc(1, sizeof(*tm));
    if (tm == NULL) {
        return NULL;
    }

    tm->server = server;
    tm->font = font;
    tm->sort = SORT_CPU;
    tm->selected_pid = 0;
    snprintf(tm->status, sizeof(tm->status), "Reading processes...");

    tm->snapshot = recon_proc_snapshot_create();
    if (tm->snapshot == NULL) {
        free(tm);
        return NULL;
    }

    tm->panel = recon_panel_create(&server->scene->tree, WINDOW_WIDTH, WINDOW_HEIGHT);
    if (tm->panel == NULL) {
        recon_proc_snapshot_destroy(tm->snapshot);
        free(tm);
        return NULL;
    }
    recon_panel_set_enabled(tm->panel, false);

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    tm->timer = wl_event_loop_add_timer(loop, on_timer, tm);

    return tm;
}

void recon_taskmgr_destroy(struct recon_taskmgr *tm) {
    if (tm == NULL) {
        return;
    }
    if (tm->timer != NULL) {
        wl_event_source_remove(tm->timer);
    }
    recon_panel_destroy(tm->panel);
    recon_proc_snapshot_destroy(tm->snapshot);
    free(tm);
}

void recon_taskmgr_center(struct recon_taskmgr *tm, int screen_w, int screen_h) {
    if (tm == NULL) {
        return;
    }
    tm->x = (screen_w - WINDOW_WIDTH) / 2;
    tm->y = (screen_h - WINDOW_HEIGHT) / 2;
    if (tm->x < 0) {
        tm->x = 0;
    }
    if (tm->y < 0) {
        tm->y = 0;
    }
    recon_panel_set_position(tm->panel, tm->x, tm->y);
}

void recon_taskmgr_show(struct recon_taskmgr *tm) {
    if (tm == NULL || tm->visible) {
        return;
    }

    tm->visible = true;
    recon_panel_set_enabled(tm->panel, true);
    recon_panel_raise_to_top(tm->panel);

    /* Two samples in quick succession: CPU is a rate, so the first reading has
     * nothing to compare against and would show every process at zero. */
    sample(tm);
    sample(tm);
    redraw(tm);

    if (tm->timer != NULL) {
        wl_event_source_timer_update(tm->timer, REFRESH_MS);
    }
    wlr_log(WLR_INFO, "ReconOS: task manager opened");
}

void recon_taskmgr_hide(struct recon_taskmgr *tm) {
    if (tm == NULL || !tm->visible) {
        return;
    }

    tm->visible = false;
    tm->dragging = false;
    tm->menu = MENU_NONE;
    recon_panel_set_enabled(tm->panel, false);

    /* Stop sampling. A closed window should cost nothing. */
    if (tm->timer != NULL) {
        wl_event_source_timer_update(tm->timer, 0);
    }
    recon_damage_all(tm->server);
    wlr_log(WLR_INFO, "ReconOS: task manager closed");
}

void recon_taskmgr_toggle(struct recon_taskmgr *tm) {
    if (tm == NULL) {
        return;
    }
    if (tm->visible) {
        recon_taskmgr_hide(tm);
    } else {
        recon_taskmgr_show(tm);
    }
}

bool recon_taskmgr_visible(struct recon_taskmgr *tm) {
    return tm != NULL && tm->visible;
}

void recon_taskmgr_raise(struct recon_taskmgr *tm) {
    if (tm != NULL && tm->visible) {
        recon_panel_raise_to_top(tm->panel);
    }
}

/* --- Input --- */

static bool to_local(struct recon_taskmgr *tm, double lx, double ly, int *px, int *py) {
    int local_x = (int)lx - tm->x;
    int local_y = (int)ly - tm->y;
    if (local_x < 0 || local_y < 0 ||
            local_x >= WINDOW_WIDTH || local_y >= WINDOW_HEIGHT) {
        return false;
    }
    *px = local_x;
    *py = local_y;
    return true;
}

bool recon_taskmgr_contains_point(struct recon_taskmgr *tm, double lx, double ly) {
    int px, py;
    return tm != NULL && tm->visible && to_local(tm, lx, ly, &px, &py);
}

static void end_selected_task(struct recon_taskmgr *tm) {
    if (tm->selected_pid <= 0) {
        snprintf(tm->status, sizeof(tm->status), "Select a process first");
        return;
    }

    /* Ask politely. A program given SIGTERM can save its work; only a refusal
     * to exit justifies forcing it, which is a separate decision. */
    if (recon_proc_terminate(tm->selected_pid)) {
        snprintf(tm->status, sizeof(tm->status),
            "Asked process %d to close", (int)tm->selected_pid);
    } else {
        snprintf(tm->status, sizeof(tm->status),
            "Could not end process %d", (int)tm->selected_pid);
    }
    tm->selected_pid = 0;
}

bool recon_taskmgr_handle_click(struct recon_taskmgr *tm, double lx, double ly,
        bool pressed) {
    if (tm == NULL || !tm->visible) {
        return false;
    }

    if (!pressed) {
        tm->dragging = false;
        return true;
    }

    int px, py;
    if (!to_local(tm, lx, ly, &px, &py)) {
        return false;
    }

    recon_panel_raise_to_top(tm->panel);
    uint32_t hit = recon_hit_test(tm->panel, px, py);

    if (hit >= HIT_DROP_BASE && hit < HIT_ROW_BASE && tm->menu != MENU_NONE) {
        int index = (int)(hit - HIT_DROP_BASE);
        enum open_menu which = tm->menu;
        tm->menu = MENU_NONE;

        if (which == MENU_FILE) {
            if (index == 0) {
                /* New Task launches the configured terminal, which is the
                 * closest thing to a run box until there is one. */
                recon_spawn(tm->server, NULL);
                snprintf(tm->status, sizeof(tm->status), "Started a new task");
            } else {
                recon_taskmgr_hide(tm);
                return true;
            }
        } else if (which == MENU_VIEW) {
            tm->sort = (index == 0) ? SORT_NAME
                : (index == 1) ? SORT_CPU : SORT_MEMORY;
            apply_sort(tm);
        }
        redraw(tm);
        return true;
    }

    /* A click anywhere else closes an open dropdown first. */
    if (tm->menu != MENU_NONE && hit != HIT_MENU_FILE && hit != HIT_MENU_VIEW) {
        tm->menu = MENU_NONE;
        redraw(tm);
        return true;
    }

    switch (hit) {
    case HIT_MENU_FILE:
        tm->menu = (tm->menu == MENU_FILE) ? MENU_NONE : MENU_FILE;
        redraw(tm);
        return true;

    case HIT_MENU_VIEW:
        tm->menu = (tm->menu == MENU_VIEW) ? MENU_NONE : MENU_VIEW;
        redraw(tm);
        return true;

    case HIT_CLOSE:
        recon_taskmgr_hide(tm);
        return true;

    case HIT_TITLEBAR:
        tm->dragging = true;
        tm->drag_offset_x = lx - tm->x;
        tm->drag_offset_y = ly - tm->y;
        return true;

    case HIT_END_TASK:
        end_selected_task(tm);
        redraw(tm);
        return true;

    case HIT_SORT_NAME:
        tm->sort = SORT_NAME;
        apply_sort(tm);
        redraw(tm);
        return true;

    case HIT_SORT_CPU:
        tm->sort = SORT_CPU;
        apply_sort(tm);
        redraw(tm);
        return true;

    case HIT_SORT_MEM:
        tm->sort = SORT_MEMORY;
        apply_sort(tm);
        redraw(tm);
        return true;

    default:
        break;
    }

    if (hit >= HIT_ROW_BASE) {
        size_t index = (size_t)(tm->scroll + (int)(hit - HIT_ROW_BASE));
        const struct recon_process *proc = recon_proc_at(tm->snapshot, index);
        /* Remember the pid, not the row: the list re-sorts underneath it. */
        tm->selected_pid = proc != NULL ? proc->pid : 0;
        redraw(tm);
        return true;
    }

    /* Anywhere else on the window still belongs to the window. */
    return true;
}

void recon_taskmgr_handle_motion(struct recon_taskmgr *tm, double lx, double ly) {
    if (tm == NULL || !tm->visible || !tm->dragging) {
        return;
    }
    tm->x = (int)(lx - tm->drag_offset_x);
    tm->y = (int)(ly - tm->drag_offset_y);
    recon_panel_set_position(tm->panel, tm->x, tm->y);
    recon_damage_all(tm->server);
}

void recon_taskmgr_handle_scroll(struct recon_taskmgr *tm, double delta) {
    if (tm == NULL || !tm->visible) {
        return;
    }

    int rows = visible_rows();
    int count = (int)recon_proc_count(tm->snapshot);
    int max_scroll = count > rows ? count - rows : 0;

    tm->scroll += (delta > 0) ? 3 : -3;
    if (tm->scroll > max_scroll) {
        tm->scroll = max_scroll;
    }
    if (tm->scroll < 0) {
        tm->scroll = 0;
    }
    redraw(tm);
}
