/*
 * The calendar. See include/recon_calendar.h.
 *
 * The date arithmetic is the clock's. There is exactly one piece of code in
 * ReconOS that knows how many days February has, and a calendar with a second
 * copy of that knowledge would be a second place for it to be wrong.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_calendar.h"
#include "recon_clock.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_server.h"
#include "recon_theme.h"
#include "recon_ui.h"

#define COLOR_BG THEME(SURFACE)
#define COLOR_PANEL THEME(SURFACE_ALT)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_DIM THEME(SURFACE_TEXT_DIM)
#define COLOR_SELECTED THEME(SELECTION)
#define COLOR_SELECTED_TEXT THEME(SELECTION_TEXT)
#define COLOR_ACCENT THEME(ACCENT)
#define COLOR_SEPARATOR THEME(MENU_SEPARATOR)
#define COLOR_WARNING THEME(WARNING)
#define COLOR_BAR THEME(BAR)

#define PADDING 10
#define HEADER_HEIGHT 34
#define WEEKDAY_HEIGHT 20
#define BAR_HEIGHT 30
#define SIDE_WIDTH 230

#define HIT_PREVIOUS (RECON_APPWIN_HIT_USER + 1)
#define HIT_NEXT (RECON_APPWIN_HIT_USER + 2)
#define HIT_TODAY (RECON_APPWIN_HIT_USER + 3)
#define HIT_ADD (RECON_APPWIN_HIT_USER + 4)
#define HIT_FIELD (RECON_APPWIN_HIT_USER + 5)
#define HIT_DAY_BASE (RECON_APPWIN_HIT_USER + 100)
#define HIT_EVENT_BASE (RECON_APPWIN_HIT_USER + 200)

static const char *const MONTHS[] = { "January", "February", "March", "April",
    "May", "June", "July", "August", "September", "October", "November",
    "December" };
static const char *const SHORT_DAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu",
    "Fri", "Sat" };

/* --- The events --- */

static struct recon_calendar_event g_events[RECON_CALENDAR_EVENTS_MAX];
static int g_count;
static bool g_loaded;
static char g_error[192];

const char *recon_calendar_last_error(void) {
    return g_error;
}

static void events_path(char *out, size_t size) {
    snprintf(out, size, "%s/%s", recon_fs_user_dir(""), RECON_CALENDAR_FILE);
}

/*
 * One event per line: `YYYY-MM-DD HH:MM text`, or `YYYY-MM-DD -- text` for
 * something with no time.
 *
 * Fixed-width fields at the front so a line can be read without a parser, and
 * so somebody editing the file by hand can see the shape from one example.
 */
static void parse_line(char *line) {
    if (g_count >= RECON_CALENDAR_EVENTS_MAX) {
        return;
    }
    if (line[0] == '#' || line[0] == '\0') {
        return;
    }

    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    int consumed = 0;

    if (sscanf(line, "%4d-%2d-%2d %2d:%2d %n", &year, &month, &day, &hour,
            &minute, &consumed) == 5 && consumed > 0) {
        struct recon_calendar_event *e = &g_events[g_count];
        e->year = year;
        e->month = month;
        e->day = day;
        e->minute_of_day = hour * 60 + minute;
        recon_text_copy(e->text, sizeof(e->text), line + consumed);
        g_count++;
        return;
    }

    consumed = 0;
    if (sscanf(line, "%4d-%2d-%2d -- %n", &year, &month, &day,
            &consumed) == 3 && consumed > 0) {
        struct recon_calendar_event *e = &g_events[g_count];
        e->year = year;
        e->month = month;
        e->day = day;
        e->minute_of_day = -1;
        recon_text_copy(e->text, sizeof(e->text), line + consumed);
        g_count++;
    }

    /* A line that matches neither is left alone and not counted. It is
     * somebody's file; refusing to understand a line is not a reason to
     * delete it, and the writer below only ever rewrites what it parsed. */
}

