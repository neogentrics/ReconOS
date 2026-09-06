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
#include "recon_crypt.h"
#include "recon_filedlg.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_mail.h"
#include "recon_smtp.h"
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
/* The body box is a place text is typed, so it takes the field roles rather
 * than the surface ones -- a skin makes those different on purpose. */
#define COLOR_FIELD THEME(FIELD)
#define COLOR_FIELD_TEXT THEME(FIELD_TEXT)

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

/*
 * Fields on the setup screen, in the order they are tabbed through.
 *
 * Both halves of an account on one form. Reading and sending are separate
 * protocols on separate servers, and they are the same account to the person
 * filling it in -- two forms would mean somebody who set up reading and then
 * could not work out why sending did nothing.
 */
enum setup_field {
    FIELD_HOST,
    FIELD_USER,
    FIELD_PORT,
    FIELD_SEND_HOST,
    FIELD_SEND_PORT,
    FIELD_FROM,
    FIELD_COUNT,
};

/*
 * Fields on the compose screen. The body is last because it is the tall one.
 *
 * Cc and Bcc are separate fields rather than one list with a marker, because
 * they are separate things: everyone here receives the letter, and only the
 * first two are named in it. A single field with a convention for "hide this
 * one" would put that distinction in somebody's typing, and typing is where it
 * would eventually be got wrong.
 */
enum compose_field {
    COMPOSE_TO,
    COMPOSE_CC,
    COMPOSE_BCC,
    COMPOSE_SUBJECT,
    COMPOSE_BODY,
    COMPOSE_COUNT,
};

enum mail_screen {
    SCREEN_SETUP,
    SCREEN_PASSWORD,
    SCREEN_MAIL,
    SCREEN_COMPOSE,
};

#define HIT_FIELD_BASE (RECON_APPWIN_HIT_USER + 100)
#define HIT_PROTOCOL (RECON_APPWIN_HIT_USER + 10)
#define HIT_SAVE (RECON_APPWIN_HIT_USER + 11)
#define HIT_CONNECT (RECON_APPWIN_HIT_USER + 12)
#define HIT_FORGET (RECON_APPWIN_HIT_USER + 13)
#define HIT_REFRESH (RECON_APPWIN_HIT_USER + 14)
#define HIT_BACK (RECON_APPWIN_HIT_USER + 15)
#define HIT_SECURITY (RECON_APPWIN_HIT_USER + 19)
#define HIT_WRITE (RECON_APPWIN_HIT_USER + 16)
#define HIT_SEND (RECON_APPWIN_HIT_USER + 17)
#define HIT_DISCARD (RECON_APPWIN_HIT_USER + 18)
#define HIT_ATTACH (RECON_APPWIN_HIT_USER + 20)
#define HIT_COMPOSE_BASE (RECON_APPWIN_HIT_USER + 300)
/* One per attached file, so the X beside each is its own thing. Above the
 * compose fields and below the row ladder, which is checked last. */
#define HIT_ATTACHED_BASE (RECON_APPWIN_HIT_USER + 320)
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

    /* The letter being written, and the account it would go through. */
    struct recon_smtp_account sending;
    struct recon_edit compose[COMPOSE_COUNT];
    int compose_focused;

    /*
     * The files chosen to travel with it -- paths only, until Send.
     *
     * Deliberately not read here. A file picked at nine and sent at eleven
     * should be the file as it is at eleven, and holding the bytes for two
     * hours would send a version of it that no longer exists anywhere. It also
     * means a window left open with a large file attached costs nothing.
     */
    char attached[RECON_SMTP_ATTACHMENTS_MAX][RECON_PATH_MAX];
    int attached_count;
    struct recon_filedlg dialog;

    struct recon_smtp_session *sender;
    /* True from pressing Send until the server answers, so the button can say
     * so and cannot be pressed twice. */
    bool sending_now;

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

/* --- Sending --- */

static void on_send_progress(void *user, const char *what) {
    struct recon_mailwin *m = user;
    set_message(m, false, "%s", what);
    recon_appwin_refresh(m->win);
}

