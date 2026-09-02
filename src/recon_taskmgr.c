/*
 * ReconOS Task Manager. See include/recon_taskmgr.h.
 *
 * Only the contents are implemented here. The window itself -- frame, title
 * bar, minimize, maximize, close, dragging -- comes from the built-in window
 * framework, which is why those behave identically to every other window.
 *
 * Sampling runs on a timer that only ticks while the window is on screen. A
 * task manager that keeps polling after you minimize it is exactly the idle
 * cost this system is meant not to have.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

#include "recon_appwin.h"
#include "recon_procinfo.h"
#include "recon_server.h"
#include "recon_taskmgr.h"
#include "recon_ui.h"

/* --- Layout, relative to the content area --- */

#define MENUBAR_HEIGHT 20
#define DROPDOWN_ITEM_HEIGHT 22
#define DROPDOWN_WIDTH 150
#define HEADER_HEIGHT 22
#define ROW_HEIGHT 18
#define FOOTER_HEIGHT 40
#define BUTTON_WIDTH 96
#define BUTTON_HEIGHT 24
#define PADDING 8

#define COL_NAME 6
#define COL_PID 250
#define COL_CPU 330
#define COL_MEM 420
#define COL_STATE 510

#define REFRESH_MS 1000

/* --- Colours --- */

#define COLOR_FRAME RECON_RGB(0xC0, 0xC0, 0xC0)
#define COLOR_LIST_BG RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_HEADER RECON_RGB(0xD4, 0xD4, 0xD4)
#define COLOR_TEXT RECON_RGB(0x10, 0x10, 0x10)
#define COLOR_ROW_ALT RECON_RGB(0xF2, 0xF2, 0xF6)
#define COLOR_ROW_SELECTED RECON_RGB(0x30, 0x50, 0x90)
#define COLOR_ROW_SELECTED_TEXT RECON_RGB(0xFF, 0xFF, 0xFF)
#define COLOR_BUTTON RECON_RGB(0xC8, 0xC8, 0xC8)
#define COLOR_STATUS RECON_RGB(0x30, 0x30, 0x30)
#define COLOR_MENUBAR RECON_RGB(0xC8, 0xC8, 0xC8)
#define COLOR_MENU_HILITE RECON_RGB(0x30, 0x50, 0x90)
#define COLOR_MENU_HILITE_TEXT RECON_RGB(0xFF, 0xFF, 0xFF)

/* Application hit ids, above the framework's own. */
#define HIT_END_TASK (RECON_APPWIN_HIT_USER + 1)
#define HIT_SORT_NAME (RECON_APPWIN_HIT_USER + 2)
#define HIT_SORT_CPU (RECON_APPWIN_HIT_USER + 3)
#define HIT_SORT_MEM (RECON_APPWIN_HIT_USER + 4)
#define HIT_MENU_FILE (RECON_APPWIN_HIT_USER + 5)
#define HIT_MENU_VIEW (RECON_APPWIN_HIT_USER + 6)
#define HIT_DROP_BASE (RECON_APPWIN_HIT_USER + 10)
#define HIT_TAB_BASE (RECON_APPWIN_HIT_USER + 20)
#define HIT_ROW_BASE (RECON_APPWIN_HIT_USER + 100)

#define TAB_HEIGHT 24
#define TAB_WIDTH 110

enum sort_mode {
    SORT_CPU,
    SORT_MEMORY,
    SORT_NAME,
};

enum open_menu {
    MENU_NONE,
    MENU_FILE,
    MENU_VIEW,
};

/*
 * Applications are what ReconOS started; Processes is everything else running
 * in userspace. Kernel workers appear in neither: ReconOS did not start them
 * and cannot usefully act on them.
 */
enum tab {
    TAB_APPLICATIONS,
    TAB_PROCESSES,
};

static const char *const TAB_LABELS[] = { "Applications", "Processes" };
#define TAB_COUNT ((int)(sizeof(TAB_LABELS) / sizeof(TAB_LABELS[0])))

static const char *const FILE_ITEMS[] = { "New Task", "Exit" };
static const char *const VIEW_ITEMS[] = { "Sort by Name", "Sort by CPU", "Sort by Memory" };