void recon_calendar_load(void) {
    g_count = 0;
    g_loaded = true;

    char path[RECON_PATH_MAX];
    events_path(path, sizeof(path));

    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        return;   /* No diary yet is not a fault. */
    }

    char *line = text;
    while (line != NULL && *line != '\0') {
        char *end = strchr(line, '\n');
        if (end != NULL) {
            *end = '\0';
        }
        parse_line(line);
        line = (end != NULL) ? end + 1 : NULL;
    }

    free(text);
}

static bool save(void) {
    char text[RECON_CALENDAR_EVENTS_MAX * (RECON_CALENDAR_TEXT_MAX + 24) + 256];
    size_t used = 0;

    int n = snprintf(text, sizeof(text),
        "# ReconOS calendar. One event per line:\n"
        "#   YYYY-MM-DD HH:MM  something at a time\n"
        "#   YYYY-MM-DD --     something on a day\n");
    if (n > 0) {
        used = (size_t)n;
    }

    for (int i = 0; i < g_count && used < sizeof(text); i++) {
        const struct recon_calendar_event *e = &g_events[i];

        if (e->minute_of_day >= 0) {
            n = snprintf(text + used, sizeof(text) - used,
                "%04d-%02d-%02d %02d:%02d %s\n", e->year, e->month, e->day,
                e->minute_of_day / 60, e->minute_of_day % 60, e->text);
        } else {
            n = snprintf(text + used, sizeof(text) - used,
                "%04d-%02d-%02d -- %s\n", e->year, e->month, e->day, e->text);
        }
        if (n > 0 && (size_t)n < sizeof(text) - used) {
            used += (size_t)n;
        }
    }

    char path[RECON_PATH_MAX];
    events_path(path, sizeof(path));

    if (!recon_fs_write("/", path, text, used)) {
        snprintf(g_error, sizeof(g_error), "%s", recon_fs_last_error());
        return false;
    }
    return true;
}

/* Whether `e` comes before `f` within a day: all-day first, then by time. */
static bool earlier(const struct recon_calendar_event *e,
        const struct recon_calendar_event *f) {
    if (e->minute_of_day < 0 && f->minute_of_day >= 0) {
        return true;
    }
    if (e->minute_of_day >= 0 && f->minute_of_day < 0) {
        return false;
    }
    return e->minute_of_day < f->minute_of_day;
}

/*
 * The events on one day, in the order a day is read: all-day things first,
 * then by time, and ties in the order they were written down.
 *
 * Gathered into a list of indices and sorted, rather than picked out one at a
 * time by comparison. The first version did the latter and needed a tie-break
 * between two events at the same minute, which it did by comparing their
 * addresses -- correct by accident, and unreadable. Ties broken by position
 * in the file mean two things at 09:00 stay in the order somebody entered
 * them, which is the only order that means anything.
 */
static int gather(int year, int month, int day, int *out, int max) {
    if (!g_loaded) {
        recon_calendar_load();
    }

    int found = 0;
    for (int i = 0; i < g_count && found < max; i++) {
        const struct recon_calendar_event *e = &g_events[i];
        if (e->year == year && e->month == month && e->day == day) {
            out[found++] = i;
        }
    }

    /* Insertion sort: these are the events on one day, so the list is short
     * and the simplest correct thing is the right thing. */
    for (int i = 1; i < found; i++) {
        int hold = out[i];
        int at = i - 1;
        while (at >= 0 && earlier(&g_events[hold], &g_events[out[at]])) {
            out[at + 1] = out[at];
            at--;
        }
        out[at + 1] = hold;
    }

    return found;
}

int recon_calendar_count_on(int year, int month, int day) {
    int order[RECON_CALENDAR_EVENTS_MAX];
    return gather(year, month, day, order, RECON_CALENDAR_EVENTS_MAX);
}

bool recon_calendar_on(int year, int month, int day, int index,
        struct recon_calendar_event *out) {
    int order[RECON_CALENDAR_EVENTS_MAX];
    int found = gather(year, month, day, order, RECON_CALENDAR_EVENTS_MAX);
    if (index < 0 || index >= found) {
        return false;
    }
    if (out != NULL) {
        *out = g_events[order[index]];
    }
    return true;
}

