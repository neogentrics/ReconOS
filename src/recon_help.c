/*
 * Help. See include/recon_help.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_fs.h"
#include "recon_help.h"
#include "recon_icons.h"
#include "recon_modules.h"
#include "recon_registry.h"
#include "recon_server.h"
#include "recon_shell.h"
#include "recon_theme.h"
#include "recon_ui.h"

#define TOPICS_MAX 64
#define TITLE_MAX 48
#define LINES_MAX 512
#define LINE_MAX 200
/* One paragraph of source, gathered before it is wrapped. Sized by the length
 * of the longest paragraph anybody would write, not by any count of lines. */
#define PARAGRAPH_MAX 4096

#define SIDEBAR_WIDTH 190
#define PADDING 10
#define ROW_HEIGHT 24
#define LINE_SPACING 3

#define HIT_TOPIC_BASE (RECON_APPWIN_HIT_USER + 1)
#define HIT_SIDEBAR (RECON_APPWIN_HIT_USER + 199)
#define HIT_BODY (RECON_APPWIN_HIT_USER + 200)

/* A scrollbar wide enough to see and narrow enough not to be a column. */
#define BAR_WIDTH 6

#define COLOR_BG THEME(SURFACE)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_DIM THEME(SURFACE_TEXT_DIM)
#define COLOR_SIDEBAR THEME(WINDOW_FRAME)
#define COLOR_SELECTION THEME(SELECTION)
#define COLOR_SELECTION_TEXT THEME(SELECTION_TEXT)
#define COLOR_SEPARATOR THEME(MENU_SEPARATOR)
#define COLOR_HEADING THEME(ACCENT)

struct topic {
    char file[RECON_NAME_MAX];
    char title[TITLE_MAX];
    /* True for a change-log entry rather than a help page. The two are one
     * list with a divider, not two lists: they are read the same way. */
    bool is_change;
};

/*
 * A block of text wrapped to a width, and where the reader is in it.
 *
 * Separate from the window because two windows show one: the help itself, and
 * the notice that appears after an update. They differ in what text they hold
 * and in what surrounds it, not in how prose is broken into lines.
 */
struct page {
    char lines[LINES_MAX][LINE_MAX];
    int line_count;

    /* What the lines were broken to fit. Re-wrapped when the window changes
     * width, which is also how "not wrapped yet" is said: zero. */
    int wrapped_width;

    int scroll;
    int visible_lines;
};

struct recon_help {
    struct recon_font *font;
    struct recon_appwin *win;

    struct topic topics[TOPICS_MAX];
    int topic_count;
    int selected;

    /*
     * How far down the topic list is.
     *
     * There was no such thing, and the list is one page of help topics plus
     * one entry per version -- forty-odd rows in a window that shows
     * eighteen. Everything past the fifth or sixth version was drawn off the
     * bottom and could not be reached at all, which is a change log that
     * exists and cannot be read.
     */
    int topic_scroll;
    int topic_rows;

    /*
     * Which pane the pointer is over.
     *
     * The scroll callback is given a direction and nothing else, so without
     * this the wheel could only ever move one of the two lists -- and it
     * moved the wrong one, because the list somebody is pointing at is the
     * list they mean.
     */
    uint32_t hover;

    struct page page;
};

/* A slim bar showing how far down a list is, and how much of it there is. */
static void draw_scrollbar(struct recon_panel *panel, int x, int y, int h,
        int first, int shown, int total) {
    if (total <= shown || h <= 0) {
        return;
    }

    recon_fill_rect(panel, x, y, BAR_WIDTH, h, COLOR_SEPARATOR);

    int height = h * shown / total;
    if (height < 12) {
        height = 12;
    }
    if (height > h) {
        height = h;
    }

    int room = h - height;
    int most = total - shown;
    int offset = (most > 0) ? room * first / most : 0;

    recon_fill_role(panel, x, y + offset, BAR_WIDTH, height,
        RECON_THEME_SURFACE_TEXT_DIM);
}

/* --- Writing the shipped copy out --- */