#define FILE_COUNT ((int)(sizeof(FILE_ITEMS) / sizeof(FILE_ITEMS[0])))
#define VIEW_COUNT ((int)(sizeof(VIEW_ITEMS) / sizeof(VIEW_ITEMS[0])))

struct recon_taskmgr {
    struct recon_server *server;
    struct recon_font *font;
    struct recon_appwin *win;
    struct recon_proc_snapshot *snapshot;

    enum sort_mode sort;
    enum tab tab;
    enum open_menu menu;
    int scroll;
    pid_t selected_pid; /* survives re-sorting, unlike a row index */

    /* Rows drawn last time, so scrolling knows its own limits. */
    int rows_visible;
    /* Rows the current tab actually has, for the same reason. */
    int rows_matching;

    /* The compositor's own session, so its children can be recognised. */
    int own_session;

    struct wl_event_source *timer;
    char status[128];
};

static int menu_item_x(int index) {
    return PADDING + index * 52;
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
        "%d shown   CPU %.0f%%   Memory %zu / %zu MB",
        tm->rows_matching,
        recon_proc_total_cpu_percent(tm->snapshot),
        recon_proc_used_memory_kb(tm->snapshot) / 1024,
        recon_proc_total_memory_kb(tm->snapshot) / 1024);
}

static int on_timer(void *data) {
    struct recon_taskmgr *tm = data;
    sample(tm);
    recon_appwin_refresh(tm->win);
    wl_event_source_timer_update(tm->timer, REFRESH_MS);
    return 0;
}

/*
 * Whether a process belongs in the current tab.
 *
 * ReconOS launches its clients as its own children, so anything sharing its
 * process group was started from the desktop. That is what Applications
 * means here; Processes widens it to all of userspace.
 */
static bool in_current_tab(struct recon_taskmgr *tm, const struct recon_process *proc) {
    if (proc->kernel_thread) {
        return false;
    }
    if (tm->tab == TAB_PROCESSES) {
        return true;
    }
    return proc->session == tm->own_session;
}

/* --- Drawing --- */

static void draw_menubar(struct recon_taskmgr *tm, struct recon_panel *p,
        int x, int y, int w) {
    int ascent = recon_font_ascent(tm->font);
    int baseline = y + (MENUBAR_HEIGHT + ascent) / 2 - 2;

    recon_fill_rect(p, x, y, w, MENUBAR_HEIGHT, COLOR_MENUBAR);
    recon_fill_rect(p, x, y + MENUBAR_HEIGHT - 1, w, 1, RECON_RGB(0x90, 0x90, 0x90));

    static const char *const LABELS[] = { "File", "View" };
    static const uint32_t IDS[] = { HIT_MENU_FILE, HIT_MENU_VIEW };
    static const enum open_menu MENUS[] = { MENU_FILE, MENU_VIEW };

    for (int i = 0; i < 2; i++) {
        int lx = x + menu_item_x(i);
        int lw = recon_text_width(tm->font, LABELS[i]) + 16;
        bool open = (tm->menu == MENUS[i]);

        if (open) {
            recon_fill_rect(p, lx - 8, y + 2, lw, MENUBAR_HEIGHT - 4, COLOR_MENU_HILITE);
        }
        recon_draw_text(p, tm->font, lx, baseline, lw, LABELS[i],
            open ? COLOR_MENU_HILITE_TEXT : COLOR_TEXT);
        recon_hit_add(p, lx - 8, y, lw, MENUBAR_HEIGHT, IDS[i]);
    }
}