bool recon_calendar_add(int year, int month, int day, int minute_of_day,
        const char *text) {
    if (!g_loaded) {
        recon_calendar_load();
    }
    if (text == NULL || *text == '\0') {
        snprintf(g_error, sizeof(g_error), "an event needs something written "
            "in it");
        return false;
    }
    if (g_count >= RECON_CALENDAR_EVENTS_MAX) {
        snprintf(g_error, sizeof(g_error), "that is as many as this calendar "
            "holds");
        return false;
    }

    struct recon_calendar_event *e = &g_events[g_count];
    e->year = year;
    e->month = month;
    e->day = day;
    e->minute_of_day = minute_of_day;
    recon_text_copy(e->text, sizeof(e->text), text);
    g_count++;

    if (!save()) {
        g_count--;   /* It did not land, so it is not there. */
        return false;
    }
    return true;
}

bool recon_calendar_remove(int year, int month, int day, int index) {
    int order[RECON_CALENDAR_EVENTS_MAX];
    int found = gather(year, month, day, order, RECON_CALENDAR_EVENTS_MAX);
    if (index < 0 || index >= found) {
        snprintf(g_error, sizeof(g_error), "there is nothing there to remove");
        return false;
    }
    int at = order[index];

    struct recon_calendar_event going = g_events[at];
    for (int i = at; i + 1 < g_count; i++) {
        g_events[i] = g_events[i + 1];
    }
    g_count--;

    if (!save()) {
        /* Put it back rather than leave the file and the list disagreeing. */
        for (int i = g_count; i > at; i--) {
            g_events[i] = g_events[i - 1];
        }
        g_events[at] = going;
        g_count++;
        return false;
    }
    return true;
}

/* --- The window --- */

struct calendar_window {
    struct recon_font *font;
    struct recon_appwin *win;

    /* The month on show, and the day picked out in it. */
    int year, month, day;

    /* Typing a new event. The text carries an optional leading time, because
     * "0930 dentist" is one thing to type and two fields is two things to
     * fill in and a Tab to remember. */
    struct recon_edit adding;
    bool typing;

    char message[192];
    bool message_is_warning;
};