static void on_sent(void *user) {
    struct recon_mailwin *m = user;
    m->sending_now = false;

    /*
     * Back to the list, and the letter is cleared.
     *
     * Staying on a screen showing a message that has already gone invites
     * pressing Send again, and the second copy is indistinguishable from the
     * first to everybody except the person who received both.
     */
    m->screen = SCREEN_MAIL;
    for (int i = 0; i < COMPOSE_COUNT; i++) {
        recon_edit_begin(&m->compose[i], "", false);
        m->compose[i].active = false;
    }
    /* Including what was attached. A letter that cleared its text and kept its
     * files would put them on the next one silently. */
    m->attached_count = 0;
    set_message(m, false, "Sent.");
    recon_appwin_refresh(m->win);
}

static void on_send_failed(void *user, const char *why) {
    struct recon_mailwin *m = user;
    m->sending_now = false;

    /*
     * The letter is left exactly as it was. A failure that also loses what
     * somebody wrote is two failures, and the second one is the one they will
     * remember.
     */
    set_message(m, true, "%s", why);
    recon_appwin_refresh(m->win);
}

static const struct recon_smtp_handlers SEND_HANDLERS = {
    .progress = on_send_progress,
    .sent = on_sent,
    .failed = on_send_failed,
};

static void stop_sending(struct recon_mailwin *m) {
    if (m->sender != NULL) {
        recon_smtp_close(m->sender);
        m->sender = NULL;
    }
    m->sending_now = false;
}

static void start_writing(struct recon_mailwin *m) {
    if (!recon_smtp_account_get(&m->sending) || m->sending.host[0] == '\0') {
        set_message(m, true, "There is no sending server set up. Change the "
            "account and fill in the sending half.");
        return;
    }
    if (m->sending.from[0] == '\0') {
        set_message(m, true, "There is no address to send from. Change the "
            "account and fill in 'Your address'.");
        return;
    }

    for (int i = 0; i < COMPOSE_COUNT; i++) {
        recon_edit_begin(&m->compose[i], "", false);
        m->compose[i].active = false;
    }
    m->attached_count = 0;
    /* The body is the one field where Enter means a new line rather than
     * "done". See recon_edit's multiline flag. */
    m->compose[COMPOSE_BODY].multiline = true;

    m->compose[COMPOSE_TO].active = true;
    m->compose_focused = COMPOSE_TO;
    m->screen = SCREEN_COMPOSE;
    m->message[0] = '\0';
    m->message_is_error = false;
}

/*
 * The last part of a path, which is the part a recipient should see.
 *
 * A message that named the folder a file came out of would be telling everyone
 * who receives it something about how this machine is arranged, which is
 * nobody's business and was never the point of attaching a file.
 */
static const char *name_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return (slash != NULL && slash[1] != '\0') ? slash + 1 : path;
}