static void draw_tabs(struct recon_taskmgr *tm, struct recon_panel *p,
        int x, int y, int w) {
    int ascent = recon_font_ascent(tm->font);

    recon_fill_rect(p, x, y, w, TAB_HEIGHT, COLOR_FRAME);

    for (int i = 0; i < TAB_COUNT; i++) {
        int tx = x + PADDING + i * (TAB_WIDTH + 2);
        bool active = (tm->tab == (enum tab)i);

        /* The active tab is drawn a shade lighter and without a bottom edge,
         * so it reads as continuous with the list below it. */
        recon_fill_rect(p, tx, y + 2, TAB_WIDTH, TAB_HEIGHT - 2,
            active ? COLOR_LIST_BG : COLOR_BUTTON);
        recon_draw_bevel(p, tx, y + 2, TAB_WIDTH, TAB_HEIGHT - 2, false);

        int label_w = recon_text_width(tm->font, TAB_LABELS[i]);
        recon_draw_text(p, tm->font, tx + (TAB_WIDTH - label_w) / 2,
            y + 2 + (TAB_HEIGHT - 2 + ascent) / 2 - 2, TAB_WIDTH - 8,
            TAB_LABELS[i], COLOR_TEXT);
        recon_hit_add(p, tx, y + 2, TAB_WIDTH, TAB_HEIGHT - 2, HIT_TAB_BASE + i);
    }
}

static void draw_header(struct recon_taskmgr *tm, struct recon_panel *p,
        int x, int y, int w) {
    int baseline = y + (HEADER_HEIGHT + recon_font_ascent(tm->font)) / 2 - 2;

    recon_fill_rect(p, x, y, w, HEADER_HEIGHT, COLOR_HEADER);
    recon_fill_rect(p, x, y + HEADER_HEIGHT - 1, w, 1, RECON_RGB(0x80, 0x80, 0x80));

    /* A marker shows which column the list is ordered by. */
    const char *name_label = tm->sort == SORT_NAME ? "Name  v" : "Name";
    const char *cpu_label = tm->sort == SORT_CPU ? "CPU  v" : "CPU";
    const char *mem_label = tm->sort == SORT_MEMORY ? "Memory  v" : "Memory";

    recon_draw_text(p, tm->font, x + COL_NAME, baseline, 200, name_label, COLOR_TEXT);
    recon_draw_text(p, tm->font, x + COL_PID, baseline, 70, "PID", COLOR_TEXT);
    recon_draw_text(p, tm->font, x + COL_CPU, baseline, 80, cpu_label, COLOR_TEXT);
    recon_draw_text(p, tm->font, x + COL_MEM, baseline, 80, mem_label, COLOR_TEXT);
    recon_draw_text(p, tm->font, x + COL_STATE, baseline, 40, "St", COLOR_TEXT);

    recon_hit_add(p, x + COL_NAME, y, COL_PID - COL_NAME, HEADER_HEIGHT, HIT_SORT_NAME);
    recon_hit_add(p, x + COL_CPU, y, COL_MEM - COL_CPU, HEADER_HEIGHT, HIT_SORT_CPU);
    recon_hit_add(p, x + COL_MEM, y, COL_STATE - COL_MEM, HEADER_HEIGHT, HIT_SORT_MEM);
}

