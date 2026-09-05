/*
 * The Mail window. See include/recon_mailwin.h.
 *
 * Three screens in one window, because they are one task with three
 * prerequisites: an account, a password, and then the mail. Which one shows is
 * decided by what the window has, not by anything somebody has to navigate to.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_icons.h"
#include "recon_mail.h"
#include "recon_mailwin.h"
#include "recon_net.h"
#include "recon_server.h"
#include "recon_theme.h"
#include "recon_ui.h"

#define COLOR_BG THEME(SURFACE)
#define COLOR_PANEL THEME(SURFACE_ALT)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_DIM THEME(SURFACE_TEXT_DIM)
#define COLOR_SELECTED THEME(SELECTION)
#define COLOR_SELECTED_TEXT THEME(SELECTION_TEXT)
#define COLOR_SEPARATOR THEME(MENU_SEPARATOR)
#define COLOR_BAR THEME(BAR)
#define COLOR_WARNING THEME(WARNING)

#define PADDING 8
#define ROW_HEIGHT 38
#define FIELD_HEIGHT 24
#define BUTTON_HEIGHT 26
#define BAR_HEIGHT 26

/*
 * How many headers to ask for.
 *
 * A screenful and then some. A mailbox with forty thousand messages in it is
 * an ordinary thing and fetching all of them to fill one list is how a mail
 * client earns a reputation; this is the newest fifty and a button would be
 * the way to ask for more.
 */
#define FETCH_LIMIT 50

/* Fields on the setup screen, in the order they are tabbed through. */
enum setup_field {
    FIELD_HOST,
    FIELD_USER,
    FIELD_PORT,
    FIELD_COUNT,
};

enum mail_screen {
    SCREEN_SETUP,
    SCREEN_PASSWORD,
    SCREEN_MAIL,
};

#define HIT_FIELD_BASE (RECON_APPWIN_HIT_USER + 100)
#define HIT_PROTOCOL (RECON_APPWIN_HIT_USER + 10)
#define HIT_SAVE (RECON_APPWIN_HIT_USER + 11)
#define HIT_CONNECT (RECON_APPWIN_HIT_USER + 12)
#define HIT_FORGET (RECON_APPWIN_HIT_USER + 13)
#define HIT_REFRESH (RECON_APPWIN_HIT_USER + 14)
#define HIT_BACK (RECON_APPWIN_HIT_USER + 15)
#define HIT_ROW_BASE (RECON_APPWIN_HIT_USER + 200)

struct recon_mailwin {
    struct recon_font *font;
    struct recon_appwin *win;

    enum mail_screen screen;
    struct recon_mail_account account;

    /* The setup form. */
    struct recon_edit fields[FIELD_COUNT];
    int focused;

    /*
     * The password, held here and nowhere else.
     *
     * Not written to the registry, not written to a file. It lives for as long
     * as this window does and goes when it does. See the long note in
     * recon_mail.h about why that is the honest answer for now.
     */
    struct recon_edit password;

    struct recon_mail_session *session;
    int selected;
    int scroll;
    /* True while reading one message rather than the list. */
    bool reading;
    int body_scroll;

    char message[256];
    bool message_is_error;
};

static void set_message(struct recon_mailwin *m, bool error, const char *fmt,
        ...) __attribute__((format(printf, 3, 4)));

static void set_message(struct recon_mailwin *m, bool error, const char *fmt,
        ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(m->message, sizeof(m->message), fmt, args);
    va_end(args);
    m->message_is_error = error;
}

/* --- The session --- */

static void on_changed(void *user) {
    struct recon_mailwin *m = user;
    recon_appwin_refresh(m->win);
}

static void on_finished(void *user, bool ok, const char *error) {
    struct recon_mailwin *m = user;

    if (!ok) {
        set_message(m, true, "%s", error);
        /*
         * Back to the password screen rather than staying on an empty list.
         * Almost every way this fails is something somebody has to retype, and
         * a list that is empty because the sign-in was refused looks exactly
         * like a list that is empty because there is no mail.
         */
        m->screen = SCREEN_PASSWORD;
        recon_edit_begin(&m->password, "", false);
        m->password.masked = true;
    }
    recon_appwin_refresh(m->win);
}

static const struct recon_mail_handlers HANDLERS = {
    .changed = on_changed,
    .finished = on_finished,
};