int recon_help_write_defaults(void) {
    recon_fs_mkdir("/", RECON_DIR_HELP);

    char index_path[RECON_PATH_MAX];
    if (!recon_fs_join(index_path, sizeof(index_path), RECON_DIR_HELP,
            RECON_HELP_INDEX)) {
        return 0;
    }

    size_t size = 0;
    char *index = recon_asset_read("help/" RECON_HELP_INDEX, &size);
    if (index == NULL) {
        return 0;
    }

    int written = 0;
    char *saveptr = NULL;

    /*
     * Copied one at a time from the index rather than by listing the asset
     * folder, so a file left behind in the source tree does not become a
     * topic nobody meant to ship.
     */
    for (char *line = strtok_r(index, "\n", &saveptr);
            line != NULL;
            line = strtok_r(NULL, "\n", &saveptr)) {

        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            continue;
        }
        *tab = '\0';

        char asset[RECON_PATH_MAX];
        snprintf(asset, sizeof(asset), "help/%s", line);

        size_t topic_size = 0;
        char *topic = recon_asset_read(asset, &topic_size);
        if (topic == NULL) {
            continue;
        }

        char out[RECON_PATH_MAX];
        if (recon_fs_join(out, sizeof(out), RECON_DIR_HELP, line) &&
                recon_fs_write("/", out, topic, topic_size)) {
            written++;
        }
        free(topic);
    }

    /*
     * The index last. Anything reading it can then rely on every file it
     * names being there, rather than on the copy having got that far.
     */
    free(index);
    index = recon_asset_read("help/" RECON_HELP_INDEX, &size);
    if (index != NULL) {
        recon_fs_write("/", index_path, index, size);
        free(index);
    }

    return written;
}

/* --- Reading it back --- */

static void load_index(struct recon_help *help) {
    help->topic_count = 0;

    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), RECON_DIR_HELP, RECON_HELP_INDEX)) {
        return;
    }

    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        return;
    }

    char *saveptr = NULL;
    for (char *line = strtok_r(text, "\n", &saveptr);
            line != NULL && help->topic_count < TOPICS_MAX;
            line = strtok_r(NULL, "\n", &saveptr)) {

        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            continue;
        }
        *tab = '\0';

        struct topic *topic = &help->topics[help->topic_count];
        snprintf(topic->file, sizeof(topic->file), "%s", line);
        snprintf(topic->title, sizeof(topic->title), "%s", tab + 1);
        topic->is_change = (strncmp(line, "changes-", 8) == 0);
        help->topic_count++;
    }

    free(text);
}

/* Add one wrapped paragraph to the page. */
static void wrap_paragraph(struct page *page, struct recon_font *font,
        const char *text, int width, const char *hanging) {
    char current[LINE_MAX];
    current[0] = '\0';

    const char *word = text;
    while (*word == ' ') {
        word++;
    }

    while (*word != '\0' && page->line_count < LINES_MAX) {
        const char *end = strchr(word, ' ');
        size_t length = (end != NULL) ? (size_t)(end - word) : strlen(word);

        /*
         * A "word" longer than a whole line is broken rather than dropped. A
         * path or an address can be longer than any line, and a break in the
         * middle of one is easier to live with than a missing tail.
         */
        size_t most = sizeof(current) - strlen(hanging) - 1;
        if (length > most) {
            length = most;
        }

        /*
         * Two ways a word does not belong on this line: too wide to draw, and
         * too long to hold. The second only comes up on a very wide window,
         * where a line of text can outrun the buffer before it outruns the
         * pane, and it wraps for the same reason the first does.
         */
        size_t have = strlen(current);
        size_t separator = (have > 0) ? 1 : 0;
        bool overflows = (have + separator + length >= sizeof(current));

        char candidate[LINE_MAX * 2];
        snprintf(candidate, sizeof(candidate), "%s%s%.*s", current,
            separator ? " " : "", (int)length, word);

        if (current[0] != '\0' &&
                (overflows ||
                 recon_text_width(font, candidate) > width)) {
            snprintf(page->lines[page->line_count++], LINE_MAX, "%s", current);

            /* A list item indents its continuation, so the bullet marks the
             * start of the item rather than the start of every line of it. */
            snprintf(current, sizeof(current), "%s%.*s", hanging,
                (int)length, word);
        } else {
            /* Appended rather than copied back from the candidate, so the
             * bound is the one `overflows` just checked. */
            if (separator) {
                current[have] = ' ';
            }
            memcpy(current + have + separator, word, length);
            current[have + separator + length] = '\0';
        }

        word += length;
        while (*word == ' ') {
            word++;
        }
    }

    if (current[0] != '\0' && page->line_count < LINES_MAX) {
        snprintf(page->lines[page->line_count++], LINE_MAX, "%s", current);
    }
}

