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

#include "recon_apps.h"
#include "recon_appwin.h"
#include "recon_icons.h"
#include "recon_procinfo.h"
#include "recon_server.h"
#include "recon_modules.h"
#include "recon_shell.h"
#include "recon_taskmgr.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_users.h"

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

/*
 * Columns are data rather than constants, because they can be dragged.
 *
 * They used to be five #defines holding x positions, which meant the widths
 * were not represented anywhere -- a column's width was the gap to the next
 * one's offset, and there was nothing to change when somebody wanted a wider
 * Name. Widths are the real quantity; positions are worked out from them.
 */
#define COLUMNS_MAX 6
#define COLUMN_MIN 40
#define COL_PAD 6

/* How close to a boundary counts as grabbing it. Wide enough to hit without
 * aiming, narrow enough not to swallow clicks meant for the header itself. */
#define DIVIDER_GRAB 5

#define REFRESH_MS 1000

/* --- Colours --- */

#define COLOR_FRAME THEME(WINDOW_FRAME)
#define COLOR_LIST_BG THEME(SURFACE)
#define COLOR_HEADER THEME(SURFACE_HEADER)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_ROW_ALT THEME(SURFACE_ALT)
#define COLOR_ROW_SELECTED THEME(SELECTION)
#define COLOR_ROW_SELECTED_TEXT THEME(SELECTION_TEXT)
#define COLOR_BUTTON THEME(BUTTON)
#define COLOR_STATUS THEME(SURFACE_TEXT_DIM)
#define COLOR_MENUBAR THEME(MENU)
#define COLOR_MENU_HILITE THEME(MENU_HILITE)
#define COLOR_MENU_HILITE_TEXT THEME(MENU_HILITE_TEXT)
/* For the one row in a list that wants acting on. */
#define COLOR_ALERT THEME(WARNING)

/* Application hit ids, above the framework's own. */
#define HIT_END_TASK (RECON_APPWIN_HIT_USER + 1)
#define HIT_SORT_NAME (RECON_APPWIN_HIT_USER + 2)
#define HIT_SORT_CPU (RECON_APPWIN_HIT_USER + 3)
#define HIT_SORT_MEM (RECON_APPWIN_HIT_USER + 4)
#define HIT_MENU_FILE (RECON_APPWIN_HIT_USER + 5)
#define HIT_MENU_VIEW (RECON_APPWIN_HIT_USER + 6)
#define HIT_DROP_BASE (RECON_APPWIN_HIT_USER + 10)
#define HIT_RUN_TASK (RECON_APPWIN_HIT_USER + 7)
#define HIT_DISCONNECT (RECON_APPWIN_HIT_USER + 8)
#define HIT_MANAGE_USERS (RECON_APPWIN_HIT_USER + 9)
#define HIT_TAB_BASE (RECON_APPWIN_HIT_USER + 20)
#define HIT_DIVIDER_BASE (RECON_APPWIN_HIT_USER + 30)
#define HIT_EXPAND_BASE (RECON_APPWIN_HIT_USER + 50)
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
 * Applications lists open windows; Processes lists running programs.
 *
 * These are genuinely different things, and listing windows is what makes the
 * Applications view honest. The calculator and this task manager are built
 * into ReconOS and share its process, so no process list could ever show them
 * separately -- while sudo, which is a process, is not an application by any
 * useful definition.
 */
enum tab {
    TAB_APPLICATIONS,
    TAB_PROCESSES,
    TAB_USERS,
};

static const char *const TAB_LABELS[] = { "Applications", "Processes", "Users" };
#define TAB_COUNT ((int)(sizeof(TAB_LABELS) / sizeof(TAB_LABELS[0])))

struct column {
    const char *label;
    int width;
};

/*
 * The starting widths for each tab. A user drags from here; nothing resets
 * them afterwards, so a window remembers its layout for as long as it lives.
 */
static const struct column DEFAULT_COLUMNS[TAB_COUNT][COLUMNS_MAX] = {
    [TAB_APPLICATIONS] = {
        { "Task", 240 }, { "Status", 120 }, { "Type", 90 }, { "Memory", 110 },
    },
    [TAB_PROCESSES] = {
        { "Name", 220 }, { "PID", 70 }, { "CPU", 80 }, { "Memory", 100 },
        { "State", 60 },
    },
    /*
     * The figures on this tab are the machine's, attributed to the account
     * that is using it. ReconOS runs as one process, so there is no honest
     * per-account split to report yet -- an account that is signed out shows
     * dashes rather than zeroes, because zero would be a claim and a dash is
     * an admission.
     */
    /* Narrower than the others so all six fit the window it opens at: a
     * column past the right edge is a column nobody knows is there. */
    [TAB_USERS] = {
        { "User", 150 }, { "Status", 100 }, { "CPU", 60 }, { "Memory", 90 },
        { "Disk", 60 }, { "Network", 70 },
    },
};