static void draw_rows(struct recon_taskmgr *tm, struct recon_panel *p,
        int x, int y, int w, int rows) {
    int ascent = recon_font_ascent(tm->font);
    recon_fill_rect(p, x, y, w, rows * ROW_HEIGHT, COLOR_LIST_BG);

    /* Walk the snapshot, skipping what this tab does not show, and take the
     * slice the scroll position asks for. */
    size_t count = recon_proc_count(tm->snapshot);
    int matching = 0;
    int row = 0;

    for (size_t index = 0; index < count && row < rows; index++) {
        const struct recon_process *proc = recon_proc_at(tm->snapshot, index);
        if (proc == NULL || !in_current_tab(tm, proc)) {
            continue;
        }
        if (matching++ < tm->scroll) {
            continue;
        }

        int ry = y + row * ROW_HEIGHT;
        int baseline = ry + (ROW_HEIGHT + ascent) / 2 - 2;
        bool selected = (proc->pid == tm->selected_pid);

        if (selected) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_SELECTED);
        } else if (row % 2 == 1) {
            /* Banding makes a long list easier to read across. */
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        recon_color text = selected ? COLOR_ROW_SELECTED_TEXT : COLOR_TEXT;
        char buffer[32];

        recon_draw_text(p, tm->font, x + COL_NAME, baseline,
            COL_PID - COL_NAME - 10, proc->name, text);

        snprintf(buffer, sizeof(buffer), "%d", (int)proc->pid);
        recon_draw_text(p, tm->font, x + COL_PID, baseline, 70, buffer, text);

        snprintf(buffer, sizeof(buffer), "%.1f%%", proc->cpu_percent);
        recon_draw_text(p, tm->font, x + COL_CPU, baseline, 80, buffer, text);

        if (proc->memory_kb >= 1024) {
            snprintf(buffer, sizeof(buffer), "%.1f MB", proc->memory_kb / 1024.0);
        } else {
            snprintf(buffer, sizeof(buffer), "%zu KB", proc->memory_kb);
        }
        recon_draw_text(p, tm->font, x + COL_MEM, baseline, 80, buffer, text);

        snprintf(buffer, sizeof(buffer), "%c", proc->state);
        recon_draw_text(p, tm->font, x + COL_STATE, baseline, 30, buffer, text);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + row);
        row++;
    }

    /* Total for this tab, so scrolling knows how far it may go. */
    int total = 0;
    for (size_t index = 0; index < count; index++) {
        const struct recon_process *proc = recon_proc_at(tm->snapshot, index);
        if (proc != NULL && in_current_tab(tm, proc)) {
            total++;
        }
    }
    tm->rows_matching = total;
}

static void draw_footer(struct recon_taskmgr *tm, struct recon_panel *p,
        int x, int y, int w) {
    int ascent = recon_font_ascent(tm->font);
    recon_fill_rect(p, x, y, w, FOOTER_HEIGHT, COLOR_FRAME);

    int bx = x + w - PADDING - BUTTON_WIDTH;
    int by = y + (FOOTER_HEIGHT - BUTTON_HEIGHT) / 2;
    recon_fill_rect(p, bx, by, BUTTON_WIDTH, BUTTON_HEIGHT, COLOR_BUTTON);
    recon_draw_bevel(p, bx, by, BUTTON_WIDTH, BUTTON_HEIGHT, false);
    recon_draw_text(p, tm->font, bx + 16, by + (BUTTON_HEIGHT + ascent) / 2 - 2,
        BUTTON_WIDTH - 20, "End Task", COLOR_TEXT);
    recon_hit_add(p, bx, by, BUTTON_WIDTH, BUTTON_HEIGHT, HIT_END_TASK);

    recon_draw_text(p, tm->font, x + PADDING, y + (FOOTER_HEIGHT + ascent) / 2 - 2,
        bx - x - PADDING * 2, tm->status, COLOR_STATUS);
}

/* Drawn last, so it covers the rows beneath it. */
static void draw_dropdown(struct recon_taskmgr *tm, struct recon_panel *p,
        int x, int y) {
    if (tm->menu == MENU_NONE) {
        return;
    }

    const char *const *items = tm->menu == MENU_FILE ? FILE_ITEMS : VIEW_ITEMS;
    int count = tm->menu == MENU_FILE ? FILE_COUNT : VIEW_COUNT;
    int dx = x + menu_item_x(tm->menu == MENU_FILE ? 0 : 1) - 8;
    int dy = y + MENUBAR_HEIGHT;
    int height = count * DROPDOWN_ITEM_HEIGHT + 4;
    int ascent = recon_font_ascent(tm->font);

    recon_fill_rect(p, dx, dy, DROPDOWN_WIDTH, height, COLOR_MENUBAR);
    recon_draw_bevel(p, dx, dy, DROPDOWN_WIDTH, height, false);
    recon_stroke_rect(p, dx, dy, DROPDOWN_WIDTH, height, RECON_RGB(0x40, 0x40, 0x40));

    for (int i = 0; i < count; i++) {
        int iy = dy + 2 + i * DROPDOWN_ITEM_HEIGHT;
        recon_draw_text(p, tm->font, dx + 12,
            iy + (DROPDOWN_ITEM_HEIGHT + ascent) / 2 - 2,
            DROPDOWN_WIDTH - 24, items[i], COLOR_TEXT);
        recon_hit_add(p, dx, iy, DROPDOWN_WIDTH, DROPDOWN_ITEM_HEIGHT,
            HIT_DROP_BASE + i);
    }
}