/*
 * Wrap a block of text to the width it will be drawn at.
 *
 * Paragraphs are reflowed, not lines. The source is hard-wrapped to about
 * eighty characters because it is a file people edit, and treating each of
 * those lines as its own paragraph produced a ragged column -- a full line,
 * then two words, then a full line -- which reads as though the text were
 * broken rather than as though the window were narrow.
 *
 * A blank line ends a paragraph. So does a line starting a list item, since
 * a list of three things is three paragraphs however it is punctuated.
 */
static void wrap_text(struct page *page, struct recon_font *font,
        const char *source, int width) {
    page->line_count = 0;
    page->wrapped_width = width;

    if (source == NULL) {
        return;
    }

    /* Copied because the split below marks line ends in place, and callers
     * hand this in from buffers they still own. */
    char *text = strdup(source);
    if (text == NULL) {
        return;
    }

    char paragraph[PARAGRAPH_MAX];
    size_t used = 0;
    paragraph[0] = '\0';
    bool is_item = false;

    /*
     * Split by hand rather than with strtok_r, which runs a row of newlines
     * together as one separator. Here a blank line is not a separator to be
     * skipped -- it is the mark between one paragraph and the next, and
     * losing it welds two changes into a single block of prose.
     */
    char *rest = text;
    char *line = rest;
    bool done = false;

    while (page->line_count < LINES_MAX) {
        char *newline = NULL;
        if (!done) {
            newline = strchr(rest, '\n');
            if (newline != NULL) {
                *newline = '\0';
            }
            line = rest;
        } else {
            line = NULL;
        }

        bool blank = (line == NULL) || (*line == '\0');
        bool starts_item = (line != NULL) &&
            (line[0] == '-' && line[1] == ' ');

        /* Finish what has been gathered when the paragraph ends. */
        if ((blank || starts_item) && used > 0) {
            wrap_paragraph(page, font, paragraph, width, is_item ? "  " : "");
            used = 0;
            paragraph[0] = '\0';
        }

        if (line == NULL) {
            break;
        }

        if (blank) {
            /* One blank line between paragraphs, however many the source
             * happens to have. */
            if (page->line_count > 0 &&
                    page->lines[page->line_count - 1][0] != '\0') {
                page->lines[page->line_count++][0] = '\0';
            }
        } else {
            if (used == 0) {
                is_item = starts_item;
            }
            /*
             * A paragraph longer than the buffer is wrapped in two pieces
             * rather than cut off at the buffer's end. The join is invisible
             * -- both pieces are wrapped to the same width -- and no sentence
             * goes missing because somebody wrote a long one.
             */
            size_t room = sizeof(paragraph) - used;
            size_t want = strlen(line) + (used > 0 ? 1 : 0);
            if (want >= room) {
                wrap_paragraph(page, font, paragraph, width,
                    is_item ? "  " : "");
                used = 0;
                paragraph[0] = '\0';
            }

            int n = snprintf(paragraph + used, sizeof(paragraph) - used,
                "%s%s", used > 0 ? " " : "", line);
            if (n > 0 && (size_t)n < sizeof(paragraph) - used) {
                used += (size_t)n;
            }
        }

        if (newline != NULL) {
            rest = newline + 1;
        } else {
            /* The last line, then one more turn with nothing, so a paragraph
             * ending at the end of the file is still wrapped. */
            done = true;
        }
    }

    free(text);
}

/* The chosen topic, read from disk and wrapped. */
static void wrap_topic(struct recon_help *help, int width) {
    help->page.line_count = 0;
    help->page.wrapped_width = width;

    if (help->selected < 0 || help->selected >= help->topic_count) {
        return;
    }

    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), RECON_DIR_HELP,
            help->topics[help->selected].file)) {
        return;
    }

    size_t size = 0;
    char *text = recon_fs_read("/", path, &size);
    if (text == NULL) {
        snprintf(help->page.lines[0], LINE_MAX, "This page is missing.");
        help->page.line_count = 1;
        return;
    }

    wrap_text(&help->page, help->font, text, width);
    free(text);
}

static void choose(struct recon_help *help, int index) {
    if (index < 0 || index >= help->topic_count) {
        return;
    }
    help->selected = index;
    help->page.scroll = 0;
    /* Re-wrapped on the next draw, which is where the width is known. */
    help->page.wrapped_width = 0;
}

/* --- Drawing --- */