static void send_now(struct recon_mailwin *m) {
    if (m->sending_now) {
        return;
    }

    /*
     * Refused rather than truncated, for the reason the setup form gives about
     * server names and with more at stake here.
     *
     * The edit field holds more than a letter does. Cutting an address to fit
     * would send to a different address from the one on screen, and the screen
     * would go on showing the one that was typed -- so there would be nothing
     * to notice until somebody else received it.
     */
    const char *subject = m->compose[COMPOSE_SUBJECT].text;

    struct recon_smtp_letter letter;
    memset(&letter, 0, sizeof(letter));

    /* The three address fields and where each one goes. Named in a table so
     * adding a fourth is one line and cannot forget the length check. */
    const struct {
        int field;
        char *into;
        size_t room;
        const char *label;
    } LISTS[3] = {
        { COMPOSE_TO,  letter.to,  sizeof(letter.to),  "To" },
        { COMPOSE_CC,  letter.cc,  sizeof(letter.cc),  "Cc" },
        { COMPOSE_BCC, letter.bcc, sizeof(letter.bcc), "Bcc" },
    };

    for (int i = 0; i < 3; i++) {
        const char *text = m->compose[LISTS[i].field].text;
        size_t length = strlen(text);
        if (length >= LISTS[i].room) {
            set_message(m, true, "The %s list is too long -- %zu characters, "
                "and there is room for %zu.", LISTS[i].label, length,
                LISTS[i].room - 1);
            return;
        }
        memcpy(LISTS[i].into, text, length + 1);
    }

    if (strlen(subject) >= sizeof(letter.subject)) {
        set_message(m, true, "That subject is too long -- %zu characters, and "
            "there is room for %zu.", strlen(subject),
            sizeof(letter.subject) - 1);
        return;
    }

    memcpy(letter.subject, subject, strlen(subject) + 1);
    letter.body = m->compose[COMPOSE_BODY].text;

    /*
     * The attached files, read now rather than when they were chosen.
     *
     * A file picked at nine and sent at eleven should be the file as it is at
     * eleven. Reading it at the moment of sending is also the only way to
     * notice that it has been deleted or renamed since -- which is a thing to
     * say out loud, not a thing to send an empty part about.
     *
     * Freed on every path out of this function, including the ones that fail,
     * which is why they go into a local array rather than into the letter
     * alone: the letter borrows these bytes and does not own them.
     */
    char *held[RECON_SMTP_ATTACHMENTS_MAX];
    memset(held, 0, sizeof(held));
    bool read_them_all = true;

    for (int i = 0; i < m->attached_count && read_them_all; i++) {
        size_t size = 0;
        held[i] = recon_fs_read("/", m->attached[i], &size);
        if (held[i] == NULL) {
            set_message(m, true, "'%s' could not be read: %s",
                name_of(m->attached[i]), recon_fs_last_error());
            read_them_all = false;
            break;
        }

        letter.attachment[i].bytes = (const unsigned char *)held[i];
        letter.attachment[i].size = size;
        snprintf(letter.attachment[i].name,
            sizeof(letter.attachment[i].name), "%s",
            name_of(m->attached[i]));
        letter.attachment_count = i + 1;
    }

    #define LET_GO() do { \
        for (int k = 0; k < RECON_SMTP_ATTACHMENTS_MAX; k++) { \
            free(held[k]); \
        } \
    } while (0)

    if (!read_them_all) {
        LET_GO();
        return;
    }

    /*
     * Checked here as well as inside recon_smtp_send, so the reason appears
     * beside the field somebody is still looking at rather than as the result
     * of a connection that was never made.
     */
    char why[192];
    if (!recon_smtp_letter_ok(&letter, why, sizeof(why))) {
        set_message(m, true, "%s", why);
        LET_GO();
        return;
    }

    if (m->password.text[0] == '\0') {
        set_message(m, true, "The password is needed to send as well as to "
            "read. Change the account and sign in again.");
        LET_GO();
        return;
    }

    stop_sending(m);

    /*
     * The same password as reading, which is right for nearly every provider
     * and is stated rather than assumed: a separate sending password is a
     * seventh field that is usually a copy of one already filled in, and a
     * field like that is filled in wrong.
     */
    m->sender = recon_smtp_send(&m->sending, m->password.text, &letter,
        &SEND_HANDLERS, m);

    /*
     * Released here whether it worked or not. recon_smtp_send composes the
     * whole message into a buffer of its own before it returns, so by this
     * point nothing it holds points back at these bytes -- which is the fact
     * that makes borrowing them safe, and is worth saying because it is the
     * only reason this free is not a use-after-free.
     */
    LET_GO();
    #undef LET_GO

    if (m->sender == NULL) {
        set_message(m, true, "%s", recon_smtp_last_error());
        return;
    }
    m->sending_now = true;
}

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

    recon_smtp_account_get(&m->sending);
    if (m->sending.port <= 0) {
        m->sending.port = RECON_SMTP_TLS_PORT;
    }
    char send_port[16];
    snprintf(send_port, sizeof(send_port), "%d", m->sending.port);

    recon_edit_begin(&m->fields[FIELD_HOST], m->account.host, false);
    recon_edit_begin(&m->fields[FIELD_USER], m->account.user, false);
    recon_edit_begin(&m->fields[FIELD_PORT], port, false);
    recon_edit_begin(&m->fields[FIELD_SEND_HOST], m->sending.host, false);
    recon_edit_begin(&m->fields[FIELD_SEND_PORT], send_port, false);
    recon_edit_begin(&m->fields[FIELD_FROM], m->sending.from, false);

    /*
     * `active` set directly, and *not* recon_edit_end.
     *
     * recon_edit_end means "done, throw it away" and clears the text -- which
     * is right for a rename somebody escaped out of, and wrong for a form
     * field that is merely not the one with the caret in it. Using it here
     * emptied the port and username the moment the form was built, and the
     * form looked exactly like a form nobody had filled in.
     */
    for (int i = 0; i < FIELD_COUNT; i++) {
        m->fields[i].active = (i == FIELD_HOST);
    }
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

    /*
     * The sending half, and it is allowed to be empty.
     *
     * Somebody who only wants to read their mail should not be stopped at a
     * form asking for a server they do not have. Leaving it blank means Write
     * says why it cannot, which is a better place to find out than a form that
     * will not let you past.
     */
    const char *send_host = m->fields[FIELD_SEND_HOST].text;
    const char *from = m->fields[FIELD_FROM].text;

    if (send_host[0] != '\0') {
        if (strlen(send_host) >= sizeof(m->sending.host) ||
                strlen(from) >= sizeof(m->sending.from)) {
            set_message(m, true, "That sending server or address is too long "
                "to save.");
            return;
        }

        memcpy(m->sending.host, send_host, strlen(send_host) + 1);
        memcpy(m->sending.from, from, strlen(from) + 1);
        /* The same username as reading, because for nearly every provider it
         * is -- and a seventh field that is usually a copy of the second is a
         * field people fill in wrong. */
        memcpy(m->sending.user, m->account.user, strlen(m->account.user) + 1);

        m->sending.port = atoi(m->fields[FIELD_SEND_PORT].text);
        if (m->sending.port <= 0 || m->sending.port > 65535) {
            m->sending.port = m->sending.security == RECON_SMTP_STARTTLS
                ? RECON_SMTP_STARTTLS_PORT : RECON_SMTP_TLS_PORT;
        }
        recon_smtp_account_set(&m->sending);
    } else {
        recon_smtp_account_clear();
        memset(&m->sending, 0, sizeof(m->sending));
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
        "Sending server", "Sending port", "Your address",
    };

    for (int i = 0; i < FIELD_COUNT; i++) {
        /*
         * A gap and a heading where the form changes subject. Six fields in
         * one column is a form somebody fills in from top to bottom without
         * noticing that halfway down it stopped being about the same server.
         */
        if (i == FIELD_SEND_HOST) {
            y += 6;
            recon_draw_text(p, m->font, x, y + ascent, w,
                "Sending, which is a different server", COLOR_TEXT);
            y += line + 2;
            recon_draw_text(p, m->font, x, y + ascent, w,
                "Leave blank to only read mail. Encrypted from the first "
                "byte, so port 465 rather than 587.", COLOR_DIM);
            y += line + 6;
        }

        /* Wide enough for "Sending server", which the 90 this used to be cut
         * to "Sending ser...". A label that does not fit is a form asking a
         * question it did not finish. */
        recon_draw_text(p, m->font, x, y + ascent, 112, LABELS[i], COLOR_TEXT);

        int fx = x + 118;
        int fw = (i == FIELD_PORT || i == FIELD_SEND_PORT)
            ? 80 : w - 118 - PADDING;
        recon_edit_draw(p, m->font, fx, y, fw, FIELD_HEIGHT, &m->fields[i]);
        recon_hit_add(p, fx, y, fw, FIELD_HEIGHT, HIT_FIELD_BASE + i);
        y += FIELD_HEIGHT + 6;
    }

    /*
     * How the sending connection gets encrypted, as one control saying what it
     * is rather than a port number deciding it. Guessing from the port would
     * mean a typed number silently choosing how carefully the connection is
     * made, and the two are not equally safe to get wrong.
     */
    recon_draw_text(p, m->font, x, y + ascent, 112, "Encryption", COLOR_TEXT);
    draw_button(m, p, x + 118, y - 2,
        m->sending.security == RECON_SMTP_STARTTLS
            ? "STARTTLS -- plain, then upgraded (587)"
            : "TLS from the first byte (465)",
        HIT_SECURITY, true);
    y += BUTTON_HEIGHT + 4;

    recon_draw_text(p, m->font, x, y + ascent, w,
        "STARTTLS is required to succeed: a server that does not offer it "
        "ends the session.", COLOR_DIM);
    y += line + PADDING;

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

    /*
     * Writing without connecting first.
     *
     * Sending and reading are different servers, and requiring a successful
     * connection to one before a letter can be handed to the other is a
     * coupling with nothing behind it -- a mail server being down should not
     * stop somebody writing. The password is the only thing sending needs from
     * this screen, and it is on it.
     */
    struct recon_smtp_account sending;
    bool can_write = recon_smtp_account_get(&sending) &&
        sending.host[0] != '\0' && m->password.text[0] != '\0';
    bx = draw_button(m, p, bx, y, "Write a letter", HIT_WRITE, can_write);

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

