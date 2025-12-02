#define WLR_USE_UNSTABLE
#define _POSIX_C_SOURCE 200112L
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* --- STRUCTS --- */

struct recon_server {
    struct wl_display* wl_display;
    struct wlr_backend* backend;
    struct wlr_renderer* renderer;
    struct wlr_allocator* allocator;

    struct wlr_seat* seat;
    struct wlr_output_layout* output_layout; /* NEW: Manages screen positions */
    struct wlr_cursor* cursor;               /* NEW: Represents the mouse */
    struct wlr_xcursor_manager* cursor_mgr;  /* NEW: Draws the cursor icon */

    struct wl_listener new_output;
    struct wl_listener new_input;
    struct wl_listener cursor_motion;        /* NEW: Listen for mouse movement */
    struct wl_listener cursor_motion_absolute;
    struct wl_listener request_cursor;
};

struct recon_output {
    struct wlr_output* wlr_output;
    struct recon_server* server;
    struct wl_listener frame;
    struct wl_listener destroy;
};

struct recon_keyboard {
    struct wlr_keyboard* wlr_keyboard;
    struct recon_server* server;
    struct wl_listener modifiers;
    struct wl_listener key;
    struct wl_listener destroy;
};

/* --- MOUSE HANDLING (NEW) --- */

static void server_cursor_motion(struct wl_listener* listener, void* data) {
    struct recon_server* server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event* event = data;

    /* Move the cursor relative to its current position */
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);

    /* Tell the system "Hey, something moved, please redraw!" */
    /* (This part is simplified for now) */
}

static void server_cursor_motion_absolute(struct wl_listener* listener, void* data) {
    struct recon_server* server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event* event = data;
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
}

/* --- KEYBOARD HANDLING --- */

static void server_keyboard_key(struct wl_listener* listener, void* data) {
    struct recon_keyboard* keyboard = wl_container_of(listener, keyboard, key);
    struct wlr_keyboard_key_event* event = data;

    /* Check for ALT + Q to Quit */
    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t* syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state, keycode, &syms);

    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
        for (int i = 0; i < nsyms; i++) {
            if ((modifiers & WLR_MODIFIER_ALT) && syms[i] == XKB_KEY_q) {
                printf("ReconOS: ALT+Q pressed. Shutting down...\n");
                wl_display_terminate(keyboard->server->wl_display);
            }
        }
    }
}

static void server_keyboard_modifiers(struct wl_listener* listener, void* data) {
    struct recon_keyboard* keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->wlr_keyboard->modifiers);
}

static void server_new_keyboard(struct recon_server* server, struct wlr_input_device* device) {
    struct recon_keyboard* keyboard = calloc(1, sizeof(struct recon_keyboard));
    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard_from_input_device(device);

    struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap* keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(keyboard->wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    keyboard->key.notify = server_keyboard_key;
    wl_signal_add(&keyboard->wlr_keyboard->events.key, &keyboard->key);
    keyboard->modifiers.notify = server_keyboard_modifiers;
    wl_signal_add(&keyboard->wlr_keyboard->events.modifiers, &keyboard->modifiers);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
}

static void server_new_input(struct wl_listener* listener, void* data) {
    struct recon_server* server = wl_container_of(listener, server, new_input);
    struct wlr_input_device* device = data;

    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        server_new_keyboard(server, device);
    }
    else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        /* NEW: Attach mouse to our cursor system */
        wlr_cursor_attach_input_device(server->cursor, device);
        printf("ReconOS: Mouse Attached!\n");
    }
}

/* --- RENDERING --- */

static void output_frame(struct wl_listener* listener, void* data) {
    struct recon_output* output = wl_container_of(listener, output, frame);
    struct wlr_renderer* renderer = output->server->renderer;

    if (!wlr_output_attach_render(output->wlr_output, NULL)) return;

    int width, height;
    wlr_output_effective_resolution(output->wlr_output, &width, &height);
    wlr_renderer_begin(renderer, width, height);

    /* 1. Paint Background (Blue) */
    float color[4] = { 0.1f, 0.1f, 0.3f, 1.0f };
    wlr_renderer_clear(renderer, color);

    /* 2. Paint Cursor (Hardware Cursor logic is handled by wlroots backend usually,
       but we ensure software fallback happens if needed) */
       /* Note: For simplicity, wlr_cursor handles setting the image on the output. */

    wlr_renderer_end(renderer);
    wlr_output_commit(output->wlr_output);
    wlr_output_schedule_frame(output->wlr_output);
}

static void output_destroy(struct wl_listener* listener, void* data) {
    struct recon_output* output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    free(output);
}

static void server_new_output(struct wl_listener* listener, void* data) {
    struct recon_server* server = wl_container_of(listener, server, new_output);
    struct wlr_output* wlr_output = data;
    struct recon_output* output = calloc(1, sizeof(struct recon_output));
    output->wlr_output = wlr_output;
    output->server = server;

    /* Initialize Renderer */
    if (!wlr_output_init_render(wlr_output, server->allocator, server->renderer)) {
        return;
    }

    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    /* Resolution Setup */
    if (!wl_list_empty(&wlr_output->modes)) {
        struct wlr_output_mode* mode = wlr_output_preferred_mode(wlr_output);
        wlr_output_set_mode(wlr_output, mode);
        wlr_output_enable(wlr_output, true);
        wlr_output_commit(wlr_output);
    }
    else {
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        wlr_output_state_set_custom_mode(&state, 1024, 768, 60000);
        wlr_output_commit_state(wlr_output, &state);
        wlr_output_state_finish(&state);
    }

    /* NEW: Add this screen to the Layout Manager */
    wlr_output_layout_add_auto(server->output_layout, wlr_output);

    /* Start Loop */
    wlr_output_schedule_frame(output->wlr_output);
}

/* --- MAIN --- */

int main(int argc, char** argv) {
    wlr_log_init(WLR_DEBUG, NULL);
    struct recon_server server;

    server.wl_display = wl_display_create();
    server.backend = wlr_backend_autocreate(server.wl_display, NULL);
    server.renderer = wlr_renderer_autocreate(server.backend);
    wlr_renderer_init_wl_display(server.renderer, server.wl_display);
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);

    /* NEW: Layout & Cursor Setup */
    server.output_layout = wlr_output_layout_create(server.wl_display);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

    server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    wlr_xcursor_manager_load(server.cursor_mgr, 1);

    /* Set the default cursor icon (Left Pointer) */
    wlr_xcursor_manager_set_cursor_image(server.cursor_mgr, "left_ptr", server.cursor);

    /* Cursor Events */
    server.cursor_motion.notify = server_cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);

    /* Seat & Input */
    server.seat = wlr_seat_create(server.wl_display, "seat0");
    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);
    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    if (!wlr_backend_start(server.backend)) {
        wl_display_destroy(server.wl_display);
        return 1;
    }

    printf("ReconOS 0.2 Initialized. Move your mouse!\n");
    wl_display_run(server.wl_display);

    wl_display_destroy_clients(server.wl_display);
    wl_display_destroy(server.wl_display);
    return 0;
}