static const int COLUMN_COUNT[TAB_COUNT] = { 4, 5, 6 };

static const char *const FILE_ITEMS[] = { "Exit" };
static const char *const VIEW_ITEMS[] = { "Sort by Name", "Sort by CPU", "Sort by Memory" };

#define FILE_COUNT ((int)(sizeof(FILE_ITEMS) / sizeof(FILE_ITEMS[0])))
#define VIEW_COUNT ((int)(sizeof(VIEW_ITEMS) / sizeof(VIEW_ITEMS[0])))

struct recon_taskmgr {
    /* The application a force-close question is about, or 0. */
    uint32_t pending_force;

    struct recon_server *server;
    struct recon_font *font;
    struct recon_appwin *win;
    struct recon_proc_snapshot *snapshot;

    enum sort_mode sort;
    enum tab tab;
    enum open_menu menu;
    int scroll;
    pid_t selected_pid; /* survives re-sorting, unlike a row index */
    int selected_row;   /* Applications view; windows have no stable id here */

    /* Rows drawn last time, so scrolling knows its own limits. */
    int rows_visible;
    /* Rows the current tab actually has, for the same reason. */
    int rows_matching;

    /* The compositor's own session, so its children can be recognised. */
    int own_session;

    /* Column widths, per tab, as the user has left them. */
    struct column columns[TAB_COUNT][COLUMNS_MAX];

    /*
     * A column boundary being dragged: which one, where the pointer went
     * down, and how wide the column was then. Measured from the start of the
     * drag rather than accumulated frame by frame, so a fast drag that skips
     * pixels does not drift away from the pointer.
     */
    int dragging_column;   /* -1 when nothing is being dragged. */
    int drag_start_x;
    int drag_start_width;

    /* Which account's applications are shown expanded on the Users tab. */
    int expanded_user;     /* -1 for none. */

    /* Which dropdown entry the pointer is over, or -1. */
    int menu_hover;

    /* Typing the name of something to run. */
    bool running_task;
    struct recon_edit task_name;

    struct wl_event_source *timer;
    char status[128];
};

/* --- Columns --- */

static int column_count(const struct recon_taskmgr *tm) {
    return COLUMN_COUNT[tm->tab];
}

/* Where a column starts, relative to the left edge of the list. */
static int column_x(const struct recon_taskmgr *tm, int index) {
    int x = COL_PAD;
    for (int i = 0; i < index && i < COLUMNS_MAX; i++) {
        x += tm->columns[tm->tab][i].width;
    }
    return x;
}

static int column_width(const struct recon_taskmgr *tm, int index) {
    return tm->columns[tm->tab][index].width - COL_PAD;
}

/*
 * Draw a row's cells into the column layout.
 *
 * One place that knows where a column is, so the header and the rows cannot
 * drift apart -- which they would, in three separate row drawers, the first
 * time a width changed.
 */
static void draw_cells(struct recon_taskmgr *tm, struct recon_panel *p,
        int x, int baseline, const char *const *cells,
        const recon_color *colors, recon_color fallback) {
    int count = column_count(tm);
    for (int i = 0; i < count; i++) {
        if (cells[i] == NULL) {
            continue;
        }
        recon_draw_text(p, tm->font, x + column_x(tm, i), baseline,
            column_width(tm, i), cells[i],
            colors != NULL ? colors[i] : fallback);
    }
}

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

/* Kernel workers are not programs ReconOS started or can act on. */
static bool in_current_tab(struct recon_taskmgr *tm, const struct recon_process *proc) {
    (void)tm;
    return !proc->kernel_thread;
}

/*
 * One row of the Applications view.
 *
 * Taken from the application table rather than assembled here from windows.
 * "What is running" is a question about applications, and a window list is not
 * the same answer: several built-in applications share ReconOS's process, and
 * a client program can own more than one window.
 */
struct app_row {
    struct recon_app_info info;
};

/* Memory of the process behind a window, or 0 if it has none of its own. */
static size_t memory_for_pid(struct recon_taskmgr *tm, pid_t pid) {
    if (pid <= 0) {
        return 0;
    }
    size_t count = recon_proc_count(tm->snapshot);
    for (size_t i = 0; i < count; i++) {
        const struct recon_process *proc = recon_proc_at(tm->snapshot, i);
        if (proc != NULL && proc->pid == pid) {
            return proc->memory_kb;
        }
    }
    return 0;
}