/* --- Writing one --- */

/*
 * How tall the body box is, in lines.
 *
 * Whatever is left after the two header fields and the buttons, which is the
 * right answer for a box whose whole job is to hold as much as it can.
 */
static void draw_compose(struct recon_mailwin *m, struct recon_panel *p,
        int x, int y, int w, int h) {
    int ascent = recon_font_ascent(m->font);
    int line = recon_font_line_height(m->font);
    int bottom = y + h;

    recon_draw_text(p, m->font, x, y + ascent, w, "Write a letter", COLOR_TEXT);
    y += line + 2;

    char from[512];
    snprintf(from, sizeof(from), "From %s, through %s", m->sending.from,
        m->sending.host);
    recon_draw_text(p, m->font, x, y + ascent, w, from, COLOR_DIM);
    y += line + PADDING;

    /*
     * Every field above the body, in the order they are typed. Four rows now
     * rather than two, which is four rows the body does not get -- but a Bcc
     * that has to be found somewhere is a Bcc nobody uses, and this window
     * exists to be usable rather than to be tall.
     */
    static const char *const LABELS[4] = { "To", "Cc", "Bcc", "Subject" };
    for (int i = 0; i < 4; i++) {
        recon_draw_text(p, m->font, x, y + ascent, 70, LABELS[i], COLOR_TEXT);
        recon_edit_draw(p, m->font, x + 76, y, w - 76 - PADDING, FIELD_HEIGHT,
            &m->compose[i]);
        recon_hit_add(p, x + 76, y, w - 76 - PADDING, FIELD_HEIGHT,
            HIT_COMPOSE_BASE + i);
        y += FIELD_HEIGHT + 6;
    }

    /*
     * Said once, beside the field, rather than assumed known. The difference
     * between Cc and Bcc is the whole reason both exist and it is not visible
     * from the labels.
     */
    recon_draw_text(p, m->font, x, y + ascent, w,
        "Everyone above receives it. Only To and Cc are named in the letter.",
        COLOR_DIM);
    y += line + 4;

    /*
     * What is attached, one per line, each with its own way of being taken off
     * again.
     *
     * Named rather than counted. "3 files attached" is a thing somebody has to
     * open something else to check, and the moment worth catching is the one
     * where the wrong file is on the list.
     */
    for (int i = 0; i < m->attached_count; i++) {
        char shown[RECON_PATH_MAX + 32];
        snprintf(shown, sizeof(shown), "%s", name_of(m->attached[i]));

        int remove_w = recon_text_width(m->font, "Remove") + 16;
        int remove_x = x + w - PADDING - remove_w;

        recon_draw_text(p, m->font, x + 4, y + ascent, remove_w > 0
            ? remove_x - x - 12 : w, shown, COLOR_TEXT);

        recon_fill_rect(p, remove_x, y - 1, remove_w, line + 2, COLOR_PANEL);
        recon_draw_button_edge(p, remove_x, y - 1, remove_w, line + 2, false,
            COLOR_BG);
        recon_draw_text(p, m->font, remove_x + 8, y + ascent, remove_w - 8,
            "Remove", COLOR_TEXT);
        recon_hit_add(p, remove_x, y - 1, remove_w, line + 2,
            HIT_ATTACHED_BASE + (uint32_t)i);

        y += line + 4;
    }

    /* The buttons are placed from the bottom, so the body gets the rest. */
    int buttons_y = bottom - BUTTON_HEIGHT;
    int box_h = buttons_y - PADDING - y;
    if (box_h < line * 3) {
        box_h = line * 3;
    }

    /*
     * The body, drawn line by line rather than by recon_edit_draw -- which
     * draws one line and scrolls it sideways, which is right for a filename
     * and useless for a letter.
     *
     * No wrapping. A long line runs off the right edge and is still there;
     * wrapping it would mean deciding where words break and then mapping the
     * caret through that, which is a text engine rather than a text box. What
     * is here is enough to type a letter with newlines in it, and it says so
     * by simply not pretending otherwise.
     */
    recon_fill_rect(p, x, y, w - PADDING, box_h, COLOR_FIELD);
    recon_draw_bevel(p, x, y, w - PADDING, box_h, true);
    recon_hit_add(p, x, y, w - PADDING, box_h,
        HIT_COMPOSE_BASE + COMPOSE_BODY);

    const struct recon_edit *body = &m->compose[COMPOSE_BODY];
    int ty = y + 3;
    int caret_x = x + 4;
    int caret_y = ty;
    int at = 0;

    while (ty + line <= y + box_h) {
        int end = at;
        while (body->text[end] != '\0' && body->text[end] != '\n') {
            end++;
        }

        char one[RECON_EDIT_MAX];
        int length = end - at;
        if (length > (int)sizeof(one) - 1) {
            length = (int)sizeof(one) - 1;
        }
        memcpy(one, body->text + at, (size_t)length);
        one[length] = '\0';

        recon_draw_text(p, m->font, x + 4, ty + ascent, w - PADDING - 8, one,
            COLOR_FIELD_TEXT);

        /* Where the caret is, found while walking the same lines rather than
         * by a second pass that could disagree with this one. */
        if (body->caret >= at && body->caret <= end) {
            char upto[RECON_EDIT_MAX];
            int n = body->caret - at;
            if (n > (int)sizeof(upto) - 1) {
                n = (int)sizeof(upto) - 1;
            }
            memcpy(upto, body->text + at, (size_t)n);
            upto[n] = '\0';
            caret_x = x + 4 + recon_text_width(m->font, upto);
            caret_y = ty;
        }

        if (body->text[end] == '\0') {
            break;
        }
        at = end + 1;
        ty += line;
    }

    if (body->active) {
        recon_fill_rect(p, caret_x, caret_y + 1, 1, line - 2, THEME(CARET));
    }

    int bx = draw_button(m, p, x, buttons_y,
        m->sending_now ? "Sending..." : "Send", HIT_SEND, !m->sending_now);
    bx = draw_button(m, p, bx, buttons_y, "Discard", HIT_DISCARD,
        !m->sending_now);

    /* Greyed at the limit rather than removed, so the reason the button will
     * not respond is the button itself saying so. */
    draw_button(m, p, bx, buttons_y, "Attach a file", HIT_ATTACH,
        !m->sending_now &&
        m->attached_count < RECON_SMTP_ATTACHMENTS_MAX);
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
    bx = draw_button(m, p, bx, y, "Write", HIT_WRITE, true);
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
    case SCREEN_COMPOSE:
        draw_compose(m, p, inner_x, inner_y, inner_w, inner_h);
        break;
    }

    /*
     * The file picker over everything, last, so its hit regions win.
     *
     * Drawn over the whole content area rather than beside the letter: while
     * it is up it is the only thing that can be used, and covering what is
     * behind it is what makes that obvious rather than something to find out
     * by clicking.
     */
    if (recon_filedlg_is_open(&m->dialog)) {
        recon_filedlg_draw(&m->dialog, p, m->font, x, y, w, h);
    }

    (void)line;
}