static void say(struct calendar_window *cw, bool warning, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void say(struct calendar_window *cw, bool warning, const char *fmt,
        ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(cw->message, sizeof(cw->message), fmt, args);
    va_end(args);
    cw->message_is_warning = warning;
}

/*
 * Days in a month, from the clock's arithmetic rather than a table.
 *
 * Asking what the day before the first of next month is means February is
 * right in 2100 for the same reason everything else is: there is one piece of
 * code that knows, and this is not a second one.
 */
static int days_in(int year, int month) {
    int next_year = (month == 12) ? year + 1 : year;
    int next_month = (month == 12) ? 1 : month + 1;

    int64_t here = recon_clock_epoch_of(year, month, 1);
    int64_t next = recon_clock_epoch_of(next_year, next_month, 1);
    return (int)((next - here) / 86400);
}

/*
 * Which weekday the first of a month falls on, 0 = Sunday.
 *
 * Asked at noon rather than midnight. `break_up` applies the reader's zone
 * offset, and a date taken at midnight UTC lands on the day before anywhere
 * west of it -- which would put every month's grid one column out for half
 * the world.
 */
static int first_weekday(int year, int month) {
    struct recon_clock_time t;
    recon_clock_break_up(recon_clock_epoch_of(year, month, 1) + 12 * 3600, &t);
    return t.weekday;
}

static void go_to_today(struct calendar_window *cw) {
    struct recon_clock_time now;
    recon_clock_now(&now);
    cw->year = now.year;
    cw->month = now.month;
    cw->day = now.day;
}

static void shift_month(struct calendar_window *cw, int by) {
    int month = cw->month + by;
    int year = cw->year;
    while (month > 12) {
        month -= 12;
        year++;
    }
    while (month < 1) {
        month += 12;
        year--;
    }
    cw->year = year;
    cw->month = month;

    /* The 31st does not exist in every month. Somebody moving from March to
     * February should land on the last day rather than nowhere. */
    int last = days_in(year, month);
    if (cw->day > last) {
        cw->day = last;
    }
}

/* --- Drawing --- */

static void calendar_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct calendar_window *cw = user;
    int ascent = recon_font_ascent(cw->font);
    int line = recon_font_line_height(cw->font);
    if (line <= 0) {
        line = 18;
    }

    recon_fill_rect(panel, x, y, w, h, COLOR_BG);

    struct recon_clock_time now;
    recon_clock_now(&now);

    /* --- The month, and the way through the months --- */

    char title[64];
    snprintf(title, sizeof(title), "%s %d", MONTHS[(cw->month - 1) % 12],
        cw->year);

    int bx = x + PADDING;
    int bw = 28;
    recon_fill_rect(panel, bx, y + 6, bw, HEADER_HEIGHT - 12, COLOR_BG);
    recon_draw_button_edge(panel, bx, y + 6, bw, HEADER_HEIGHT - 12, false,
        COLOR_BAR);
    recon_draw_text(panel, cw->font, bx + 11, y + 6 + ascent + 3, bw, "<",
        COLOR_TEXT);
    recon_hit_add(panel, bx, y + 6, bw, HEADER_HEIGHT - 12, HIT_PREVIOUS);
    recon_hit_tip(panel, "The month before");

    bx += bw + 4;
    recon_fill_rect(panel, bx, y + 6, bw, HEADER_HEIGHT - 12, COLOR_BG);
    recon_draw_button_edge(panel, bx, y + 6, bw, HEADER_HEIGHT - 12, false,
        COLOR_BAR);
    recon_draw_text(panel, cw->font, bx + 11, y + 6 + ascent + 3, bw, ">",
        COLOR_TEXT);
    recon_hit_add(panel, bx, y + 6, bw, HEADER_HEIGHT - 12, HIT_NEXT);
    recon_hit_tip(panel, "The month after");

    bx += bw + 10;
    recon_draw_text(panel, cw->font, bx, y + 6 + ascent + 3, 200, title,
        COLOR_TEXT);

    int today_w = recon_text_width(cw->font, "Today") + 20;
    int today_x = x + w - SIDE_WIDTH - PADDING - today_w;
    recon_fill_rect(panel, today_x, y + 6, today_w, HEADER_HEIGHT - 12,
        COLOR_BG);
    recon_draw_button_edge(panel, today_x, y + 6, today_w, HEADER_HEIGHT - 12,
        false, COLOR_BAR);
    recon_draw_text(panel, cw->font, today_x + 10, y + 6 + ascent + 3,
        today_w, "Today", COLOR_TEXT);
    recon_hit_add(panel, today_x, y + 6, today_w, HEADER_HEIGHT - 12,
        HIT_TODAY);

    /* --- The grid --- */

    int grid_x = x + PADDING;
    int grid_w = w - SIDE_WIDTH - PADDING * 3;
    if (grid_w < 140) {
        grid_w = 140;
    }
    int cell_w = grid_w / 7;

    int wy = y + HEADER_HEIGHT;
    for (int i = 0; i < 7; i++) {
        int tw = recon_text_width(cw->font, SHORT_DAYS[i]);
        recon_draw_text(panel, cw->font,
            grid_x + i * cell_w + (cell_w - tw) / 2, wy + ascent, cell_w,
            SHORT_DAYS[i], COLOR_DIM);
    }
    recon_fill_rect(panel, grid_x, wy + WEEKDAY_HEIGHT - 2, cell_w * 7, 1,
        COLOR_SEPARATOR);

    int gy = wy + WEEKDAY_HEIGHT;
    int grid_h = h - (gy - y) - BAR_HEIGHT - PADDING;
    int rows = 6;
    int cell_h = grid_h / rows;
    if (cell_h < line + 6) {
        cell_h = line + 6;
    }

    int lead = first_weekday(cw->year, cw->month);
    int last = days_in(cw->year, cw->month);

    for (int cell = 0; cell < rows * 7; cell++) {
        int day = cell - lead + 1;
        if (day < 1 || day > last) {
            continue;
        }

        int col = cell % 7;
        int row = cell / 7;
        int cx = grid_x + col * cell_w;
        int cy = gy + row * cell_h;

        bool chosen = day == cw->day;
        bool is_today = day == now.day && cw->month == now.month &&
            cw->year == now.year;

        if (chosen) {
            recon_fill_rect(panel, cx, cy, cell_w - 1, cell_h - 1,
                COLOR_SELECTED);
        } else if (is_today) {
            recon_fill_rect(panel, cx, cy, cell_w - 1, cell_h - 1,
                COLOR_PANEL);
        }

        /* Two digits and a terminator is all a day number needs; the
         * compiler cannot know `day` is bounded by the loop above, so the
         * buffer is sized for what an int could hold rather than for what
         * this one does. */
        char number[16];
        snprintf(number, sizeof(number), "%d", day);
        recon_draw_text(panel, cw->font, cx + 6, cy + ascent + 3, cell_w - 8,
            number, chosen ? COLOR_SELECTED_TEXT : COLOR_TEXT);

        /*
         * A dot for a day with something on it.
         *
         * Not the count and not the text: a month grid is read by scanning
         * for which days are busy, and a number in every cell is a grid
         * nobody can scan. The list beside it says what.
         */
        int on_it = recon_calendar_count_on(cw->year, cw->month, day);
        if (on_it > 0) {
            recon_fill_rect(panel, cx + 6, cy + cell_h - 9, 5, 5,
                chosen ? COLOR_SELECTED_TEXT : COLOR_ACCENT);
        }

        /* Today is ringed as well as filled, so it is still findable when
         * some other day is the chosen one. */
        if (is_today) {
            recon_stroke_rect(panel, cx, cy, cell_w - 1, cell_h - 1,
                COLOR_ACCENT);
        }

        recon_hit_add(panel, cx, cy, cell_w - 1, cell_h - 1,
            HIT_DAY_BASE + (uint32_t)day);
    }

    /* --- The chosen day, down the side --- */

    int sx = x + w - SIDE_WIDTH - PADDING;
    int sy = y + HEADER_HEIGHT;
    int sw = SIDE_WIDTH;

    char heading[96];
    snprintf(heading, sizeof(heading), "%d %s", cw->day,
        MONTHS[(cw->month - 1) % 12]);
    recon_draw_text(panel, cw->font, sx, sy + ascent, sw, heading, COLOR_TEXT);
    sy += line + 6;

    int on_it = recon_calendar_count_on(cw->year, cw->month, cw->day);
    if (on_it == 0) {
        recon_draw_text(panel, cw->font, sx, sy + ascent, sw,
            "Nothing on this day.", COLOR_DIM);
        sy += line + 4;
    }

    for (int i = 0; i < on_it; i++) {
        struct recon_calendar_event e;
        if (!recon_calendar_on(cw->year, cw->month, cw->day, i, &e)) {
            break;
        }
        if (sy + line * 2 > y + h - BAR_HEIGHT - PADDING) {
            recon_draw_text(panel, cw->font, sx, sy + ascent, sw,
                "More, further down the file.", COLOR_DIM);
            break;
        }

        char when[16];
        if (e.minute_of_day >= 0) {
            snprintf(when, sizeof(when), "%02d:%02d", e.minute_of_day / 60,
                e.minute_of_day % 60);
        } else {
            recon_text_copy(when, sizeof(when), "all day");
        }

        recon_fill_rect(panel, sx, sy, sw, line + 4, COLOR_PANEL);
        recon_draw_text(panel, cw->font, sx + 4, sy + ascent + 2, 54, when,
            COLOR_DIM);
        recon_draw_text(panel, cw->font, sx + 60, sy + ascent + 2, sw - 64,
            e.text, COLOR_TEXT);

        recon_hit_add(panel, sx, sy, sw, line + 4,
            HIT_EVENT_BASE + (uint32_t)i);
        recon_hit_tip(panel, "Click to remove");

        sy += line + 6;
    }

    /* --- Writing one down --- */

    sy += 4;
    if (cw->typing) {
        recon_edit_draw(panel, cw->font, sx, sy, sw, line + 8, &cw->adding);
        recon_hit_add(panel, sx, sy, sw, line + 8, HIT_FIELD);
        sy += line + 12;
        recon_draw_text(panel, cw->font, sx, sy + ascent, sw,
            "Enter to keep it. Escape to stop.", COLOR_DIM);
        sy += line;
        recon_draw_text(panel, cw->font, sx, sy + ascent, sw,
            "Start with a time -- 0930 -- or don't.", COLOR_DIM);
    } else {
        int add_w = recon_text_width(cw->font, "Write Something Down") + 20;
        recon_fill_rect(panel, sx, sy, add_w, line + 10, COLOR_BG);
        recon_draw_button_edge(panel, sx, sy, add_w, line + 10, false,
            COLOR_BAR);
        recon_draw_text(panel, cw->font, sx + 10, sy + ascent + 5, add_w,
            "Write Something Down", COLOR_TEXT);
        recon_hit_add(panel, sx, sy, add_w, line + 10, HIT_ADD);
    }

    /* --- The bar --- */

    int by = y + h - BAR_HEIGHT;
    recon_fill_rect(panel, x, by, w, BAR_HEIGHT, COLOR_BG);
    recon_fill_rect(panel, x, by, w, 1, COLOR_BAR);

    const char *shown = cw->message[0] != '\0' ? cw->message
        : "Click a day. The dot means something is written down on it.";
    recon_draw_text(panel, cw->font, x + PADDING,
        by + (BAR_HEIGHT + ascent) / 2 - 2, w - PADDING * 2, shown,
        cw->message[0] != '\0' && cw->message_is_warning
            ? COLOR_WARNING : COLOR_DIM);
}

