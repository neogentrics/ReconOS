/*
 * Which of an application's callbacks a window transition fires.
 *
 * --- Why this file exists ---
 *
 * v0.4.74 and v0.4.75 both turned on one distinction: `closed` fires when a
 * window is closed, and `visibility(false)` fires for **close and minimize
 * alike**. Getting that backwards is not a small bug. The browser would sign
 * you out of everything every time you put the window down; the player would
 * stop the music.
 *
 * Both times I wrote that the distinction could not be checked in a suite --
 * that it needed a window and a server, so a live run was the only instrument.
 * **That was wrong, and it was wrong the second time after being repeated
 * rather than rechecked.** `struct recon_server` is opaque to `recon_appwin.c`
 * and only ever tested against NULL; `struct recon_panel` never leaves its
 * hands. So the whole file links against forty-four stubs and a dummy pointer,
 * and the transitions run for real.
 *
 * --- What the stubs are and are not ---
 *
 * They are not a compositor. They are the smallest thing that lets the real
 * `recon_appwin.c` run: they answer plausibly and record nothing, because
 * **nothing here is a claim about them.** Every assertion in this file is
 * about which of *the application's own* callbacks were reached, in what
 * order, and that is decided entirely by code in `recon_appwin.c`.
 *
 * Showing a window draws it, so the drawing calls are reached and have to be
 * harmless rather than tripwires. That is the one place a stub does real work:
 * `recon_server_window_panel` hands back a pointer that is never dereferenced
 * by anything, here or there.
 *
 * --- What this still cannot see ---
 *
 * That `recon_apps.c` calls `recon_appwin_hide` when somebody clicks the X,
 * and that a browser or a player is wired to the hook at all. Those stay with
 * `scripts/cookie-survives-shot.sh` and `scripts/player-close-shot.sh`, which
 * drive a real desktop. This file owns the half underneath them: given a
 * close, which callbacks run.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_error.h"
#include "recon_help.h"
#include "recon_icons.h"
#include "recon_registry.h"
#include "recon_server_facts.h"
#include "recon_shell.h"
#include "recon_theme.h"
#include "recon_titlebar.h"
#include "recon_ui.h"
#include "recon_widget.h"

static int g_checks;
static int g_failures;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_text(const char *got, const char *want, const char *what) {
    g_checks++;
    if (got == NULL || strcmp(got, want) != 0) {
        g_failures++;
        printf("  FAIL: %s\n    wanted: \"%s\"\n    got:    \"%s\"\n", what,
            want, got != NULL ? got : "(nothing)");
    }
}

/* --- What the application saw ------------------------------------------ */

/*
 * A log rather than a set of counters.
 *
 * "Was `closed` called" is the smaller question. The one that matters is
 * *what else happened around it* -- `visibility(false)` has to fire on a close
 * too, and it has to fire first, so an application that stops work in one and
 * puts things away in the other is not asked to put things away while it is
 * still running.
 */
#define LOG_MAX 32

static char g_log[LOG_MAX][24];
static int g_log_count;

static void note(const char *what) {
    if (g_log_count < LOG_MAX) {
        snprintf(g_log[g_log_count], sizeof(g_log[0]), "%s", what);
    }
    g_log_count++;
}

static void log_clear(void) {
    g_log_count = 0;
}

/* The log as one line, which is what a failure should print: an order is
 * easier to read wrong than to see wrong. */
static const char *log_text(void) {
    static char out[LOG_MAX * 24];
    out[0] = '\0';
    for (int i = 0; i < g_log_count && i < LOG_MAX; i++) {
        if (i > 0) {
            strncat(out, " ", sizeof(out) - strlen(out) - 1);
        }
        strncat(out, g_log[i], sizeof(out) - strlen(out) - 1);
    }
    return out;
}

static void on_visibility(void *user, bool visible) {
    (void)user;
    note(visible ? "visible" : "hidden");
}

static void on_closed(void *user) {
    (void)user;
    note("closed");
}

static void on_draw(void *user, struct recon_panel *panel, int x, int y,
        int w, int h) {
    (void)user; (void)panel; (void)x; (void)y; (void)w; (void)h;
    note("draw");
}

/* --- The tests --------------------------------------------------------- */

static struct recon_server *fake_server(void) {
    /*
     * `recon_appwin.c` tests this for NULL and stores it, and every use of it
     * goes through a function this file provides. So the address of something
     * is enough, and a null pointer is not -- create refuses one.
     */
    static int nothing;
    return (struct recon_server *)&nothing;
}