static void help_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct recon_help *help = user;
    int ascent = recon_font_ascent(help->font);
    int line_height = recon_font_line_height(help->font) + LINE_SPACING;
    if (line_height <= 0) {
        line_height = 16;
    }

    recon_fill_rect(panel, x, y, w, h, COLOR_BG);

    /*
     * No recon_hit_clear here.
     *
     * The window clears the panel's hit regions itself and then draws the
     * frame -- close, maximize, minimize and the draggable title bar -- and
     * only afterwards calls this. Clearing again wiped all four, so the Help
     * window could not be closed or moved: the only two windows in the
     * system that did this were the two written last, and both were broken
     * the same way for the same reason.
     */

    /* --- The topics --- */
    recon_fill_rect(panel, x, y, SIDEBAR_WIDTH, h, COLOR_SIDEBAR);
    recon_fill_rect(panel, x + SIDEBAR_WIDTH, y, 1, h, COLOR_SEPARATOR);

    /*
     * How many rows fit, and keeping the chosen one among them.
     *
     * Chased rather than merely clamped: the arrow keys walk the list, and a
     * selection that walked off the bottom would move the highlight to
     * somewhere nobody can see while the page beside it changed.
     */
    help->topic_rows = (h - PADDING) / ROW_HEIGHT;
    if (help->topic_rows < 1) {
        help->topic_rows = 1;
    }

    if (help->selected >= 0) {
        if (help->selected < help->topic_scroll) {
            help->topic_scroll = help->selected;
        } else if (help->selected >= help->topic_scroll + help->topic_rows) {
            help->topic_scroll = help->selected - help->topic_rows + 1;
        }
    }
    if (help->topic_scroll > help->topic_count - help->topic_rows) {
        help->topic_scroll = help->topic_count - help->topic_rows;
    }
    if (help->topic_scroll < 0) {
        help->topic_scroll = 0;
    }

    /*
     * The whole sidebar takes the wheel, not only the rows: a list is
     * scrolled by pointing at it, and the gap under the last row is still
     * pointing at it.
     *
     * Registered **before** the rows, because the last region added wins and
     * this one covers all of them. Added after, it swallowed every click on
     * the list -- the topics were drawn, highlighted under the pointer, and
     * could not be chosen. A region that exists to catch what the others miss
     * has to sit underneath them.
     */
    recon_hit_add(panel, x, y, SIDEBAR_WIDTH, h, HIT_SIDEBAR);

    int ty = y + PADDING / 2;
    bool divided = false;

    for (int row = 0; row < help->topic_rows; row++) {
        int i = help->topic_scroll + row;
        if (i >= help->topic_count) {
            break;
        }

        /*
         * A rule where the change log begins. The two halves are read
         * differently -- one is looked up, the other is read through -- and a
         * single unbroken list of thirty entries hides that.
         *
         * Only drawn when the boundary is actually on screen: scrolled past,
         * it is a rule with nothing above it.
         */
        if (help->topics[i].is_change && !divided) {
            divided = true;
            if (i > help->topic_scroll) {
                ty += 6;
                recon_fill_rect(panel, x + PADDING, ty,
                    SIDEBAR_WIDTH - PADDING * 2 - BAR_WIDTH, 1,
                    COLOR_SEPARATOR);
                ty += 4;

                recon_draw_text(panel, help->font, x + PADDING, ty + ascent,
                    SIDEBAR_WIDTH - PADDING * 2, "What changed", COLOR_DIM);
                ty += line_height + 2;
            }
        }

        if (ty + ROW_HEIGHT > y + h) {
            break;
        }

        bool chosen = (i == help->selected);
        if (chosen) {
            recon_fill_role(panel, x + 2, ty, SIDEBAR_WIDTH - 4 - BAR_WIDTH,
                ROW_HEIGHT, RECON_THEME_SELECTION);
        }

        recon_draw_text(panel, help->font, x + PADDING,
            ty + (ROW_HEIGHT + ascent) / 2 - 2,
            SIDEBAR_WIDTH - PADDING * 2 - BAR_WIDTH, help->topics[i].title,
            chosen ? COLOR_SELECTION_TEXT : COLOR_TEXT);

        recon_hit_add(panel, x + 2, ty, SIDEBAR_WIDTH - 4 - BAR_WIDTH,
            ROW_HEIGHT, HIT_TOPIC_BASE + i);
        ty += ROW_HEIGHT;
    }

    draw_scrollbar(panel, x + SIDEBAR_WIDTH - BAR_WIDTH - 2, y + PADDING / 2,
        h - PADDING, help->topic_scroll, help->topic_rows,
        help->topic_count);

    /* --- The page --- */
    int body_x = x + SIDEBAR_WIDTH + 1 + PADDING * 2;
    int body_w = w - (body_x - x) - PADDING * 2;
    if (body_w < 80) {
        body_w = 80;
    }

    if (help->page.wrapped_width != body_w) {
        wrap_topic(help, body_w);
    }

    int body_y = y + PADDING;

    if (help->selected >= 0 && help->selected < help->topic_count) {
        recon_draw_text(panel, help->font, body_x, body_y + ascent, body_w,
            help->topics[help->selected].title, COLOR_HEADING);
        body_y += line_height + 4;
    }

    /*
     * Counted after the heading has taken its room, and with a line left for
     * the "more below" note.
     *
     * It was counted before both, so the page drew one line too many and the
     * note landed on top of it -- two sentences in the same place, which
     * reads as the text being broken rather than as the window being full.
     */
    help->page.visible_lines =
        (y + h - PADDING - line_height - body_y) / line_height;
    if (help->page.visible_lines < 1) {
        help->page.visible_lines = 1;
    }

    if (help->page.scroll > help->page.line_count - help->page.visible_lines) {
        help->page.scroll = help->page.line_count - help->page.visible_lines;
    }
    if (help->page.scroll < 0) {
        help->page.scroll = 0;
    }

    for (int i = 0; i < help->page.visible_lines; i++) {
        int index = help->page.scroll + i;
        if (index >= help->page.line_count) {
            break;
        }

        /*
         * A subheading, drawn as one.
         *
         * The help is written as Markdown and read from it, so a line that
         * starts with hashes is a heading -- and until now it was drawn with
         * the hashes still on the front, which is the source leaking through
         * into the page. Nobody writing the help asked for "### Making a skin
         * of your own" to appear on screen.
         */
        const char *line = help->page.lines[index];
        if (line[0] == '#') {
            const char *text = line;
            while (*text == '#') {
                text++;
            }
            while (*text == ' ') {
                text++;
            }
            recon_draw_text(panel, help->font, body_x,
                body_y + i * line_height + ascent, body_w, text,
                COLOR_HEADING);
            continue;
        }

        recon_draw_text(panel, help->font, body_x,
            body_y + i * line_height + ascent, body_w, line, COLOR_TEXT);
    }

    draw_scrollbar(panel, x + w - BAR_WIDTH - 2, y + PADDING,
        h - PADDING * 2, help->page.scroll, help->page.visible_lines,
        help->page.line_count);

    recon_hit_add(panel, x + SIDEBAR_WIDTH + 1, y, w - SIDEBAR_WIDTH - 1, h,
        HIT_BODY);

    /* How much more there is, when there is more. */
    if (help->page.line_count > help->page.visible_lines) {
        char more[64];
        int remaining = help->page.line_count - help->page.scroll - help->page.visible_lines;
        if (remaining > 0) {
            snprintf(more, sizeof(more), "%d more line%s below", remaining,
                remaining == 1 ? "" : "s");
            recon_draw_text(panel, help->font, body_x,
                y + h - PADDING + ascent - line_height, body_w, more,
                COLOR_DIM);
        }
    }
}