#define MAX_APP_ROWS 32

/* Ask the application table what is running, and attribute memory where a
 * process can be pointed at. */
static int collect_apps(struct recon_taskmgr *tm, struct app_row *rows, int max) {
    int total = recon_apps_refresh();
    int count = 0;

    for (int i = 0; i < total && count < max; i++) {
        if (!recon_apps_at(i, &rows[count].info)) {
            continue;
        }
        /* Only where there is a process to look up. A built-in application
         * already carries the size of its own window, and overwriting that
         * with the zero a missing pid produces was what left the column
         * empty. */
        if (rows[count].info.pid > 0) {
            rows[count].info.memory_kb = memory_for_pid(tm, rows[count].info.pid);
        }
        count++;
    }
    return count;
}

/*
 * What kind of thing this is.
 *
 * "ReconOS" named where it came from and said nothing about what it is. A
 * column headed Type should answer "what type of application is this",
 * and the two types that exist are the ones the system ships and the ones
 * that connect to it.
 */
static const char *app_kind_name(const struct recon_app_info *info) {
    return info->kind == RECON_APP_KIND_BUILTIN
        ? "System app" : "Client app";
}

static const char *app_state_name(const struct recon_app_info *info) {
    switch (info->state) {
    case RECON_APP_NOT_RESPONDING: return "Not responding";
    case RECON_APP_MINIMIZED:      return "Minimized";
    default:                       return "Running";
    }
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
    int count = column_count(tm);

    recon_fill_rect(p, x, y, w, HEADER_HEIGHT, COLOR_HEADER);
    recon_fill_rect(p, x, y + HEADER_HEIGHT - 1, w, 1, RECON_RGB(0x80, 0x80, 0x80));

    /* Which column the Processes list is ordered by, if any. */
    static const uint32_t SORT_HIT[] = {
        HIT_SORT_NAME, 0, HIT_SORT_CPU, HIT_SORT_MEM, 0,
    };

    for (int i = 0; i < count; i++) {
        int cx = x + column_x(tm, i);

        char label[48];
        bool sorted_by = tm->tab == TAB_PROCESSES &&
            ((i == 0 && tm->sort == SORT_NAME) ||
             (i == 2 && tm->sort == SORT_CPU) ||
             (i == 3 && tm->sort == SORT_MEMORY));
        snprintf(label, sizeof(label), "%s%s",
            tm->columns[tm->tab][i].label, sorted_by ? "  v" : "");

        recon_draw_text(p, tm->font, cx, baseline, column_width(tm, i),
            label, COLOR_TEXT);

        /* Clicking a heading sorts by it, where that means anything. */
        if (tm->tab == TAB_PROCESSES && i < (int)(sizeof(SORT_HIT) /
                sizeof(SORT_HIT[0])) && SORT_HIT[i] != 0) {
            recon_hit_add(p, cx, y, column_width(tm, i), HEADER_HEIGHT,
                SORT_HIT[i]);
        }

        /*
         * The boundary at the right of this column, drawn as a rule and
         * offered as a grab handle. Added after the heading so it wins the
         * click where the two overlap -- dragging a boundary is the more
         * precise gesture, and losing it to a sort would be maddening.
         */
        if (i < count - 1) {
            int bx = x + column_x(tm, i + 1) - COL_PAD / 2;
            recon_fill_rect(p, bx, y + 3, 1, HEADER_HEIGHT - 6,
                RECON_RGB(0x80, 0x80, 0x80));
            recon_hit_add(p, bx - DIVIDER_GRAB, y, DIVIDER_GRAB * 2,
                HEADER_HEIGHT, HIT_DIVIDER_BASE + i);
        }
    }
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

        char pid[16], cpu[16], memory[24], state[4];
        snprintf(pid, sizeof(pid), "%d", (int)proc->pid);
        snprintf(cpu, sizeof(cpu), "%.1f%%", proc->cpu_percent);
        if (proc->memory_kb >= 1024) {
            snprintf(memory, sizeof(memory), "%.1f MB", proc->memory_kb / 1024.0);
        } else {
            snprintf(memory, sizeof(memory), "%zu KB", proc->memory_kb);
        }
        snprintf(state, sizeof(state), "%c", proc->state);

        const char *cells[COLUMNS_MAX] = { proc->name, pid, cpu, memory, state };
        draw_cells(tm, p, x, baseline, cells, NULL, text);

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

static void draw_app_rows(struct recon_taskmgr *tm, struct recon_panel *p,
        int x, int y, int w, int rows) {
    int ascent = recon_font_ascent(tm->font);
    recon_fill_rect(p, x, y, w, rows * ROW_HEIGHT, COLOR_LIST_BG);

    struct app_row list[MAX_APP_ROWS];
    int count = collect_apps(tm, list, MAX_APP_ROWS);
    tm->rows_matching = count;

    for (int row = 0; row < rows && tm->scroll + row < count; row++) {
        const struct app_row *entry = &list[tm->scroll + row];
        int ry = y + row * ROW_HEIGHT;
        int baseline = ry + (ROW_HEIGHT + ascent) / 2 - 2;
        bool selected = (tm->selected_row == tm->scroll + row);

        if (selected) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_SELECTED);
        } else if (row % 2 == 1) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
        }

        const struct recon_app_info *info = &entry->info;

        recon_color text = selected ? COLOR_ROW_SELECTED_TEXT : COLOR_TEXT;

        /* An application that is not answering is said so in the accent
         * colour: it is the one row in the list that wants acting on. */
        recon_color state_colour = text;
        if (info->state == RECON_APP_NOT_RESPONDING && !selected) {
            state_colour = COLOR_ALERT;
        }

        /* Applications built into ReconOS share its process, so they have no
         * memory figure of their own to report. */
        char memory[32];
        if (info->memory_kb >= 1024) {
            snprintf(memory, sizeof(memory), "%.1f MB", info->memory_kb / 1024.0);
        } else if (info->memory_kb > 0) {
            snprintf(memory, sizeof(memory), "%zu KB", info->memory_kb);
        } else {
            snprintf(memory, sizeof(memory), "-");
        }

        const char *cells[COLUMNS_MAX] = {
            info->name, app_state_name(info), app_kind_name(info), memory,
        };
        const recon_color colors[COLUMNS_MAX] = {
            text, state_colour, text, text, text, text,
        };
        draw_cells(tm, p, x, baseline, cells, colors, text);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + row);
    }
}