static void disconnect(struct recon_mailwin *m) {
    if (m->session != NULL) {
        recon_mail_close(m->session);
        m->session = NULL;
    }
    m->selected = 0;
    m->scroll = 0;
    m->reading = false;
}

static void connect_now(struct recon_mailwin *m) {
    disconnect(m);

    if (m->password.text[0] == '\0') {
        set_message(m, true, "A password is needed.");
        return;
    }

    m->session = recon_mail_open(&m->account, m->password.text, FETCH_LIMIT,
        &HANDLERS, m);
    if (m->session == NULL) {
        set_message(m, true, "%s", recon_mail_last_error());
        return;
    }

    m->screen = SCREEN_MAIL;
    m->message[0] = '\0';
    m->message_is_error = false;
}

/* --- Setup --- */

static void load_form(struct recon_mailwin *m) {
    char port[16];
    snprintf(port, sizeof(port), "%d", m->account.port);

    recon_edit_begin(&m->fields[FIELD_HOST], m->account.host, false);
    recon_edit_begin(&m->fields[FIELD_USER], m->account.user, false);
    recon_edit_begin(&m->fields[FIELD_PORT], port, false);

    /*
     * `active` set directly, and *not* recon_edit_end.
     *
     * recon_edit_end means "done, throw it away" and clears the text -- which
     * is right for a rename somebody escaped out of, and wrong for a form
     * field that is merely not the one with the caret in it. Using it here
     * emptied the port and username the moment the form was built, and the
     * form looked exactly like a form nobody had filled in.
     */
    m->fields[FIELD_USER].active = false;
    m->fields[FIELD_PORT].active = false;
    m->focused = FIELD_HOST;
}

static void save_form(struct recon_mailwin *m) {
    const char *host = m->fields[FIELD_HOST].text;
    const char *who = m->fields[FIELD_USER].text;

    if (host[0] == '\0') {
        set_message(m, true, "A server is needed. Something like "
            "imap.example.com.");
        return;
    }

    /*
     * Refused rather than truncated.
     *
     * The edit field holds more than the account does, and quietly cutting a
     * long name to fit would save an account that connects somewhere other
     * than where somebody typed -- and the field would still show what they
     * typed, so there would be nothing to notice.
     */
    if (strlen(host) >= sizeof(m->account.host)) {
        set_message(m, true, "That server name is too long -- %zu characters, "
            "and there is room for %zu.", strlen(host),
            sizeof(m->account.host) - 1);
        return;
    }
    if (strlen(who) >= sizeof(m->account.user)) {
        set_message(m, true, "That username is too long -- %zu characters, "
            "and there is room for %zu.", strlen(who),
            sizeof(m->account.user) - 1);
        return;
    }

    /* Both lengths are checked above, so this cannot truncate. memcpy rather
     * than snprintf because the compiler can see that this one is safe and
     * cannot see that the other one had been made safe. */
    memcpy(m->account.host, host, strlen(host) + 1);
    memcpy(m->account.user, who, strlen(who) + 1);
    m->account.port = atoi(m->fields[FIELD_PORT].text);

    if (m->account.port <= 0 || m->account.port > 65535) {
        m->account.port = m->account.protocol == RECON_MAIL_POP3
            ? RECON_MAIL_POP3_PORT : RECON_MAIL_IMAP_PORT;
    }
    if (m->account.name[0] == '\0') {
        snprintf(m->account.name, sizeof(m->account.name), "Mail");
    }

    if (!recon_mail_account_set(&m->account)) {
        set_message(m, true, "%s", recon_mail_last_error());
        return;
    }

    m->screen = SCREEN_PASSWORD;
    recon_edit_begin(&m->password, "", false);
    m->password.masked = true;
    m->message[0] = '\0';
    m->message_is_error = false;
}

/* Switching protocol moves the port with it, unless it has been changed to
 * something that is neither default -- somebody who typed 1993 meant it. */
static void toggle_protocol(struct recon_mailwin *m) {
    int old_default = m->account.protocol == RECON_MAIL_POP3
        ? RECON_MAIL_POP3_PORT : RECON_MAIL_IMAP_PORT;
    int typed = atoi(m->fields[FIELD_PORT].text);

    m->account.protocol = m->account.protocol == RECON_MAIL_IMAP
        ? RECON_MAIL_POP3 : RECON_MAIL_IMAP;

    int new_default = m->account.protocol == RECON_MAIL_POP3
        ? RECON_MAIL_POP3_PORT : RECON_MAIL_IMAP_PORT;

    if (typed == old_default || typed == 0) {
        char port[16];
        snprintf(port, sizeof(port), "%d", new_default);
        recon_edit_begin(&m->fields[FIELD_PORT], port, false);
        m->fields[FIELD_PORT].active = (m->focused == FIELD_PORT);
    }
}

