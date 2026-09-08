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
#include "recon_css.h"
#include "recon_html.h"
#include "recon_http.h"
#include "recon_icons.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_widget.h"
#include "recon_web.h"

/*
 * The decoder, included here rather than linked, the way every other place in
 * ReconOS that reads a picture does it. STB_IMAGE_IMPLEMENTATION is defined
 * exactly once in this program and it is not here.
 */
#include "stb_image.h"

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

/*
 * --- The chrome, top to bottom ---
 *
 *   tab strip     which page is in front, and the way to a new one
 *   toolbar       back, forward, stop/reload, home | address | star, menu
 *   bookmarks     when it is turned on
 *   the page
 *   find bar      when it is open
 *   status        what the page is, and what the text size is
 *
 * Each of these is a band the page is inset by rather than something drawn
 * over it. A bar over the page hides part of it, which for the find bar is
 * the one thing it must not do -- the match you are looking for is as likely
 * to be under the bar as anywhere else.
 */
#define TABSTRIP_HEIGHT 28
#define BAR_HEIGHT 34
#define MARKS_HEIGHT 26
#define STRIP_HEIGHT 28
#define STATUS_HEIGHT 24
#define PADDING 10
#define FIELD_HEIGHT 24
#define BUTTON_WIDTH 30

/* A tab shrinks as more open, down to a floor -- below which a tab shows no
 * title at all, and a strip of eight identical stubs is a strip that says
 * nothing. Past the floor the strip scrolls rather than shrinking further. */
#define TAB_MIN_WIDTH 84
#define TAB_MAX_WIDTH 190

/*
 * How far one press of zoom moves, and how far it may go.
 *
 * The floor and ceiling are not politeness: at 25% the page is unreadable and
 * at 400% one word fills the window, and both are states somebody can reach by
 * holding a key down and then cannot read their way out of.
 */
#define ZOOM_STEP 10
#define ZOOM_MIN 50
#define ZOOM_MAX 250

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

/*
 * The hit ids.
 *
 * The bases are spaced with room for the whole of what they index -- links go
 * up to two thousand, so they start last and everything else fits below them.
 * A base that overlaps the next one is a click on a bookmark that opens a
 * link, and nothing about the drawing code would look wrong.
 */
#define HIT_BACK (RECON_APPWIN_HIT_USER + 1)
#define HIT_FORWARD (RECON_APPWIN_HIT_USER + 2)
#define HIT_RELOAD (RECON_APPWIN_HIT_USER + 3)
#define HIT_ADDRESS (RECON_APPWIN_HIT_USER + 4)
#define HIT_HOME (RECON_APPWIN_HIT_USER + 5)
#define HIT_STAR (RECON_APPWIN_HIT_USER + 6)
#define HIT_MENU (RECON_APPWIN_HIT_USER + 7)
#define HIT_NEWTAB (RECON_APPWIN_HIT_USER + 8)
#define HIT_FIND_FIELD (RECON_APPWIN_HIT_USER + 9)
#define HIT_FIND_PREV (RECON_APPWIN_HIT_USER + 10)
#define HIT_FIND_NEXT (RECON_APPWIN_HIT_USER + 11)
#define HIT_FIND_CLOSE (RECON_APPWIN_HIT_USER + 12)

#define HIT_MENU_BASE (RECON_APPWIN_HIT_USER + 20)     /* MENU_COUNT */
#define HIT_TAB_BASE (RECON_APPWIN_HIT_USER + 50)      /* TABS_MAX */
#define HIT_TABCLOSE_BASE (RECON_APPWIN_HIT_USER + 70) /* TABS_MAX */
#define HIT_MARK_BASE (RECON_APPWIN_HIT_USER + 100)    /* BOOKMARKS_MAX */
#define HIT_HIST_BASE (RECON_APPWIN_HIT_USER + 200)    /* HISTORY_MAX */
#define HIT_LINK_BASE (RECON_APPWIN_HIT_USER + 1000)

/*
 * What is in the toolbar menu.
 *
 * One table, read by both the drawing and the click handling, so an entry
 * cannot be drawn in one place and acted on in another. That is not a
 * hypothetical: the entries move whenever one is added, and a second list of
 * cases in a switch would go stale silently.
 */
enum web_menu_item {
    MENU_NEW_TAB,
    MENU_CLOSE_TAB,
    MENU_SEP_1,
    MENU_FIND,
    MENU_ZOOM_IN,
    MENU_ZOOM_OUT,
    MENU_ZOOM_RESET,
    MENU_SEP_2,
    MENU_ADD_BOOKMARK,
    MENU_SHOW_MARKS,
    MENU_BOOKMARKS,
    MENU_SEP_3,
    MENU_HISTORY,
    MENU_SET_HOME,
    MENU_COUNT
};

static const struct {
    const char *label;
    const char *keys;                 /* NULL for an entry with no shortcut */
    bool separator;
} MENU[MENU_COUNT] = {
    [MENU_NEW_TAB]       = { "New tab",            "Ctrl+T", false },
    [MENU_CLOSE_TAB]     = { "Close tab",          "Ctrl+W", false },
    [MENU_SEP_1]         = { NULL,                 NULL,     true  },
    [MENU_FIND]          = { "Find on page",       "Ctrl+F", false },
    [MENU_ZOOM_IN]       = { "Zoom in",            "Ctrl++", false },
    [MENU_ZOOM_OUT]      = { "Zoom out",           "Ctrl+-", false },
    [MENU_ZOOM_RESET]    = { "Reset text size",    "Ctrl+0", false },
    [MENU_SEP_2]         = { NULL,                 NULL,     true  },
    [MENU_ADD_BOOKMARK]  = { "Bookmark this page", "Ctrl+D", false },
    [MENU_SHOW_MARKS]    = { "Show bookmarks bar", NULL,     false },
    [MENU_BOOKMARKS]     = { "All bookmarks",      NULL,     false },
    [MENU_SEP_3]         = { NULL,                 NULL,     true  },
    [MENU_HISTORY]       = { "History",            "Ctrl+H", false },
    [MENU_SET_HOME]      = { "Set as home page",   NULL,     false },
};

#define MENU_WIDTH 232
#define MENU_ROW 22
#define MENU_SEPARATOR_ROW 7

/*
 * How many pictures one page may have, and how big one may be.
 *
 * Bounds rather than policy. A page that names four hundred images is a page
 * that would otherwise open four hundred connections, and a "picture" of forty
 * megabytes is not a picture, it is somebody finding out what this does with
 * one.
 */
/*
 * How much markup is worth keeping to parse a second time.
 *
 * A page that links stylesheets is parsed twice: once to find out what it
 * asks for, and again once they are here. Holding the source is what makes
 * the second pass possible, and holding a ten-megabyte document to restyle it
 * later is a cost every page would pay for the benefit of a few. Past this,
 * the page is shown as its first pass and never restyled.
 */
#define SOURCE_MAX (2 * 1024 * 1024)

#define IMAGES_MAX 48
#define IMAGE_BYTES_MAX (8 * 1024 * 1024)

/*
 * A picture on the page.
 *
 * `pixels` is RGBA at `width` by `height`, ours to free. `tried` says the
 * fetch has finished, whether or not it produced anything -- the two together
 * are three states: not asked for yet, asked for and failed, and here.
 */
struct web_image {
    char url[2048];
    unsigned char *pixels;
    int width;
    int height;
    bool tried;
};

/*
 * --- One page, and everything being done to it ---
 *
 * This was the whole application until tabs: a window held one document, one
 * history, one set of fetches in flight. Everything below the toolbar is per
 * page, so the split is exactly there -- the tab owns the document and the
 * work, the window owns the chrome around it.
 *
 * `font` and `win` are the window's, copied here rather than reached through
 * `owner`, because they are read on nearly every line of the drawing code and
 * never change for the life of the tab.
 *
 * The reason this matters beyond tidiness: a fetch belongs to the tab that
 * started it. Every HTTP callback is handed the *tab*, so a page that finishes
 * loading while you are looking at another one lands in its own tab instead of
 * overwriting whatever is in front of you.
 */
struct web_tab {
    struct recon_web *owner;

    struct recon_font *font;          /* the window's, for the bar */
    struct recon_appwin *win;

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

    /*
     * --- The pictures ---
     *
     * One entry per distinct address on the page, fetched one at a time after
     * the document itself has arrived and been drawn.
     *
     * One at a time, and after: a page is readable the moment its words are
     * there, and thirty sockets opened at once to decorate it would make the
     * words wait for the decoration. Each picture redraws the page as it
     * lands, so the page fills in rather than appearing complete or not at
     * all.
     *
     * A failure is remembered as a failure. Without that, a picture the
     * server will not give up is asked for again on every redraw, which is a
     * page that never stops loading.
     */
    struct web_image images[IMAGES_MAX];
    int image_count;
    int fetching;                          /* which one, or -1 */
    struct recon_http_request *image_request;

    /*
     * --- The stylesheets ---
     *
     * The page's own `<style>` elements go in as it is parsed. The ones it
     * links to have to be fetched, which means the page is parsed twice: once
     * to find out what it asks for, and again once the answers are here.
     *
     * `source` is the markup, kept for exactly that second pass and freed the
     * moment there is nothing left to fetch. A page larger than the cap is
     * shown from the first pass and never restyled, which is the honest
     * trade: holding a ten-megabyte document to restyle it later is a cost
     * paid by every page for the benefit of a few.
     */
    struct recon_css_sheet *sheet;
    char *source;
    size_t source_length;
    int sheets_wanted;
    int sheets_done;
    struct recon_http_request *sheet_request;
    bool restyled;

    /*
     * What this tab is called, and how big its text is.
     *
     * The title is the page's, falling back to its host -- a tab strip of
     * eight tabs all saying "Loading" is a strip that tells you nothing, and
     * the host is known before the document is.
     */
    char label[96];

    /*
     * A block this has been asked to scroll to, or -1.
     *
     * Where a block *is* is only known during the pass that lays the page
     * out, so following "#install" cannot scroll when the link is clicked --
     * it records which block, and the next draw finds it. The same two-step
     * as find, and for the same reason.
     */
    int jump_to;

    /*
     * Text size, as a percentage. Per tab rather than per window, because a
     * page you have zoomed in to read is a property of that page.
     */
    int zoom;
};

/* --- The window, and the chrome around whichever tab is in front --- */

/*
 * How many tabs one window may have.
 *
 * A bound, not a prediction. Each tab carries its own image table and its own
 * history, so a tab is not free, and a window with ninety of them is somebody
 * finding out what this does rather than somebody reading.
 */
#define TABS_MAX 12

/* How many addresses are kept, and how long one may be. */
#define BOOKMARKS_MAX 64

/* Where the bookmarks live, in the ReconOS filesystem. */
#define BOOKMARKS_PATH "/Users/Shared/Web/bookmarks.txt"

struct web_bookmark {
    char label[96];
    char url[RECON_HTTP_URL_MAX];
};

/*
 * Which strip of chrome is open below the toolbar.
 *
 * One at a time, and the page moves down to make room, because a bar drawn
 * over the page hides the thing being searched -- which for the find bar is
 * the one thing it must not do.
 */
enum web_strip {
    STRIP_NONE = 0,
    STRIP_FIND,
    STRIP_HISTORY,
};

struct recon_web {
    struct recon_font *font;
    struct recon_appwin *win;

    struct recon_edit address;

    struct web_tab *tabs[TABS_MAX];
    int tab_count;
    int active;

    /* Where the Home button goes, and where a new tab starts. */
    char home[RECON_HTTP_URL_MAX];

    struct web_bookmark bookmarks[BOOKMARKS_MAX];
    int bookmark_count;
    bool show_bookmarks;

    enum web_strip strip;
    struct recon_edit find;
    int find_count;                   /* matches on the page, after a search */
    int find_at;                      /* which one is highlighted, or -1 */

    /*
     * Somebody pressed Next and the page has not been laid out since.
     *
     * Where a match *is* only becomes known during the pass that draws it, so
     * the jump cannot happen when the button is pressed -- it happens on the
     * draw after, and this is what carries the intent across.
     *
     * A flag rather than doing it every frame on purpose. Scrolling to the
     * current match on every redraw would fight the scroll wheel: you would
     * push the page down and it would spring back to the match.
     */
    bool find_follow;

    /* The toolbar menu, open or not, and which entry the pointer is on. */
    bool menu_open;
};


/* --- Tabs --- */

static void forget_images(struct web_tab *w);
static void forget_sheets(struct web_tab *w);
static void go_to(struct web_tab *w, const struct recon_http_url *url,
    bool remember);

/* Whichever tab is in front. Never NULL: a window with no tabs closes. */
static struct web_tab *front(struct recon_web *w) {
    if (w->tab_count <= 0) {
        return NULL;
    }
    if (w->active < 0) {
        w->active = 0;
    }
    if (w->active >= w->tab_count) {
        w->active = w->tab_count - 1;
    }
    return w->tabs[w->active];
}

/*
 * A tab is heap-allocated because it is not small: an image table of
 * forty-eight entries and a history of sixty-four addresses is most of a
 * quarter of a megabyte, and twelve of those inside the window struct would
 * be three megabytes of window whether or not anybody opened a second tab.
 */
static struct web_tab *tab_new(struct recon_web *w) {
    if (w->tab_count >= TABS_MAX) {
        return NULL;
    }
    struct web_tab *t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->owner = w;
    t->font = w->font;
    t->win = w->win;
    t->at = -1;
    t->fetching = -1;
    t->zoom = 100;
    t->jump_to = -1;
    snprintf(t->label, sizeof(t->label), "New tab");

    w->tabs[w->tab_count++] = t;
    return t;
}

static void tab_free(struct web_tab *t) {
    if (t == NULL) {
        return;
    }
    /*
     * Everything in flight is cancelled first. A reply that arrives for a
     * freed tab is a write through a dangling pointer, and it is the kind
     * that happens rarely enough to survive testing.
     */
    if (t->request != NULL) {
        recon_http_cancel(t->request);
    }
    if (t->image_request != NULL) {
        recon_http_cancel(t->image_request);
    }
    if (t->sheet_request != NULL) {
        recon_http_cancel(t->sheet_request);
    }
    forget_images(t);
    forget_sheets(t);
    if (t->page != NULL) {
        recon_html_free(t->page);
    }
    free(t);
}