static void taskmgr_draw(void *user, struct recon_panel *p, int x, int y, int w, int h) {
    struct recon_taskmgr *tm = user;

    recon_fill_rect(p, x, y, w, h, COLOR_FRAME);

    int menubar_y = y;
    int tabs_y = menubar_y + MENUBAR_HEIGHT;
    int header_y = tabs_y + TAB_HEIGHT;
    int rows_y = header_y + HEADER_HEIGHT;
    int rows_area = h - MENUBAR_HEIGHT - TAB_HEIGHT - HEADER_HEIGHT - FOOTER_HEIGHT;
    int rows = rows_area > 0 ? rows_area / ROW_HEIGHT : 0;
    tm->rows_visible = rows;

    draw_menubar(tm, p, x, menubar_y, w);
    draw_tabs(tm, p, x, tabs_y, w);
    draw_header(tm, p, x, header_y, w);
    draw_rows(tm, p, x, rows_y, w, rows);
    draw_footer(tm, p, x, y + h - FOOTER_HEIGHT, w);

    /* A sunken edge so the list reads as inset. */
    recon_stroke_rect(p, x - 1, header_y - 1, w + 2,
        HEADER_HEIGHT + rows * ROW_HEIGHT + 2, RECON_RGB(0x80, 0x80, 0x80));

    draw_dropdown(tm, p, x, menubar_y);
}

/* The process shown on a given visible row, or NULL. */
static const struct recon_process *process_for_row(struct recon_taskmgr *tm, int row) {
    size_t count = recon_proc_count(tm->snapshot);
    int matching = 0;
    int current = 0;

    for (size_t index = 0; index < count; index++) {
        const struct recon_process *proc = recon_proc_at(tm->snapshot, index);
        if (proc == NULL || !in_current_tab(tm, proc)) {
            continue;
        }
        if (matching++ < tm->scroll) {
            continue;
        }
        if (current++ == row) {
            return proc;
        }
    }
    return NULL;
}

/* --- Input --- */

static void end_selected_task(struct recon_taskmgr *tm) {
    if (tm->selected_pid <= 0) {
        snprintf(tm->status, sizeof(tm->status), "Select a process first");
        return;
    }

    /* Ask politely. A program given a terminate request can save and close;
     * forcing it is a separate decision. */
    if (recon_proc_terminate(tm->selected_pid)) {
        snprintf(tm->status, sizeof(tm->status),
            "Asked process %d to close", (int)tm->selected_pid);
    } else {
        snprintf(tm->status, sizeof(tm->status),
            "Could not end process %d", (int)tm->selected_pid);
    }
    tm->selected_pid = 0;
}