/* --- Input --- */

static bool help_click(void *user, uint32_t hit_id, int cx, int cy,
        bool pressed) {
    struct recon_help *help = user;
    (void)cx; (void)cy;

    if (!pressed) {
        return false;
    }
    if (hit_id >= HIT_TOPIC_BASE && hit_id < HIT_BODY) {
        choose(help, (int)(hit_id - HIT_TOPIC_BASE));
        return true;
    }
    return hit_id == HIT_BODY;
}

static void help_motion(void *user, uint32_t hit_id, int cx, int cy) {
    struct recon_help *help = user;
    (void)cx; (void)cy;
    help->hover = hit_id;
}

static void help_scroll(void *user, double delta) {
    struct recon_help *help = user;
    int by = (delta > 0) ? 3 : -3;

    /* The topic list when the pointer is on it, the page otherwise. */
    bool on_topics = (help->hover == HIT_SIDEBAR) ||
        (help->hover >= HIT_TOPIC_BASE && help->hover < HIT_SIDEBAR);

    if (on_topics) {
        help->topic_scroll += by;

        /*
         * Clamped here as well as in the drawing, because the drawing also
         * chases the selection -- and scrolling past the end and being pulled
         * back by the selection would make the wheel feel like it was
         * fighting the mouse.
         */
        if (help->topic_scroll > help->topic_count - help->topic_rows) {
            help->topic_scroll = help->topic_count - help->topic_rows;
        }
        if (help->topic_scroll < 0) {
            help->topic_scroll = 0;
        }
        return;
    }

    help->page.scroll += by;
    if (help->page.scroll < 0) {
        help->page.scroll = 0;
    }
}