/*
 * Take what was typed and make an event of it.
 *
 * A leading four digits is a time, so "0930 dentist" is one thing to type.
 * Anything else is an all-day entry, because most of what people write in a
 * calendar is a day rather than a moment.
 */
static void keep_typed(struct calendar_window *cw) {
    const char *text = cw->adding.text;
    while (*text == ' ') {
        text++;
    }
    if (*text == '\0') {
        cw->typing = false;
        return;
    }

    int minute = -1;
    if (text[0] >= '0' && text[0] <= '9' && text[1] >= '0' &&
            text[1] <= '9' && text[2] >= '0' && text[2] <= '9' &&
            text[3] >= '0' && text[3] <= '9' &&
            (text[4] == ' ' || text[4] == '\0')) {
        int hour = (text[0] - '0') * 10 + (text[1] - '0');
        int mins = (text[2] - '0') * 10 + (text[3] - '0');
        if (hour < 24 && mins < 60) {
            minute = hour * 60 + mins;
            text += 4;
            while (*text == ' ') {
                text++;
            }
        }
    }

    if (*text == '\0') {
        say(cw, true, "A time on its own is not an event. Say what it is.");
        return;
    }

    if (!recon_calendar_add(cw->year, cw->month, cw->day, minute, text)) {
        say(cw, true, "%s", recon_calendar_last_error());
        return;
    }

    cw->typing = false;
    recon_edit_end(&cw->adding);
    say(cw, false, "Written down for %d %s.", cw->day,
        MONTHS[(cw->month - 1) % 12]);
}