static bool taskmgr_click(void *user, uint32_t hit_id, int cx, int cy, bool pressed) {
    struct recon_taskmgr *tm = user;
    if (!pressed) {
        return false;
    }

    /* An open dropdown covers whatever is beneath it, so it answers first. */
    if (tm->menu != MENU_NONE) {
        if (hit_id >= HIT_DROP_BASE && hit_id < HIT_ROW_BASE) {
            int index = (int)(hit_id - HIT_DROP_BASE);
            enum open_menu which = tm->menu;
            tm->menu = MENU_NONE;

            if (which == MENU_FILE) {
                if (index == 0) {
                    /* New Task starts the configured terminal, the nearest
                     * thing to a run box until there is one. */
                    recon_spawn(tm->server, NULL);
                    snprintf(tm->status, sizeof(tm->status), "Started a new task");
                } else {
                    recon_appwin_hide(tm->win);
                }
            } else {
                tm->sort = (index == 0) ? SORT_NAME
                    : (index == 1) ? SORT_CPU : SORT_MEMORY;
                apply_sort(tm);
            }
            return true;
        }

        if (hit_id != HIT_MENU_FILE && hit_id != HIT_MENU_VIEW) {
            tm->menu = MENU_NONE;
            return true;
        }
    }

    switch (hit_id) {
    case HIT_MENU_FILE:
        tm->menu = (tm->menu == MENU_FILE) ? MENU_NONE : MENU_FILE;
        return true;
    case HIT_MENU_VIEW:
        tm->menu = (tm->menu == MENU_VIEW) ? MENU_NONE : MENU_VIEW;
        return true;
    case HIT_END_TASK:
        end_selected_task(tm);
        return true;
    case HIT_SORT_NAME:
        tm->sort = SORT_NAME;
        apply_sort(tm);
        return true;
    case HIT_SORT_CPU:
        tm->sort = SORT_CPU;
        apply_sort(tm);
        return true;
    case HIT_SORT_MEM:
        tm->sort = SORT_MEMORY;
        apply_sort(tm);
        return true;
    default:
        break;
    }

    if (hit_id >= HIT_TAB_BASE && hit_id < HIT_ROW_BASE) {
        int index = (int)(hit_id - HIT_TAB_BASE);
        if (index >= 0 && index < TAB_COUNT) {
            tm->tab = (enum tab)index;
            tm->scroll = 0;
        }
        return true;
    }

    if (hit_id >= HIT_ROW_BASE) {
        const struct recon_process *proc =
            process_for_row(tm, (int)(hit_id - HIT_ROW_BASE));
        /* Remember the pid, not the row: the list re-sorts underneath it. */
        tm->selected_pid = proc != NULL ? proc->pid : 0;
        return true;
    }

    return false;
}

static void taskmgr_scroll(void *user, double delta) {
    struct recon_taskmgr *tm = user;
    int count = tm->rows_matching;
    int max_scroll = count > tm->rows_visible ? count - tm->rows_visible : 0;

    tm->scroll += (delta > 0) ? 3 : -3;
    if (tm->scroll > max_scroll) {
        tm->scroll = max_scroll;
    }
    if (tm->scroll < 0) {
        tm->scroll = 0;
    }
}

/* Sampling follows visibility: a hidden window should cost nothing. */
static void taskmgr_visibility(void *user, bool visible) {
    struct recon_taskmgr *tm = user;

    if (visible) {
        /* Two samples in quick succession: CPU is a rate, so the first has
         * nothing to compare against and every process would read zero. */
        sample(tm);
        sample(tm);
        if (tm->timer != NULL) {
            wl_event_source_timer_update(tm->timer, REFRESH_MS);
        }
    } else {
        tm->menu = MENU_NONE;
        if (tm->timer != NULL) {
            wl_event_source_timer_update(tm->timer, 0);
        }
    }
}

static void taskmgr_destroy(void *user) {
    struct recon_taskmgr *tm = user;
    if (tm->timer != NULL) {
        wl_event_source_remove(tm->timer);
    }
    recon_proc_snapshot_destroy(tm->snapshot);
    free(tm);
}

static const struct recon_appwin_impl TASKMGR_IMPL = {
    .title = "ReconOS Task Manager",
    .default_width = 560,
    .default_height = 440,
    .min_width = 420,
    .min_height = 260,
    .draw = taskmgr_draw,
    .click = taskmgr_click,
    .scroll = taskmgr_scroll,
    .visibility = taskmgr_visibility,
    .destroy = taskmgr_destroy,
};

struct recon_appwin *recon_taskmgr_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_taskmgr *tm = calloc(1, sizeof(*tm));
    if (tm == NULL) {
        return NULL;
    }

    tm->server = server;
    tm->font = font;
    tm->sort = SORT_CPU;
    tm->tab = TAB_APPLICATIONS;
    tm->own_session = (int)getsid(0);
    snprintf(tm->status, sizeof(tm->status), "Reading processes...");

    tm->snapshot = recon_proc_snapshot_create();
    if (tm->snapshot == NULL) {
        free(tm);
        return NULL;
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
    tm->timer = wl_event_loop_add_timer(loop, on_timer, tm);

    tm->win = recon_appwin_create(server, font, &TASKMGR_IMPL, tm);
    if (tm->win == NULL) {
        taskmgr_destroy(tm);
        return NULL;
    }
    return tm->win;
}
