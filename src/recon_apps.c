/*
 * The running application table. See include/recon_apps.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <wayland-server-core.h>

#include "recon_apps.h"
#include "recon_appwin.h"
#include "recon_procinfo.h"
#include "recon_server.h"
#include "recon_shell.h"

#define APPS_MAX 64

/*
 * What is known about one application, including the parts that must survive
 * a refresh: the id it was given, and when it was asked to close.
 *
 * Keyed by the window or toplevel pointer rather than by position, because
 * the list is rebuilt constantly and position means nothing across rebuilds.
 */
struct app_entry {
    uint32_t id;
    enum recon_app_kind kind;
    struct recon_appwin *window;
    struct recon_toplevel *toplevel;

    char name[96];
    pid_t pid;

    /* When close was asked for, in milliseconds since ReconOS started. Zero
     * means it has not been asked. */
    uint64_t close_requested_ms;

    bool present; /* seen during the last refresh */
};

static struct recon_server *g_server;
static struct app_entry g_apps[APPS_MAX];
static int g_count;
static uint32_t g_next_id = 1;

/* Milliseconds since some fixed point. Monotonic, so a clock change cannot
 * make an application look unresponsive. */
static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void recon_apps_init(struct recon_server *server) {
    g_server = server;
    g_count = 0;
    g_next_id = 1;
}

void recon_apps_finish(void) {
    g_server = NULL;
    g_count = 0;
}

/* --- Finding entries --- */

static struct app_entry *entry_for_window(struct recon_appwin *win) {
    for (int i = 0; i < g_count; i++) {
        if (g_apps[i].window == win) {
            return &g_apps[i];
        }
    }
    return NULL;
}

static struct app_entry *entry_for_toplevel(struct recon_toplevel *toplevel) {
    for (int i = 0; i < g_count; i++) {
        if (g_apps[i].toplevel == toplevel) {
            return &g_apps[i];
        }
    }
    return NULL;
}

static struct app_entry *entry_for_id(uint32_t id) {
    for (int i = 0; i < g_count; i++) {
        if (g_apps[i].id == id) {
            return &g_apps[i];
        }
    }
    return NULL;
}

static struct app_entry *new_entry(void) {
    if (g_count >= APPS_MAX) {
        return NULL;
    }
    struct app_entry *entry = &g_apps[g_count++];
    memset(entry, 0, sizeof(*entry));
    entry->id = g_next_id++;
    return entry;
}

/* --- Registration --- */

void recon_apps_add_builtin(struct recon_appwin *win) {
    if (win == NULL || entry_for_window(win) != NULL) {
        return;
    }
    struct app_entry *entry = new_entry();
    if (entry == NULL) {
        return;
    }
    entry->kind = RECON_APP_KIND_BUILTIN;
    entry->window = win;
    snprintf(entry->name, sizeof(entry->name), "%s", recon_appwin_title(win));
}

void recon_apps_add_client(struct recon_toplevel *toplevel) {
    if (toplevel == NULL || entry_for_toplevel(toplevel) != NULL) {
        return;
    }
    struct app_entry *entry = new_entry();
    if (entry == NULL) {
        return;
    }
    entry->kind = RECON_APP_KIND_CLIENT;
    entry->toplevel = toplevel;
    entry->pid = (pid_t)recon_toplevel_pid(toplevel);
    snprintf(entry->name, sizeof(entry->name), "%s", recon_toplevel_title(toplevel));
}

void recon_apps_remove_client(struct recon_toplevel *toplevel) {
    for (int i = 0; i < g_count; i++) {
        if (g_apps[i].toplevel != toplevel) {
            continue;
        }
        /* Order does not matter, so the last entry fills the gap. */
        g_apps[i] = g_apps[g_count - 1];
        g_count--;
        return;
    }
}

/* --- Listing --- */

int recon_apps_refresh(void) {
    if (g_server == NULL) {
        return 0;
    }

    for (int i = 0; i < g_count; i++) {
        g_apps[i].present = false;
    }

    /*
     * Built-in applications are listed while their window is open. They are
     * not removed when closed -- an application that is not running is not in
     * the table, which is the same rule a client follows.
     */
    struct recon_shell *shell = g_server->shell;
    int builtin = recon_shell_app_count(shell);
    for (int i = 0; i < builtin; i++) {
        struct recon_appwin *win = recon_shell_app_at(shell, i);
        if (!recon_appwin_is_open(win)) {
            continue;
        }

        struct app_entry *entry = entry_for_window(win);
        if (entry == NULL) {
            recon_apps_add_builtin(win);
            entry = entry_for_window(win);
        }
        if (entry != NULL) {
            entry->present = true;
            snprintf(entry->name, sizeof(entry->name), "%s",
                recon_appwin_title(win));
        }
    }

    struct recon_toplevel *toplevel;
    wl_list_for_each(toplevel, &g_server->toplevels, link) {
        struct app_entry *entry = entry_for_toplevel(toplevel);
        if (entry == NULL) {
            recon_apps_add_client(toplevel);
            entry = entry_for_toplevel(toplevel);
        }
        if (entry != NULL) {
            entry->present = true;
            snprintf(entry->name, sizeof(entry->name), "%s",
                recon_toplevel_title(toplevel));
            entry->pid = (pid_t)recon_toplevel_pid(toplevel);
        }
    }

    /* Drop what is no longer running. Walked backwards so removing an entry
     * cannot skip the one that takes its place. */
    for (int i = g_count - 1; i >= 0; i--) {
        if (!g_apps[i].present) {
            g_apps[i] = g_apps[g_count - 1];
            g_count--;
        }
    }

    return g_count;
}