static bool help_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_help *help = user;
    (void)modifiers;

    switch (sym) {
    case XKB_KEY_Down:
        help->page.scroll++;
        return true;
    case XKB_KEY_Up:
        if (help->page.scroll > 0) {
            help->page.scroll--;
        }
        return true;
    case XKB_KEY_Page_Down:
        help->page.scroll += help->page.visible_lines;
        return true;
    case XKB_KEY_Page_Up:
        help->page.scroll -= help->page.visible_lines;
        if (help->page.scroll < 0) {
            help->page.scroll = 0;
        }
        return true;
    case XKB_KEY_Home:
        help->page.scroll = 0;
        return true;
    /* Stepping through the topics from the keyboard, so the whole document
     * can be read without reaching for the mouse. */
    case XKB_KEY_Right:
        choose(help, help->selected + 1);
        return true;
    case XKB_KEY_Left:
        choose(help, help->selected - 1);
        return true;
    default:
        return false;
    }
}

static void help_describe(void *user, char *out, size_t size) {
    struct recon_help *help = user;
    snprintf(out, size, "topic %d of %d: %s, line %d of %d",
        help->selected + 1, help->topic_count,
        (help->selected >= 0 && help->selected < help->topic_count)
            ? help->topics[help->selected].title : "(none)",
        help->page.scroll + 1, help->page.line_count);
}

static void help_destroy(void *user) {
    free(user);
}

static const struct recon_appwin_impl HELP_IMPL = {
    .title = "Help",
    .help = "Getting around",
    .icon = RECON_ICON_HELP,
    .default_width = 640,
    .default_height = 460,
    .min_width = 420,
    .min_height = 260,
    .draw = help_draw,
    .click = help_click,
    .key = help_key,
    .motion = help_motion,
    .scroll = help_scroll,
    .describe = help_describe,
    .destroy = help_destroy,
};

struct recon_appwin *recon_help_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_help *help = calloc(1, sizeof(*help));
    if (help == NULL) {
        return NULL;
    }

    help->font = font;
    load_index(help);
    help->selected = help->topic_count > 0 ? 0 : -1;

    help->win = recon_appwin_create(server, font, &HELP_IMPL, help);
    if (help->win == NULL) {
        free(help);
        return NULL;
    }
    return help->win;
}

void recon_help_show_topic(struct recon_appwin *win, const char *title) {
    if (win == NULL || title == NULL) {
        return;
    }
    struct recon_help *help = recon_appwin_user(win);
    if (help == NULL) {
        return;
    }

    /* Re-read first: the topics may have been rewritten by an update since
     * this window was built. */
    load_index(help);

    for (int i = 0; i < help->topic_count; i++) {
        if (strcasecmp(help->topics[i].title, title) == 0) {
            choose(help, i);

            /*
             * And redraw. Without this the page was chosen and the window
             * went on showing the one before it -- which looked exactly like
             * the topic not being found, and was found only by asking the
             * window what it thought it was showing.
             *
             * Here rather than at the call sites, because every caller of
             * this wants the window to show what it has just been told to.
             */
            recon_appwin_refresh(win);
            return;
        }
    }
}

const char *recon_help_current_changes(void) {
    static char text[2048];
    text[0] = '\0';

    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), RECON_DIR_HELP,
            RECON_HELP_INDEX)) {
        return text;
    }

    size_t size = 0;
    char *index = recon_fs_read("/", path, &size);
    if (index == NULL) {
        return text;
    }

    char wanted[64];
    snprintf(wanted, sizeof(wanted), "v%s", RECONOS_VERSION);

    char file[RECON_NAME_MAX];
    file[0] = '\0';

    char *saveptr = NULL;
    for (char *line = strtok_r(index, "\n", &saveptr);
            line != NULL;
            line = strtok_r(NULL, "\n", &saveptr)) {
        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            continue;
        }
        *tab = '\0';
        if (strcmp(tab + 1, wanted) == 0) {
            snprintf(file, sizeof(file), "%s", line);
            break;
        }
    }
    free(index);

    if (file[0] == '\0') {
        return text;    /* The log says nothing about this version. */
    }

    if (!recon_fs_join(path, sizeof(path), RECON_DIR_HELP, file)) {
        return text;
    }

    char *body = recon_fs_read("/", path, &size);
    if (body != NULL) {
        snprintf(text, sizeof(text), "%s", body);
        free(body);
    }
    return text;
}