/*
 * --- The Users tab ---
 *
 * Who has an account, which of them is using the machine, and what that is
 * costing. An account can be expanded to show what it has open.
 *
 * The figures are honest about their limits. ReconOS is a single process, so
 * what the machine is using cannot be split between accounts; the account
 * that is signed in gets the machine's figures because it is the only one
 * spending anything, and every other account gets a dash. A dash says "not
 * known"; a zero would say "none", which is a different and false claim.
 * When there is a kernel underneath keeping per-account accounts, these
 * become real numbers without this tab changing shape.
 */
static void draw_user_rows(struct recon_taskmgr *tm, struct recon_panel *p,
        int x, int y, int w, int rows) {
    int ascent = recon_font_ascent(tm->font);
    recon_fill_rect(p, x, y, w, rows * ROW_HEIGHT, COLOR_LIST_BG);

    const char *signed_in = recon_users_current();
    int accounts = recon_users_count();

    /* Applications, fetched once: the expanded account lists them. */
    struct app_row apps[MAX_APP_ROWS];
    int app_count = collect_apps(tm, apps, MAX_APP_ROWS);

    int row = 0;
    int drawn = 0;   /* Counting expanded children too, for scrolling. */

    for (int i = 0; i < accounts && row < rows; i++) {
        struct recon_user user;
        if (!recon_users_at(i, &user)) {
            break;
        }

        bool active = signed_in != NULL && strcmp(signed_in, user.name) == 0;
        bool expanded = (tm->expanded_user == i);

        if (drawn++ >= tm->scroll) {
            int ry = y + row * ROW_HEIGHT;
            int baseline = ry + (ROW_HEIGHT + ascent) / 2 - 2;
            bool selected = (tm->selected_row == i);

            if (selected) {
                recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_SELECTED);
            } else if (row % 2 == 1) {
                recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
            }

            recon_color text = selected ? COLOR_ROW_SELECTED_TEXT : COLOR_TEXT;

            char cpu[16] = "-", memory[24] = "-";
            const char *disk = "-";
            const char *network = "-";
            if (active) {
                snprintf(cpu, sizeof(cpu), "%.0f%%",
                    recon_proc_total_cpu_percent(tm->snapshot));
                snprintf(memory, sizeof(memory), "%zu MB",
                    recon_proc_used_memory_kb(tm->snapshot) / 1024);
            }

            /*
             * A disclosure triangle, and the name indented past it, so the
             * account reads as something with contents rather than as a row.
             */
            int tx = x + column_x(tm, 0) + 2;
            int ty = ry + ROW_HEIGHT / 2;
            for (int t = 0; t < 4; t++) {
                if (expanded) {
                    recon_fill_rect(p, tx + 3 - t, ty - 1 + t, t * 2 + 1, 1, text);
                } else {
                    recon_fill_rect(p, tx + t, ty - 3 + t, 1, 7 - t * 2, text);
                }
            }
            recon_hit_add(p, x, ry, column_x(tm, 0) + 14, ROW_HEIGHT,
                HIT_EXPAND_BASE + i);

            char name[RECON_USERS_NAME_MAX + 8];
            snprintf(name, sizeof(name), "   %s", user.name);

            const char *cells[COLUMNS_MAX] = {
                name,
                active ? "Signed in"
                       : (user.role == RECON_ROLE_ADMINISTRATOR
                            ? "Administrator" : "Limited"),
                cpu, memory, disk, network,
            };
            draw_cells(tm, p, x, baseline, cells, NULL, text);

            recon_hit_add(p, x + column_x(tm, 0) + 14, ry,
                w - column_x(tm, 0) - 14, ROW_HEIGHT, HIT_ROW_BASE + i);
            row++;
        }

        if (!expanded) {
            continue;
        }

        /* What that account has open. Only the signed-in one has anything:
         * nothing of another account's is running to be listed. */
        for (int a = 0; a < app_count && row < rows; a++) {
            if (!active) {
                break;
            }
            if (drawn++ < tm->scroll) {
                continue;
            }

            int ry = y + row * ROW_HEIGHT;
            int baseline = ry + (ROW_HEIGHT + ascent) / 2 - 2;
            if (row % 2 == 1) {
                recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_ROW_ALT);
            }

            /* Sized for an application's name, not an account's: this row
             * carries what the account is running. */
            char child[128];
            snprintf(child, sizeof(child), "        %s", apps[a].info.name);

            const char *cells[COLUMNS_MAX] = {
                child, app_state_name(&apps[a].info), NULL, NULL, NULL, NULL,
            };
            draw_cells(tm, p, x, baseline, cells, NULL, COLOR_STATUS);
            row++;
        }

        if (expanded && !active && row < rows) {
            if (drawn++ >= tm->scroll) {
                int ry = y + row * ROW_HEIGHT;
                recon_draw_text(p, tm->font, x + column_x(tm, 0) + 20,
                    ry + (ROW_HEIGHT + ascent) / 2 - 2, w - 40,
                    "Nothing of this account's is running.", COLOR_STATUS);
                row++;
            }
        }
    }

    tm->rows_matching = drawn;
}

