/*
 * The web viewer window. See include/recon_web.h.
 *
 * Layout happens during drawing rather than in a pass of its own, and the link
 * hit regions are registered as the words are placed. That is not laziness: it
 * means what is clickable is, by construction, exactly what was drawn. A
 * separate layout pass is a second set of arithmetic that can disagree with the
 * first, and the disagreement shows up as a link that is a few pixels from
 * where it looks.
 *
 * The cost is that the total height is known only after a draw, so the first
 * frame of a page cannot size its scrollbar. It is measured once when the page
 * arrives -- the same code, with the drawing turned off.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_fs.h"
#include "recon_html.h"
#include "recon_http.h"
#include "recon_icons.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_web.h"

#define COLOR_BG THEME(SURFACE)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_DIM THEME(SURFACE_TEXT_DIM)
#define COLOR_LINK THEME(ACCENT)
#define COLOR_BAR THEME(BAR)
#define COLOR_RULE THEME(MENU_SEPARATOR)
#define COLOR_WARNING THEME(WARNING)
#define COLOR_QUOTE THEME(SURFACE_ALT)

/*
 * What this application is called, in one place.
 *
 * It is both the name in the Apps menu and the name the network permission and
 * the firewall know it by, and those have to be the same string. They were
 * "Web" and "Browser": everything compiled, and granting Web the network left
 * a program that could not reach it, complaining about a name that appears
 * nowhere in the interface.
 */
#define WEB_APPLICATION "Web"

#define BAR_HEIGHT 34
#define STATUS_HEIGHT 24
#define PADDING 10
#define FIELD_HEIGHT 24
#define BUTTON_WIDTH 30

/* Narrow, and the same width the Control Panel uses. There is no shared
 * scrollbar in recon_ui; each window that wants one draws it. */
#define SCROLLBAR_WIDTH 7

/* The body text size, and how much bigger each heading level is. h1 is the
 * biggest and h6 is barely distinguishable from text, which is what those
 * levels mean. */
#define BODY_SIZE 15
static const int HEADING_SIZE[7] = { 0, 26, 22, 19, 17, 16, 15 };

/* How far a list item is indented per level of nesting. */
#define INDENT 22

/* Where the visit history can go back to. Bounded, and old entries fall off
 * the front -- a history nobody can exhaust is a list that grows forever. */
#define HISTORY_MAX 64

#define HIT_BACK (RECON_APPWIN_HIT_USER + 1)
#define HIT_FORWARD (RECON_APPWIN_HIT_USER + 2)
#define HIT_RELOAD (RECON_APPWIN_HIT_USER + 3)
#define HIT_ADDRESS (RECON_APPWIN_HIT_USER + 4)
#define HIT_LINK_BASE (RECON_APPWIN_HIT_USER + 100)

struct recon_web {
    struct recon_font *font;          /* the window's, for the bar */
    struct recon_appwin *win;

    struct recon_edit address;
    struct recon_http_request *request;
    struct recon_html_document *page;

    /* Where the page on screen came from, which is not always what was typed:
     * a redirect changes it, and an address bar showing the old one is lying
     * about what is on screen. */
    struct recon_http_url url;
    bool have_url;

    /* Back and forward. `at` is where in the list the current page is, so
     * going back does not throw away what is in front of it. */
    struct recon_http_url history[HISTORY_MAX];
    int history_count;
    int at;

    int scroll;
    int content_height;
    int viewport_height;

    char status[256];
    bool status_is_error;
    bool loading;
    size_t received;
};

