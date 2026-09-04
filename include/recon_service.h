/*
 * Services: the parts of ReconOS that run, and can be stopped and started.
 *
 * Not a process table. ReconOS has no processes of its own -- when it launches
 * something it asks Linux to make the process -- and Watchtower already has a
 * tab showing the machine's. A service here is a *subsystem of ReconOS*: the
 * desktop shell, the control socket, the network listener, the firewall. Each
 * has a state, and each can be turned off and on again without restarting the
 * whole system.
 *
 * --- Why this exists ---
 *
 * The desktop shell owns the taskbar, the desktop, window management and the
 * File Explorer's frame. It is the thing everything else stands on, and until
 * now the only way to recover it from a bad state was to restart ReconOS --
 * which closes every window and signs everybody out to fix a taskbar.
 *
 * Every system that has this has it for the same reason, and calls it the same
 * thing: something you can restart on its own when it has gone wrong.
 *
 * --- What a restart means ---
 *
 * Stopping a service takes down what it built. Starting one builds it again
 * from settings, not from whatever it had in memory -- so a restart is a
 * repair rather than a way of preserving a broken state more carefully.
 *
 * For the shell that has a consequence worth stating: **application windows
 * survive it.** They are not the shell's to destroy; the application registry
 * owns them, and the shell borrows them to draw a taskbar and route clicks. A
 * shell restart puts back the taskbar, the desktop and the menus, and the
 * Notepad somebody was typing in is still open with their text in it.
 *
 * --- Essential services ---
 *
 * Some cannot be stopped, only restarted: there is no state of the system in
 * which having no desktop shell is what somebody wanted. Offering a Stop
 * button that leaves a person looking at a blank screen with no taskbar and no
 * menu would be offering a way to lose the machine. Restart is the operation
 * that was actually wanted.
 */

#ifndef RECON_SERVICE_H
#define RECON_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#define RECON_SERVICES_MAX 16
#define RECON_SERVICE_NAME_MAX 40

enum recon_service_state {
    /* Built and doing its job. */
    RECON_SERVICE_RUNNING,
    /* Deliberately not running. */
    RECON_SERVICE_STOPPED,
    /*
     * Asked to start and would not.
     *
     * Distinct from stopped, and the distinction is the point: stopped is a
     * decision and failed is a problem, and a list that showed them the same
     * way would hide every fault behind something that looks intentional.
     */
    RECON_SERVICE_FAILED,
};

struct recon_service_info {
    char name[RECON_SERVICE_NAME_MAX];
    /* What it does, in a few words. */
    char detail[128];
    enum recon_service_state state;
    /* Restart only; see the note above. */
    bool essential;
    /* The error code from the last failure, or "" -- so a failed service says
     * which fault it was rather than only that there was one. */
    char problem[16];
    /* How many times it has been started, this run. One is a normal system;
     * more than one means somebody has been repairing something. */
    int starts;
};

/*
 * What a service has to be able to do.
 *
 * `start` returns false when it could not, and is expected to have raised its
 * own error code by then -- the registry records the failure, and the service
 * is the only thing that knows what went wrong.
 *
 * `stop` cannot fail. There is no useful answer to "I could not stop", and a
 * service that refuses to stop is a service nobody can repair.
 */
struct recon_service_impl {
    const char *name;
    const char *detail;
    bool essential;
    bool (*start)(void *user);
    void (*stop)(void *user);
};

/*
 * Add a service. `running` says whether it is already up at the moment of
 * registering, which for most of them it is: they are built during startup and
 * registered afterwards, and claiming they were stopped would make the list
 * wrong from the first frame.
 */
bool recon_service_register(const struct recon_service_impl *impl, void *user,
    bool running);

int recon_service_count(void);
bool recon_service_at(int index, struct recon_service_info *out);

/* By name, case-insensitively, because this is a name people type. */
bool recon_service_find(const char *name, struct recon_service_info *out);

bool recon_service_start(const char *name);
bool recon_service_stop(const char *name);

/*
 * Stop and start again. The only operation offered for an essential service,
 * and the one that is almost always meant for the others too.
 *
 * A service that was not running is simply started, because "restart" on
 * something that is off means the same thing to everybody who has ever asked
 * for it.
 */
bool recon_service_restart(const char *name);

const char *recon_service_state_name(enum recon_service_state state);
const char *recon_service_last_error(void);

#endif