/* --- The notice shown after an update --- */

/*
 * A second window, over the same text.
 *
 * Somebody who has just signed in after an update has not gone looking for
 * anything; the news has to come to them once, and then never again. So this
 * is a window rather than a page of the help, and pressing OK is what records
 * that it was read.
 */

#define NOTICE_PADDING 16
#define NOTICE_BUTTON_HEIGHT 26

#define HIT_NOTICE_OK (RECON_APPWIN_HIT_USER + 1)
#define HIT_NOTICE_ALL (RECON_APPWIN_HIT_USER + 2)

struct recon_notice {
    struct recon_server *server;
    struct recon_font *font;
    struct recon_appwin *win;

    /* The version this notice is about, held so the button that opens the
     * full log can go straight to the matching entry. */
    char version[32];

    struct page page;
};

/* The version this account has already been shown, or "" for none. */
static const char *notice_seen(void) {
    return recon_registry_get(RECON_REG_USER, RECON_HELP_SEEN_KEY, "");
}

bool recon_help_notice_due(void) {
    const char *changes = recon_help_current_changes();
    if (changes == NULL || changes[0] == '\0') {
        /*
         * Nothing written about this version. Better to say nothing than to
         * open an empty window, and the account is not marked as having seen
         * it -- so writing the entry later still reaches them.
         */
        return false;
    }
    return strcmp(notice_seen(), RECONOS_VERSION) != 0;
}

/* Draw one button and return where the next one to its left would start. */
static int notice_button(struct recon_notice *notice, struct recon_panel *p,
        int right, int y, const char *label, uint32_t id) {
    int ascent = recon_font_ascent(notice->font);
    int width = recon_text_width(notice->font, label) + 26;
    int x = right - width;

    recon_fill_role(p, x, y, width, NOTICE_BUTTON_HEIGHT, RECON_THEME_BUTTON);
    recon_draw_button_edge(p, x, y, width, NOTICE_BUTTON_HEIGHT, false,
        COLOR_BG);

    recon_draw_text(p, notice->font, x + 13,
        y + (NOTICE_BUTTON_HEIGHT + ascent) / 2 - 2, width - 20, label,
        THEME(BUTTON_TEXT));
    recon_hit_add(p, x, y, width, NOTICE_BUTTON_HEIGHT, id);

    return x - 8;
}

static void notice_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct recon_notice *notice = user;
    int ascent = recon_font_ascent(notice->font);
    int line_height = recon_font_line_height(notice->font) + LINE_SPACING;
    if (line_height <= 0) {
        line_height = 16;
    }

    /* No recon_hit_clear: see the note in help_draw. Clearing here wiped the
     * frame's own buttons, so this window could not be closed or moved
     * either. */
    recon_fill_rect(panel, x, y, w, h, COLOR_BG);

    int text_x = x + NOTICE_PADDING;
    int text_w = w - NOTICE_PADDING * 2;
    if (text_w < 80) {
        text_w = 80;
    }

    char heading[64];
    snprintf(heading, sizeof(heading), "ReconOS %s", notice->version);
    recon_draw_text(panel, notice->font, text_x, y + NOTICE_PADDING + ascent,
        text_w, heading, COLOR_HEADING);

    int body_y = y + NOTICE_PADDING + line_height + 4;
    recon_draw_text(panel, notice->font, text_x, body_y + ascent, text_w,
        "What changed in this version:", COLOR_DIM);
    body_y += line_height + 6;

    if (notice->page.wrapped_width != text_w) {
        wrap_text(&notice->page, notice->font, recon_help_current_changes(),
            text_w);
    }

    /* The buttons keep their room whatever the text does, so a long entry
     * scrolls rather than pushing OK off the bottom of the window. */
    int buttons_y = y + h - NOTICE_PADDING - NOTICE_BUTTON_HEIGHT;
    int room = buttons_y - body_y - NOTICE_PADDING;

    notice->page.visible_lines = room / line_height;
    if (notice->page.visible_lines < 1) {
        notice->page.visible_lines = 1;
    }

    if (notice->page.scroll >
            notice->page.line_count - notice->page.visible_lines) {
        notice->page.scroll =
            notice->page.line_count - notice->page.visible_lines;
    }
    if (notice->page.scroll < 0) {
        notice->page.scroll = 0;
    }

    for (int i = 0; i < notice->page.visible_lines; i++) {
        int index = notice->page.scroll + i;
        if (index >= notice->page.line_count) {
            break;
        }
        recon_draw_text(panel, notice->font, text_x,
            body_y + i * line_height + ascent, text_w,
            notice->page.lines[index], COLOR_TEXT);
    }

    int next = notice_button(notice, panel, x + w - NOTICE_PADDING, buttons_y,
        "OK", HIT_NOTICE_OK);
    notice_button(notice, panel, next, buttons_y, "All changes",
        HIT_NOTICE_ALL);

    /* How much more there is, on the left of the buttons, where there is
     * room for it whatever the window is doing. */
    if (notice->page.line_count > notice->page.visible_lines) {
        int remaining = notice->page.line_count - notice->page.scroll -
            notice->page.visible_lines;
        if (remaining > 0) {
            char more[64];
            snprintf(more, sizeof(more), "%d more line%s below", remaining,
                remaining == 1 ? "" : "s");
            recon_draw_text(panel, notice->font, text_x,
                buttons_y + (NOTICE_BUTTON_HEIGHT + ascent) / 2 - 2,
                text_w / 2, more, COLOR_DIM);
        }
    }
}