static bool calendar_click(void *user, uint32_t hit_id, int cx, int cy,
        bool pressed) {
    struct calendar_window *cw = user;
    (void)cx;
    (void)cy;

    if (!pressed) {
        return false;
    }

    if (hit_id >= HIT_EVENT_BASE) {
        int index = (int)(hit_id - HIT_EVENT_BASE);
        if (recon_calendar_remove(cw->year, cw->month, cw->day, index)) {
            say(cw, false, "Taken off.");
        } else {
            say(cw, true, "%s", recon_calendar_last_error());
        }
        return true;
    }

    if (hit_id >= HIT_DAY_BASE) {
        cw->day = (int)(hit_id - HIT_DAY_BASE);
        cw->message[0] = '\0';
        return true;
    }

    switch (hit_id) {
    case HIT_PREVIOUS:
        shift_month(cw, -1);
        return true;
    case HIT_NEXT:
        shift_month(cw, 1);
        return true;
    case HIT_TODAY:
        go_to_today(cw);
        return true;
    case HIT_ADD:
    case HIT_FIELD:
        if (!cw->typing) {
            cw->typing = true;
            recon_edit_begin(&cw->adding, "", false);
        }
        return true;
    default:
        return false;
    }
}

static bool calendar_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct calendar_window *cw = user;

    if (cw->typing) {
        if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
            keep_typed(cw);
            return true;
        }
        if (sym == XKB_KEY_Escape) {
            cw->typing = false;
            recon_edit_end(&cw->adding);
            return true;
        }
        return recon_edit_key(&cw->adding, sym, modifiers);
    }

    int last = days_in(cw->year, cw->month);

    switch (sym) {
    case XKB_KEY_Left:
        cw->day = cw->day > 1 ? cw->day - 1 : 1;
        return true;
    case XKB_KEY_Right:
        cw->day = cw->day < last ? cw->day + 1 : last;
        return true;
    case XKB_KEY_Up:
        cw->day = cw->day > 7 ? cw->day - 7 : cw->day;
        return true;
    case XKB_KEY_Down:
        cw->day = cw->day + 7 <= last ? cw->day + 7 : cw->day;
        return true;
    case XKB_KEY_Page_Up:
        shift_month(cw, -1);
        return true;
    case XKB_KEY_Page_Down:
        shift_month(cw, 1);
        return true;
    case XKB_KEY_Home:
        go_to_today(cw);
        return true;
    case XKB_KEY_Return:
        cw->typing = true;
        recon_edit_begin(&cw->adding, "", false);
        return true;
    default:
        return false;
    }
}