int recon_apps_count(void) {
    return g_count;
}

/* Fill in the parts that are worked out rather than stored. */
static void describe(const struct app_entry *entry, struct recon_app_info *out) {
    memset(out, 0, sizeof(*out));
    out->id = entry->id;
    out->kind = entry->kind;
    out->pid = entry->pid;
    out->close_requested = entry->close_requested_ms != 0;
    snprintf(out->name, sizeof(out->name), "%s", entry->name);

    /*
     * What a built-in application costs, rather than nothing at all.
     *
     * These share ReconOS's process, so no per-application figure can be read
     * from the system. The window's own pixel buffer can: it is real, belongs
     * to exactly this application, and changes when the window is resized.
     * The column used to say "in ReconOS", which was true and answered a
     * different question than the one the column asks.
     *
     * A client's memory is filled in by whoever has the process list, since
     * that is where a pid can be looked up.
     */
    if (entry->kind == RECON_APP_KIND_BUILTIN) {
        out->memory_kb = recon_appwin_memory_kb(entry->window);
    }

    bool minimized = (entry->kind == RECON_APP_KIND_BUILTIN)
        ? recon_appwin_is_minimized(entry->window)
        : recon_toplevel_is_minimized(entry->toplevel);

    /*
     * Not responding is only claimed about something that was asked to close
     * and did not. Guessing it from anything else -- a slow redraw, say --
     * would put the word in front of users who have done nothing wrong.
     */
    if (entry->close_requested_ms != 0 &&
            now_ms() - entry->close_requested_ms > RECON_APP_UNRESPONSIVE_MS) {
        out->state = RECON_APP_NOT_RESPONDING;
    } else if (minimized) {
        out->state = RECON_APP_MINIMIZED;
    } else {
        out->state = RECON_APP_RUNNING;
    }
}

bool recon_apps_at(int index, struct recon_app_info *out) {
    if (index < 0 || index >= g_count || out == NULL) {
        return false;
    }
    describe(&g_apps[index], out);
    return true;
}

bool recon_apps_find(uint32_t id, struct recon_app_info *out) {
    struct app_entry *entry = entry_for_id(id);
    if (entry == NULL || out == NULL) {
        return false;
    }
    describe(entry, out);
    return true;
}

/* --- Ending them --- */

bool recon_apps_end(uint32_t id) {
    struct app_entry *entry = entry_for_id(id);
    if (entry == NULL) {
        return false;
    }

    if (entry->kind == RECON_APP_KIND_BUILTIN) {
        /*
         * Closing the window is the whole of it. There is no process to end:
         * a built-in shares ReconOS's, and killing that would take the
         * desktop down with the application.
         */
        recon_appwin_hide(entry->window);
        return true;
    }

    /*
     * Ask through the window, so the program can save first or decline. The
     * clock starts here; if it has not gone in a few seconds it is reported
     * as not responding and forcing becomes available.
     */
    recon_toplevel_close(entry->toplevel);
    if (entry->close_requested_ms == 0) {
        entry->close_requested_ms = now_ms();
    }
    return true;
}

bool recon_apps_can_force(uint32_t id) {
    struct app_entry *entry = entry_for_id(id);
    if (entry == NULL || entry->kind != RECON_APP_KIND_CLIENT) {
        return false;
    }
    /* Only after asking. A force button offered first is a force button
     * pressed first, and the polite close is the one that lets work be
     * saved. */
    return entry->pid > 0 && entry->close_requested_ms != 0;
}

bool recon_apps_force_end(uint32_t id) {
    struct app_entry *entry = entry_for_id(id);
    if (entry == NULL || entry->kind != RECON_APP_KIND_CLIENT || entry->pid <= 0) {
        return false;
    }

    /*
     * Terminate first, which a program can still handle, and only then kill.
     * Going straight to kill would lose work in a program that was merely
     * slow to answer.
     */
    if (recon_proc_terminate(entry->pid)) {
        return true;
    }
    return recon_proc_kill(entry->pid);
}