/* Remember that this account has seen it, and close. */
static void notice_dismiss(struct recon_notice *notice) {
    recon_registry_set(RECON_REG_USER, RECON_HELP_SEEN_KEY, RECONOS_VERSION);
    recon_appwin_hide(notice->win);
}

static bool notice_click(void *user, uint32_t hit_id, int cx, int cy,
        bool pressed) {
    struct recon_notice *notice = user;
    (void)cx; (void)cy;

    if (!pressed) {
        return false;
    }

    if (hit_id == HIT_NOTICE_OK) {
        notice_dismiss(notice);
        return true;
    }

    if (hit_id == HIT_NOTICE_ALL) {
        /*
         * Opening the whole log counts as having read this much of it. The
         * alternative -- leaving it unseen so it comes back next time --
         * punishes the person who took the trouble to read further.
         */
        struct recon_shell *shell = notice->server->shell;
        recon_registry_set(RECON_REG_USER, RECON_HELP_SEEN_KEY,
            RECONOS_VERSION);
        recon_appwin_hide(notice->win);

        recon_shell_open_named(shell, "Help");
        recon_help_show_topic(recon_installed_app_existing("Help"),
            notice->version);
        return true;
    }

    return false;
}

static void notice_scroll(void *user, double delta) {
    struct recon_notice *notice = user;
    notice->page.scroll += (delta > 0) ? 3 : -3;
    if (notice->page.scroll < 0) {
        notice->page.scroll = 0;
    }
}

static bool notice_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_notice *notice = user;
    (void)modifiers;

    switch (sym) {
    /* Both, because this is a notice rather than a question: there is no
     * answer to give, only an acknowledgement, and either key gives it. */
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_Escape:
        notice_dismiss(notice);
        return true;
    case XKB_KEY_Down:
        notice->page.scroll++;
        return true;
    case XKB_KEY_Up:
        if (notice->page.scroll > 0) {
            notice->page.scroll--;
        }
        return true;
    case XKB_KEY_Page_Down:
        notice->page.scroll += notice->page.visible_lines;
        return true;
    case XKB_KEY_Page_Up:
        notice->page.scroll -= notice->page.visible_lines;
        if (notice->page.scroll < 0) {
            notice->page.scroll = 0;
        }
        return true;
    default:
        return false;
    }
}

static void notice_describe(void *user, char *out, size_t size) {
    struct recon_notice *notice = user;
    snprintf(out, size, "%s, %d lines, seen version \"%s\"", notice->version,
        notice->page.line_count, notice_seen());
}

static void notice_destroy(void *user) {
    free(user);
}

static const struct recon_appwin_impl NOTICE_IMPL = {
    .title = "What's New",
    .icon = RECON_ICON_HELP,
    .default_width = 460,
    .default_height = 340,
    .min_width = 320,
    .min_height = 200,
    .draw = notice_draw,
    .click = notice_click,
    .key = notice_key,
    .scroll = notice_scroll,
    .describe = notice_describe,
    .destroy = notice_destroy,
};

struct recon_appwin *recon_help_notice_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_notice *notice = calloc(1, sizeof(*notice));
    if (notice == NULL) {
        return NULL;
    }

    notice->server = server;
    notice->font = font;
    snprintf(notice->version, sizeof(notice->version), "v%s", RECONOS_VERSION);

    notice->win = recon_appwin_create(server, font, &NOTICE_IMPL, notice);
    if (notice->win == NULL) {
        free(notice);
        return NULL;
    }
    return notice->win;
}
