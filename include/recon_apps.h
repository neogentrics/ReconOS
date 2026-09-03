/*
 * The running application table.
 *
 * ReconOS needs to know what is running as *applications*, which is not the
 * same question as what processes exist. Several built-in applications share
 * ReconOS's own process, so no process list could ever show them separately;
 * a client program has a process of its own but may own several windows, and
 * listing its windows is not listing the program.
 *
 * This is the list the task manager works from, and the thing that makes
 * "end this application" mean something specific for each kind:
 *
 *   built-in   its windows are closed and its state reset. There is no
 *              process to kill -- killing it would take ReconOS with it.
 *   client     asked to close through its own window first, so it can save.
 *              If it does not answer, it can be terminated, and then killed.
 *
 * An application that has been asked to close and has not answered within a
 * few seconds is reported as not responding, which is the only honest basis
 * for offering to force it.
 */

#ifndef RECON_APPS_H
#define RECON_APPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct recon_server;
struct recon_appwin;
struct recon_toplevel;

/* How long an unanswered close request waits before the application is called
 * not responding. Long enough that a program saving a file is not accused of
 * hanging; short enough to be useful. */
#define RECON_APP_UNRESPONSIVE_MS 5000

enum recon_app_kind {
    /* Part of ReconOS, sharing its process. */
    RECON_APP_KIND_BUILTIN,
    /* A separate program connected over Wayland. */
    RECON_APP_KIND_CLIENT,
};

enum recon_app_state {
    RECON_APP_RUNNING,
    RECON_APP_MINIMIZED,
    /* Asked to close and not answering. */
    RECON_APP_NOT_RESPONDING,
};

struct recon_app_info {
    /* Stable for as long as the application runs, so a selection in the task
     * manager survives the list being rebuilt underneath it. */
    uint32_t id;
    char name[96];
    enum recon_app_kind kind;
    enum recon_app_state state;

    /* 0 for built-in applications: they have no process of their own, and
     * showing ReconOS's pid against each of them would invite someone to
     * kill it. */
    pid_t pid;

    /* Memory in KB, or 0 where it cannot be attributed to this application
     * alone. */
    size_t memory_kb;

    bool close_requested;
};

void recon_apps_init(struct recon_server *server);
void recon_apps_finish(void);

/* --- Registration --- */

void recon_apps_add_builtin(struct recon_appwin *win);
void recon_apps_add_client(struct recon_toplevel *toplevel);
void recon_apps_remove_client(struct recon_toplevel *toplevel);

/* --- Listing --- */

/*
 * Rebuild the list from what is actually running and return how many there
 * are. Call before reading, so the answer is current rather than remembered.
 */
int recon_apps_refresh(void);

int recon_apps_count(void);
bool recon_apps_at(int index, struct recon_app_info *out);
bool recon_apps_find(uint32_t id, struct recon_app_info *out);

/* --- Ending them --- */

/*
 * Ask an application to close. A built-in closes its window; a client is sent
 * a close request it may decline or delay, and the wait starts here.
 */
bool recon_apps_end(uint32_t id);

/*
 * Stop it without asking. Only meaningful for a client, and only offered once
 * it has been asked and has not answered -- a force button that appears
 * before the polite one has been tried is a force button people press first.
 */
bool recon_apps_force_end(uint32_t id);

/* Whether forcing is a thing that could work on this application. */
bool recon_apps_can_force(uint32_t id);

#endif