static void set_status(struct recon_web *w, bool error, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_status(struct recon_web *w, bool error, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(w->status, sizeof(w->status), fmt, args);
    va_end(args);
    w->status_is_error = error;
}

/* --- Fetching --- */

static void go_to(struct recon_web *w, const struct recon_http_url *url,
    bool remember);

static void on_progress(void *user, size_t received) {
    struct recon_web *w = user;
    w->received = received;
    /* Only every few kilobytes: a status line rewritten on every packet is a
     * status line nobody can read and a redraw nobody asked for. */
    if (received % 16384 < 4096) {
        set_status(w, false, "Reading... %zu KB", received / 1024);
        recon_appwin_refresh(w->win);
    }
}

/* Measure the page without drawing it, so the scrollbar is right on the first
 * frame. Defined after the layout it shares. */
static int measure(struct recon_web *w, int width);

/* Everything that happens once a document has been read, from wherever.
 * Defined below, beside the one other thing that reads one. */
static void show_document(struct recon_web *w, struct recon_html_document *page,
    const char *shown, size_t length);

static void on_done(void *user, bool ok, char *body, size_t length,
        const char *content_type, const struct recon_http_url *final_url,
        const char *error) {
    struct recon_web *w = user;

    w->request = NULL;
    w->loading = false;

    if (!ok) {
        set_status(w, true, "%s", error != NULL ? error : "that did not work");
        recon_appwin_refresh(w->win);
        return;
    }

    /*
     * text/plain is read as text rather than as markup. A large fraction of
     * what is worth reading is a plain file -- an RFC, a changelog, a README
     * -- and running one through an HTML parser eats every "<" in it.
     */
    bool is_html = (content_type == NULL || content_type[0] == '\0' ||
        strstr(content_type, "html") != NULL);
    struct recon_html_document *page = is_html
        ? recon_html_parse(body, length) : recon_html_plain(body, length);
    free(body);

    /* Where it actually came from, which a redirect may have changed. */
    w->url = *final_url;
    w->have_url = true;
    if (w->history_count > 0 && w->at >= 0 && w->at < w->history_count) {
        w->history[w->at] = w->url;
    }

    char text[RECON_HTTP_URL_MAX];
    recon_http_format_url(&w->url, text, sizeof(text));
    show_document(w, page, text, length);
}

static const struct recon_http_handlers HANDLERS = {
    .progress = on_progress,
    .done = on_done,
};

static void go_to(struct recon_web *w, const struct recon_http_url *url,
        bool remember) {
    if (w->request != NULL) {
        recon_http_cancel(w->request);
        w->request = NULL;
    }

    if (remember) {
        /*
         * Following a link from halfway back through the history throws away
         * what was in front of it, which is what every browser does and is the
         * only sensible answer: the forward list was a different journey.
         */
        w->history_count = (w->at >= 0) ? w->at + 1 : 0;
        if (w->history_count >= HISTORY_MAX) {
            memmove(w->history, w->history + 1,
                sizeof(w->history[0]) * (HISTORY_MAX - 1));
            w->history_count = HISTORY_MAX - 1;
        }
        w->history[w->history_count] = *url;
        w->at = w->history_count;
        w->history_count++;
    }

    char text[RECON_HTTP_URL_MAX];
    recon_http_format_url(url, text, sizeof(text));

    w->loading = true;
    w->received = 0;
    set_status(w, false, "Asking %s...", url->host);

    w->request = recon_http_get(WEB_APPLICATION, url, &HANDLERS, w);
    if (w->request == NULL) {
        w->loading = false;
        set_status(w, true, "%s", recon_http_last_error());
    }
    recon_appwin_refresh(w->win);
}

static void go_typed(struct recon_web *w) {
    struct recon_http_url url;
    if (!recon_http_parse_url(w->address.text,
            w->have_url ? &w->url : NULL, &url)) {
        set_status(w, true, "%s", recon_http_last_error());
        return;
    }
    go_to(w, &url, true);
}

/* --- Laying the page out --- */

/*
 * One pass that both measures and draws.
 *
 * `panel` NULL means measure only, which is how the height is known before the
 * first frame. Sharing the arithmetic is the point: two copies of a word-wrap
 * loop drift, and the drift shows up as a scrollbar that does not reach the
 * bottom of the page.
 */
struct flow {
    struct recon_web *w;
    struct recon_panel *panel;        /* NULL to measure */
    int origin_x, origin_y;           /* where the content area starts */
    int width;
    int scroll;
    int height;                       /* filled in as it goes */
    int clip_top, clip_bottom;        /* rows outside this are not drawn */
};

/* The face and size a run wants. */
static struct recon_font *font_for(unsigned style, int size) {
    if ((style & RECON_HTML_MONO) != 0) {
        return recon_font_monospace(size);
    }
    if ((style & RECON_HTML_BOLD) != 0) {
        return recon_font_bold(size);
    }
    return recon_font_system(size);
}

/* Draw one word, if this pass draws. */
static void put_word(struct flow *f, struct recon_font *font, int x, int y,
        const char *text, size_t length, unsigned style, int link) {
    if (f->panel == NULL) {
        return;
    }

    int screen_y = y - f->scroll + f->origin_y;
    if (screen_y + 24 < f->clip_top || screen_y > f->clip_bottom) {
        return;                        /* off-screen; nothing to draw or hit */
    }

    /*
     * And off the right, which preformatted text does: it is not wrapped, so a
     * line wider than the window runs past it. Nothing there can be clicked,
     * but a hit region can still be *registered* there, and the hit table is
     * finite -- a long enough <pre> would fill it with regions nobody can
     * reach and leave no room for the ones they can.
     */
    if (x > f->width) {
        return;
    }

    char word[512];
    size_t take = length < sizeof(word) - 1 ? length : sizeof(word) - 1;
    memcpy(word, text, take);
    word[take] = '\0';

    bool is_link = (style & RECON_HTML_LINK) != 0 && link >= 0;
    int ascent = recon_font_ascent(font);
    int screen_x = x + f->origin_x;

    recon_draw_text(f->panel, font, screen_x, screen_y + ascent,
        f->width - x, word, is_link ? COLOR_LINK : COLOR_TEXT);

    if (is_link) {
        int w = recon_text_width(font, word);
        /* Underlined, not only coloured. A link that is only a different
         * colour is invisible to a reader who cannot see that difference,
         * which is the same reason the accessibility skins exist. */
        recon_fill_rect(f->panel, screen_x, screen_y + ascent + 2, w, 1,
            COLOR_LINK);
        recon_hit_add(f->panel, screen_x, screen_y, w,
            recon_font_line_height(font), HIT_LINK_BASE + link);
    }
}

/*
 * Flow one block's runs, wrapping at the width.
 *
 * Words are split on spaces and placed one at a time. A word wider than the
 * whole column is placed anyway and overhangs, rather than being broken --
 * breaking a word is a decision about somebody's language and getting it wrong
 * is worse than a line that is too long.
 */
static void flow_block(struct flow *f, const struct recon_html_block_entry *b) {
    struct recon_web *w = f->w;

    int size = BODY_SIZE;
    int indent = 0;
    const char *marker = NULL;

    switch (b->kind) {
    case RECON_HTML_HEADING:
        size = HEADING_SIZE[(b->level >= 1 && b->level <= 6) ? b->level : 6];
        break;
    case RECON_HTML_LIST_ITEM:
        indent = INDENT * (b->level > 0 ? b->level : 1);
        marker = "\xE2\x80\xA2";                        /* a bullet */
        break;
    case RECON_HTML_QUOTE:
        indent = INDENT;
        break;
    case RECON_HTML_RULE: {
        f->height += 6;
        if (f->panel != NULL) {
            int screen_y = f->height - f->scroll + f->origin_y;
            if (screen_y >= f->clip_top && screen_y <= f->clip_bottom) {
                recon_fill_rect(f->panel, f->origin_x, screen_y, f->width, 1,
                    COLOR_RULE);
            }
        }
        f->height += 7;
        return;
    }
    case RECON_HTML_PRE:
    case RECON_HTML_PARAGRAPH:
        break;
    }

    /* Space above a heading, so it belongs to what follows rather than
     * floating between two paragraphs. */
    if (b->kind == RECON_HTML_HEADING) {
        f->height += size / 2;
    }

    struct recon_font *measure_font = font_for(
        b->kind == RECON_HTML_HEADING ? RECON_HTML_BOLD : 0, size);
    int line_height = recon_font_line_height(measure_font);
    if (line_height <= 0) {
        line_height = size + 4;
    }

    if (marker != NULL && f->panel != NULL) {
        int screen_y = f->height - f->scroll + f->origin_y;
        if (screen_y >= f->clip_top && screen_y <= f->clip_bottom) {
            recon_draw_text(f->panel, measure_font,
                f->origin_x + indent - 14,
                screen_y + recon_font_ascent(measure_font), 12, marker,
                COLOR_DIM);
        }
    }

    if (b->kind == RECON_HTML_QUOTE && f->panel != NULL) {
        /* A rule down the left, which is what a quotation looks like
         * everywhere and does not need a colour to read as one. */
        int screen_y = f->height - f->scroll + f->origin_y;
        recon_fill_rect(f->panel, f->origin_x + 4, screen_y, 2, line_height,
            COLOR_RULE);
    }

    int x = indent;
    int y = f->height;
    bool anything_on_line = false;

    for (int i = 0; i < b->run_count; i++) {
        const struct recon_html_run *run =
            recon_html_run_at(w->page, b->first_run + i);
        if (run == NULL) {
            continue;
        }

        unsigned style = run->style;
        if (b->kind == RECON_HTML_HEADING) {
            style |= RECON_HTML_BOLD;
        }
        if (b->kind == RECON_HTML_PRE) {
            style |= RECON_HTML_MONO;
        }
        struct recon_font *font = font_for(style, size);

        /*
         * Preformatted text is not wrapped. Its author chose where the lines
         * end, and rewrapping it destroys the one thing they controlled.
         */
        if (b->kind == RECON_HTML_PRE) {
            put_word(f, font, x, y, run->text, run->length, style, run->link);

            /* Measured, not one character's width times the length. That is
             * right for a monospaced face and silently wrong for any other,
             * and this decides where the *next* run starts. */
            char measured[512];
            size_t take = run->length < sizeof(measured) - 1
                ? run->length : sizeof(measured) - 1;
            memcpy(measured, run->text, take);
            measured[take] = '\0';
            x += recon_text_width(font, measured);

            anything_on_line = true;
            continue;
        }

        size_t at = 0;
        while (at < run->length) {
            /* One word, and the space after it if there is one. */
            size_t word = at;
            while (word < run->length && run->text[word] != ' ') {
                word++;
            }
            size_t word_length = word - at;

            if (word_length > 0) {
                char text[512];
                size_t take = word_length < sizeof(text) - 1
                    ? word_length : sizeof(text) - 1;
                memcpy(text, run->text + at, take);
                text[take] = '\0';

                int wide = recon_text_width(font, text);

                if (anything_on_line && x + wide > f->width) {
                    x = indent;
                    y += line_height;
                    anything_on_line = false;
                }

                put_word(f, font, x, y, run->text + at, word_length, style,
                    run->link);
                x += wide;
                anything_on_line = true;
            }

            /* The space, which is a space in this font at this size. */
            if (word < run->length) {
                x += recon_text_width(font, " ");
                at = word + 1;
            } else {
                at = word;
            }
        }
    }

    f->height = y + line_height;

    /* A gap after a block, so paragraphs are paragraphs. Smaller after a
     * list item, because a list is one thing. */
    f->height += (b->kind == RECON_HTML_LIST_ITEM) ? 3 : 8;
}

static int run_flow(struct flow *f) {
    struct recon_web *w = f->w;
    f->height = 0;

    for (int i = 0; i < recon_html_block_count(w->page); i++) {
        const struct recon_html_block_entry *b = recon_html_block_at(w->page, i);
        if (b == NULL) {
            continue;
        }

        /*
         * A block entirely above the viewport still has to be flowed, because
         * its height is what puts the next one in the right place -- but its
         * words are not drawn, which put_word decides. Skipping the flow
         * entirely would need the height known in advance, which is the thing
         * being computed.
         */
        flow_block(f, b);
    }
    return f->height;
}

static int measure(struct recon_web *w, int width) {
    struct flow f;
    memset(&f, 0, sizeof(f));
    f.w = w;
    f.panel = NULL;
    f.width = width;
    return run_flow(&f);
}

/* --- Drawing --- */

/* A scrollbar down the right, drawn only when there is more than fits. */
static void draw_scrollbar(struct recon_panel *p, int x, int y, int h,
        int scroll, int visible, int total) {
    if (total <= visible || visible <= 0 || h <= 0) {
        return;
    }

    recon_fill_rect(p, x, y, SCROLLBAR_WIDTH, h, COLOR_BAR);

    /* The thumb is at least a few pixels tall however long the page is. A
     * thumb of zero height on a very long document is a scrollbar that says
     * nothing about where you are. */
    int thumb = (int)((long long)h * visible / total);
    if (thumb < 12) {
        thumb = 12;
    }
    if (thumb > h) {
        thumb = h;
    }

    int most = total - visible;
    int at = (most > 0) ? (int)((long long)(h - thumb) * scroll / most) : 0;

    recon_fill_rect(p, x + 1, y + at, SCROLLBAR_WIDTH - 2, thumb, COLOR_DIM);
}

static void web_draw(void *user, struct recon_panel *p, int x, int y, int width,
        int height) {
    struct recon_web *w = user;
    int ascent = recon_font_ascent(w->font);

    recon_fill_rect(p, x, y, width, height, COLOR_BG);

    /* --- The bar --- */
    recon_fill_rect(p, x, y, width, BAR_HEIGHT, COLOR_BAR);
    recon_fill_rect(p, x, y + BAR_HEIGHT - 1, width, 1, COLOR_RULE);

    int bx = x + 6;
    int by = y + (BAR_HEIGHT - FIELD_HEIGHT) / 2;

    bool can_back = w->at > 0;
    bool can_forward = w->at >= 0 && w->at + 1 < w->history_count;

    static const struct { const char *glyph; uint32_t hit; } BUTTONS[] = {
        { "\xE2\x86\x90", HIT_BACK },       /* left arrow */
        { "\xE2\x86\x92", HIT_FORWARD },    /* right arrow */
        { "\xE2\x86\xBB", HIT_RELOAD },     /* a circling arrow */
    };

    for (int i = 0; i < 3; i++) {
        bool on = (i == 0) ? can_back
            : (i == 1) ? can_forward
            : w->have_url;

        recon_draw_button_edge(p, bx, by, BUTTON_WIDTH, FIELD_HEIGHT, false,
            COLOR_BAR);
        int gw = recon_text_width(w->font, BUTTONS[i].glyph);
        recon_draw_text(p, w->font, bx + (BUTTON_WIDTH - gw) / 2,
            by + (FIELD_HEIGHT + ascent) / 2 - 2, BUTTON_WIDTH,
            BUTTONS[i].glyph, on ? COLOR_TEXT : COLOR_DIM);
        if (on) {
            recon_hit_add(p, bx, by, BUTTON_WIDTH, FIELD_HEIGHT,
                BUTTONS[i].hit);
        }
        bx += BUTTON_WIDTH + 4;
    }

    int field_width = width - (bx - x) - 8;
    recon_edit_draw(p, w->font, bx, by, field_width, FIELD_HEIGHT, &w->address);
    recon_hit_add(p, bx, by, field_width, FIELD_HEIGHT, HIT_ADDRESS);

    /* --- The status line --- */
    int status_y = y + height - STATUS_HEIGHT;
    recon_fill_rect(p, x, status_y, width, STATUS_HEIGHT, COLOR_BAR);
    recon_fill_rect(p, x, status_y, width, 1, COLOR_RULE);
    recon_draw_text(p, w->font, x + 8,
        status_y + (STATUS_HEIGHT + ascent) / 2 - 1, width - 16, w->status,
        w->status_is_error ? COLOR_WARNING : COLOR_DIM);

    /* --- The page --- */
    int top = y + BAR_HEIGHT + PADDING;
    int bottom = status_y - PADDING;
    w->viewport_height = bottom - top;

    if (w->page == NULL || recon_html_block_count(w->page) == 0) {
        if (!w->loading && w->status[0] == '\0') {
            recon_draw_text(p, w->font, x + PADDING, top + ascent,
                width - PADDING * 2,
                "Type an address above. This is a viewer for simple pages: "
                "no stylesheets, no scripts, no pictures.", COLOR_DIM);
        }
        return;
    }

    /* Content is inset from the right by the scrollbar's width whether or not
     * one is showing, so the text does not reflow when it appears. */
    int content_width = width - PADDING * 2 - SCROLLBAR_WIDTH;

    if (w->content_height <= 0) {
        w->content_height = measure(w, content_width);
    }

    int most = w->content_height - w->viewport_height;
    if (most < 0) {
        most = 0;
    }
    if (w->scroll > most) {
        w->scroll = most;
    }
    if (w->scroll < 0) {
        w->scroll = 0;
    }

    struct flow f;
    memset(&f, 0, sizeof(f));
    f.w = w;
    f.panel = p;
    f.origin_x = x + PADDING;
    f.origin_y = top;
    f.width = content_width;
    f.scroll = w->scroll;
    f.clip_top = top;
    f.clip_bottom = bottom;

    w->content_height = run_flow(&f);

    if (w->content_height > w->viewport_height) {
        draw_scrollbar(p, x + width - SCROLLBAR_WIDTH, top,
            w->viewport_height, w->scroll, w->viewport_height,
            w->content_height);
    }
}

/* --- Input --- */

static bool web_click(void *user, uint32_t hit, int cx, int cy, bool pressed) {
    struct recon_web *w = user;
    (void)cx;
    (void)cy;

    if (!pressed || hit < RECON_APPWIN_HIT_USER) {
        return false;
    }

    if (hit >= HIT_LINK_BASE) {
        int link = (int)(hit - HIT_LINK_BASE);
        const char *href = recon_html_link_at(w->page, link);
        if (href == NULL) {
            return true;
        }

        struct recon_http_url next;
        if (!recon_http_parse_url(href, w->have_url ? &w->url : NULL, &next)) {
            set_status(w, true, "%s", recon_http_last_error());
            return true;
        }

        /*
         * A link to the page already showing.
         *
         * Almost always a fragment -- "#notes" -- and there is no anchor
         * navigation here, so following it would fetch the same document again,
         * throw away the scroll position, and add a history entry that goes
         * nowhere. Saying so is more use than doing that.
         */
        char here[RECON_HTTP_URL_MAX];
        char there[RECON_HTTP_URL_MAX];
        recon_http_format_url(&w->url, here, sizeof(here));
        recon_http_format_url(&next, there, sizeof(there));
        if (w->have_url && strcmp(here, there) == 0) {
            set_status(w, false, "That points at a place on this page, which "
                "this cannot jump to yet.");
            return true;
        }

        go_to(w, &next, true);
        return true;
    }

    switch (hit) {
    case HIT_BACK:
        if (w->at > 0) {
            w->at--;
            go_to(w, &w->history[w->at], false);
        }
        return true;
    case HIT_FORWARD:
        if (w->at + 1 < w->history_count) {
            w->at++;
            go_to(w, &w->history[w->at], false);
        }
        return true;
    case HIT_RELOAD:
        if (w->have_url) {
            go_to(w, &w->url, false);
        }
        return true;
    case HIT_ADDRESS:
        /* The whole address selected, so typing replaces it -- which is what
         * somebody clicking an address bar almost always means. */
        recon_edit_begin(&w->address, w->address.text, false);
        return true;
    default:
        return false;
    }
}

static bool web_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_web *w = user;

    if (w->address.active) {
        switch (recon_edit_key(&w->address, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            w->address.active = false;
            go_typed(w);
            return true;
        case RECON_EDIT_CANCEL:
            w->address.active = false;
            return true;
        case RECON_EDIT_CHANGED:
            return true;
        case RECON_EDIT_IGNORED:
            break;
        }
    }

    int page = w->viewport_height > 40 ? w->viewport_height - 20 : 40;

    switch (sym) {
    case XKB_KEY_Down:      w->scroll += 40; return true;
    case XKB_KEY_Up:        w->scroll -= 40; return true;
    case XKB_KEY_Page_Down:
    case XKB_KEY_space:     w->scroll += page; return true;
    case XKB_KEY_Page_Up:   w->scroll -= page; return true;
    case XKB_KEY_Home:      w->scroll = 0; return true;
    case XKB_KEY_End:       w->scroll = w->content_height; return true;

    case XKB_KEY_l:
    case XKB_KEY_L:
        /* Ctrl+L to the address bar, which is where every browser puts it. */
        if ((modifiers & RECON_MOD_CTRL) != 0) {
            recon_edit_begin(&w->address, w->address.text, false);
            return true;
        }
        return false;

    case XKB_KEY_BackSpace:
        if (w->at > 0) {
            w->at--;
            go_to(w, &w->history[w->at], false);
        }
        return true;

    default:
        return false;
    }
}

static void web_scroll(void *user, double delta) {
    struct recon_web *w = user;
    w->scroll -= (int)(delta * 48);
    if (w->scroll < 0) {
        w->scroll = 0;
    }
}

static void web_describe(void *user, char *out, size_t size) {
    struct recon_web *w = user;
    char address[RECON_HTTP_URL_MAX] = "(none)";
    if (w->have_url) {
        recon_http_format_url(&w->url, address, sizeof(address));
    }

    snprintf(out, size,
        "  address: %s\n"
        "  title: %s\n"
        "  blocks: %d\n"
        "  history: %d, at %d\n"
        "  scroll: %d of %d\n"
        "  loading: %s\n"
        "  status: %s\n",
        address, w->page != NULL ? recon_html_title(w->page) : "",
        recon_html_block_count(w->page), w->history_count, w->at,
        w->scroll, w->content_height, w->loading ? "yes" : "no", w->status);
}

static void web_destroy(void *user) {
    struct recon_web *w = user;
    if (w->request != NULL) {
        recon_http_cancel(w->request);
    }
    recon_html_free(w->page);
    free(w);
}

static const struct recon_appwin_impl WEB_IMPL = {
    .title = WEB_APPLICATION,
    .help = "Networking",
    .icon = RECON_ICON_WEB,
    .default_width = 800,
    .default_height = 600,
    .min_width = 360,
    .min_height = 240,
    .draw = web_draw,
    .click = web_click,
    .key = web_key,
    .scroll = web_scroll,
    .describe = web_describe,
    .destroy = web_destroy,
};

/*
 * Everything that has to happen once a document has been read, from wherever.
 *
 * Shared by the fetch and by opening a file, because the two differ only in
 * where the bytes came from -- and a second copy of "work out the title, reset
 * the scroll, say how big it is" is a second copy that gets one of them wrong.
 */
static void show_document(struct recon_web *w, struct recon_html_document *page,
        const char *shown, size_t length) {
    recon_html_free(w->page);
    w->page = page;

    recon_edit_begin(&w->address, shown, false);
    w->address.active = false;

    /*
     * The window's title comes from the document, which means it comes from
     * somebody else. Truncated, and stripped of control characters -- a title
     * containing a newline or a backspace is not a title, it is somebody
     * finding out what this system's title bar does with one.
     */
    const char *title = recon_html_title(w->page);
    const char *source = (title != NULL && title[0] != '\0') ? title : shown;

    char window_title[160];
    size_t used = 0;
    for (const unsigned char *c = (const unsigned char *)source;
            *c != '\0' && used < sizeof(window_title) - 1; c++) {
        /* UTF-8 sequences are all above 0x7F, so they pass through. */
        if (*c >= 0x20 && *c != 0x7F) {
            window_title[used++] = (char)*c;
        }
    }
    window_title[used] = '\0';
    recon_appwin_set_title(w->win, window_title);

    w->scroll = 0;
    w->content_height = 0;

    if (recon_html_needs_scripting(w->page)) {
        set_status(w, true, "That page builds itself with JavaScript, which "
            "this does not have. There is nothing to show.");
    } else if (recon_html_block_count(w->page) == 0) {
        set_status(w, true, "There is nothing readable there.");
    } else {
        set_status(w, false, "%d blocks, %zu KB",
            recon_html_block_count(w->page), length / 1024);
    }

    recon_appwin_refresh(w->win);
}

bool recon_web_open_path(struct recon_appwin *win, const char *path) {
    if (win == NULL || path == NULL) {
        return false;
    }
    struct recon_web *w = recon_appwin_user(win);
    if (w == NULL) {
        return false;
    }

    if (w->request != NULL) {
        recon_http_cancel(w->request);
        w->request = NULL;
        w->loading = false;
    }

    size_t length = 0;
    char *text = recon_fs_read("/", path, &length);
    if (text == NULL) {
        set_status(w, true, "%s", recon_fs_last_error());
        recon_appwin_refresh(w->win);
        return false;
    }

    /*
     * There is no address, so relative links in this document have nothing to
     * resolve against. Said by leaving have_url false, which the link handler
     * reads -- rather than inventing a base, which would send somebody to a
     * server that has nothing to do with the file they opened.
     */
    w->have_url = false;
    w->history_count = 0;
    w->at = -1;

    const char *dot = strrchr(path, '.');
    bool markup = dot != NULL &&
        (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0);

    show_document(w, markup ? recon_html_parse(text, length)
                            : recon_html_plain(text, length),
        path, length);
    free(text);
    return true;
}

struct recon_appwin *recon_web_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_web *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return NULL;
    }

    w->font = font;
    w->at = -1;
    recon_edit_begin(&w->address, "", false);
    w->address.active = false;

    w->win = recon_appwin_create(server, font, &WEB_IMPL, w);
    if (w->win == NULL) {
        free(w);
        return NULL;
    }
    return w->win;
}