static void test_a_close_says_so_and_a_minimize_does_not(void) {
    printf("closing fires closed; minimizing fires only visibility\n");

    static const struct recon_appwin_impl IMPL = {
        .title = "Test",
        .help = "",
        .draw = on_draw,
        .visibility = on_visibility,
        .closed = on_closed,
    };

    struct recon_appwin *win =
        recon_appwin_create(fake_server(), NULL, &IMPL, NULL);
    check(win != NULL, "a window can be made without a compositor");
    if (win == NULL) {
        return;
    }

    log_clear();
    recon_appwin_show(win);
    check(g_log_count > 0 && strcmp(g_log[0], "visible") == 0,
        "opening says visible");

    /*
     * Minimize. **This is the assertion both fixes turn on.**
     *
     * `visibility(false)` fires, because from the point of view of "should I
     * still be decoding this video" a minimized window is the same as a closed
     * one. `closed` must not, because from the point of view of "is this
     * session over" they are nothing alike.
     */
    log_clear();
    recon_appwin_minimize(win);
    check_text(log_text(), "hidden",
        "MINIMIZE SAYS HIDDEN AND NOTHING ELSE -- a `closed` here signs you "
        "out of a browser and stops the music every time a window is put down");

    log_clear();
    recon_appwin_restore(win);
    check(g_log_count > 0 && strcmp(g_log[0], "visible") == 0,
        "restoring says visible again");

    /*
     * And a close, which is both: stop working, and the session is over.
     * In that order, so an application is not asked to put things away while
     * it still believes it is running.
     */
    log_clear();
    recon_appwin_hide(win);
    check_text(log_text(), "hidden closed",
        "CLOSING SAYS BOTH, HIDDEN FIRST");

    recon_appwin_destroy(win);
}

static void test_a_transition_that_changes_nothing_says_nothing(void) {
    printf("and a transition that does not happen fires nothing\n");

    static const struct recon_appwin_impl IMPL = {
        .title = "Test",
        .help = "",
        .draw = on_draw,
        .visibility = on_visibility,
        .closed = on_closed,
    };

    struct recon_appwin *win =
        recon_appwin_create(fake_server(), NULL, &IMPL, NULL);
    if (win == NULL) {
        check(false, "a window to work with");
        return;
    }

    /*
     * Closing a closed window is what "Show Desktop" and the taskbar can both
     * arrange, and an application that heard two closes would seal its jar
     * twice or, worse, act on the second as though something had changed.
     */
    log_clear();
    recon_appwin_hide(win);
    check_text(log_text(), "",
        "a window that was never shown is not closed");

    recon_appwin_show(win);
    log_clear();
    recon_appwin_hide(win);
    check_text(log_text(), "hidden closed", "the first close says so");

    log_clear();
    recon_appwin_hide(win);
    check_text(log_text(), "", "AND THE SECOND SAYS NOTHING");

    /* The same for minimize, from a window that is already minimized. */
    recon_appwin_show(win);
    recon_appwin_minimize(win);
    log_clear();
    recon_appwin_minimize(win);
    check_text(log_text(), "", "minimizing twice says it once");

    recon_appwin_destroy(win);
}

static void test_an_application_need_not_want_either(void) {
    printf("an impl that implements neither is not a crash\n");

    /*
     * Eight of the ten built-in applications implement neither hook, so the
     * NULL checks around both are the common path rather than the careful one.
     */
    static const struct recon_appwin_impl BARE = {
        .title = "Bare",
        .help = "",
    };

    struct recon_appwin *win =
        recon_appwin_create(fake_server(), NULL, &BARE, NULL);
    check(win != NULL, "a window with no callbacks at all");
    if (win == NULL) {
        return;
    }

    log_clear();
    recon_appwin_show(win);
    recon_appwin_minimize(win);
    recon_appwin_restore(win);
    recon_appwin_hide(win);
    check_text(log_text(), "", "and it goes through every transition quietly");

    recon_appwin_destroy(win);
}

static void test_closed_without_visibility(void) {
    printf("and one that wants only the close gets only the close\n");

    /*
     * Which is the shape the browser and the player both use: they do not care
     * about minimize at all, so they implement `closed` and leave `visibility`
     * NULL. If the two were ever collapsed back into one callback, this is the
     * test that would notice.
     */
    static const struct recon_appwin_impl ONLY_CLOSED = {
        .title = "Only closed",
        .help = "",
        .closed = on_closed,
    };

    struct recon_appwin *win =
        recon_appwin_create(fake_server(), NULL, &ONLY_CLOSED, NULL);
    if (win == NULL) {
        check(false, "a window to work with");
        return;
    }

    recon_appwin_show(win);

    log_clear();
    recon_appwin_minimize(win);
    check_text(log_text(), "", "minimize reaches it not at all");

    recon_appwin_restore(win);
    log_clear();
    recon_appwin_hide(win);
    check_text(log_text(), "closed", "and a close reaches it once");

    recon_appwin_destroy(win);
}

int main(void) {
    printf("\n--- what a window tells an application ---\n\n");

    test_a_close_says_so_and_a_minimize_does_not();
    test_a_transition_that_changes_nothing_says_nothing();
    test_an_application_need_not_want_either();
    test_closed_without_visibility();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
