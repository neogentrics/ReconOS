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
#define HIT_BODY (RECON_APPWIN_HIT_USER + 200)

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

struct recon_help {
    struct recon_font *font;
    struct recon_appwin *win;

    struct topic topics[TOPICS_MAX];
    int topic_count;
    int selected;

    /* The chosen topic, already wrapped to the width it was wrapped at. */
    char lines[LINES_MAX][LINE_MAX];
    int line_count;
    int wrapped_width;

    int scroll;
    int visible_lines;
};

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
static void wrap_paragraph(struct recon_help *help, const char *text,
        int width, const char *hanging) {
    char current[LINE_MAX];
    current[0] = '\0';

    const char *word = text;
    while (*word == ' ') {
        word++;
    }

    while (*word != '\0' && help->line_count < LINES_MAX) {
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
                 recon_text_width(help->font, candidate) > width)) {
            snprintf(help->lines[help->line_count++], LINE_MAX, "%s", current);

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

    if (current[0] != '\0' && help->line_count < LINES_MAX) {
        snprintf(help->lines[help->line_count++], LINE_MAX, "%s", current);
    }
}

/*
 * Wrap the chosen topic to the width it will be drawn at.
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
static void wrap_topic(struct recon_help *help, int width) {
    help->line_count = 0;
    help->wrapped_width = width;

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
        snprintf(help->lines[0], LINE_MAX, "This page is missing.");
        help->line_count = 1;
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

    while (help->line_count < LINES_MAX) {
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
            wrap_paragraph(help, paragraph, width, is_item ? "  " : "");
            used = 0;
            paragraph[0] = '\0';
        }

        if (line == NULL) {
            break;
        }

        if (blank) {
            /* One blank line between paragraphs, however many the source
             * happens to have. */
            if (help->line_count > 0 &&
                    help->lines[help->line_count - 1][0] != '\0') {
                help->lines[help->line_count++][0] = '\0';
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
                wrap_paragraph(help, paragraph, width, is_item ? "  " : "");
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

static void choose(struct recon_help *help, int index) {
    if (index < 0 || index >= help->topic_count) {
        return;
    }
    help->selected = index;
    help->scroll = 0;
    /* Re-wrapped on the next draw, which is where the width is known. */
    help->wrapped_width = 0;
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
    recon_hit_clear(panel);

    /* --- The topics --- */
    recon_fill_rect(panel, x, y, SIDEBAR_WIDTH, h, COLOR_SIDEBAR);
    recon_fill_rect(panel, x + SIDEBAR_WIDTH, y, 1, h, COLOR_SEPARATOR);

    int ty = y + PADDING / 2;
    bool divided = false;

    for (int i = 0; i < help->topic_count; i++) {
        /*
         * A rule where the change log begins. The two halves are read
         * differently -- one is looked up, the other is read through -- and a
         * single unbroken list of thirty entries hides that.
         */
        if (help->topics[i].is_change && !divided) {
            divided = true;
            ty += 6;
            recon_fill_rect(panel, x + PADDING, ty, SIDEBAR_WIDTH - PADDING * 2,
                1, COLOR_SEPARATOR);
            ty += 4;

            recon_draw_text(panel, help->font, x + PADDING, ty + ascent,
                SIDEBAR_WIDTH - PADDING * 2, "What changed", COLOR_DIM);
            ty += line_height + 2;
        }

        if (ty + ROW_HEIGHT > y + h) {
            break;
        }

        bool chosen = (i == help->selected);
        if (chosen) {
            recon_fill_role(panel, x + 2, ty, SIDEBAR_WIDTH - 4, ROW_HEIGHT,
                RECON_THEME_SELECTION);
        }

        recon_draw_text(panel, help->font, x + PADDING,
            ty + (ROW_HEIGHT + ascent) / 2 - 2,
            SIDEBAR_WIDTH - PADDING * 2, help->topics[i].title,
            chosen ? COLOR_SELECTION_TEXT : COLOR_TEXT);

        recon_hit_add(panel, x + 2, ty, SIDEBAR_WIDTH - 4, ROW_HEIGHT,
            HIT_TOPIC_BASE + i);
        ty += ROW_HEIGHT;
    }

    /* --- The page --- */
    int body_x = x + SIDEBAR_WIDTH + 1 + PADDING * 2;
    int body_w = w - (body_x - x) - PADDING * 2;
    if (body_w < 80) {
        body_w = 80;
    }

    if (help->wrapped_width != body_w) {
        wrap_topic(help, body_w);
    }

    int body_y = y + PADDING;
    help->visible_lines = (h - PADDING * 2 - line_height) / line_height;
    if (help->visible_lines < 1) {
        help->visible_lines = 1;
    }

    if (help->selected >= 0 && help->selected < help->topic_count) {
        recon_draw_text(panel, help->font, body_x, body_y + ascent, body_w,
            help->topics[help->selected].title, COLOR_HEADING);
        body_y += line_height + 4;
    }

    if (help->scroll > help->line_count - help->visible_lines) {
        help->scroll = help->line_count - help->visible_lines;
    }
    if (help->scroll < 0) {
        help->scroll = 0;
    }

    for (int i = 0; i < help->visible_lines; i++) {
        int index = help->scroll + i;
        if (index >= help->line_count) {
            break;
        }
        recon_draw_text(panel, help->font, body_x,
            body_y + i * line_height + ascent, body_w,
            help->lines[index], COLOR_TEXT);
    }

    recon_hit_add(panel, x + SIDEBAR_WIDTH + 1, y, w - SIDEBAR_WIDTH - 1, h,
        HIT_BODY);

    /* How much more there is, when there is more. */
    if (help->line_count > help->visible_lines) {
        char more[64];
        int remaining = help->line_count - help->scroll - help->visible_lines;
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

static void help_scroll(void *user, double delta) {
    struct recon_help *help = user;
    help->scroll += (delta > 0) ? 3 : -3;
    if (help->scroll < 0) {
        help->scroll = 0;
    }
}

static bool help_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_help *help = user;
    (void)modifiers;

    switch (sym) {
    case XKB_KEY_Down:
        help->scroll++;
        return true;
    case XKB_KEY_Up:
        if (help->scroll > 0) {
            help->scroll--;
        }
        return true;
    case XKB_KEY_Page_Down:
        help->scroll += help->visible_lines;
        return true;
    case XKB_KEY_Page_Up:
        help->scroll -= help->visible_lines;
        if (help->scroll < 0) {
            help->scroll = 0;
        }
        return true;
    case XKB_KEY_Home:
        help->scroll = 0;
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
        help->scroll + 1, help->line_count);
}

static void help_destroy(void *user) {
    free(user);
}

static const struct recon_appwin_impl HELP_IMPL = {
    .title = "Help",
    .icon = RECON_ICON_HELP,
    .default_width = 640,
    .default_height = 460,
    .min_width = 420,
    .min_height = 260,
    .draw = help_draw,
    .click = help_click,
    .key = help_key,
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