/* --- Drawing --- */

/* A button, returning where the next one starts. */
static int draw_button(struct recon_mailwin *m, struct recon_panel *p, int x,
        int y, const char *label, uint32_t hit, bool enabled) {
    int width = recon_text_width(m->font, label) + 28;
    int ascent = recon_font_ascent(m->font);

    recon_fill_rect(p, x, y, width, BUTTON_HEIGHT, COLOR_BAR);
    recon_draw_button_edge(p, x, y, width, BUTTON_HEIGHT, false, COLOR_BG);

    int tw = recon_text_width(m->font, label);
    recon_draw_text(p, m->font, x + (width - tw) / 2,
        y + (BUTTON_HEIGHT + ascent) / 2 - 2, width - 8, label,
        enabled ? COLOR_TEXT : COLOR_DIM);

    if (enabled) {
        recon_hit_add(p, x, y, width, BUTTON_HEIGHT, hit);
    }
    return x + width + 6;
}

static void draw_setup(struct recon_mailwin *m, struct recon_panel *p,
        int x, int y, int w) {
    int ascent = recon_font_ascent(m->font);
    int line = recon_font_line_height(m->font);

    recon_draw_text(p, m->font, x, y + ascent, w, "Set up mail", COLOR_TEXT);
    y += line + 4;

    recon_draw_text(p, m->font, x, y + ascent, w,
        "The connection is encrypted and the server's certificate is checked. "
        "There is no unencrypted option.", COLOR_DIM);
    y += line + PADDING;

    static const char *const LABELS[FIELD_COUNT] = {
        "Server", "Username", "Port",
    };

    for (int i = 0; i < FIELD_COUNT; i++) {
        recon_draw_text(p, m->font, x, y + ascent, 90, LABELS[i], COLOR_TEXT);

        int fx = x + 96;
        int fw = (i == FIELD_PORT) ? 80 : w - 96 - PADDING;
        recon_edit_draw(p, m->font, fx, y, fw, FIELD_HEIGHT, &m->fields[i]);
        recon_hit_add(p, fx, y, fw, FIELD_HEIGHT, HIT_FIELD_BASE + i);
        y += FIELD_HEIGHT + 6;
    }

    /* The protocol, as one control that says what it is rather than two
     * radio buttons where only one can be right. */
    recon_draw_text(p, m->font, x, y + ascent, 90, "Reading", COLOR_TEXT);
    draw_button(m, p, x + 96, y - 2,
        m->account.protocol == RECON_MAIL_IMAP
            ? "IMAP -- the mail stays on the server"
            : "POP3 -- the mail is downloaded here",
        HIT_PROTOCOL, true);
    y += BUTTON_HEIGHT + PADDING;

    draw_button(m, p, x, y, "Save", HIT_SAVE, true);
}

static void draw_password(struct recon_mailwin *m, struct recon_panel *p,
        int x, int y, int w) {
    int ascent = recon_font_ascent(m->font);
    int line = recon_font_line_height(m->font);

    char heading[256];
    snprintf(heading, sizeof(heading), "Sign in to %s", m->account.host);
    recon_draw_text(p, m->font, x, y + ascent, w, heading, COLOR_TEXT);
    y += line + 4;

    char as[256];
    snprintf(as, sizeof(as), "as %s, over %s", m->account.user,
        m->account.protocol == RECON_MAIL_IMAP ? "IMAP" : "POP3");
    recon_draw_text(p, m->font, x, y + ascent, w, as, COLOR_DIM);
    y += line + PADDING;

    recon_draw_text(p, m->font, x, y + ascent, 90, "Password", COLOR_TEXT);
    recon_edit_draw(p, m->font, x + 96, y, w - 96 - PADDING, FIELD_HEIGHT,
        &m->password);
    recon_hit_add(p, x + 96, y, w - 96 - PADDING, FIELD_HEIGHT,
        HIT_FIELD_BASE + FIELD_COUNT);
    y += FIELD_HEIGHT + PADDING;

    int bx = draw_button(m, p, x, y, "Connect", HIT_CONNECT, true);
    draw_button(m, p, bx, y, "Change the account", HIT_FORGET, true);
    y += BUTTON_HEIGHT + PADDING;

    /*
     * Said plainly rather than buried. Somebody typing a password into a new
     * program is entitled to know what happens to it, and "nothing" is a
     * better answer than most programs can give.
     */
    recon_draw_text(p, m->font, x, y + ascent, w,
        "The password is not saved. It is used for this connection and "
        "forgotten when the window closes.", COLOR_DIM);
}