static void tab_close(struct recon_web *w, int index) {
    if (index < 0 || index >= w->tab_count) {
        return;
    }

    /*
     * The last tab is not closed, it is emptied. A window with no tabs has
     * nothing to draw and no way to get anywhere, which is a window that has
     * to close itself -- and a browser that vanishes when you close a tab is
     * a browser people lose work in.
     */
    if (w->tab_count == 1) {
        struct web_tab *t = w->tabs[0];
        if (t->request != NULL) {
            recon_http_cancel(t->request);
            t->request = NULL;
        }
        forget_images(t);
        forget_sheets(t);
        if (t->page != NULL) {
            recon_html_free(t->page);
            t->page = NULL;
        }
        t->have_url = false;
        t->history_count = 0;
        t->at = -1;
        t->scroll = 0;
        t->content_height = 0;
        t->loading = false;
        t->status[0] = '\0';
        snprintf(t->label, sizeof(t->label), "New tab");
        recon_edit_begin(&w->address, "", false);
        w->address.active = false;
        return;
    }

    tab_free(w->tabs[index]);
    for (int i = index; i + 1 < w->tab_count; i++) {
        w->tabs[i] = w->tabs[i + 1];
    }
    w->tab_count--;

    /*
     * Closing the tab in front leaves the one that was to its right in front,
     * or the last one if it was the last -- which is what every browser does
     * and what the hand expects when closing several in a row.
     */
    if (w->active > index || w->active >= w->tab_count) {
        w->active--;
    }
    if (w->active < 0) {
        w->active = 0;
    }
}

/* --- Bookmarks --- */

/*
 * Stored as one line per bookmark, `url<tab>label`.
 *
 * A tab rather than a space because a title has spaces in it and an address
 * does not, so the split is unambiguous with no quoting and no escape rules
 * to get wrong. Kept in the shared area rather than an account's, because the
 * account that reads them is the account that is signed in and there is one.
 */
static void bookmarks_load(struct recon_web *w) {
    w->bookmark_count = 0;

    size_t length = 0;
    char *text = recon_fs_read("/", BOOKMARKS_PATH, &length);
    if (text == NULL) {
        return;
    }

    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save);
            line != NULL && w->bookmark_count < BOOKMARKS_MAX;
            line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        if (tab == NULL || tab == line) {
            continue;
        }
        *tab = '\0';
        struct web_bookmark *b = &w->bookmarks[w->bookmark_count++];
        snprintf(b->url, sizeof(b->url), "%s", line);
        snprintf(b->label, sizeof(b->label), "%s",
            tab[1] != '\0' ? tab + 1 : line);
    }
    free(text);
}

static void bookmarks_save(struct recon_web *w) {
    char text[BOOKMARKS_MAX * (sizeof(((struct web_bookmark *)0)->url) + 100)];
    size_t used = 0;

    for (int i = 0; i < w->bookmark_count; i++) {
        int wrote = snprintf(text + used, sizeof(text) - used, "%s\t%s\n",
            w->bookmarks[i].url, w->bookmarks[i].label);
        if (wrote < 0 || (size_t)wrote >= sizeof(text) - used) {
            break;
        }
        used += (size_t)wrote;
    }

    recon_fs_mkdir("/", "/Users/Shared/Web");
    recon_fs_write("/", BOOKMARKS_PATH, text, used);
}