static void draw_footer(struct recon_taskmgr *tm, struct recon_panel *p,
        int x, int y, int w) {
    int ascent = recon_font_ascent(tm->font);
    recon_fill_rect(p, x, y, w, FOOTER_HEIGHT, COLOR_FRAME);

    int by = y + (FOOTER_HEIGHT - BUTTON_HEIGHT) / 2;

    /* Buttons are laid out from the right, so the rightmost is the one this
     * tab is mostly for. */
    struct {
        const char *label;
        uint32_t id;
    } buttons[3];
    int count = 0;

    if (tm->tab == TAB_USERS) {
        buttons[count++] = (typeof(buttons[0])){ "Manage Accounts",
            HIT_MANAGE_USERS };
        buttons[count++] = (typeof(buttons[0])){ "Disconnect", HIT_DISCONNECT };
        buttons[count++] = (typeof(buttons[0])){ "Run New Task", HIT_RUN_TASK };
    } else {
        buttons[count++] = (typeof(buttons[0])){ "End Task", HIT_END_TASK };
        buttons[count++] = (typeof(buttons[0])){ "Run New Task", HIT_RUN_TASK };
    }

    int bx = x + w - PADDING;
    for (int i = 0; i < count; i++) {
        int width = recon_text_width(tm->font, buttons[i].label) + 24;
        if (width < BUTTON_WIDTH) {
            width = BUTTON_WIDTH;
        }
        bx -= width;

        recon_fill_rect(p, bx, by, width, BUTTON_HEIGHT, COLOR_BUTTON);
        recon_draw_bevel(p, bx, by, width, BUTTON_HEIGHT, false);

        int label_w = recon_text_width(tm->font, buttons[i].label);
        recon_draw_text(p, tm->font, bx + (width - label_w) / 2,
            by + (BUTTON_HEIGHT + ascent) / 2 - 2, width - 8,
            buttons[i].label, COLOR_TEXT);
        recon_hit_add(p, bx, by, width, BUTTON_HEIGHT, buttons[i].id);

        bx -= 6;
    }

    /*
     * The name of something to run goes where the status line is, because
     * that is the space next to the button that asked for it. A field
     * somewhere else would be a field you have to go and find.
     */
    if (tm->running_task) {
        int field_w = bx - x - PADDING * 2;
        if (field_w > 240) {
            field_w = 240;
        }
        if (field_w > 40) {
            recon_edit_draw(p, tm->font, x + PADDING,
                y + (FOOTER_HEIGHT - BUTTON_HEIGHT) / 2, field_w,
                BUTTON_HEIGHT, &tm->task_name);
        }
        return;
    }

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

        /*
         * The entry under the pointer is marked. Without this the menu showed
         * its entries and gave no sign which one a click would take, which is
         * the one thing a menu has to say. Every other menu in the system
         * already does it; this one was written before there was a
         * convention to follow.
         */
        bool hovered = (tm->menu_hover == i);
        if (hovered) {
            recon_fill_rect(p, dx + 1, iy, DROPDOWN_WIDTH - 2,
                DROPDOWN_ITEM_HEIGHT, COLOR_MENU_HILITE);
        }

        recon_draw_text(p, tm->font, dx + 12,
            iy + (DROPDOWN_ITEM_HEIGHT + ascent) / 2 - 2,
            DROPDOWN_WIDTH - 24, items[i],
            hovered ? COLOR_MENU_HILITE_TEXT : COLOR_TEXT);
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
    switch (tm->tab) {
    case TAB_APPLICATIONS: draw_app_rows(tm, p, x, rows_y, w, rows); break;
    case TAB_USERS:        draw_user_rows(tm, p, x, rows_y, w, rows); break;
    default:               draw_rows(tm, p, x, rows_y, w, rows); break;
    }
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

/* The answer to "force this application to stop?". */
static void taskmgr_answered(void *user, int choice) {
    struct recon_taskmgr *tm = user;

    uint32_t id = tm->pending_force;
    tm->pending_force = 0;

    if (choice != 0 || id == 0) {
        snprintf(tm->status, sizeof(tm->status), "Nothing was ended");
        recon_appwin_refresh(tm->win);
        return;
    }

    struct recon_app_info info;
    if (!recon_apps_find(id, &info)) {
        snprintf(tm->status, sizeof(tm->status), "It has already gone");
    } else if (recon_apps_force_end(id)) {
        snprintf(tm->status, sizeof(tm->status), "Ended '%s'", info.name);
    } else {
        snprintf(tm->status, sizeof(tm->status), "Could not end '%s'", info.name);
    }

    recon_appwin_refresh(tm->win);
}

static void end_selected_task(struct recon_taskmgr *tm) {
    if (tm->tab == TAB_APPLICATIONS) {
        struct app_row list[MAX_APP_ROWS];
        int count = collect_apps(tm, list, MAX_APP_ROWS);
        if (tm->selected_row < 0 || tm->selected_row >= count) {
            snprintf(tm->status, sizeof(tm->status), "Select a task first");
            return;
        }

        const struct recon_app_info *info = &list[tm->selected_row].info;

        /*
         * A program that has been asked and has not answered can be stopped
         * outright, after saying what that costs. Offering force before the
         * polite close has been tried would make force the thing people
         * press first, and it loses unsaved work.
         */
        if (recon_apps_can_force(info->id) &&
                info->state == RECON_APP_NOT_RESPONDING) {
            tm->pending_force = info->id;

            char message[256];
            snprintf(message, sizeof(message),
                "'%s' is not responding. End it now? Anything unsaved is lost.",
                info->name);

            const char *buttons[2] = { "End Now", "Wait" };
            recon_appwin_ask(tm->win, "End Application", message, buttons, 2,
                taskmgr_answered);
            return;
        }

        if (recon_apps_end(info->id)) {
            snprintf(tm->status, sizeof(tm->status),
                info->kind == RECON_APP_KIND_BUILTIN
                    ? "Closed '%s'"
                    : "Asked '%s' to close",
                info->name);
        } else {
            snprintf(tm->status, sizeof(tm->status),
                "Could not end '%s'", info->name);
        }

        tm->selected_row = -1;
        return;
    }

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

/*
 * Dragging a column boundary.
 *
 * The width follows the pointer's distance from where the drag began rather
 * than being nudged by each step, so a drag that outruns the redraw lands
 * where the pointer is instead of somewhere behind it.
 */
static void drag_column_to(struct recon_taskmgr *tm, int cx) {
    if (tm->dragging_column < 0) {
        return;
    }
    int width = tm->drag_start_width + (cx - tm->drag_start_x);
    if (width < COLUMN_MIN) {
        width = COLUMN_MIN;
    }
    tm->columns[tm->tab][tm->dragging_column].width = width;
}

/* Open whatever was typed into Run New Task. */
static void run_typed_task(struct recon_taskmgr *tm) {
    const char *name = tm->task_name.text;

    if (*name == '\0') {
        tm->running_task = false;
        recon_edit_end(&tm->task_name);
        tm->status[0] = '\0';
        return;
    }

    /*
     * Resolved through the application registry, so "terminal" finds
     * "Terminal" and "ReconOS Terminal" finds it too -- the same rule
     * shortcuts and the Start menu use. A second rule for the same question
     * is a second rule to keep in step.
     */
    const char *found = recon_installed_app_resolve(name);
    if (found == NULL) {
        snprintf(tm->status, sizeof(tm->status),
            "Nothing installed is called '%s'.", name);
        return;
    }

    tm->running_task = false;
    recon_edit_end(&tm->task_name);

    recon_shell_open_named(tm->server->shell, found);
    snprintf(tm->status, sizeof(tm->status), "Started '%s'.", found);
}

static bool taskmgr_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_taskmgr *tm = user;

    if (!tm->running_task) {
        return false;
    }

    switch (recon_edit_key(&tm->task_name, sym, modifiers)) {
    case RECON_EDIT_COMMIT:
        run_typed_task(tm);
        return true;
    case RECON_EDIT_CANCEL:
        tm->running_task = false;
        recon_edit_end(&tm->task_name);
        tm->status[0] = '\0';
        return true;
    case RECON_EDIT_CHANGED:
    case RECON_EDIT_IGNORED:
        return true;
    }
    return true;
}

/* A menu left open on a window that is no longer in front belongs to
 * nothing, so it closes when the window stops being in front. */
static void taskmgr_focus_changed(void *user, bool focused) {
    struct recon_taskmgr *tm = user;
    if (!focused && tm->menu != MENU_NONE) {
        tm->menu = MENU_NONE;
        tm->menu_hover = -1;
        recon_appwin_refresh(tm->win);
    }
}

/* Say that a column boundary can be dragged, by changing the pointer over it.
 * It could be dragged before this; nothing on screen said so. */
static const char *taskmgr_cursor(void *user, uint32_t hit_id) {
    struct recon_taskmgr *tm = user;

    if (tm->dragging_column >= 0) {
        return "col-resize";
    }
    if (hit_id >= HIT_DIVIDER_BASE && hit_id < HIT_EXPAND_BASE) {
        return "col-resize";
    }
    return NULL;
}

static void taskmgr_motion(void *user, uint32_t hit_id, int cx, int cy) {
    struct recon_taskmgr *tm = user;
    (void)cy;

    if (tm->dragging_column >= 0) {
        drag_column_to(tm, cx);
        recon_appwin_refresh(tm->win);
        return;
    }

    /* Track the dropdown entry under the pointer, and redraw only when it
     * changes: repainting the window on every step across a menu would be a
     * lot of work to show the same picture. */
    int hover = -1;
    if (tm->menu != MENU_NONE && hit_id >= HIT_DROP_BASE &&
            hit_id < HIT_TAB_BASE) {
        hover = (int)(hit_id - HIT_DROP_BASE);
    }
    if (hover != tm->menu_hover) {
        tm->menu_hover = hover;
        recon_appwin_refresh(tm->win);
    }
}

static bool taskmgr_click(void *user, uint32_t hit_id, int cx, int cy, bool pressed) {
    struct recon_taskmgr *tm = user;

    /* A drag ends wherever the button comes up, including outside the
     * window: a boundary left stuck to the pointer is worse than a boundary
     * in the wrong place. */
    if (!pressed) {
        if (tm->dragging_column >= 0) {
            drag_column_to(tm, cx);
            tm->dragging_column = -1;
            return true;
        }
        return false;
    }

    if (hit_id >= HIT_DIVIDER_BASE && hit_id < HIT_EXPAND_BASE) {
        int index = (int)(hit_id - HIT_DIVIDER_BASE);
        if (index >= 0 && index < column_count(tm)) {
            tm->dragging_column = index;
            tm->drag_start_x = cx;
            tm->drag_start_width = tm->columns[tm->tab][index].width;
        }
        return true;
    }

    if (hit_id >= HIT_EXPAND_BASE && hit_id < HIT_ROW_BASE) {
        int index = (int)(hit_id - HIT_EXPAND_BASE);
        tm->expanded_user = (tm->expanded_user == index) ? -1 : index;
        tm->selected_row = index;
        return true;
    }

    /* An open dropdown covers whatever is beneath it, so it answers first. */
    if (tm->menu != MENU_NONE) {
        if (hit_id >= HIT_DROP_BASE && hit_id < HIT_ROW_BASE) {
            int index = (int)(hit_id - HIT_DROP_BASE);
            enum open_menu which = tm->menu;
            tm->menu = MENU_NONE;

            if (which == MENU_FILE) {
                /* New Task will return when there is a way to name what to
                 * run. Until then Exit is the only honest entry. */
                recon_appwin_hide(tm->win);
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

    /*
     * Run New Task works now that there is a registry of installed
     * applications to run one from. It asks for a name and opens it.
     *
     * Disconnect is still here and still does not, because it needs more than
     * one session to disconnect from and there is one.
     */
    case HIT_RUN_TASK:
        if (tm->running_task) {
            /* Pressing it again is "go", not "start over": the field is
             * already there and the button is the obvious thing to press. */
            run_typed_task(tm);
            return true;
        }
        tm->running_task = true;
        recon_edit_begin(&tm->task_name, "", false);
        snprintf(tm->status, sizeof(tm->status),
            "The name of an application, then Enter.");
        return true;

    case HIT_DISCONNECT:
        snprintf(tm->status, sizeof(tm->status),
            "Disconnect is not built yet: one session at a time so far.");
        return true;

    case HIT_MANAGE_USERS:
        /* This one is real: it is where accounts are managed. */
        recon_shell_open_named(tm->server->shell, "Control Panel");
        snprintf(tm->status, sizeof(tm->status), "Opened the Control Panel.");
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

    /* Bounded at the dividers, which sit between the tabs and the rows. */
    if (hit_id >= HIT_TAB_BASE && hit_id < HIT_DIVIDER_BASE) {
        int index = (int)(hit_id - HIT_TAB_BASE);
        if (index >= 0 && index < TAB_COUNT) {
            tm->tab = (enum tab)index;
            tm->scroll = 0;
            tm->selected_row = -1;
            tm->selected_pid = 0;
            tm->status[0] = '\0';
        }
        return true;
    }

    /* On the Users tab a row id is the account's index, not a screen row:
     * the list has children in it, so the two do not correspond. */
    if (hit_id >= HIT_ROW_BASE && tm->tab == TAB_USERS) {
        tm->selected_row = (int)(hit_id - HIT_ROW_BASE);
        return true;
    }

    if (hit_id >= HIT_ROW_BASE && tm->tab == TAB_APPLICATIONS) {
        tm->selected_row = tm->scroll + (int)(hit_id - HIT_ROW_BASE);
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

/*
 * What this window has open, in words.
 *
 * Not decoration: it is how a test asks which tab is showing and how wide a
 * column is, without measuring a screenshot to find out.
 */
static void taskmgr_describe(void *user, char *out, size_t size) {
    struct recon_taskmgr *tm = user;

    char widths[128];
    size_t used = 0;
    for (int i = 0; i < column_count(tm) && used < sizeof(widths) - 1; i++) {
        int written = snprintf(widths + used, sizeof(widths) - used, "%s%s=%d",
            i > 0 ? " " : "", tm->columns[tm->tab][i].label,
            tm->columns[tm->tab][i].width);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }

    snprintf(out, size,
        "  tab: %s\n"
        "  rows: %d matching, %d visible, scroll %d\n"
        "  selected: row %d, pid %d\n"
        "  expanded account: %d\n"
        "  dragging column: %d\n"
        "  columns: %s\n"
        "  status: %s\n",
        TAB_LABELS[tm->tab], tm->rows_matching, tm->rows_visible, tm->scroll,
        tm->selected_row, (int)tm->selected_pid, tm->expanded_user,
        tm->dragging_column, widths, tm->status);
}

static const struct recon_appwin_impl TASKMGR_IMPL = {
    /*
     * "ReconOS Task Manager" said the system's name twice -- once in the
     * title bar of a ReconOS window, once in the title of a ReconOS
     * application. A tower has a watchtower: the place you go to see what is
     * moving. The old name still finds it, so anything written down before
     * the rename keeps working.
     */
    .title = "Watchtower",
    .icon = RECON_ICON_TASKMGR,
    .default_width = 560,
    .default_height = 440,
    .min_width = 420,
    .min_height = 260,
    .draw = taskmgr_draw,
    .click = taskmgr_click,
    .key = taskmgr_key,
    .motion = taskmgr_motion,
    .cursor = taskmgr_cursor,
    .focus_changed = taskmgr_focus_changed,
    .scroll = taskmgr_scroll,
    .describe = taskmgr_describe,
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
    tm->selected_row = -1;
    tm->own_session = (int)getsid(0);
    tm->dragging_column = -1;
    tm->expanded_user = -1;
    tm->menu_hover = -1;
    memcpy(tm->columns, DEFAULT_COLUMNS, sizeof(tm->columns));
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