static void calendar_scroll(void *user, double delta) {
    shift_month(user, delta > 0 ? 1 : -1);
}

static void calendar_describe(void *user, char *out, size_t size) {
    struct calendar_window *cw = user;
    snprintf(out, size,
        "  showing: %s %d\n"
        "  chosen day: %d\n"
        "  on that day: %d\n"
        "  typing: %s\n",
        MONTHS[(cw->month - 1) % 12], cw->year, cw->day,
        recon_calendar_count_on(cw->year, cw->month, cw->day),
        cw->typing ? "yes" : "no");
}

static void calendar_destroy(void *user) {
    struct calendar_window *cw = user;
    if (cw->typing) {
        recon_edit_end(&cw->adding);
    }
    free(cw);
}

static const struct recon_appwin_impl CALENDAR_IMPL = {
    .title = "Calendar",
    .help = "Writing",
    .icon = RECON_ICON_CALENDAR,
    .default_width = 700,
    .default_height = 460,
    .min_width = 460,
    .min_height = 320,
    .draw = calendar_draw,
    .click = calendar_click,
    .key = calendar_key,
    .scroll = calendar_scroll,
    .describe = calendar_describe,
    .destroy = calendar_destroy,
};

struct recon_appwin *recon_calendar_create(struct recon_server *server,
        struct recon_font *font) {
    struct calendar_window *cw = calloc(1, sizeof(*cw));
    if (cw == NULL) {
        return NULL;
    }

    cw->font = font;
    go_to_today(cw);
    recon_calendar_load();

    cw->win = recon_appwin_create(server, font, &CALENDAR_IMPL, cw);
    if (cw->win == NULL) {
        free(cw);
        return NULL;
    }
    return cw->win;
}