/* Where this address is in the list, or -1. */
static int bookmark_of(const struct recon_web *w, const char *url) {
    for (int i = 0; i < w->bookmark_count; i++) {
        if (strcmp(w->bookmarks[i].url, url) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * The star adds and removes, which is the same control doing one thing:
 * "is this page kept?" A separate remove would be a second control that is
 * only ever right when the first one is wrong.
 */
static void bookmark_toggle(struct recon_web *w) {
    struct web_tab *t = front(w);
    if (t == NULL || !t->have_url) {
        return;
    }

    char url[RECON_HTTP_URL_MAX];
    recon_http_format_url(&t->url, url, sizeof(url));

    int at = bookmark_of(w, url);
    if (at >= 0) {
        for (int i = at; i + 1 < w->bookmark_count; i++) {
            w->bookmarks[i] = w->bookmarks[i + 1];
        }
        w->bookmark_count--;
    } else if (w->bookmark_count < BOOKMARKS_MAX) {
        struct web_bookmark *b = &w->bookmarks[w->bookmark_count++];
        snprintf(b->url, sizeof(b->url), "%s", url);
        snprintf(b->label, sizeof(b->label), "%s", t->label);
    } else {
        return;
    }
    bookmarks_save(w);
}

static void set_status(struct web_tab *w, bool error, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_status(struct web_tab *w, bool error, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(w->status, sizeof(w->status), fmt, args);
    va_end(args);
    w->status_is_error = error;
}

/* --- Fetching --- */

static void go_to(struct web_tab *w, const struct recon_http_url *url,
    bool remember);

static void on_progress(void *user, size_t received) {
    struct web_tab *w = user;
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
static int measure(struct web_tab *w, int width);

/* --- The stylesheets --- */

/* Both defined below, beside the things they are shared with. */
static void show_document(struct web_tab *w,
    struct recon_html_document *page, const char *shown, size_t length);
static void fetch_next_sheet(struct web_tab *w);
static void restyle(struct web_tab *w);

/* The markup only. The sheet stays: the document on screen was parsed with
 * it and the rules are not copied into the document. */
static void forget_sheets_source(struct web_tab *w) {
    free(w->source);
    w->source = NULL;
    w->source_length = 0;
}

static void forget_sheets(struct web_tab *w) {
    if (w->sheet_request != NULL) {
        recon_http_cancel(w->sheet_request);
        w->sheet_request = NULL;
    }
    recon_css_free(w->sheet);
    w->sheet = NULL;
    free(w->source);
    w->source = NULL;
    w->source_length = 0;
    w->sheets_wanted = 0;
    w->sheets_done = 0;
    w->restyled = false;
}

static void on_sheet_done(void *user, bool ok, char *body, size_t length,
        const char *content_type, const struct recon_http_url *final_url,
        const char *error) {
    struct web_tab *w = user;
    (void)content_type; (void)final_url; (void)error;

    w->sheet_request = NULL;
    w->sheets_done++;

    if (ok && body != NULL && length > 0 && w->sheet != NULL) {
        recon_css_add(w->sheet, body, length);
    }
    free(body);

    if (w->sheets_done >= w->sheets_wanted) {
        /*
         * Once, when the last one is in, rather than after each. A page
         * redrawn per stylesheet would move under somebody reading it, and
         * the middle states are wrong anyway -- half a cascade is not a
         * cascade.
         */
        restyle(w);
        return;
    }
    fetch_next_sheet(w);
}

static const struct recon_http_handlers SHEET_HANDLERS = {
    .progress = NULL,
    .done = on_sheet_done,
};

/*
 * Ask for the next stylesheet the page named.
 *
 * In the order the page named them, and one at a time, because order is half
 * of the cascade and a queue is the only way to keep it with one connection
 * at a time.
 */
static void fetch_next_sheet(struct web_tab *w) {
    if (w->sheet_request != NULL || w->page == NULL) {
        return;
    }
    while (w->sheets_done < w->sheets_wanted) {
        const char *href = recon_html_stylesheet_at(w->page, w->sheets_done);
        struct recon_http_url url;
        if (href == NULL || !recon_http_parse_url(href, &w->url, &url)) {
            w->sheets_done++;
            continue;
        }
        w->sheet_request = recon_http_get(WEB_APPLICATION, &url,
            &SHEET_HANDLERS, w);
        if (w->sheet_request == NULL) {
            w->sheets_done++;
            continue;
        }
        return;
    }
    restyle(w);
}

/*
 * Parse the page again, with the stylesheets it asked for.
 *
 * The scroll position is kept: somebody is reading, and a page that jumps to
 * the top when its stylesheet arrives is a page that punished them for
 * starting early.
 */
static void restyle(struct web_tab *w) {
    if (w->source == NULL || w->restyled) {
        forget_sheets_source(w);
        return;
    }
    w->restyled = true;

    char *markup = w->source;
    size_t length = w->source_length;
    w->source = NULL;
    w->source_length = 0;

    int was_scroll = w->scroll;

    char shown[RECON_HTTP_URL_MAX];
    recon_http_format_url(&w->url, shown, sizeof(shown));

    struct recon_html_document *page =
        recon_html_parse_styled(markup, length, w->sheet);
    free(markup);

    if (page != NULL) {
        show_document(w, page, shown, length);
        w->scroll = was_scroll;
        recon_appwin_refresh(w->win);
    }
}

/* --- The pictures --- */

static void fetch_next_image(struct web_tab *w);

static void forget_images(struct web_tab *w) {
    if (w->image_request != NULL) {
        recon_http_cancel(w->image_request);
        w->image_request = NULL;
    }
    for (int i = 0; i < w->image_count; i++) {
        free(w->images[i].pixels);
    }
    memset(w->images, 0, sizeof(w->images));
    w->image_count = 0;
    w->fetching = -1;
}

/*
 * Every distinct picture the page names, resolved against the page it was
 * named in.
 *
 * Distinct because a page uses the same icon fifteen times and fetching it
 * fifteen times is fourteen requests nobody asked for. The block keeps the
 * index, so the same bytes are drawn everywhere the page asked for them.
 */
static void collect_images(struct web_tab *w) {
    forget_images(w);
    if (w->page == NULL || !w->have_url) {
        return;
    }

    int blocks = recon_html_block_count(w->page);
    for (int i = 0; i < blocks && w->image_count < IMAGES_MAX; i++) {
        const struct recon_html_block_entry *b = recon_html_block_at(w->page, i);
        if (b == NULL || b->kind != RECON_HTML_IMAGE || b->source < 0) {
            continue;
        }

        const char *href = recon_html_link_at(w->page, b->source);
        if (href == NULL || href[0] == '\0') {
            continue;
        }

        /*
         * A data: URL is a picture written into the page itself. Not followed
         * -- it is not a fetch, it is a decode, and a page may carry
         * megabytes of them.
         */
        if (strncasecmp(href, "data:", 5) == 0) {
            continue;
        }

        struct recon_http_url resolved;
        if (!recon_http_parse_url(href, &w->url, &resolved)) {
            continue;
        }

        char text[RECON_HTTP_URL_MAX];
        recon_http_format_url(&resolved, text, sizeof(text));

        bool seen = false;
        for (int j = 0; j < w->image_count && !seen; j++) {
            seen = strcmp(w->images[j].url, text) == 0;
        }
        if (seen) {
            continue;
        }
        snprintf(w->images[w->image_count].url,
            sizeof(w->images[w->image_count].url), "%s", text);
        w->image_count++;
    }
}

/* Which entry holds this block's picture, or -1. */
static int image_for(struct web_tab *w,
        const struct recon_html_block_entry *b) {
    if (b->source < 0 || w->page == NULL || !w->have_url) {
        return -1;
    }
    const char *href = recon_html_link_at(w->page, b->source);
    if (href == NULL) {
        return -1;
    }
    struct recon_http_url resolved;
    if (!recon_http_parse_url(href, &w->url, &resolved)) {
        return -1;
    }
    char text[RECON_HTTP_URL_MAX];
    recon_http_format_url(&resolved, text, sizeof(text));

    for (int i = 0; i < w->image_count; i++) {
        if (strcmp(w->images[i].url, text) == 0) {
            return i;
        }
    }
    return -1;
}

static void on_image_done(void *user, bool ok, char *body, size_t length,
        const char *content_type, const struct recon_http_url *final_url,
        const char *error) {
    struct web_tab *w = user;
    (void)content_type; (void)final_url; (void)error;

    w->image_request = NULL;
    int at = w->fetching;
    w->fetching = -1;

    if (at >= 0 && at < w->image_count) {
        w->images[at].tried = true;
        if (ok && body != NULL && length > 0 && length <= IMAGE_BYTES_MAX) {
            int channels = 0;
            w->images[at].pixels = stbi_load_from_memory(
                (const unsigned char *)body, (int)length,
                &w->images[at].width, &w->images[at].height, &channels, 4);
        }
    }
    free(body);

    /*
     * Redrawn whether or not that one worked: a picture that failed changes
     * the page too, from a gap waiting for something to its alt text.
     */
    recon_appwin_refresh(w->win);
    fetch_next_image(w);
}

static const struct recon_http_handlers IMAGE_HANDLERS = {
    .progress = NULL,
    .done = on_image_done,
};

static void fetch_next_image(struct web_tab *w) {
    if (w->image_request != NULL) {
        return;
    }
    for (int i = 0; i < w->image_count; i++) {
        if (w->images[i].tried) {
            continue;
        }
        struct recon_http_url url;
        if (!recon_http_parse_url(w->images[i].url, NULL, &url)) {
            w->images[i].tried = true;
            continue;
        }
        w->fetching = i;
        w->image_request = recon_http_get(WEB_APPLICATION, &url,
            &IMAGE_HANDLERS, w);
        if (w->image_request == NULL) {
            /* Refused before it started -- no network permission, or an
             * address this cannot follow. It has been tried. */
            w->images[i].tried = true;
            w->fetching = -1;
            continue;
        }
        return;
    }
}

static void on_done(void *user, bool ok, char *body, size_t length,
        const char *content_type, const struct recon_http_url *final_url,
        const char *error) {
    struct web_tab *w = user;

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

    /*
     * Made UTF-8 first, if it is not already.
     *
     * Everything downstream -- the parser, the font, the title bar -- assumes
     * UTF-8, and a page in Windows-1252 read as UTF-8 is a page of dropped
     * accents. Decided by looking at the bytes rather than by believing the
     * header, because a page that says Latin-1 and is really UTF-8 is common
     * enough that trusting the label would break pages that work.
     */
    size_t converted_length = 0;
    char *converted = recon_html_to_utf8(body, length, content_type,
        &converted_length);
    if (converted != NULL) {
        free(body);
        body = converted;
        length = converted_length;
    }

    /*
     * A sheet per page, holding the page's own `<style>` elements as it is
     * parsed and, after they arrive, the ones it linked to.
     */
    forget_sheets(w);
    w->sheet = recon_css_new();

    struct recon_html_document *page = is_html
        ? recon_html_parse_styled(body, length, w->sheet)
        : recon_html_plain(body, length);

    /*
     * The markup, kept only if it might be worth parsing again. Copied rather
     * than adopted because `body` is the caller's to free on this line and
     * the second pass happens some seconds later.
     */
    if (is_html && page != NULL && length > 0 && length <= SOURCE_MAX &&
            recon_html_stylesheet_count(page) > 0) {
        w->source = malloc(length + 1);
        if (w->source != NULL) {
            memcpy(w->source, body, length);
            w->source[length] = '\0';
            w->source_length = length;
        }
    }
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

static void go_to(struct web_tab *w, const struct recon_http_url *url,
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

static void go_typed(struct web_tab *w) {
    if (w->owner == NULL) {
        return;
    }

    const char *text = w->owner->address.text;
    while (*text == ' ' || *text == '\t') {
        text++;
    }

    /*
     * --- What somebody typing in the address bar meant ---
     *
     * A link on a page is relative to that page. **Text typed into the
     * address bar is not**, and treating it as though it were is the bug this
     * comment exists for: on gaming.recontowers.com, typing "example.com" and
     * pressing Return went to gaming.recontowers.com/example.com. So once you
     * were on any page at all, you could not reach a different site by typing
     * its name -- you could only reach paths on the site you were already on.
     *
     * The one form that *is* worth resolving is a path: typing "/about" means
     * the about page of the site being read, and there is nowhere else it
     * could mean. Anything else -- a bare host, a full address, a name with a
     * dot in it -- is somewhere to go.
     *
     * A fragment gets the base too, so typing "#notes" is the same as
     * clicking a link to it.
     */
    bool relative = (text[0] == '/' || text[0] == '#');
    const struct recon_http_url *base =
        (relative && w->have_url) ? &w->url : NULL;

    struct recon_http_url url;
    if (!recon_http_parse_url(text, base, &url)) {
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
/*
 * --- Tables ---
 *
 * A table used to be one line per row with the cells run together by spaces.
 * You could read it and you could not scan it: "0.4.13 8 September Package
 * signing" is three facts with nothing to say where one ends.
 *
 * Columns need a width, and a column's width is a property of the *table*
 * rather than of any row -- it is the widest that column gets anywhere. So it
 * is measured once, over every row, before the first one is drawn.
 *
 * What this does not do is the rest of table layout. No colspan, no rowspan,
 * no nested tables, no borders, and a cell whose text is wider than its share
 * of the window is left to overlap rather than wrapped inside its column --
 * wrapping inside a cell means a row of variable height, which means
 * measuring the height before placing the row, which is the whole layout
 * problem again one level down. Lining the columns up is most of what a table
 * is for and it is the part that can be had honestly.
 */
#define COLUMNS_MAX 12

struct columns {
    int width[COLUMNS_MAX];
    int count;

    /* Where the table this describes starts, so a row can tell whether the
     * measurement it is holding is its own table's. */
    int first_block;
};

struct flow {
    struct web_tab *w;
    struct recon_panel *panel;        /* NULL to measure */
    int origin_x, origin_y;           /* where the content area starts */
    int width;
    int scroll;
    int height;                       /* filled in as it goes */
    int clip_top, clip_bottom;        /* rows outside this are not drawn */

    /*
     * The paper under this page, and whether the page chose it.
     *
     * Both, because they answer different questions. `paper` is what every
     * colour on the page has to be readable against, and there is always one.
     * `page_chose` says whether it came from the page or from the skin, which
     * is what decides whether the *skin's* colours may be used unchanged: on
     * the skin's own paper they are, by construction, and on somebody else's
     * they are just another colour that has to be checked.
     */
    recon_color paper;
    bool page_chose;

    /*
     * The ink for ordinary text and the ink for links, both already checked
     * against the paper above.
     *
     * Worked out once when the flow is set up rather than per word: they do
     * not vary within a page, and doing it per word meant the same three
     * comparisons several thousand times for one answer.
     */
    recon_color text_ink;
    recon_color link_ink;

    /*
     * The quieter ink, for list markers, and the colour of a rule.
     *
     * Here for the same reason as the two above: they are drawn on the page's
     * paper, so they are the page's problem, not the skin's. A dim grey that
     * reads on the skin's surface is invisible on a page's near-black one, and
     * a horizontal rule nobody can see is a horizontal rule that is not there.
     */
    recon_color dim_ink;
    recon_color rule_ink;

    /*
     * --- Find on page ---
     *
     * `find` is what is being looked for, or NULL. `find_seen` counts the
     * matches as the page is laid out and `find_at` says which one is the
     * current match, so the count and the highlight come out of the same pass
     * that draws the words.
     *
     * The same pass on purpose. Counting matches separately means walking the
     * document a second time with a second idea of what a word is, and the
     * two disagree the moment one of them is changed -- which shows up as
     * "3 of 12" with the third one highlighted somewhere else.
     */
    const char *find;
    int find_seen;
    int find_at;
    int find_y;                       /* where the current match ended up */

    /* The block being looked for, and where it turned out to be. */
    int jump_to;
    int jump_y;
    bool jump_found;

    /*
     * The columns of the table being drawn, measured once when its first row
     * is reached and used by every row after it.
     *
     * `columns.count` of zero means no table is open, which is the ordinary
     * state of a page.
     */
    struct columns columns;
};

/*
 * What a page said, if it can be read on the paper under it; the fallback if
 * not.
 *
 * The check is not optional and has no way to be turned off. What changed is
 * only which paper it asks about -- it used to ask about the skin's surface
 * even on a page painting its own, which is the wrong question and gave the
 * wrong answer in both directions: a light grey heading on a page's near-black
 * paper was rejected for being unreadable on white, and the near-black body
 * text it was replaced with then went on the near-black paper.
 */
static recon_color on_paper(const struct flow *f, unsigned rgb,
        recon_color fallback) {
    return recon_color_readable_on(f->paper,
        RECON_RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF),
        fallback, fallback);
}

/*
 * Case-insensitive substring.
 *
 * Written out rather than `strcasestr`, which is a GNU extension and is not
 * there in the standalone test targets -- the same reason recon_css.c has one
 * of these. Searching for "Games" has to find "games", because nobody typing
 * into a find bar is thinking about capital letters.
 */
static bool contains_fold(const char *haystack, const char *needle) {
    if (needle == NULL || needle[0] == '\0' || haystack == NULL) {
        return false;
    }
    size_t n = strlen(needle);
    for (const char *at = haystack; *at != '\0'; at++) {
        if (strncasecmp(at, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

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
        const char *text, size_t length, unsigned style, int link,
        bool has_colour, unsigned colour) {
    /*
     * --- Is this word a match? Asked first, before anything returns ---
     *
     * A match is a fact about the *document*, and everything below this is
     * about the *screen*. Counting after the off-screen return counted only
     * the matches already visible, so "1 of 2" meant two on this screenful --
     * and searching for a word further down the page found nothing at all
     * while the word sat there in the text.
     *
     * It is also what makes the jump possible: `find_y` has to be the
     * position of a match that is *not* on screen, which is precisely the one
     * the early return had thrown away.
     */
    bool matched = false;
    bool current = false;
    if (f->find != NULL && f->find[0] != '\0') {
        char probe[512];
        size_t take = length < sizeof(probe) - 1 ? length : sizeof(probe) - 1;
        memcpy(probe, text, take);
        probe[take] = '\0';

        if (contains_fold(probe, f->find)) {
            matched = true;
            current = (f->find_seen == f->find_at);
            if (current) {
                f->find_y = y;
            }
            f->find_seen++;
        }
    }

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

    /*
     * --- Whose colour ---
     *
     * The skin's, unless the page asked for one and the one it asked for can
     * actually be read on the skin's page.
     *
     * Both halves matter. Ignoring the page loses the distinction a document
     * drew between its own parts. Obeying it blindly is worse: a page written
     * for a white background says `color: #f8f8f8` for something it puts on a
     * dark panel, and a reader that takes it draws white on white. The panel
     * here belongs to the skin, so the page's colour has to be checked
     * against the skin's paper -- which is exactly what
     * recon_color_readable_on does, and it is the third place in this system
     * to need it.
     *
     * A link keeps the skin's accent whatever the page says. It is the one
     * colour here that carries a *meaning* -- "this goes somewhere" -- and a
     * meaning whose colour changes per page is one nobody can learn.
     */
    recon_color ink = is_link ? f->link_ink : f->text_ink;
    if (!is_link && has_colour) {
        ink = on_paper(f, colour, f->text_ink);
    }

    /*
     * --- The mark, which is only about drawing ---
     *
     * Whether this word matched was settled at the top, where the document is
     * what is being asked about. This half is the paint: behind the word
     * rather than a colour change, because a page already uses colour to mean
     * things and a match that recolours a word competes with whatever the
     * page was saying with that colour. A block behind it does not.
     *
     * The current match takes the accent and the others a wash of it, so
     * "which one am I on" reads without counting.
     */
    if (matched) {
        recon_color mark = current
            ? f->link_ink : recon_color_mix(f->paper, f->link_ink, 90);

        recon_fill_rect(f->panel, screen_x - 1, screen_y,
            recon_text_width(font, word) + 2,
            recon_font_line_height(font), mark);

        if (current) {
            /* On the accent, the ordinary ink may vanish. */
            ink = recon_color_readable_on(mark, ink, f->paper, f->paper);
        }
    }

    recon_draw_text(f->panel, font, screen_x, screen_y + ascent,
        f->width - x, word, ink);

    (void)is_link;
}

/*
 * --- A line, held until its width is known ---
 *
 * Centring a line means knowing how wide it came out before placing its first
 * word, and how wide it came out is only known once the last one is placed.
 * That was the whole reason `text-align` was read, recorded and then not
 * drawn: the flow puts each word down as it reaches it, and by the time the
 * width is known the words are already somewhere.
 *
 * So the words are held instead, and the line is drawn when it ends -- at a
 * wrap, or at the end of the block. Each one keeps the x it would have had on
 * a left-aligned line, and the flush adds one offset to all of them. That is
 * the whole of it: no second pass over the document, no second idea of where
 * a word goes, and left alignment comes out with an offset of zero, which is
 * bit-for-bit what it did before.
 *
 * The underlines are held with them and shifted by the same amount. A rule
 * drawn at the unshifted position under a centred line is a rule sitting to
 * the left of the words it belongs to, which is the sort of thing that gets
 * noticed six changes later.
 */
#define HELD_WORDS_MAX 256
#define HELD_RULES_MAX 32

struct held_word {
    struct recon_font *font;
    const char *text;
    size_t length;
    int x;
    int y;
    unsigned style;
    int link;
    bool has_colour;
    unsigned colour;
};

struct held_rule {
    struct recon_font *font;
    int from_x;
    int to_x;
    int y;
    int link;
};

struct line {
    struct held_word words[HELD_WORDS_MAX];
    int word_count;
    struct held_rule rules[HELD_RULES_MAX];
    int rule_count;

    /*
     * Set when a line had more words than can be held.
     *
     * A line of two hundred and fifty-seven words at this width does not
     * happen in prose; it happens in a `<pre>` of minified script, and the
     * honest answer there is to draw what was held and leave the rest rather
     * than to drop the line. The alignment of such a line is wrong, and a
     * line nobody can read the end of anyway is the right place to be wrong.
     */
    bool full;
};

static void hold_word(struct line *l, struct recon_font *font, int x, int y,
        const char *text, size_t length, unsigned style, int link,
        bool has_colour, unsigned colour) {
    if (l->word_count >= HELD_WORDS_MAX) {
        l->full = true;
        return;
    }
    l->words[l->word_count++] = (struct held_word){
        .font = font, .text = text, .length = length,
        .x = x, .y = y, .style = style, .link = link,
        .has_colour = has_colour, .colour = colour,
    };
}

static void hold_rule(struct line *l, struct recon_font *font, int from_x,
        int to_x, int y, int link) {
    if (l->rule_count >= HELD_RULES_MAX) {
        return;
    }
    l->rules[l->rule_count++] = (struct held_rule){
        .font = font, .from_x = from_x, .to_x = to_x, .y = y, .link = link,
    };
}

/*
 * The rule under a link, and the region that opens it.
 *
 * Drawn for a whole run of words rather than for each -- which is what a link
 * is. Per word, the spaces between them are not underlined and every gap is a
 * break in the line: "We have a year to fix security everywhere" came out as
 * eight separate underlines with seven holes, which reads as damage rather
 * than as a link.
 *
 * The region goes with it for the same reason, and one region instead of
 * eight is seven fewer out of a table that is finite.
 */
static void underline_link(struct flow *f, struct recon_font *font,
        int from_x, int to_x, int y, int link) {
    if (f->panel == NULL || link < 0 || to_x <= from_x) {
        return;
    }
    int screen_y = y - f->scroll + f->origin_y;
    if (screen_y + 24 < f->clip_top || screen_y > f->clip_bottom) {
        return;
    }
    int ascent = recon_font_ascent(font);
    int screen_x = from_x + f->origin_x;

    /* Underlined, not only coloured. A link that is only a different colour
     * is invisible to a reader who cannot see that difference, which is the
     * same reason the accessibility skins exist. */
    recon_fill_rect(f->panel, screen_x, screen_y + ascent + 2,
        to_x - from_x, 1, f->link_ink);
    recon_hit_add(f->panel, screen_x, screen_y, to_x - from_x,
        recon_font_line_height(font), HIT_LINK_BASE + link);
}

/*
 * Flow one block's runs, wrapping at the width.
 *
 * Words are split on spaces and placed one at a time. A word wider than the
 * whole column is placed anyway and overhangs, rather than being broken --
 * breaking a word is a decision about somebody's language and getting it wrong
 * is worse than a line that is too long.
 */
/*
 * Draw a held line, shifted by whatever its alignment asks for.
 *
 * `used` is where the pen ended up, so `used - indent` is the width the line
 * actually came to. Everything is placed relative to that one number.
 *
 * Justified is not here. `text-align: justify` means changing the space
 * *between* words rather than moving the line, which is a different operation
 * on a different unit, and doing it badly -- by stretching the last line, or
 * by leaving rivers -- looks worse than not doing it. Left is the honest
 * answer until it is done properly.
 */
static void flush_line(struct flow *f, struct line *l, int align, int indent,
        int used) {
    int shift = 0;
    if (!l->full && used > indent) {
        int room = f->width - indent;
        int width = used - indent;
        if (align == RECON_CSS_ALIGN_CENTRE) {
            shift = (room - width) / 2;
        } else if (align == RECON_CSS_ALIGN_RIGHT) {
            shift = room - width;
        }
        /* A line wider than the room it has is already past the edge; moving
         * it further would push its start off the other side. */
        if (shift < 0) {
            shift = 0;
        }
    }

    for (int i = 0; i < l->word_count; i++) {
        const struct held_word *w = &l->words[i];
        put_word(f, w->font, w->x + shift, w->y, w->text, w->length,
            w->style, w->link, w->has_colour, w->colour);
    }
    for (int i = 0; i < l->rule_count; i++) {
        const struct held_rule *r = &l->rules[i];
        underline_link(f, r->font, r->from_x + shift, r->to_x + shift,
            r->y, r->link);
    }

    l->word_count = 0;
    l->rule_count = 0;
    l->full = false;
}

/* How wide one run is, in the face the row will draw it in. */
static int run_width(const struct recon_html_run *r, struct recon_font *font) {
    char text[512];
    size_t take = r->length < sizeof(text) - 1 ? r->length : sizeof(text) - 1;
    memcpy(text, r->text, take);
    text[take] = '\0';
    return recon_text_width(font, text);
}

/*
 * Measure every column of the table beginning at `first`.
 *
 * A table is the run of consecutive RECON_HTML_ROW blocks starting there --
 * which is what a table is once the tags are gone, and is why a row is a kind
 * of its own.
 */
static void measure_columns(struct flow *f, int first, int size,
        struct columns *out) {
    memset(out, 0, sizeof(*out));
    out->first_block = first;

    struct recon_font *font = font_for(0, size);
    struct recon_font *head = font_for(RECON_HTML_BOLD, size);
    int total = recon_html_block_count(f->w->page);

    for (int i = first; i < total; i++) {
        const struct recon_html_block_entry *row =
            recon_html_block_at(f->w->page, i);
        if (row == NULL || row->kind != RECON_HTML_ROW) {
            break;
        }

        int column = -1;
        int used = 0;
        for (int j = 0; j < row->run_count; j++) {
            const struct recon_html_run *r =
                recon_html_run_at(f->w->page, row->first_run + j);
            if (r == NULL) {
                continue;
            }

            if (r->starts_cell || column < 0) {
                /* Bank the column that just ended before starting the next. */
                if (column >= 0 && column < COLUMNS_MAX &&
                        used > out->width[column]) {
                    out->width[column] = used;
                }
                column++;
                used = 0;
                if (column >= out->count) {
                    out->count = column + 1;
                }
            }
            used += run_width(r,
                (r->style & RECON_HTML_BOLD) != 0 ? head : font);
        }
        if (column >= 0 && column < COLUMNS_MAX && used > out->width[column]) {
            out->width[column] = used;
        }
    }

    if (out->count > COLUMNS_MAX) {
        out->count = COLUMNS_MAX;
    }

    /*
     * A gap between columns, and a ceiling on the lot.
     *
     * Without the ceiling a table with one very long cell pushes every column
     * after it off the right-hand side, and the columns that would have fit
     * are lost to one that never would. Scaled down together instead, which
     * keeps their relative widths -- the shape of the table survives even
     * when its size cannot.
     */
    int gap = recon_text_width(font, "  ");
    int total_width = 0;
    for (int i = 0; i < out->count; i++) {
        out->width[i] += gap;
        total_width += out->width[i];
    }
    if (total_width > f->width && total_width > 0) {
        for (int i = 0; i < out->count; i++) {
            out->width[i] = out->width[i] * f->width / total_width;
        }
    }
}

static void flow_block(struct flow *f, const struct recon_html_block_entry *b) {
    struct web_tab *w = f->w;

    /*
     * Zoom multiplies whatever the kind and the stylesheet settle on, rather
     * than replacing it: a heading at 150% is half again as big as *that
     * heading*, not half again as big as body text. Applied at the end, below,
     * for the same reason -- it is the last word, not the first.
     */
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
    case RECON_HTML_ROW:
        /* A row is laid out like a paragraph. What makes it a row is where
         * its cells are placed, which happens in the run loop below. */
        break;
    case RECON_HTML_RULE: {
        f->height += 6;
        if (f->panel != NULL) {
            int screen_y = f->height - f->scroll + f->origin_y;
            if (screen_y >= f->clip_top && screen_y <= f->clip_bottom) {
                recon_fill_rect(f->panel, f->origin_x, screen_y, f->width, 1,
                    f->rule_ink);
            }
        }
        f->height += 7;
        return;
    }
    case RECON_HTML_IMAGE: {
        /*
         * A picture that has arrived is drawn; one that has not is left to
         * fall through to its alt text below, which is what the block has
         * been carrying all along.
         *
         * Never enlarged past its own size, and shrunk to the column when it
         * is wider: a 2000-pixel photograph blown down to the width is the
         * photograph, and a 16-pixel icon blown up to it is not more of the
         * icon.
         */
        int at = image_for(f->w, b);
        if (at < 0 || f->w->images[at].pixels == NULL) {
            break;
        }

        int iw = f->w->images[at].width;
        int ih = f->w->images[at].height;
        if (iw <= 0 || ih <= 0) {
            break;
        }
        if (iw > f->width) {
            ih = (int)((long long)ih * f->width / iw);
            iw = f->width;
            if (ih < 1) {
                ih = 1;
            }
        }

        f->height += 4;
        if (f->panel != NULL) {
            int screen_y = f->height - f->scroll + f->origin_y;
            if (screen_y + ih >= f->clip_top && screen_y <= f->clip_bottom) {
                recon_draw_image_clipped(f->panel, f->origin_x, screen_y,
                    iw, ih, f->w->images[at].pixels,
                    f->w->images[at].width, f->w->images[at].height,
                    f->clip_top, f->clip_bottom);
            }
        }
        f->height += ih + 6;
        return;
    }
    case RECON_HTML_PRE:
    case RECON_HTML_PARAGRAPH:
        break;
    }

    /*
     * What a stylesheet said, over what the kind decided.
     *
     * A percentage rather than a size, because the kind is still what makes a
     * heading big: `font-size: 90%` on an h2 means nine tenths of *that
     * heading's* size, not nine tenths of body text. Clamped either side --
     * the value came from somebody else's file, and a size of four is a line
     * nobody can read while a size of two hundred is one word per screen.
     */
    if (b->size_percent > 0) {
        /*
         * Smaller as asked; larger by half of what was asked.
         *
         * Not a fudge -- a difference between this and a browser that has to
         * be reasoned about. A page sets a large size because it has a layout
         * to fill: columns, a sidebar, a header the text sits beside. This has
         * one column the width of the window, so the same number is a great
         * deal more of the screen here than it is there. Taken whole,
         * wikipedia.org's portal went from thirteen visible lines to nine.
         *
         * Smaller is left alone because it means the opposite: a page marking
         * something down is de-emphasising it, and that reads the same in one
         * column as in six.
         */
        int percent = b->size_percent;
        if (percent > 100) {
            percent = 100 + (percent - 100) / 2;
        }
        size = size * percent / 100;
        if (size < 8) {
            size = 8;
        }
        if (size > 40) {
            size = 40;
        }
    }

    /*
     * And then zoom, last, over whatever the kind and the stylesheet settled
     * on. It is the reader's word about the whole page, so it multiplies
     * rather than replaces -- a heading at 150% is half again as big as that
     * heading, not half again as big as body text.
     *
     * Its own floor and ceiling, wider than the ones above: those bound what a
     * *page* may ask for, and this is what the person reading asked for.
     */
    if (f->w->zoom > 0 && f->w->zoom != 100) {
        size = size * f->w->zoom / 100;
        if (size < 6) {
            size = 6;
        }
        if (size > 72) {
            size = 72;
        }
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
                f->dim_ink);
        }
    }

    if (b->kind == RECON_HTML_QUOTE && f->panel != NULL) {
        /* A rule down the left, which is what a quotation looks like
         * everywhere and does not need a colour to read as one. */
        int screen_y = f->height - f->scroll + f->origin_y;
        recon_fill_rect(f->panel, f->origin_x + 4, screen_y, 2, line_height,
            f->rule_ink);
    }

    int x = indent;
    int y = f->height;
    bool anything_on_line = false;

    /*
     * Where this block's words go until the line they are on ends.
     *
     * `align` is the block's, from the stylesheet, and 0 -- unset -- is left,
     * which is what everything did before and what the shift computes to.
     */
    struct line held;
    memset(&held, 0, sizeof(held));
    int align = b->align;

    /* Which cell of a row is being drawn, and whether the pen still has to
     * move to its column. -1 because the first `starts_cell` makes it 0. */
    int cell_index = -1;
    bool cell_pending = false;

    /*
     * Where the link being drawn began on this line, and in which face.
     *
     * Outside the run loop, because one link is often several runs: `<a>Free
     * software <em>can</em> be commercial</a>` is three, and a rule that
     * closed at every run boundary put a hole either side of the emphasised
     * word -- which is the same fault as BG-161 one level up.
     */
    int link_from = -1;
    int link_last = -1;
    struct recon_font *link_font = NULL;

    for (int i = 0; i < b->run_count; i++) {
        const struct recon_html_run *run =
            recon_html_run_at(w->page, b->first_run + i);
        if (run == NULL) {
            continue;
        }

        /*
         * A cell boundary, noted before the run is drawn.
         *
         * `cell_index` counts them, and starts at -1 so the first cell is
         * column zero. A row whose first run does not say `starts_cell` --
         * a `<tr>` with text loose in it -- is treated as one unnamed cell
         * before column zero, which is what the pen is already doing.
         */
        if (b->kind == RECON_HTML_ROW && run->starts_cell) {
            cell_pending = true;
            cell_index++;
        }

        unsigned style = run->style;
        if (b->kind == RECON_HTML_HEADING) {
            style |= RECON_HTML_BOLD;
        }
        /* A header row is bold. `<th>` records itself as level 1 on the row
         * it is in -- the field a row otherwise has no use for -- and this is
         * the one thing that makes the top of a table read as a heading
         * rather than as more data. */
        if (b->kind == RECON_HTML_ROW && b->level == 1) {
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
            hold_word(&held, font, x, y, run->text, run->length, style,
                run->link, run->has_colour, run->colour);

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

            /*
             * --- A cell begins, so the pen moves to its column ---
             *
             * Set when the run started one; acted on at the first word of it,
             * because that is where the pen is about to be used. A column
             * this cannot fit -- more columns than COLUMNS_MAX, or a cell
             * whose text is wider than its share -- leaves the pen where it
             * was, which is the old behaviour: the cells run on with a space
             * between them, and the row is still readable.
             */
            if (cell_pending && at == 0) {
                cell_pending = false;
                if (cell_index >= 0 && cell_index < f->columns.count) {
                    int wanted = indent;
                    for (int c = 0; c < cell_index; c++) {
                        wanted += f->columns.width[c];
                    }
                    if (wanted > x) {
                        x = wanted;
                    }
                }
            }

            if (word_length > 0) {
                char text[512];
                size_t take = word_length < sizeof(text) - 1
                    ? word_length : sizeof(text) - 1;
                memcpy(text, run->text + at, take);
                text[take] = '\0';

                int wide = recon_text_width(font, text);

                if (anything_on_line && x + wide > f->width) {
                    /* The rule ends at the edge of the line it was on. */
                    if (link_from >= 0) {
                        hold_rule(&held, font, link_from, x, y, link_last);
                        link_from = -1;
                    }
                    /*
                     * The line is finished, so its width is known and it can
                     * be drawn. Everything above this point only decided
                     * *where on the line* each word goes; this is where the
                     * line itself is placed.
                     */
                    flush_line(f, &held, align, indent, x);
                    x = indent;
                    y += line_height;
                    anything_on_line = false;
                }

                bool is_link = (style & RECON_HTML_LINK) != 0 && run->link >= 0;
                if (link_from >= 0 && (!is_link || run->link != link_last)) {
                    hold_rule(&held, font, link_from, x, y, link_last);
                    link_from = -1;
                }
                if (is_link && link_from < 0) {
                    link_from = x;
                    link_last = run->link;
                    link_font = font;
                }

                hold_word(&held, font, x, y, run->text + at, word_length,
                    style, run->link, run->has_colour, run->colour);
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

    /* A link that reached the end of the block. */
    if (link_from >= 0 && link_font != NULL) {
        hold_rule(&held, link_font, link_from, x, y, link_last);
    }

    /* And the last line, which ends because the block does. */
    flush_line(f, &held, align, indent, x);

    f->height = y + line_height;

    /* A gap after a block, so paragraphs are paragraphs. Smaller after a
     * list item, because a list is one thing. */
    f->height += (b->kind == RECON_HTML_LIST_ITEM) ? 3 : 8;
}

/*
 * Settle the paper and the inks for one page.
 *
 * The skin's, unless the page painted its own -- and then everything the skin
 * would have contributed has to be checked against the page's paper instead,
 * because the skin's colours are chosen to read on the skin's surface and that
 * surface is no longer what is underneath.
 *
 * The link accent is checked like everything else. It is the one colour that
 * carries a meaning -- "this goes somewhere" -- so it is kept wherever it can
 * be, and a meaning nobody can see is not one either.
 */
static void settle_paper(struct flow *f) {
    unsigned rgb = 0;
    f->page_chose = f->w->page != NULL &&
        recon_html_page_background(f->w->page, &rgb);

    if (!f->page_chose) {
        f->paper = COLOR_BG;
        f->text_ink = COLOR_TEXT;
        f->link_ink = COLOR_LINK;
        f->dim_ink = COLOR_DIM;
        f->rule_ink = COLOR_RULE;
        return;
    }

    f->paper = RECON_RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);

    /*
     * The skin's own ink, if it reads here; otherwise the near-black or
     * near-white this paper takes.
     *
     * Not pure black and pure white: at full contrast the text buzzes against
     * a saturated ground, which is why no skin in this system uses either.
     * `recon_color_readable_on` is given both so it can pick the direction it
     * needs, which is the argument pair it exists to take.
     */
    recon_color dark = RECON_RGB(0x14, 0x14, 0x16);
    recon_color light = RECON_RGB(0xEC, 0xEC, 0xF0);
    recon_color plain =
        recon_color_readable_on(f->paper, COLOR_TEXT, light, dark);

    f->text_ink = plain;
    f->link_ink = recon_color_readable_on(f->paper, COLOR_LINK, plain, plain);

    /*
     * Quieter than the text but still on the paper. Mixed towards the paper
     * rather than picked, because "quieter" is a distance from the ink and
     * not a colour of its own -- and mixing keeps it on whichever side of the
     * paper the ink ended up on, which picking would not.
     */
    f->dim_ink = recon_color_mix(plain, f->paper, 96);
    f->rule_ink = recon_color_mix(plain, f->paper, 176);
}

static int run_flow(struct flow *f) {
    struct web_tab *w = f->w;
    f->height = 0;

    for (int i = 0; i < recon_html_block_count(w->page); i++) {
        const struct recon_html_block_entry *b = recon_html_block_at(w->page, i);
        if (b == NULL) {
            continue;
        }

        /*
         * The first row of a table measures the whole of it.
         *
         * Here rather than inside flow_block, because a column's width is a
         * property of the table and every row after this one needs the same
         * answer. Recognised by being a row that the block before was not.
         */
        if (b->kind == RECON_HTML_ROW) {
            const struct recon_html_block_entry *previous = (i > 0)
                ? recon_html_block_at(w->page, i - 1) : NULL;
            if (previous == NULL || previous->kind != RECON_HTML_ROW) {
                measure_columns(f, i, BODY_SIZE, &f->columns);
            }
        } else {
            f->columns.count = 0;
        }

        /*
         * Where this block starts, before it is flowed -- which is the answer
         * to "where is #install", and is only knowable here.
         */
        if (i == f->jump_to) {
            f->jump_y = f->height;
            f->jump_found = true;
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

static int measure(struct web_tab *w, int width) {
    struct flow f;
    memset(&f, 0, sizeof(f));
    f.w = w;
    f.panel = NULL;
    f.width = width;
    return run_flow(&f);
}

/* --- Drawing --- */

/*
 * A padlock, drawn rather than typed.
 *
 * The system font is DejaVu Sans, which has the arrows, the star, the house
 * and the triple bar this toolbar uses -- and not U+1F512, which lives in the
 * emoji block. A glyph the font does not have draws as nothing, and a security
 * indicator that is sometimes invisible is worse than none: its absence is
 * what says "not secure".
 */
static void draw_lock(struct recon_panel *p, int x, int y, recon_color ink,
        bool closed) {
    /* The body: five wide, four tall, with the keyhole left out of it. */
    recon_fill_rect(p, x, y + 4, 8, 6, ink);

    /* The shackle: two uprights and a top, open on one side when it is not. */
    recon_fill_rect(p, x + 1, y + 1, 1, 3, ink);
    recon_fill_rect(p, x + 2, y, 4, 1, ink);
    if (closed) {
        recon_fill_rect(p, x + 6, y + 1, 1, 3, ink);
    } else {
        recon_fill_rect(p, x + 6, y + 1, 1, 1, ink);
    }
}

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

/*
 * --- The tab strip ---
 *
 * Tabs share the width, shrinking as more open, down to a floor. The one in
 * front is drawn in the page's surface colour rather than the bar's, so it
 * reads as continuous with what is below it -- which is the whole idea a tab
 * is: the front one is not a button, it is the top edge of the page.
 */
static void draw_tabs(struct recon_web *w, struct recon_panel *p,
        int x, int y, int width) {
    recon_fill_rect(p, x, y, width, TABSTRIP_HEIGHT, COLOR_BAR);

    int ascent = recon_font_ascent(w->font);
    int plus = 28;
    int room = width - plus - 6;

    int each = w->tab_count > 0 ? room / w->tab_count : room;
    if (each > TAB_MAX_WIDTH) {
        each = TAB_MAX_WIDTH;
    }
    if (each < TAB_MIN_WIDTH) {
        each = TAB_MIN_WIDTH;
    }

    int tx = x + 2;
    for (int i = 0; i < w->tab_count; i++) {
        int tw = each;
        if (tx + tw > x + room) {
            tw = x + room - tx;
        }
        if (tw < 24) {
            break;                     /* past the strip; the rest do not fit */
        }

        bool on = (i == w->active);
        struct web_tab *t = w->tabs[i];

        /*
         * RECON_WIDGET_TAB, with no label of its own.
         *
         * The look belongs to the widget layer -- flush with its neighbours,
         * `checked` for the one in front, the skin's own hover -- which is
         * what that layer is for. What it cannot do is a title on the left
         * with a close cross on the right, so the button is drawn empty and
         * its contents go on top.
         */
        struct recon_widget_button tab = {
            .x = tx, .y = y + 2, .w = tw - 1, .h = TABSTRIP_HEIGHT - 2,
            .id = HIT_TAB_BASE + (uint32_t)i,
            .label = NULL,
            .font = w->font,
            .tip = t->have_url ? t->label : "New tab",
            .behind = COLOR_BAR,
            .text = COLOR_TEXT,
            .look = RECON_WIDGET_TAB,
            .checked = on,
        };
        recon_widget_button(p, &tab);

        recon_color face = on ? THEME(SURFACE) : COLOR_BAR;
        recon_color ink = recon_color_readable_on(face,
            on ? COLOR_TEXT : COLOR_DIM, THEME(TITLE_TEXT), COLOR_TEXT);

        /*
         * The close cross is drawn rather than built as a button: a second
         * frame inside the tab's own reads as a box in a box, and at eighteen
         * pixels the frame is most of what you see. It still registers a hit
         * region, so it is as clickable as anything else.
         */
        int label_room = tw - 16;
        if (tw >= TAB_MIN_WIDTH) {
            label_room = tw - 34;
            int shut_x = tx + tw - 22;
            recon_draw_text(p, w->font, shut_x + 4,
                y + (TABSTRIP_HEIGHT + ascent) / 2, 14, "\xC3\x97", ink);
            recon_hit_add(p, shut_x, y + 4, 18, TABSTRIP_HEIGHT - 8,
                HIT_TABCLOSE_BASE + (uint32_t)i);
            recon_hit_tip(p, "Close this tab");
        }

        if (label_room > 10) {
            /*
             * A page still on its way says so, because a strip of tabs all
             * showing their old titles while three of them are loading is a
             * strip that is out of date and does not admit it.
             */
            const char *shown = t->loading ? "Loading\xE2\x80\xA6"
                : (t->label[0] != '\0' ? t->label : "New tab");
            recon_draw_text(p, w->font, tx + 9,
                y + (TABSTRIP_HEIGHT + ascent) / 2, label_room, shown, ink);
        }

        tx += each;
    }

    struct recon_widget_button add = {
        .x = x + width - plus, .y = y + 4,
        .w = 24, .h = TABSTRIP_HEIGHT - 9,
        .id = HIT_NEWTAB,
        .label = "+",
        .font = w->font,
        .tip = w->tab_count >= TABS_MAX
            ? "This window is full at twelve tabs." : "New tab",
        .behind = COLOR_BAR,
        .text = COLOR_TEXT,
        .disabled = w->tab_count >= TABS_MAX,
    };
    recon_widget_button(p, &add);

    recon_fill_rect(p, x, y + TABSTRIP_HEIGHT - 1, width, 1, COLOR_RULE);
}

/* --- The bookmarks bar --- */

static void draw_bookmarks(struct recon_web *w, struct recon_panel *p,
        int x, int y, int width) {
    recon_fill_rect(p, x, y, width, MARKS_HEIGHT, COLOR_BAR);
    recon_fill_rect(p, x, y + MARKS_HEIGHT - 1, width, 1, COLOR_RULE);

    int ascent = recon_font_ascent(w->font);

    if (w->bookmark_count == 0) {
        recon_color ink = recon_color_readable_on(COLOR_BAR, COLOR_DIM,
            THEME(TITLE_TEXT), COLOR_TEXT);
        recon_draw_text(p, w->font, x + 8, y + (MARKS_HEIGHT + ascent) / 2 - 1,
            width - 16, "No bookmarks yet \xE2\x80\x94 the star keeps one.",
            ink);
        return;
    }

    int bx = x + 4;
    for (int i = 0; i < w->bookmark_count; i++) {
        int label_width = recon_text_width(w->font, w->bookmarks[i].label);
        int tw = label_width + 16;
        if (tw > 160) {
            tw = 160;
        }
        if (bx + tw > x + width - 4) {
            break;                     /* the rest are in the menu */
        }

        struct recon_widget_button mark = {
            .x = bx, .y = y + 2, .w = tw, .h = MARKS_HEIGHT - 5,
            .id = HIT_MARK_BASE + (uint32_t)i,
            .label = w->bookmarks[i].label,
            .font = w->font,
            .tip = w->bookmarks[i].url,
            .behind = COLOR_BAR,
            .text = COLOR_TEXT,
            .look = RECON_WIDGET_PLAIN,
        };
        recon_widget_button(p, &mark);
        bx += tw + 2;
    }
}

/* --- The find bar --- */

static void draw_find(struct recon_web *w, struct recon_panel *p,
        int x, int y, int width) {
    recon_fill_rect(p, x, y, width, STRIP_HEIGHT, COLOR_BAR);
    recon_fill_rect(p, x, y, width, 1, COLOR_RULE);

    int ascent = recon_font_ascent(w->font);
    recon_color ink = recon_color_readable_on(COLOR_BAR, COLOR_TEXT,
        THEME(TITLE_TEXT), COLOR_TEXT);

    int fx = x + 8;
    recon_draw_text(p, w->font, fx, y + (STRIP_HEIGHT + ascent) / 2 - 1, 44,
        "Find", ink);
    fx += 38;

    int field = 220;
    recon_edit_draw(p, w->font, fx, y + 3, field, STRIP_HEIGHT - 7, &w->find);
    recon_hit_add(p, fx, y + 3, field, STRIP_HEIGHT - 7, HIT_FIND_FIELD);
    fx += field + 6;

    struct recon_widget_button prev = {
        .x = fx, .y = y + 3, .w = 24, .h = STRIP_HEIGHT - 7,
        .id = HIT_FIND_PREV, .label = "\xE2\x80\xB9", .font = w->font,
        .tip = "Previous match", .behind = COLOR_BAR, .text = ink,
        .disabled = w->find_count == 0,
    };
    recon_widget_button(p, &prev);
    fx += 26;

    struct recon_widget_button next = {
        .x = fx, .y = y + 3, .w = 24, .h = STRIP_HEIGHT - 7,
        .id = HIT_FIND_NEXT, .label = "\xE2\x80\xBA", .font = w->font,
        .tip = "Next match", .behind = COLOR_BAR, .text = ink,
        .disabled = w->find_count == 0,
    };
    recon_widget_button(p, &next);
    fx += 32;

    /*
     * The count, and the honest answer when there is none.
     *
     * "0 of 0" says the search ran and found nothing, which is different from
     * an empty field where nothing has been searched for -- and telling those
     * two apart is most of what this line is for.
     */
    char count[64];
    if (w->find.text[0] == '\0') {
        count[0] = '\0';
    } else if (w->find_count == 0) {
        snprintf(count, sizeof(count), "not on this page");
    } else {
        snprintf(count, sizeof(count), "%d of %d", w->find_at + 1,
            w->find_count);
    }
    if (count[0] != '\0') {
        recon_draw_text(p, w->font, fx, y + (STRIP_HEIGHT + ascent) / 2 - 1,
            width - (fx - x) - 40, count,
            w->find_count == 0 ? COLOR_WARNING : ink);
    }

    struct recon_widget_button shut = {
        .x = x + width - 28, .y = y + 3, .w = 24, .h = STRIP_HEIGHT - 7,
        .id = HIT_FIND_CLOSE, .label = "\xC3\x97", .font = w->font,
        .tip = "Close the find bar", .behind = COLOR_BAR, .text = ink,
    };
    recon_widget_button(p, &shut);
}

/* --- The menu --- */

static void draw_menu(struct recon_web *w, struct recon_panel *p,
        int x, int y, int width) {
    int height = 0;
    for (int i = 0; i < MENU_COUNT; i++) {
        height += MENU[i].separator ? MENU_SEPARATOR_ROW : MENU_ROW;
    }
    height += 8;

    int mx = x + width - MENU_WIDTH - 6;
    if (mx < x + 4) {
        mx = x + 4;
    }

    recon_fill_rect(p, mx, y, MENU_WIDTH, height, THEME(MENU));
    recon_draw_bevel(p, mx, y, MENU_WIDTH, height, false);

    struct web_tab *t = front(w);
    int ascent = recon_font_ascent(w->font);
    int my = y + 4;

    for (int i = 0; i < MENU_COUNT; i++) {
        if (MENU[i].separator) {
            recon_fill_rect(p, mx + 8, my + MENU_SEPARATOR_ROW / 2,
                MENU_WIDTH - 16, 1, THEME(MENU_SEPARATOR));
            my += MENU_SEPARATOR_ROW;
            continue;
        }

        /*
         * An entry that cannot do anything is drawn and explained rather than
         * hidden. A menu whose entries come and go is a menu nobody can learn
         * the shape of, and "why is this grey" has an answer where "where did
         * it go" does not.
         */
        bool on = true;
        if (i == MENU_ADD_BOOKMARK || i == MENU_SET_HOME || i == MENU_FIND) {
            on = (t != NULL && t->page != NULL && t->have_url);
        } else if (i == MENU_NEW_TAB) {
            on = w->tab_count < TABS_MAX;
        } else if (i == MENU_ZOOM_IN) {
            on = (t != NULL && t->zoom < ZOOM_MAX);
        } else if (i == MENU_ZOOM_OUT) {
            on = (t != NULL && t->zoom > ZOOM_MIN);
        } else if (i == MENU_ZOOM_RESET) {
            on = (t != NULL && t->zoom != 100);
        }

        /*
         * A row, not a button.
         *
         * A menu of bevelled buttons reads as a toolbar stood on its end. The
         * shell's own menus draw a highlight behind the row under the pointer
         * and nothing at all behind the others, which is what a menu looks
         * like everywhere -- so this does the same thing, through the same
         * helper.
         */
        uint32_t id = HIT_MENU_BASE + (uint32_t)i;
        bool hovered = on && recon_panel_hot(p) == id;

        if (hovered) {
            recon_widget_highlight_role(p, mx + 2, my, MENU_WIDTH - 4,
                MENU_ROW, RECON_THEME_MENU_HILITE);
        }
        recon_hit_add(p, mx + 2, my, MENU_WIDTH - 4, MENU_ROW, id);
        if (!on) {
            recon_hit_inert(p);
        }

        recon_color ink = !on ? THEME(MENU_TEXT_DISABLED)
            : hovered ? THEME(MENU_HILITE_TEXT) : THEME(MENU_TEXT);
        recon_draw_text(p, w->font, mx + 12, my + (MENU_ROW + ascent) / 2 - 1,
            MENU_WIDTH - 90, MENU[i].label, ink);

        /*
         * A tick where the entry is a state rather than an action, so
         * "Show bookmarks bar" says whether they are shown.
         */
        if (i == MENU_SHOW_MARKS && w->show_bookmarks) {
            recon_draw_text(p, w->font, mx + MENU_WIDTH - 22,
                my + (MENU_ROW + ascent) / 2 - 1, 16, "\xE2\x9C\x93", ink);
        } else if (MENU[i].keys != NULL) {
            int kw = recon_text_width(w->font, MENU[i].keys);
            /* The shortcut is quieter than the label but has to stay legible
             * on the highlight, which is a different surface from the menu. */
            recon_draw_text(p, w->font, mx + MENU_WIDTH - 12 - kw,
                my + (MENU_ROW + ascent) / 2 - 1, kw + 2, MENU[i].keys,
                hovered ? THEME(MENU_HILITE_TEXT)
                        : THEME(MENU_TEXT_DISABLED));
        }

        my += MENU_ROW;
    }
}

static void web_draw(void *user, struct recon_panel *p, int x, int y, int width,
        int height) {
    struct recon_web *w = user;
    struct web_tab *t = front(w);
    int ascent = recon_font_ascent(w->font);

    recon_fill_rect(p, x, y, width, height, COLOR_BG);

    /* --- The tab strip --- */
    draw_tabs(w, p, x, y, width);
    int bar_y = y + TABSTRIP_HEIGHT;

    /* --- The toolbar --- */
    recon_fill_rect(p, x, bar_y, width, BAR_HEIGHT, COLOR_BAR);
    recon_fill_rect(p, x, bar_y + BAR_HEIGHT - 1, width, 1, COLOR_RULE);

    int bx = x + 6;
    int by = bar_y + (BAR_HEIGHT - FIELD_HEIGHT) / 2;

    bool can_back = t != NULL && t->at > 0;
    bool can_forward = t != NULL && t->at >= 0 && t->at + 1 < t->history_count;
    bool loading = t != NULL && t->loading;

    /*
     * Stop and reload are one button, because they are one question -- "is
     * this page still coming?" -- and the answer is never both. Two buttons
     * would mean one of them is always dead.
     */
    const struct { const char *glyph; uint32_t hit; bool on; const char *tip; }
    BUTTONS[] = {
        { "\xE2\x86\x90", HIT_BACK, can_back, "Back" },
        { "\xE2\x86\x92", HIT_FORWARD, can_forward, "Forward" },
        { loading ? "\xC3\x97" : "\xE2\x86\xBB", HIT_RELOAD,
          loading || (t != NULL && t->have_url),
          loading ? "Stop loading" : "Reload this page" },
        { "\xE2\x8C\x82", HIT_HOME, w->home[0] != '\0', "Home" },
    };

    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++) {
        /*
         * Registered whether or not it can be pressed. Back with nowhere to
         * go used to register nothing, so the click fell through to the
         * toolbar behind it -- and the arrow was the one thing on the bar
         * that stayed dead under the pointer without saying why.
         */
        struct recon_widget_button button = {
            .x = bx, .y = by, .w = BUTTON_WIDTH, .h = FIELD_HEIGHT,
            .id = BUTTONS[i].hit,
            .label = BUTTONS[i].glyph,
            .font = w->font,
            .tip = BUTTONS[i].tip,
            .behind = COLOR_BAR,
            .text = BUTTONS[i].on ? COLOR_TEXT : COLOR_DIM,
            .disabled = !BUTTONS[i].on,
        };
        recon_widget_button(p, &button);
        bx += BUTTON_WIDTH + 4;
    }

    /* --- The address, with what is known about how it travelled --- */
    int right = 2 * (BUTTON_WIDTH + 4);
    int field_width = width - (bx - x) - right - 8;
    if (field_width < 80) {
        field_width = 80;
    }

    /*
     * The lock says how the page came, and only that.
     *
     * Closed for TLS, open for plain HTTP, and nothing at all when no page
     * has loaded -- a lock on an empty window would be describing something
     * that has not happened. It is deliberately not a claim about the site:
     * "the connection was encrypted" is the only thing this can actually
     * know, and dressing it up as "this page is safe" would be a lie the
     * browser tells rather than the site.
     */
    int lock_room = 0;
    if (t != NULL && t->have_url) {
        bool secure = t->url.secure;
        recon_color mark = recon_color_readable_on(COLOR_BAR,
            secure ? COLOR_TEXT : COLOR_WARNING, THEME(TITLE_TEXT),
            COLOR_TEXT);
        draw_lock(p, bx + 7, by + (FIELD_HEIGHT - 10) / 2, mark, secure);
        recon_hit_add(p, bx, by, 22, FIELD_HEIGHT, HIT_ADDRESS);
        recon_hit_tip(p, secure
            ? "The connection to this site was encrypted."
            : "Sent in the clear. Anyone between here and the site can read "
              "it.");
        lock_room = 20;
    }

    recon_edit_draw(p, w->font, bx + lock_room, by, field_width - lock_room,
        FIELD_HEIGHT, &w->address);
    recon_hit_add(p, bx + lock_room, by, field_width - lock_room, FIELD_HEIGHT,
        HIT_ADDRESS);
    bx += field_width + 4;

    /* --- The star, and the menu --- */
    char here[RECON_HTTP_URL_MAX];
    here[0] = '\0';
    if (t != NULL && t->have_url) {
        recon_http_format_url(&t->url, here, sizeof(here));
    }
    bool kept = here[0] != '\0' && bookmark_of(w, here) >= 0;

    struct recon_widget_button star = {
        .x = bx, .y = by, .w = BUTTON_WIDTH, .h = FIELD_HEIGHT,
        .id = HIT_STAR,
        .label = kept ? "\xE2\x98\x85" : "\xE2\x98\x86",
        .font = w->font,
        .tip = kept ? "Remove this bookmark" : "Bookmark this page",
        .behind = COLOR_BAR,
        .text = kept ? COLOR_LINK : COLOR_TEXT,
        .disabled = here[0] == '\0',
    };
    recon_widget_button(p, &star);
    bx += BUTTON_WIDTH + 4;

    struct recon_widget_button menu = {
        .x = bx, .y = by, .w = BUTTON_WIDTH, .h = FIELD_HEIGHT,
        .id = HIT_MENU,
        .label = "\xE2\x89\xA1",
        .font = w->font,
        .tip = "More",
        .behind = COLOR_BAR,
        .text = COLOR_TEXT,
        .checked = w->menu_open,
    };
    recon_widget_button(p, &menu);

    int top = bar_y + BAR_HEIGHT;

    /* --- The bookmarks bar --- */
    if (w->show_bookmarks) {
        draw_bookmarks(w, p, x, top, width);
        top += MARKS_HEIGHT;
    }
    top += PADDING;

    /* --- The status line --- */
    int status_y = y + height - STATUS_HEIGHT;
    int bottom = status_y - PADDING;

    /* --- The find bar, above the status and below the page --- */
    if (w->strip == STRIP_FIND) {
        bottom -= STRIP_HEIGHT;
        draw_find(w, p, x, status_y - STRIP_HEIGHT, width);
    }

    if (t != NULL) {
        t->viewport_height = bottom - top;
    }

    /*
     * The ink is chosen against the strip rather than asked of the skin, for
     * the reason the clock had to be (BG-151): this fills with the *bar*
     * colour and was writing in `surface.text-dim`, which is a dark grey
     * meant for a pale page. On a skin whose bar is a colour -- Beacon's
     * blue, Metallic in garnet -- the line was there and unreadable.
     *
     * The warning goes through the same lens. A red warning on a red bar is
     * the case where being unreadable matters most.
     */
    recon_fill_rect(p, x, status_y, width, STATUS_HEIGHT, COLOR_BAR);
    recon_fill_rect(p, x, status_y, width, 1, COLOR_RULE);

    recon_color asked = (t != NULL && t->status_is_error)
        ? COLOR_WARNING : COLOR_DIM;
    recon_color ink = recon_color_readable_on(COLOR_BAR, asked,
        THEME(TITLE_TEXT), THEME(SURFACE_TEXT));

    recon_draw_text(p, w->font, x + 8,
        status_y + (STATUS_HEIGHT + ascent) / 2 - 1, width - 90,
        t != NULL ? t->status : "", ink);

    /*
     * The text size, at the right, and only when it is not the ordinary one.
     *
     * A browser showing "100%" forever is a browser with a number on it that
     * never means anything; showing it only when it has been changed makes
     * the number the answer to "why does this page look like that".
     */
    if (t != NULL && t->zoom != 100) {
        char zoom[16];
        snprintf(zoom, sizeof(zoom), "%d%%", t->zoom);
        int zw = recon_text_width(w->font, zoom);
        struct recon_widget_button reset = {
            .x = x + width - zw - 20, .y = status_y + 2,
            .w = zw + 14, .h = STATUS_HEIGHT - 4,
            .id = HIT_MENU_BASE + MENU_ZOOM_RESET,
            .label = zoom,
            .font = w->font,
            .tip = "Back to the ordinary text size",
            .behind = COLOR_BAR,
            .text = ink,
            .look = RECON_WIDGET_PLAIN,
        };
        recon_widget_button(p, &reset);
    }

    /* --- The page --- */
    if (t == NULL || t->page == NULL || recon_html_block_count(t->page) == 0) {
        if (t != NULL && !t->loading && t->status[0] == '\0') {
            recon_draw_text(p, w->font, x + PADDING, top + ascent,
                width - PADDING * 2,
                "Type an address above, or press + for another tab. "
                "This viewer reads markup and stylesheets; it does not run "
                "scripts.", COLOR_DIM);
        }
        if (w->menu_open) {
            draw_menu(w, p, x, bar_y + BAR_HEIGHT, width);
        }
        return;
    }

    /* Content is inset from the right by the scrollbar's width whether or not
     * one is showing, so the text does not reflow when it appears. */
    int content_width = width - PADDING * 2 - SCROLLBAR_WIDTH;

    if (t->content_height <= 0) {
        t->content_height = measure(t, content_width);
    }

    int most = t->content_height - t->viewport_height;
    if (most < 0) {
        most = 0;
    }
    if (t->scroll > most) {
        t->scroll = most;
    }
    if (t->scroll < 0) {
        t->scroll = 0;
    }

    struct flow f;
    memset(&f, 0, sizeof(f));
    f.w = t;
    f.panel = p;
    f.origin_x = x + PADDING;
    f.origin_y = top;
    f.width = content_width;
    f.scroll = t->scroll;
    f.clip_top = top;
    f.clip_bottom = bottom;
    f.find = (w->strip == STRIP_FIND && w->find.text[0] != '\0')
        ? w->find.text : NULL;
    f.find_at = w->find_at;
    f.jump_to = t->jump_to;
    settle_paper(&f);

    /*
     * The paper goes down before anything on it, over the content area only.
     * The bars above and below belong to the skin whatever the page says --
     * the address bar is part of ReconOS, not part of the site, and a page
     * that could repaint it could dress itself up as the browser.
     */
    if (f.page_chose) {
        recon_fill_rect(p, x, top, width, bottom - top, f.paper);
    }

    t->content_height = run_flow(&f);
    w->find_count = f.find_seen;
    if (w->find_at >= w->find_count) {
        w->find_at = w->find_count - 1;
    }
    if (w->find_at < 0) {
        w->find_at = 0;
    }

    /*
     * --- Now that the match has a position, go to it ---
     *
     * Only when it is not already on screen. A match three lines down does
     * not need the page to move, and moving it anyway makes Next feel like it
     * jumped somewhere when it did not.
     *
     * A third of the way down rather than at the very top: a match at the top
     * edge has no context above it, and the sentence it is in usually starts
     * before it.
     */
    /*
     * --- Arrive at the named place ---
     *
     * Before the find jump, and clearing itself either way: an anchor that
     * names a block which no longer exists -- the page was restyled, or the
     * link was to a place on a page that has since been replaced -- must not
     * leave the tab trying again on every frame.
     */
    if (t->jump_to >= 0) {
        if (f.jump_found) {
            int limit = t->content_height - t->viewport_height;
            int want = f.jump_y;
            if (want > limit) {
                want = limit;
            }
            if (want < 0) {
                want = 0;
            }
            if (want != t->scroll) {
                t->scroll = want;
                recon_appwin_refresh(t->win);
            }
        }
        t->jump_to = -1;
    }

    if (w->find_follow && f.find_seen > 0) {
        w->find_follow = false;

        int line = 24;
        bool above = f.find_y < t->scroll;
        bool below = f.find_y + line > t->scroll + t->viewport_height;
        if (above || below) {
            int want = f.find_y - t->viewport_height / 3;
            int limit = t->content_height - t->viewport_height;
            if (want > limit) {
                want = limit;
            }
            if (want < 0) {
                want = 0;
            }
            if (want != t->scroll) {
                t->scroll = want;
                recon_appwin_refresh(t->win);
            }
        }
    }

    if (t->content_height > t->viewport_height) {
        draw_scrollbar(p, x + width - SCROLLBAR_WIDTH, top,
            t->viewport_height, t->scroll, t->viewport_height,
            t->content_height);
    }

    /* Last, so it is over the page rather than under it. */
    if (w->menu_open) {
        draw_menu(w, p, x, bar_y + BAR_HEIGHT, width);
    }
}


/* --- Pages this builds for itself --- */

/*
 * The history and the bookmarks are shown as *pages*.
 *
 * Not as a panel, a dialog or a list widget, because a page is a thing this
 * program already knows how to do completely: lay out, scroll, colour to the
 * skin, search with the find bar, and -- the part that matters -- make every
 * entry a link that already works. A list widget would need its own layout,
 * its own scrolling and its own click handling, all of which exist here and
 * none of which would be shared.
 *
 * The markup is generated and then parsed rather than being turned straight
 * into blocks, for the same reason: the parser is the one place that decides
 * what a document is, and a second way in is a second thing to keep in step.
 */

/* Text into markup, so a page title containing "<" is a title and not a tag. */
static void escaped(char *out, size_t size, const char *text) {
    size_t used = 0;
    out[0] = '\0';
    for (const unsigned char *c = (const unsigned char *)text;
            *c != '\0' && used + 8 < size; c++) {
        const char *as = NULL;
        switch (*c) {
        case '<':  as = "&lt;";   break;
        case '>':  as = "&gt;";   break;
        case '&':  as = "&amp;";  break;
        case '"':  as = "&quot;"; break;
        default: break;
        }
        if (as != NULL) {
            used += (size_t)snprintf(out + used, size - used, "%s", as);
        } else if (*c >= 0x20 && *c != 0x7F) {
            out[used++] = (char)*c;
            out[used] = '\0';
        }
    }
}

/*
 * Show a document this made up, rather than one that was fetched.
 *
 * `have_url` stays false, which is what tells the link handler there is no
 * base to resolve against -- and there is not: this page came from nowhere.
 * Every address on it is absolute for exactly that reason.
 */
static void show_built(struct web_tab *t, const char *html, const char *name) {
    if (t->request != NULL) {
        recon_http_cancel(t->request);
        t->request = NULL;
        t->loading = false;
    }

    forget_images(t);
    forget_sheets(t);
    t->sheet = recon_css_new();
    t->restyled = true;               /* nothing to fetch; do not try */

    t->have_url = false;
    show_document(t, recon_html_parse_styled(html, strlen(html), t->sheet),
        name, strlen(html));

    /*
     * The address bar says what this is rather than where it came from.
     * Leaving the last page's address there would be the address bar
     * describing something that is no longer on screen.
     */
    if (t->owner != NULL && front(t->owner) == t) {
        recon_edit_begin(&t->owner->address, name, false);
        t->owner->address.active = false;
    }
    snprintf(t->label, sizeof(t->label), "%s", name);
}

static void show_history(struct recon_web *w) {
    struct web_tab *t = front(w);
    if (t == NULL) {
        return;
    }

    /*
     * Bounded, and the bound is why this is built into a fixed buffer rather
     * than grown: the history is capped at HISTORY_MAX entries of a known
     * size, so the largest page this can produce is known in advance.
     */
    static char html[HISTORY_MAX * (RECON_HTTP_URL_MAX + 256) + 1024];
    size_t used = 0;

    used += (size_t)snprintf(html + used, sizeof(html) - used,
        "<h1>History</h1>"
        "<p>Where this tab has been, newest first. "
        "Back and forward walk the same list.</p>");

    if (t->history_count == 0) {
        used += (size_t)snprintf(html + used, sizeof(html) - used,
            "<p>Nothing yet.</p>");
    }

    /* Newest first, which is the order somebody looking for where they just
     * were wants -- and the opposite of the order back and forward use. */
    for (int i = t->history_count - 1; i >= 0 && used + 512 < sizeof(html);
            i--) {
        char address[RECON_HTTP_URL_MAX];
        recon_http_format_url(&t->history[i], address, sizeof(address));

        char safe[RECON_HTTP_URL_MAX + 64];
        escaped(safe, sizeof(safe), address);

        /* The one you are on is marked rather than left as a link to
         * itself -- following it would reload the page you are reading. */
        if (i == t->at) {
            used += (size_t)snprintf(html + used, sizeof(html) - used,
                "<p><b>%s</b> &mdash; here now</p>", safe);
        } else {
            used += (size_t)snprintf(html + used, sizeof(html) - used,
                "<p><a href=\"%s\">%s</a></p>", safe, safe);
        }
    }

    show_built(t, html, "History");
}

static void show_bookmarks_page(struct recon_web *w) {
    struct web_tab *t = front(w);
    if (t == NULL) {
        return;
    }

    static char html[BOOKMARKS_MAX * (RECON_HTTP_URL_MAX + 256) + 1024];
    size_t used = 0;

    used += (size_t)snprintf(html + used, sizeof(html) - used,
        "<h1>Bookmarks</h1>"
        "<p>Kept in /Users/Shared/Web/bookmarks.txt. "
        "The star on the toolbar adds and removes them.</p>");

    if (w->bookmark_count == 0) {
        used += (size_t)snprintf(html + used, sizeof(html) - used,
            "<p>None yet.</p>");
    }

    for (int i = 0; i < w->bookmark_count && used + 512 < sizeof(html); i++) {
        char safe_url[RECON_HTTP_URL_MAX + 64];
        char safe_label[256];
        escaped(safe_url, sizeof(safe_url), w->bookmarks[i].url);
        escaped(safe_label, sizeof(safe_label), w->bookmarks[i].label);

        used += (size_t)snprintf(html + used, sizeof(html) - used,
            "<p><a href=\"%s\">%s</a><br>%s</p>",
            safe_url, safe_label, safe_url);
    }

    show_built(t, html, "Bookmarks");
}

/* Open an address in whichever tab is in front. */
static void open_text(struct recon_web *w, const char *text) {
    struct web_tab *t = front(w);
    if (t == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    struct recon_http_url url;
    if (!recon_http_parse_url(text, t->have_url ? &t->url : NULL, &url)) {
        set_status(t, true, "%s", recon_http_last_error());
        return;
    }
    go_to(t, &url, true);
}

static void set_zoom(struct recon_web *w, int zoom) {
    struct web_tab *t = front(w);
    if (t == NULL) {
        return;
    }
    if (zoom < ZOOM_MIN) {
        zoom = ZOOM_MIN;
    }
    if (zoom > ZOOM_MAX) {
        zoom = ZOOM_MAX;
    }
    if (zoom == t->zoom) {
        return;
    }
    t->zoom = zoom;

    /*
     * The height is not what it was, and the scroll position that went with
     * it is not either. Recomputed on the next draw; zeroing it here would be
     * a guess, and keeping it would put the reader somewhere else on the page
     * every time they pressed the key.
     */
    t->content_height = 0;
    recon_appwin_refresh(t->win);
}

/*
 * Move to the next or previous match and put it on screen.
 *
 * The position of a match is only known after a pass that lays the page out,
 * so this changes which one is current and lets the next draw find it -- the
 * scroll follows on the draw after that. Two frames rather than one, and the
 * alternative is a third copy of the layout arithmetic run here.
 */
static void find_step(struct recon_web *w, int by) {
    if (w->find_count <= 0) {
        w->find_at = 0;
        return;
    }
    w->find_at = (w->find_at + by + w->find_count) % w->find_count;
    w->find_follow = true;

    struct web_tab *t = front(w);
    if (t != NULL) {
        recon_appwin_refresh(t->win);
    }
}

static void find_open(struct recon_web *w, bool open) {
    w->strip = open ? STRIP_FIND : STRIP_NONE;
    if (open) {
        recon_edit_focus(&w->find);
    } else {
        w->find.active = false;
        recon_edit_begin(&w->find, "", false);
        w->find.active = false;
        w->find_count = 0;
        w->find_at = 0;
    }
    struct web_tab *t = front(w);
    if (t != NULL) {
        t->content_height = 0;
        recon_appwin_refresh(t->win);
    }
}

static void menu_do(struct recon_web *w, int item) {
    struct web_tab *t = front(w);
    w->menu_open = false;

    switch (item) {
    case MENU_NEW_TAB: {
        struct web_tab *fresh = tab_new(w);
        if (fresh != NULL) {
            w->active = w->tab_count - 1;
            recon_edit_begin(&w->address, "", false);
            recon_edit_focus(&w->address);
        }
        break;
    }
    case MENU_CLOSE_TAB:
        tab_close(w, w->active);
        break;
    case MENU_FIND:
        find_open(w, true);
        break;
    case MENU_ZOOM_IN:
        set_zoom(w, (t != NULL ? t->zoom : 100) + ZOOM_STEP);
        break;
    case MENU_ZOOM_OUT:
        set_zoom(w, (t != NULL ? t->zoom : 100) - ZOOM_STEP);
        break;
    case MENU_ZOOM_RESET:
        set_zoom(w, 100);
        break;
    case MENU_ADD_BOOKMARK:
        bookmark_toggle(w);
        /* Turned on when the first one is kept: a bookmark you cannot see is
         * a bookmark you will not believe was saved. */
        if (w->bookmark_count == 1) {
            w->show_bookmarks = true;
        }
        break;
    case MENU_SHOW_MARKS:
        w->show_bookmarks = !w->show_bookmarks;
        break;
    case MENU_HISTORY:
        show_history(w);
        break;
    case MENU_BOOKMARKS:
        show_bookmarks_page(w);
        break;
    case MENU_SET_HOME:
        if (t != NULL && t->have_url) {
            recon_http_format_url(&t->url, w->home, sizeof(w->home));
            set_status(t, false, "Home is now %s", w->home);
        }
        break;
    default:
        break;
    }

    if (t != NULL) {
        recon_appwin_refresh(t->win);
    }
}

static bool web_click(void *user, uint32_t hit, int cx, int cy, bool pressed) {
    struct recon_web *w = user;
    struct web_tab *t = front(w);
    (void)cx;
    (void)cy;

    if (!pressed || hit < RECON_APPWIN_HIT_USER) {
        return false;
    }

    /*
     * A click anywhere but the menu closes it, which is what a menu does.
     * Done before the press is acted on, so the entry that was clicked still
     * runs -- a menu that closed first and dispatched second would swallow
     * every one of its own entries.
     */
    bool in_menu = (hit >= HIT_MENU_BASE && hit < HIT_MENU_BASE + MENU_COUNT);
    if (w->menu_open && !in_menu && hit != HIT_MENU) {
        w->menu_open = false;
    }

    if (in_menu) {
        menu_do(w, (int)(hit - HIT_MENU_BASE));
        return true;
    }

    if (hit >= HIT_TAB_BASE && hit < HIT_TAB_BASE + TABS_MAX) {
        int which = (int)(hit - HIT_TAB_BASE);
        if (which < w->tab_count && which != w->active) {
            w->active = which;
            struct web_tab *now = front(w);
            /*
             * The address bar follows the tab. It is the window's, not the
             * tab's, so switching without this leaves the previous page's
             * address over the new page -- which is the address bar lying,
             * and the one thing it must never do.
             */
            char shown[RECON_HTTP_URL_MAX] = "";
            if (now != NULL && now->have_url) {
                recon_http_format_url(&now->url, shown, sizeof(shown));
            }
            recon_edit_begin(&w->address, shown, false);
            w->address.active = false;

            /*
             * And the window's title, for the same reason as the address: the
             * title bar and the taskbar button both name whatever is in
             * front, and a browser whose title bar names a tab you are not
             * looking at is one you cannot find in a taskbar of six.
             */
            if (now != NULL) {
                recon_appwin_set_title(w->win,
                    now->label[0] != '\0' ? now->label : WEB_APPLICATION);
                now->content_height = 0;
            }

            /*
             * A search does not follow you between tabs. The matches were
             * counted on the page you were reading, and carrying "4 of 11"
             * onto a different document is a count about nothing.
             */
            w->find_count = 0;
            w->find_at = 0;
        }
        return true;
    }

    if (hit >= HIT_TABCLOSE_BASE && hit < HIT_TABCLOSE_BASE + TABS_MAX) {
        tab_close(w, (int)(hit - HIT_TABCLOSE_BASE));
        return true;
    }

    if (hit >= HIT_MARK_BASE && hit < HIT_MARK_BASE + BOOKMARKS_MAX) {
        int which = (int)(hit - HIT_MARK_BASE);
        if (which < w->bookmark_count) {
            open_text(w, w->bookmarks[which].url);
        }
        return true;
    }

    if (hit >= HIT_LINK_BASE) {
        if (t == NULL || t->page == NULL) {
            return true;
        }
        int link = (int)(hit - HIT_LINK_BASE);
        const char *href = recon_html_link_at(t->page, link);
        if (href == NULL) {
            return true;
        }

        struct recon_http_url next;
        if (!recon_http_parse_url(href, t->have_url ? &t->url : NULL, &next)) {
            set_status(t, true, "%s", recon_http_last_error());
            return true;
        }

        /*
         * Is this the page already showing?
         *
         * Compared as formatted addresses, which deliberately leave the
         * fragment out -- two addresses differing only after the hash are the
         * same document, and comparing with the fragment in would make every
         * anchor look like a different page and fetch it again.
         */
        char here[RECON_HTTP_URL_MAX];
        char there[RECON_HTTP_URL_MAX];
        recon_http_format_url(&t->url, here, sizeof(here));
        recon_http_format_url(&next, there, sizeof(there));

        /*
         * If it names a place, go there. If it names nothing -- a bare "#",
         * which pages use for a link that only script gives meaning to -- go
         * to the top, which is what a browser does. And if it names a place
         * this page does not have, say so rather than silently doing nothing,
         * because a link that appears dead is indistinguishable from one this
         * has failed to handle.
         */
        if (t->have_url && strcmp(here, there) == 0) {
            if (next.fragment[0] == '\0') {
                t->scroll = 0;
                recon_appwin_refresh(t->win);
                return true;
            }

            int block = recon_html_anchor_block(t->page, next.fragment);
            if (block >= 0) {
                t->jump_to = block;
                recon_appwin_refresh(t->win);
            } else {
                set_status(t, false,
                    "This page has no place called \"%.40s\".",
                    next.fragment);
            }
            return true;
        }

        go_to(t, &next, true);
        return true;
    }

    switch (hit) {
    case HIT_BACK:
        if (t != NULL && t->at > 0) {
            t->at--;
            go_to(t, &t->history[t->at], false);
        }
        return true;
    case HIT_FORWARD:
        if (t != NULL && t->at + 1 < t->history_count) {
            t->at++;
            go_to(t, &t->history[t->at], false);
        }
        return true;
    case HIT_RELOAD:
        /*
         * One button, two jobs, decided by what is happening rather than by
         * which of two buttons was pressed -- so it cannot be pressed for the
         * job it is not doing.
         */
        if (t != NULL && t->loading) {
            if (t->request != NULL) {
                recon_http_cancel(t->request);
                t->request = NULL;
            }
            t->loading = false;
            set_status(t, false, "Stopped.");
            recon_appwin_refresh(t->win);
        } else if (t != NULL && t->have_url) {
            go_to(t, &t->url, false);
        }
        return true;
    case HIT_HOME:
        if (w->home[0] != '\0') {
            open_text(w, w->home);
        }
        return true;
    case HIT_NEWTAB:
        menu_do(w, MENU_NEW_TAB);
        return true;
    case HIT_STAR:
        bookmark_toggle(w);
        if (w->bookmark_count == 1) {
            w->show_bookmarks = true;
        }
        if (t != NULL) {
            recon_appwin_refresh(t->win);
        }
        return true;
    case HIT_MENU:
        w->menu_open = !w->menu_open;
        if (t != NULL) {
            recon_appwin_refresh(t->win);
        }
        return true;
    case HIT_FIND_FIELD:
        recon_edit_focus(&w->find);
        return true;
    case HIT_FIND_NEXT:
        find_step(w, 1);
        return true;
    case HIT_FIND_PREV:
        find_step(w, -1);
        return true;
    case HIT_FIND_CLOSE:
        find_open(w, false);
        return true;
    case HIT_ADDRESS:
        /* The whole address selected, so typing replaces it -- which is what
         * somebody clicking an address bar almost always means.
         *
         * recon_edit_focus rather than recon_edit_begin with the field's own
         * text: that form aliases snprintf's source and destination, which is
         * undefined and empties the field. Clicking the address bar cleared
         * the address it was showing. BG-112. */
        recon_edit_focus(&w->address);
        return true;
    default:
        return false;
    }
}

static bool web_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_web *w = user;
    struct web_tab *t = front(w);
    bool ctrl = (modifiers & RECON_MOD_CTRL) != 0;

    /*
     * --- The shortcuts, before any field gets the key ---
     *
     * With Ctrl held, the keystroke is a command and not text. Letting the
     * address bar see Ctrl+T first would put a "t" in the address instead of
     * opening a tab, which is the bug every one of these exists to avoid.
     */
    if (ctrl) {
        switch (sym) {
        case XKB_KEY_t:
        case XKB_KEY_T:
            menu_do(w, MENU_NEW_TAB);
            return true;
        case XKB_KEY_w:
        case XKB_KEY_W:
            tab_close(w, w->active);
            if (t != NULL) {
                recon_appwin_refresh(t->win);
            }
            return true;
        case XKB_KEY_f:
        case XKB_KEY_F:
            find_open(w, true);
            return true;
        case XKB_KEY_d:
        case XKB_KEY_D:
            bookmark_toggle(w);
            if (w->bookmark_count == 1) {
                w->show_bookmarks = true;
            }
            if (t != NULL) {
                recon_appwin_refresh(t->win);
            }
            return true;
        case XKB_KEY_l:
        case XKB_KEY_L:
            recon_edit_focus(&w->address);
            if (t != NULL) {
                recon_appwin_refresh(t->win);
            }
            return true;
        case XKB_KEY_r:
        case XKB_KEY_R:
            if (t != NULL && t->have_url) {
                go_to(t, &t->url, false);
            }
            return true;
        case XKB_KEY_plus:
        case XKB_KEY_equal:
            set_zoom(w, (t != NULL ? t->zoom : 100) + ZOOM_STEP);
            return true;
        case XKB_KEY_minus:
            set_zoom(w, (t != NULL ? t->zoom : 100) - ZOOM_STEP);
            return true;
        case XKB_KEY_0:
            set_zoom(w, 100);
            return true;
        default:
            break;
        }
    }

    /* --- The find bar, while it has the caret --- */
    if (w->strip == STRIP_FIND && w->find.active) {
        switch (recon_edit_key(&w->find, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            find_step(w, 1);
            return true;
        case RECON_EDIT_CANCEL:
            find_open(w, false);
            return true;
        case RECON_EDIT_CHANGED:
            /*
             * A changed needle means a different set of matches, so the
             * position within them means nothing until they are counted
             * again. Back to the first rather than left pointing at the
             * fourth of a set that may now have two.
             */
            w->find_at = 0;
            w->find_count = 0;
            w->find_follow = true;
            if (t != NULL) {
                t->content_height = 0;
                recon_appwin_refresh(t->win);
            }
            return true;
        case RECON_EDIT_IGNORED:
            break;
        }
    }

    if (w->address.active) {
        switch (recon_edit_key(&w->address, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            w->address.active = false;
            if (t != NULL) {
                go_typed(t);
            }
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

    if (t == NULL) {
        return false;
    }

    int page = t->viewport_height > 40 ? t->viewport_height - 20 : 40;

    switch (sym) {
    case XKB_KEY_Escape:
        if (w->menu_open) {
            w->menu_open = false;
            recon_appwin_refresh(t->win);
            return true;
        }
        if (w->strip == STRIP_FIND) {
            find_open(w, false);
            return true;
        }
        return false;
    case XKB_KEY_F3:
        find_step(w, (modifiers & RECON_MOD_SHIFT) != 0 ? -1 : 1);
        return true;
    case XKB_KEY_Down:      t->scroll += 40; return true;
    case XKB_KEY_Up:        t->scroll -= 40; return true;
    case XKB_KEY_Page_Down:
    case XKB_KEY_space:     t->scroll += page; return true;
    case XKB_KEY_Page_Up:   t->scroll -= page; return true;
    case XKB_KEY_Home:      t->scroll = 0; return true;
    case XKB_KEY_End:       t->scroll = t->content_height; return true;
    default:
        return false;
    }
}

static void web_scroll(void *user, double delta) {
    struct recon_web *w = user;
    struct web_tab *t = front(w);
    if (t == NULL) {
        return;
    }
    t->scroll -= (int)(delta * 48);
    if (t->scroll < 0) {
        t->scroll = 0;
    }
}

static void web_describe(void *user, char *out, size_t size) {
    struct recon_web *w = user;
    struct web_tab *t = front(w);

    char address[RECON_HTTP_URL_MAX] = "(none)";
    if (t != NULL && t->have_url) {
        recon_http_format_url(&t->url, address, sizeof(address));
    }

    /*
     * Every tab, not only the one in front. This report is what the test
     * harness reads, and a browser whose report describes one of its twelve
     * tabs is a browser eleven twelfths of whose state cannot be checked.
     */
    size_t used = (size_t)snprintf(out, size,
        "  tabs: %d, showing %d\n"
        "  address: %s\n"
        "  title: %s\n"
        "  blocks: %d, %d named places\n"
        "  history: %d, at %d\n"
        "  scroll: %d of %d\n"
        "  zoom: %d%%\n"
        "  loading: %s\n"
        "  bookmarks: %d%s\n"
        "  find: %s (%d matches, on %d)\n"
        "  typed: %s\n"
        "  status: %s\n",
        w->tab_count, w->active + 1,
        address,
        (t != NULL && t->page != NULL) ? recon_html_title(t->page) : "",
        (t != NULL) ? recon_html_block_count(t->page) : 0,
        (t != NULL) ? recon_html_anchor_count(t->page) : 0,
        (t != NULL) ? t->history_count : 0, (t != NULL) ? t->at : -1,
        (t != NULL) ? t->scroll : 0, (t != NULL) ? t->content_height : 0,
        (t != NULL) ? t->zoom : 100,
        (t != NULL && t->loading) ? "yes" : "no",
        w->bookmark_count, w->show_bookmarks ? ", bar shown" : "",
        w->strip == STRIP_FIND ? w->find.text : "(closed)",
        w->find_count, w->find_at + 1,
        w->address.text,
        (t != NULL) ? t->status : "");

    for (int i = 0; i < w->tab_count && used < size; i++) {
        char one[RECON_HTTP_URL_MAX] = "(empty)";
        if (w->tabs[i]->have_url) {
            recon_http_format_url(&w->tabs[i]->url, one, sizeof(one));
        }
        used += (size_t)snprintf(out + used, size - used,
            "  tab %d: %s -- %s\n", i + 1, w->tabs[i]->label, one);
    }
}

static void web_destroy(void *user) {
    struct recon_web *w = user;
    /* Every tab, and each of them cancels what it has in flight: a fetch that
     * outlives the window it was for calls back into freed memory. */
    for (int i = 0; i < w->tab_count; i++) {
        tab_free(w->tabs[i]);
    }
    free(w);
}

static const struct recon_appwin_impl WEB_IMPL = {
    .title = WEB_APPLICATION,
    .help = "The web viewer",
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
static void show_document(struct web_tab *w, struct recon_html_document *page,
        const char *shown, size_t length) {
    recon_html_free(w->page);
    w->page = page;

    /*
     * The address bar belongs to the window, and only the tab in front owns
     * what it says. A page finishing in a background tab must not reach up
     * and rewrite the address of the page being read.
     */
    if (w->owner != NULL && front(w->owner) == w) {
        recon_edit_begin(&w->owner->address, shown, false);
        w->owner->address.active = false;
    }

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

    /*
     * The tab's own name, which is the window's only while this tab is in
     * front. A background tab finishing must still get its label -- that is
     * the whole point of a tab strip -- but it must not rename the window
     * out from under the page being read.
     */
    const char *name = window_title[0] != '\0' ? window_title
        : (w->have_url ? w->url.host : "Untitled");

    /* The precision says the truncation is meant. A tab is ninety-six bytes
     * wide and a page title can be any length at all, so a long one is cut --
     * and the strip clips it to the tab's width long before that anyway. */
    snprintf(w->label, sizeof(w->label), "%.*s",
        (int)sizeof(w->label) - 1, name);

    if (w->owner == NULL || front(w->owner) == w) {
        recon_appwin_set_title(w->win, window_title);
    }

    w->scroll = 0;
    w->content_height = 0;

    /*
     * A link into the middle of another page -- "somewhere.html#install" --
     * is a fetch and then a jump, and the jump can only happen once the
     * document is here. Asked for now that it is.
     */
    w->jump_to = -1;
    if (w->have_url && w->url.fragment[0] != '\0') {
        w->jump_to = recon_html_anchor_block(w->page, w->url.fragment);
    }

    /*
     * The pictures are asked for after the words are on screen, one at a
     * time. A page is readable the moment its text is there.
     */
    collect_images(w);
    fetch_next_image(w);

    /*
     * And the stylesheets, if this is the first pass over this page and it
     * asked for any. `restyled` stops the second pass starting a third.
     */
    if (!w->restyled && w->source != NULL) {
        w->sheets_wanted = recon_html_stylesheet_count(w->page);
        w->sheets_done = 0;
        if (w->sheets_wanted > 0) {
            fetch_next_sheet(w);
        } else {
            forget_sheets_source(w);
        }
    }

    if (recon_html_needs_scripting(w->page)) {
        set_status(w, true, "That page builds itself with JavaScript, which "
            "this does not have. There is nothing to show.");
    } else if (recon_html_block_count(w->page) == 0) {
        set_status(w, true, "There is nothing readable there.");
    } else if (recon_html_was_truncated(w->page)) {
        /*
         * Said out loud, because a page cut off looks exactly like a page that
         * ended.
         *
         * The ceilings themselves are right -- a page is somebody else's file
         * and a reader that grows to fit whatever it is handed is one a
         * hostile page can exhaust. What was wrong was being quiet: somebody
         * reading a truncated page has no way to know the rest is there, and
         * scrolling to the bottom of a document is not how anybody checks.
         */
        set_status(w, true, "%d blocks, %zu KB -- and this page is larger "
            "than this reader will hold, so the rest of it is not shown.",
            recon_html_block_count(w->page), length / 1024);
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
    struct recon_web *window = recon_appwin_user(win);
    if (window == NULL) {
        return false;
    }
    struct web_tab *w = front(window);
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

    forget_sheets(w);
    w->sheet = recon_css_new();
    show_document(w, markup ? recon_html_parse_styled(text, length, w->sheet)
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
    recon_edit_begin(&w->address, "", false);
    w->address.active = false;
    recon_edit_begin(&w->find, "", false);
    w->find.active = false;

    /*
     * Home is the hub.
     *
     * A browser with no home page has a Home button that does nothing, and a
     * button that does nothing is worse than no button. This is the network
     * ReconOS belongs to, which makes it the right answer as well as an
     * answer -- and the menu can change it.
     */
    snprintf(w->home, sizeof(w->home), "https://recontowers.com/");

    bookmarks_load(w);
    w->show_bookmarks = w->bookmark_count > 0;

    w->win = recon_appwin_create(server, font, &WEB_IMPL, w);
    if (w->win == NULL) {
        free(w);
        return NULL;
    }

    /*
     * The first tab is made after the window, because a tab copies the
     * window's font and handle and there is no handle until now.
     */
    if (tab_new(w) == NULL) {
        recon_appwin_destroy(w->win);
        return NULL;
    }
    return w->win;
}
