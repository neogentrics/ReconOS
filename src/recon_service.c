/*
 * Services. See include/recon_service.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ReconOS.h"
#include "recon_error.h"
#include "recon_service.h"

struct service_slot {
    struct recon_service_impl impl;
    void *user;
    struct recon_service_info info;
    bool used;
};

static struct service_slot g_services[RECON_SERVICES_MAX];
static int g_count;
static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_service_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

const char *recon_service_state_name(enum recon_service_state state) {
    switch (state) {
    case RECON_SERVICE_RUNNING: return "running";
    case RECON_SERVICE_STOPPED: return "stopped";
    case RECON_SERVICE_FAILED:  return "failed";
    default:                    return "?";
    }
}

/* --- The list --- */

bool recon_service_register(const struct recon_service_impl *impl, void *user,
        bool running) {
    if (impl == NULL || impl->name == NULL || impl->start == NULL ||
            impl->stop == NULL) {
        set_error("a service needs a name and both operations");
        return false;
    }
    if (g_count >= RECON_SERVICES_MAX) {
        set_error("there is no room for another service");
        return false;
    }

    struct service_slot *slot = &g_services[g_count];
    memset(slot, 0, sizeof(*slot));

    slot->impl = *impl;
    slot->user = user;
    slot->used = true;

    recon_text_copy(slot->info.name, sizeof(slot->info.name), impl->name);
    recon_text_copy(slot->info.detail, sizeof(slot->info.detail),
        impl->detail != NULL ? impl->detail : "");
    slot->info.essential = impl->essential;
    slot->info.state = running ? RECON_SERVICE_RUNNING : RECON_SERVICE_STOPPED;
    slot->info.starts = running ? 1 : 0;

    g_count++;
    return true;
}

int recon_service_count(void) {
    return g_count;
}

bool recon_service_at(int index, struct recon_service_info *out) {
    if (index < 0 || index >= g_count || !g_services[index].used) {
        return false;
    }
    if (out != NULL) {
        *out = g_services[index].info;
    }
    return true;
}

static struct service_slot *slot_for(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    for (int i = 0; i < g_count; i++) {
        if (g_services[i].used &&
                strcasecmp(g_services[i].info.name, name) == 0) {
            return &g_services[i];
        }
    }
    return NULL;
}

bool recon_service_find(const char *name, struct recon_service_info *out) {
    struct service_slot *slot = slot_for(name);
    if (slot == NULL) {
        return false;
    }
    if (out != NULL) {
        *out = slot->info;
    }
    return true;
}

/* --- Starting and stopping --- */

bool recon_service_start(const char *name) {
    struct service_slot *slot = slot_for(name);
    if (slot == NULL) {
        set_error("there is no service called '%s'", name != NULL ? name : "");
        return false;
    }

    if (slot->info.state == RECON_SERVICE_RUNNING) {
        /* Not an error. Somebody asking for a running service to start has
         * got what they wanted, and saying so as a failure would send them
         * looking for a problem that is not there. */
        return true;
    }

    if (!slot->impl.start(slot->user)) {
        slot->info.state = RECON_SERVICE_FAILED;
        set_error("'%s' would not start", slot->info.name);
        return false;
    }

    slot->info.state = RECON_SERVICE_RUNNING;
    slot->info.problem[0] = '\0';
    slot->info.starts++;
    return true;
}

bool recon_service_stop(const char *name) {
    struct service_slot *slot = slot_for(name);
    if (slot == NULL) {
        set_error("there is no service called '%s'", name != NULL ? name : "");
        return false;
    }

    if (slot->info.essential) {
        /*
         * Refused rather than done. There is no state of the system in which
         * having no desktop shell is what somebody wanted, and a Stop that
         * left them looking at a blank screen with no taskbar would be a way
         * to lose the machine.
         */
        set_error("'%s' cannot be stopped, only restarted", slot->info.name);
        return false;
    }

    if (slot->info.state != RECON_SERVICE_RUNNING) {
        return true;
    }

    slot->impl.stop(slot->user);
    slot->info.state = RECON_SERVICE_STOPPED;
    return true;
}

bool recon_service_restart(const char *name) {
    struct service_slot *slot = slot_for(name);
    if (slot == NULL) {
        set_error("there is no service called '%s'", name != NULL ? name : "");
        return false;
    }

    /*
     * Stopped through the implementation rather than through
     * recon_service_stop, which refuses for an essential service -- and
     * restarting an essential service is exactly what this is for.
     */
    if (slot->info.state == RECON_SERVICE_RUNNING) {
        slot->impl.stop(slot->user);
        slot->info.state = RECON_SERVICE_STOPPED;
    }

    if (!slot->impl.start(slot->user)) {
        slot->info.state = RECON_SERVICE_FAILED;
        recon_text_copy(slot->info.problem, sizeof(slot->info.problem),
            "VT-A006");
        set_error("'%s' stopped and would not start again", slot->info.name);
        return false;
    }

    slot->info.state = RECON_SERVICE_RUNNING;
    slot->info.problem[0] = '\0';
    slot->info.starts++;
    return true;
}
