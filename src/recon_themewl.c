/*
 * Telling Wayland clients what the desktop looks like.
 *
 * The server half of protocol/recon-theme.xml, which has the reasoning in it.
 * Short version: applications that ship with ReconOS read recon_theme directly
 * because they are in this process, and one that arrives later cannot. Without
 * being told, a client either guesses, ships its own theme and stops matching
 * the moment somebody changes the skin, or reads the registry behind the
 * compositor's back -- which is not a boundary at all.
 *
 * This file is the boundary. It knows about wayland and about recon_theme and
 * about nothing else, which is why it is not inside recon_theme.c: that file is
 * linked by the skin tests, deliberately, so a palette can be measured without
 * a compositor.
 */

#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>

#include "ReconOS.h"
#include "recon_theme.h"
#include "recon_themewl.h"

#include "recon-theme-protocol.h"

/* Bumped when the protocol gains something. The XML's version and this must
 * agree, and the compiler cannot check that, so it is stated in both places
 * rather than derived in one. */
#define RECON_THEME_PROTOCOL_VERSION 1

/*
 * Everything currently listening.
 *
 * A list rather than a count, because a change has to reach all of them and
 * there is no other way to find them. Each entry lives as long as its resource
 * does; the destructor takes it out.
 */
static struct wl_list g_listeners;
static bool g_started;

struct listener {
    struct wl_resource *resource;
    struct wl_list link;
};

/* --- Sending --- */

static void send_everything(struct wl_resource *resource) {
    /*
     * The whole palette in one array, in role order.
     *
     * Built on the stack each time rather than kept, because a skin can be
     * edited a colour at a time and a cached copy would be one edit behind
     * exactly when somebody is watching for the change.
     */
    uint32_t colours[RECON_THEME_ROLE_COUNT];
    for (int i = 0; i < RECON_THEME_ROLE_COUNT; i++) {
        colours[i] = recon_theme_color((enum recon_theme_role)i);
    }

    struct wl_array palette;
    wl_array_init(&palette);
    void *room = wl_array_add(&palette, sizeof(colours));
    if (room != NULL) {
        memcpy(room, colours, sizeof(colours));
        recon_theme_v1_send_palette(resource, &palette);
    }
    wl_array_release(&palette);

    int32_t metrics[RECON_METRIC_COUNT];
    for (int i = 0; i < RECON_METRIC_COUNT; i++) {
        metrics[i] = (int32_t)recon_theme_metric((enum recon_theme_metric)i);
    }

    struct wl_array numbers;
    wl_array_init(&numbers);
    room = wl_array_add(&numbers, sizeof(metrics));
    if (room != NULL) {
        memcpy(room, metrics, sizeof(metrics));
        recon_theme_v1_send_metrics(resource, &numbers);
    }
    wl_array_release(&numbers);

    const char *name = recon_theme_current();
    recon_theme_v1_send_name(resource, name != NULL ? name : "");

    /* Last, and it is what a client should redraw on. The three above arrive
     * together and redrawing on each is a visible flicker. */
    recon_theme_v1_send_done(resource);
}

void recon_themewl_changed(void) {
    if (!g_started) {
        return;
    }

    struct listener *l;
    wl_list_for_each(l, &g_listeners, link) {
        send_everything(l->resource);
    }
}

/* --- The theme object --- */

static void theme_destroy(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct recon_theme_v1_interface THEME_IMPL = {
    .destroy = theme_destroy,
};

static void theme_resource_destroy(struct wl_resource *resource) {
    struct listener *l = wl_resource_get_user_data(resource);
    if (l == NULL) {
        return;
    }
    wl_list_remove(&l->link);
    free(l);
}

/* --- The manager --- */

static void manager_get_theme(struct wl_client *client,
        struct wl_resource *resource, uint32_t id) {
    struct wl_resource *theme = wl_resource_create(client,
        &recon_theme_v1_interface, wl_resource_get_version(resource), id);
    if (theme == NULL) {
        wl_client_post_no_memory(client);
        return;
    }

    struct listener *l = calloc(1, sizeof(*l));
    if (l == NULL) {
        wl_resource_destroy(theme);
        wl_client_post_no_memory(client);
        return;
    }
    l->resource = theme;
    wl_list_insert(&g_listeners, &l->link);

    wl_resource_set_implementation(theme, &THEME_IMPL, l,
        theme_resource_destroy);

    /*
     * Sent straight away, before anything has changed.
     *
     * So a client can draw its first frame from this rather than from a guess
     * it then corrects. A protocol that only spoke on changes would leave every
     * client wrong until somebody edited a skin.
     */
    send_everything(theme);
}

static void manager_destroy(struct wl_client *client,
        struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct recon_theme_manager_v1_interface MANAGER_IMPL = {
    .destroy = manager_destroy,
    .get_theme = manager_get_theme,
};

static void manager_bind(struct wl_client *client, void *data,
        uint32_t version, uint32_t id) {
    (void)data;
    struct wl_resource *resource = wl_resource_create(client,
        &recon_theme_manager_v1_interface, (int)version, id);
    if (resource == NULL) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &MANAGER_IMPL, NULL, NULL);
}

bool recon_themewl_init(struct wl_display *display) {
    if (display == NULL) {
        return false;
    }

    wl_list_init(&g_listeners);
    g_started = true;

    return wl_global_create(display, &recon_theme_manager_v1_interface,
        RECON_THEME_PROTOCOL_VERSION, NULL, manager_bind) != NULL;
}