static void draw_reading(struct recon_mailwin *m, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(m->font);
    int line = recon_font_line_height(m->font);

    const struct recon_mail_message *message =
        recon_mail_at(m->session, m->selected);
    if (message == NULL) {
        m->reading = false;
        return;
    }

    draw_button(m, p, x, y, "Back to the list", HIT_BACK, true);
    y += BUTTON_HEIGHT + PADDING;

    recon_draw_text(p, m->font, x, y + ascent, w, message->subject, COLOR_TEXT);
    y += line;
    recon_draw_text(p, m->font, x, y + ascent, w, message->from, COLOR_DIM);
    y += line;
    recon_draw_text(p, m->font, x, y + ascent, w, message->date, COLOR_DIM);
    y += line + 4;

    recon_fill_rect(p, x, y, w, 1, COLOR_SEPARATOR);
    y += PADDING;

    if (message->body == NULL) {
        recon_draw_text(p, m->font, x, y + ascent, w,
            "Fetching...", COLOR_DIM);
        return;
    }

    /*
     * The body, one line at a time, scrolled.
     *
     * No wrapping and no HTML. A message that arrived as HTML is shown as the
     * HTML it is, which is honest and ugly; pretending to render it would mean
     * a layout engine, and pretending it is plain text by stripping the tags
     * would silently change what somebody sent.
     */
    int rows = (y + h - y) / line;
    const char *at = message->body;
    int row = 0;
    int skipped = 0;

    while (*at != '\0' && row < rows) {
        const char *end = strchr(at, '\n');
        size_t length = (end != NULL) ? (size_t)(end - at) : strlen(at);

        if (skipped >= m->body_scroll) {
            char text[512];
            size_t take = length < sizeof(text) - 1 ? length : sizeof(text) - 1;
            memcpy(text, at, take);
            text[take] = '\0';

            recon_draw_text(p, m->font, x, y + row * line + ascent, w, text,
                COLOR_TEXT);
            row++;
        } else {
            skipped++;
        }

        if (end == NULL) {
            break;
        }
        at = end + 1;
    }
}

static void draw_list(struct recon_mailwin *m, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(m->font);
    int line = recon_font_line_height(m->font);
    int count = recon_mail_count(m->session);

    int bx = draw_button(m, p, x, y, "Refresh", HIT_REFRESH, true);
    draw_button(m, p, bx, y, "Change the account", HIT_FORGET, true);
    y += BUTTON_HEIGHT + PADDING;

    int rows = (h - (y - PADDING)) / ROW_HEIGHT;
    if (rows < 1) {
        rows = 1;
    }
    if (m->scroll > count - rows) {
        m->scroll = count - rows;
    }
    if (m->scroll < 0) {
        m->scroll = 0;
    }

    if (count == 0) {
        recon_draw_text(p, m->font, x, y + ascent, w,
            recon_mail_status(m->session), COLOR_DIM);
        return;
    }

    recon_fill_rect(p, x, y, w, rows * ROW_HEIGHT, COLOR_PANEL);

    for (int row = 0; row < rows; row++) {
        int i = m->scroll + row;
        if (i >= count) {
            break;
        }

        const struct recon_mail_message *message = recon_mail_at(m->session, i);
        int ry = y + row * ROW_HEIGHT;
        bool chosen = (i == m->selected);

        if (chosen) {
            recon_fill_rect(p, x, ry, w, ROW_HEIGHT, COLOR_SELECTED);
        }

        uint32_t ink = chosen ? COLOR_SELECTED_TEXT : COLOR_TEXT;
        uint32_t dim = chosen ? COLOR_SELECTED_TEXT : COLOR_DIM;

        recon_draw_text(p, m->font, x + 6, ry + ascent + 2, w - 12,
            message->subject, ink);
        recon_draw_text(p, m->font, x + 6, ry + line + ascent + 1, w - 12,
            message->from, dim);

        recon_hit_add(p, x, ry, w, ROW_HEIGHT, HIT_ROW_BASE + i);
    }
}