/* --- Input --- */

static void open_selected(struct recon_mailwin *m);

/*
 * Put the chosen file on the list.
 *
 * Not read, and not checked for size beyond the count -- both of those happen
 * at Send, against the file as it is then. What IS checked here is that the
 * same file is not attached twice: two parts with the same name is a message
 * that looks corrupt to whoever opens it, and it is far more often a double
 * click than an intention.
 */
static void attach_chosen(struct recon_mailwin *m) {
    const char *path = recon_filedlg_path(&m->dialog);
    if (path == NULL || *path == '\0') {
        return;
    }

    if (m->attached_count >= RECON_SMTP_ATTACHMENTS_MAX) {
        set_message(m, true, "One letter can carry %d files.",
            RECON_SMTP_ATTACHMENTS_MAX);
        return;
    }

    for (int i = 0; i < m->attached_count; i++) {
        if (strcmp(m->attached[i], path) == 0) {
            set_message(m, false, "'%s' is already attached.", name_of(path));
            return;
        }
    }

    snprintf(m->attached[m->attached_count],
        sizeof(m->attached[m->attached_count]), "%s", path);
    m->attached_count++;
    set_message(m, false, "'%s' will go with the letter.", name_of(path));
}

static bool mailwin_click(void *user, uint32_t hit, int cx, int cy,
        bool pressed) {
    struct recon_mailwin *m = user;
    (void)cx;
    (void)cy;

    if (!pressed || hit < RECON_APPWIN_HIT_USER) {
        return false;
    }

    /* The file dialog takes every click while it is up, including the ones
     * that miss it: the window behind is not usable until it is answered. */
    if (recon_filedlg_is_open(&m->dialog)) {
        if (recon_filedlg_click(&m->dialog, hit) == RECON_FILEDLG_ACCEPTED) {
            attach_chosen(m);
        }
        recon_appwin_refresh(m->win);
        return true;
    }

    /* Descending, because the ladders below are unbounded. */
    /*
     * The Remove buttons, then the compose fields, then the rows. All three
     * for one reason: HIT_ROW_BASE below is an open-ended `>=` and both of the
     * others are numbered above it. Clicking the To field used to be read as
     * clicking message row one hundred, and the text went to whichever field
     * had the caret. Bounded here and unbounded there, so the order is what
     * keeps them apart.
     */
    if (hit >= HIT_ATTACHED_BASE &&
            hit < HIT_ATTACHED_BASE + RECON_SMTP_ATTACHMENTS_MAX) {
        int i = (int)(hit - HIT_ATTACHED_BASE);
        if (i >= 0 && i < m->attached_count) {
            set_message(m, false, "'%s' is no longer attached.",
                name_of(m->attached[i]));
            for (int j = i; j + 1 < m->attached_count; j++) {
                memcpy(m->attached[j], m->attached[j + 1],
                    sizeof(m->attached[j]));
            }
            m->attached_count--;
            m->attached[m->attached_count][0] = '\0';
        }
        recon_appwin_refresh(m->win);
        return true;
    }

    if (hit >= HIT_COMPOSE_BASE && hit < HIT_COMPOSE_BASE + COMPOSE_COUNT) {
        int i = (int)(hit - HIT_COMPOSE_BASE);
        for (int j = 0; j < COMPOSE_COUNT; j++) {
            m->compose[j].active = (j == i);
        }
        m->compose_focused = i;
        recon_appwin_refresh(m->win);
        return true;
    }

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
    case HIT_SECURITY: {
        /* The port moves with it, unless it has been changed to something
         * that is neither default -- the same rule the reading protocol
         * toggle follows. */
        bool was_starttls = m->sending.security == RECON_SMTP_STARTTLS;
        int old_default = was_starttls
            ? RECON_SMTP_STARTTLS_PORT : RECON_SMTP_TLS_PORT;
        int typed = atoi(m->fields[FIELD_SEND_PORT].text);

        m->sending.security = was_starttls
            ? RECON_SMTP_TLS : RECON_SMTP_STARTTLS;
        int new_default = was_starttls
            ? RECON_SMTP_TLS_PORT : RECON_SMTP_STARTTLS_PORT;

        if (typed == old_default || typed == 0) {
            char port[16];
            snprintf(port, sizeof(port), "%d", new_default);
            recon_edit_begin(&m->fields[FIELD_SEND_PORT], port, false);
            m->fields[FIELD_SEND_PORT].active =
                (m->focused == FIELD_SEND_PORT);
        }
        recon_appwin_refresh(m->win);
        return true;
    }

    case HIT_WRITE:
        start_writing(m);
        recon_appwin_refresh(m->win);
        return true;

    case HIT_SEND:
        send_now(m);
        recon_appwin_refresh(m->win);
        return true;

    case HIT_ATTACH:
        recon_filedlg_open(&m->dialog, RECON_FILEDLG_OPEN, "Attach a file",
            NULL, NULL);
        recon_appwin_refresh(m->win);
        return true;

    case HIT_DISCARD:
        /*
         * Straight back, with no "are you sure".
         *
         * Asking would be right if this were the only copy of something that
         * took a while to write, and it is not the only copy of anything --
         * nothing has been sent, nothing was on disk, and the letter existed
         * only in this window. What it costs is retyping, which is the same
         * thing a question costs when the answer is yes.
         */
        stop_sending(m);
        m->screen = SCREEN_MAIL;
        m->message[0] = '\0';
        recon_appwin_refresh(m->win);
        return true;

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

    /* The dialog first, and it takes everything -- including the keys it does
     * not use. Typing into the letter behind a picker that is waiting for an
     * answer is how somebody sends a filename to their aunt. */
    if (recon_filedlg_is_open(&m->dialog)) {
        if (recon_filedlg_key(&m->dialog, sym, modifiers) ==
                RECON_FILEDLG_ACCEPTED) {
            attach_chosen(m);
        }
        recon_appwin_refresh(m->win);
        return true;
    }

    if (m->screen == SCREEN_SETUP) {
        struct recon_edit *edit = &m->fields[m->focused];

        if (sym == XKB_KEY_Tab) {
            edit->active = false;
            m->focused = (m->focused + 1) % FIELD_COUNT;
            /*
             * recon_edit_focus, not recon_edit_begin with the field's own
             * text. That form aliases snprintf's source and destination, which
             * is undefined and here emptied the field -- so every default on
             * this form was lost to being tabbed past. BG-112.
             */
            recon_edit_focus(&m->fields[m->focused]);
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

    if (m->screen == SCREEN_COMPOSE) {
        struct recon_edit *edit = &m->compose[m->compose_focused];

        if (sym == XKB_KEY_Tab) {
            edit->active = false;
            m->compose_focused = (m->compose_focused + 1) % COMPOSE_COUNT;
            /*
             * `active` set directly rather than recon_edit_begin, which
             * selects the text so the next key replaces it. Tabbing into a
             * half-written letter and typing one character should not delete
             * the letter -- that is right for a rename and wrong for a field
             * somebody is coming back to.
             */
            m->compose[m->compose_focused].active = true;
            recon_appwin_refresh(m->win);
            return true;
        }

        switch (recon_edit_key(edit, sym, modifiers)) {
        case RECON_EDIT_COMMIT:
            /*
             * Enter in To or Subject moves on rather than sending. Sending is
             * a button, deliberately: a letter should not leave because
             * somebody finished typing an address and pressed the key they
             * press at the end of every line.
             */
            edit->active = false;
            m->compose_focused = (m->compose_focused + 1) % COMPOSE_COUNT;
            m->compose[m->compose_focused].active = true;
            recon_appwin_refresh(m->win);
            return true;
        case RECON_EDIT_CHANGED:
        case RECON_EDIT_CANCEL:
            recon_appwin_refresh(m->win);
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
    /* A send in flight holds a pointer to this window and will call back into
     * it. Closed first, so the callback cannot arrive after the memory is
     * gone. */
    stop_sending(m);
    /* The password was in this memory. Erased rather than memset, because a
     * memset nothing reads afterwards is one the compiler may delete. */
    recon_secure_erase(m, sizeof(*m));
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
