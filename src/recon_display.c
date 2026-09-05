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

#include <libdrm/drm_fourcc.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "ReconOS.h"
#include "recon_display.h"
#include "recon_server.h"

static struct recon_server *g_server;
static char g_error[192];

/*
 * Which id belongs to which display.
 *
 * Ids are handed out on first sight and remembered against the display's
 * name, which is the only thing about an output that is stable and unique --
 * a pointer is reused after an unplug, and a position changes when a
 * neighbour goes away. A monitor unplugged and plugged back in gets the id it
 * had, which is what somebody who set a resolution on it would expect.
 *
 * Small and fixed: a machine with more than sixteen displays is not the
 * machine this is being written for, and the seventeenth simply has no id
 * rather than corrupting the sixteen that do.
 */
#define DISPLAYS_MAX 16

static struct {
    char name[RECON_DISPLAY_NAME_MAX];
    int id;
} g_ids[DISPLAYS_MAX];

static int g_next_id = 1;   /* Never zero: zero means "none". */

void recon_display_init(void *backend) {
    g_server = backend;
}

const char *recon_display_last_error(void) {
    return g_error;
}

/* The id for a display, assigning one if this is the first time it is seen. */
static int id_for(const char *name) {
    if (name == NULL || *name == '\0') {
        return 0;
    }

    for (int i = 0; i < DISPLAYS_MAX; i++) {
        if (g_ids[i].id != 0 && strcmp(g_ids[i].name, name) == 0) {
            return g_ids[i].id;
        }
    }

    for (int i = 0; i < DISPLAYS_MAX; i++) {
        if (g_ids[i].id == 0) {
            snprintf(g_ids[i].name, sizeof(g_ids[i].name), "%s", name);
            g_ids[i].id = g_next_id++;
            return g_ids[i].id;
        }
    }

    /* Full. Better than reusing an id, which would silently make one display
     * answer for another. */
    return 0;
}

/* The output with this id, or NULL. */
static struct wlr_output *output_with_id(int id) {
    if (g_server == NULL || g_server->output_layout == NULL || id <= 0) {
        return NULL;
    }

    struct wlr_output_layout_output *entry;
    wl_list_for_each(entry, &g_server->output_layout->outputs, link) {
        if (entry->output != NULL && entry->output->name != NULL &&
                id_for(entry->output->name) == id) {
            return entry->output;
        }
    }
    return NULL;
}

/* The nth output in the layout, for walking. */
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

/*
 * A DRM fourcc as something this system has a name for.
 *
 * Only the two ReconOS could actually draw into are named; everything else is
 * OTHER rather than being guessed at. A format nobody can use is not more
 * useful for having been translated into a longer word.
 */
static enum recon_display_format format_of(uint32_t fourcc) {
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888: return RECON_DISPLAY_FORMAT_ARGB8888;
    case DRM_FORMAT_XRGB8888: return RECON_DISPLAY_FORMAT_XRGB8888;
    case DRM_FORMAT_INVALID:  return RECON_DISPLAY_FORMAT_UNKNOWN;
    default:                  return RECON_DISPLAY_FORMAT_OTHER;
    }
}

bool recon_display_at(int index, struct recon_display *out) {
    struct wlr_output *output = output_at(index);
    if (output == NULL || out == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s",
        output->name != NULL ? output->name : "display");
    out->id = id_for(out->name);

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

bool recon_display_mode_at(int id, int index,
        struct recon_display_mode *out) {
    struct wlr_output *output = output_with_id(id);
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

        /* The output's, not the mode's: wlroots does not vary format per
         * mode. The header says so rather than leaving it to be found. */
        out->format = format_of(output->render_format);
        return true;
    }
    return false;
}

bool recon_display_set_mode(int id, int mode_index, char *why_out,
        size_t why_size) {
    if (why_out != NULL && why_size > 0) {
        why_out[0] = '\0';
    }

    struct wlr_output *output = output_with_id(id);
    if (output == NULL) {
        snprintf(g_error, sizeof(g_error),
            "that display is not attached any more");
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
