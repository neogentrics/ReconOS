/*
 * The screens. See include/recon_display.h.
 *
 * The one place in ReconOS that asks wlroots what a display can do. Everything
 * above calls this file, so the day the answer comes from ReconOS's own kernel
 * instead, the change is here and nowhere else.
 *
 * Modes are walked out of the output layout rather than out of `recon_output`,
 * which is private to main.c and has nothing in it this needs. The layout
 * knows every output that is up, which is exactly the question.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "ReconOS.h"
#include "recon_display.h"
#include "recon_server.h"

static struct recon_server *g_server;
static char g_error[192];

void recon_display_init(struct recon_server *server) {
    g_server = server;
}

const char *recon_display_last_error(void) {
    return g_error;
}

/* The nth output in the layout, or NULL. */
static struct wlr_output *output_at(int index) {
    if (g_server == NULL || g_server->output_layout == NULL || index < 0) {
        return NULL;
    }

    int seen = 0;
    struct wlr_output_layout_output *entry;
    wl_list_for_each(entry, &g_server->output_layout->outputs, link) {
        if (seen++ == index) {
            return entry->output;
        }
    }
    return NULL;
}

int recon_display_count(void) {
    if (g_server == NULL || g_server->output_layout == NULL) {
        return 0;
    }

    int count = 0;
    struct wlr_output_layout_output *entry;
    wl_list_for_each(entry, &g_server->output_layout->outputs, link) {
        (void)entry;
        count++;
    }
    return count;
}

bool recon_display_at(int index, struct recon_display *out) {
    struct wlr_output *output = output_at(index);
    if (output == NULL || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s",
        output->name != NULL ? output->name : "display");

    out->width = output->width;
    out->height = output->height;
    out->refresh = output->refresh;

    struct wlr_output_mode *mode;
    wl_list_for_each(mode, &output->modes, link) {
        (void)mode;
        out->mode_count++;
    }

    /*
     * A display with no modes is one whose size is somebody else's decision.
     *
     * That is every nested and headless backend: ReconOS running in a window
     * on another compositor is whatever size that window is. Reporting it as
     * changeable would put a control on the page that could only ever fail.
     */
    out->can_change = out->mode_count > 0;
    return true;
}

bool recon_display_mode_at(int display, int index,
        struct recon_display_mode *out) {
    struct wlr_output *output = output_at(display);
    if (output == NULL || out == NULL || index < 0) {
        return false;
    }

    int seen = 0;
    struct wlr_output_mode *mode;
    wl_list_for_each(mode, &output->modes, link) {
        if (seen++ != index) {
            continue;
        }

        memset(out, 0, sizeof(*out));
        out->width = mode->width;
        out->height = mode->height;
        out->refresh = mode->refresh;
        out->preferred = mode->preferred;
        out->current = (output->current_mode == mode);
        return true;
    }
    return false;
}

bool recon_display_set_mode(int display, int mode_index, char *why_out,
        size_t why_size) {
    if (why_out != NULL && why_size > 0) {
        why_out[0] = '\0';
    }

    struct wlr_output *output = output_at(display);
    if (output == NULL) {
        snprintf(g_error, sizeof(g_error), "there is no display %d",
            display + 1);
        goto refused;
    }

    if (wl_list_empty(&output->modes)) {
        /*
         * Said as what it is rather than as a failure. ReconOS in a window on
         * another compositor is not a machine refusing to change resolution;
         * it is a window, and the thing to change is the window.
         */
        snprintf(g_error, sizeof(g_error),
            "'%s' takes its size from what it is running inside, so there is "
            "nothing here to change", output->name);
        goto refused;
    }

    int seen = 0;
    struct wlr_output_mode *mode;
    struct wlr_output_mode *wanted = NULL;
    wl_list_for_each(mode, &output->modes, link) {
        if (seen++ == mode_index) {
            wanted = mode;
            break;
        }
    }

    if (wanted == NULL) {
        snprintf(g_error, sizeof(g_error), "'%s' has no such size",
            output->name);
        goto refused;
    }

    /*
     * Built as a state and committed, rather than set and hoped for. A commit
     * that fails leaves the display exactly as it was, which is the behaviour
     * wanted here: a resolution that cannot be shown should not take the
     * screen away from somebody who then cannot put it back.
     */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_mode(&state, wanted);

    bool ok = wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);

    if (!ok) {
        snprintf(g_error, sizeof(g_error),
            "'%s' refused %d by %d -- it is still as it was", output->name,
            wanted->width, wanted->height);
        goto refused;
    }

    wlr_log(WLR_INFO, "ReconOS: '%s' is now %d by %d", output->name,
        wanted->width, wanted->height);
    return true;

refused:
    if (why_out != NULL && why_size > 0) {
        snprintf(why_out, why_size, "%s", g_error);
    }
    return false;
}