static void mailwin_draw(void *user, struct recon_panel *p,
        int x, int y, int w, int h) {
    struct recon_mailwin *m = user;
    int ascent = recon_font_ascent(m->font);
    int line = recon_font_line_height(m->font);

    recon_fill_rect(p, x, y, w, h, COLOR_BG);

    /* The status bar along the bottom, on every screen, because every screen
     * has something worth saying about what just happened. */
    int bar = y + h - BAR_HEIGHT;
    recon_fill_rect(p, x, bar, w, BAR_HEIGHT, COLOR_BAR);
    recon_fill_rect(p, x, bar, w, 1, COLOR_SEPARATOR);

    const char *status = m->message[0] != '\0'
        ? m->message
        : (m->session != NULL ? recon_mail_status(m->session) : "");
    recon_draw_text(p, m->font, x + PADDING, bar + (BAR_HEIGHT + ascent) / 2 - 1,
        w - PADDING * 2, status,
        m->message_is_error ? COLOR_WARNING : COLOR_DIM);

    int inner_x = x + PADDING;
    int inner_y = y + PADDING;
    int inner_w = w - PADDING * 2;
    int inner_h = h - BAR_HEIGHT - PADDING * 2;

    switch (m->screen) {
    case SCREEN_SETUP:
        draw_setup(m, p, inner_x, inner_y, inner_w);
        break;
    case SCREEN_PASSWORD:
        draw_password(m, p, inner_x, inner_y, inner_w);
        break;
    case SCREEN_MAIL:
        if (m->session == NULL) {
            recon_draw_text(p, m->font, inner_x, inner_y + ascent, inner_w,
                "Not connected.", COLOR_DIM);
            break;
        }
        if (m->reading) {
            draw_reading(m, p, inner_x, inner_y, inner_w, inner_h);
        } else {
            draw_list(m, p, inner_x, inner_y, inner_w, inner_h);
        }
        break;
    }
    (void)line;
}

/* --- Input --- */

static void open_selected(struct recon_mailwin *m);

static bool mailwin_click(void *user, uint32_t hit, int cx, int cy,
        bool pressed) {
    struct recon_mailwin *m = user;
    (void)cx;
    (void)cy;

    if (!pressed || hit < RECON_APPWIN_HIT_USER) {
        return false;
    }

    /* Descending, because the ladders below are unbounded. */
    if (hit >= HIT_ROW_BASE) {
        int i = (int)(hit - HIT_ROW_BASE);
        if (i < 0 || i >= recon_mail_count(m->session)) {
            return true;
        }

        /*
         * One click selects, and a second click on the row already selected
         * opens it -- which is how a double click behaves without anything
         * having to time one, and is what File Explorer does two windows away.
         */
        if (m->selected == i) {
            open_selected(m);
            return true;
        }

        m->selected = i;
        set_message(m, false, "Click again to read it.");
        return true;
    }
    if (hit >= HIT_FIELD_BASE) {
        int i = (int)(hit - HIT_FIELD_BASE);
        if (i == FIELD_COUNT) {
            m->password.active = true;
            return true;
        }
        if (i >= 0 && i < FIELD_COUNT) {
            m->focused = i;
            for (int f = 0; f < FIELD_COUNT; f++) {
                m->fields[f].active = (f == i);
            }
        }
        return true;
    }

    switch (hit) {
    case HIT_PROTOCOL:
        toggle_protocol(m);
        return true;
    case HIT_SAVE:
        save_form(m);
        return true;
    case HIT_CONNECT:
        connect_now(m);
        return true;
    case HIT_FORGET:
        disconnect(m);
        m->screen = SCREEN_SETUP;
        load_form(m);
        m->message[0] = '\0';
    m->message_is_error = false;
        return true;
    case HIT_REFRESH:
        connect_now(m);
        return true;
    case HIT_BACK:
        m->reading = false;
        m->body_scroll = 0;
        return true;
    default:
        return false;
    }
}

static void open_selected(struct recon_mailwin *m) {
    if (m->session == NULL || recon_mail_count(m->session) == 0) {
        return;
    }
    m->reading = true;
    m->body_scroll = 0;
    if (!recon_mail_fetch_body(m->session, m->selected)) {
        set_message(m, true, "%s", recon_mail_last_error());
    }
}

static bool mailwin_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_mailwin *m = user;

    if (m->screen == SCREEN_SETUP) {
        struct recon_edit *edit = &m->fields[m->focused];

        if (sym == XKB_KEY_Tab) {
            edit->active = false;
            m->focused = (m->focused + 1) % FIELD_COUNT;
            recon_edit_begin(&m->fields[m->focused],
                m->fields[m->focused].text, false);
            return true;
        }

        switch (recon_edit_key(edit, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            save_form(m);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_CANCEL:
            return true;
        case RECON_EDIT_IGNORED:
            return false;
        }
        return false;
    }

    if (m->screen == SCREEN_PASSWORD) {
        switch (recon_edit_key(&m->password, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            connect_now(m);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_CANCEL:
            return true;
        case RECON_EDIT_IGNORED:
            return false;
        }
        return false;
    }

    /* Reading the mail. */
    if (m->reading) {
        switch (sym) {
        case XKB_KEY_Escape:
        case XKB_KEY_BackSpace:
            m->reading = false;
            m->body_scroll = 0;
            return true;
        case XKB_KEY_Down:
            m->body_scroll++;
            return true;
        case XKB_KEY_Up:
            if (m->body_scroll > 0) {
                m->body_scroll--;
            }
            return true;
        default:
            return false;
        }
    }

    int count = recon_mail_count(m->session);
    switch (sym) {
    case XKB_KEY_Down:
        if (m->selected + 1 < count) {
            m->selected++;
        }
        return true;
    case XKB_KEY_Up:
        if (m->selected > 0) {
            m->selected--;
        }
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        open_selected(m);
        return true;
    default:
        return false;
    }
}

static void mailwin_scroll(void *user, double delta) {
    struct recon_mailwin *m = user;

    if (m->reading) {
        m->body_scroll -= (int)(delta * 3);
        if (m->body_scroll < 0) {
            m->body_scroll = 0;
        }
        return;
    }
    m->scroll -= (int)(delta * 3);
    if (m->scroll < 0) {
        m->scroll = 0;
    }
}

static void mailwin_describe(void *user, char *out, size_t size) {
    struct recon_mailwin *m = user;
    snprintf(out, size,
        "  screen: %s\n"
        "  account: %s on %s:%d (%s)\n"
        "  connected: %s\n"
        "  messages: %d\n"
        "  status: %s\n",
        m->screen == SCREEN_SETUP ? "setup"
            : (m->screen == SCREEN_PASSWORD ? "password" : "mail"),
        m->account.user[0] != '\0' ? m->account.user : "(none)",
        m->account.host[0] != '\0' ? m->account.host : "(none)",
        m->account.port,
        m->account.protocol == RECON_MAIL_IMAP ? "IMAP" : "POP3",
        m->session != NULL ? "yes" : "no",
        recon_mail_count(m->session),
        m->session != NULL ? recon_mail_status(m->session) : m->message);
}

static void mailwin_destroy(void *user) {
    struct recon_mailwin *m = user;
    disconnect(m);
    /* The password was in this memory. Cleared rather than merely freed. */
    memset(m, 0, sizeof(*m));
    free(m);
}

static const struct recon_appwin_impl MAIL_IMPL = {
    .title = "Mail",
    .help = "Networking",
    .icon = RECON_ICON_MAIL,
    .default_width = 720,
    .default_height = 520,
    .min_width = 420,
    .min_height = 300,
    .draw = mailwin_draw,
    .click = mailwin_click,
    .key = mailwin_key,
    .scroll = mailwin_scroll,
    .describe = mailwin_describe,
    .destroy = mailwin_destroy,
};

struct recon_appwin *recon_mailwin_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_mailwin *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }

    m->font = font;

    if (recon_mail_account_get(&m->account)) {
        m->screen = SCREEN_PASSWORD;
        recon_edit_begin(&m->password, "", false);
        m->password.masked = true;
    } else {
        /* A sensible starting point rather than an empty form: IMAP on its
         * usual port is what almost everybody wants, and a form that is
         * already half right is faster to check than one to fill in. */
        m->account.protocol = RECON_MAIL_IMAP;
        m->account.port = RECON_MAIL_IMAP_PORT;
        snprintf(m->account.name, sizeof(m->account.name), "Mail");
        m->screen = SCREEN_SETUP;
        load_form(m);
    }

    m->win = recon_appwin_create(server, font, &MAIL_IMPL, m);
    if (m->win == NULL) {
        free(m);
        return NULL;
    }
    return m->win;
}
