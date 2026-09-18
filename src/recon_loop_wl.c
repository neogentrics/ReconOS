/*
 * Waiting, when there is a compositor to wait on. See include/recon_loop.h.
 *
 * The other implementation of the seam, and the argument that the seam is in
 * the right place -- the same argument `src/recon_ui_fb.c` made for the
 * screen: *if it had needed the other half to change, the seam had been drawn
 * around one implementation.* It needed nothing.
 *
 * This is the file that does not build without Linux, and it is the only one
 * of the pair that does not. Everything above it -- six files and eight
 * thousand lines of session, player, photos, task manager, clock and audio --
 * now says `recon_timer_after` and means it wherever it runs.
 *
 * --- The mapping is close because wayland's shape was the right one ---
 *
 * One-shot timers, re-armed by the caller, with zero as "disarm". That is not
 * a coincidence and it is not this file bending: `include/recon_loop.h` was
 * written to what the six callers already did, and what they already did was
 * wayland's. A seam that had to translate between two different ideas of a
 * timer would have been a seam with a bug in it.
 */

#include <stdlib.h>

#include "recon_loop_wl.h"

/*
 * A loop here is wayland's, unwrapped -- `recon_loop_from_wl` is in
 * `include/recon_loop_wl.h`, and inline, for a reason that header gives.
 *
 * No allocation and no struct of our own: `struct recon_loop *` and
 * `struct wl_event_loop *` are the same pointer, cast at the boundary. The
 * alternative -- a wrapper holding one pointer -- would be an allocation that
 * can fail, on a path where failing means the desktop cannot wait for
 * anything, to store something already in hand.
 */

/*
 * A timer and a watch, though, do need one: wayland's callbacks have their own
 * shapes, and this holds the caller's beside the source so the trampoline can
 * find it.
 */
struct wl_timer {
    struct wl_event_source *source;
    recon_timer_fn fn;
    void *user;
};

struct wl_watch {
    struct wl_event_source *source;
    recon_watch_fn fn;
    void *user;
    int fd;
};

/*
 * The two directions of the same table.
 *
 * Written out rather than assumed equal. They happen to line up bit for bit
 * today and that is a coincidence of two libraries choosing the same order --
 * a seam whose whole point is that the other side can be replaced should not
 * rest on it.
 */
static uint32_t to_wl(unsigned events) {
    uint32_t mask = 0;

    if ((events & RECON_WATCH_READABLE) != 0) {
        mask |= WL_EVENT_READABLE;
    }
    if ((events & RECON_WATCH_WRITABLE) != 0) {
        mask |= WL_EVENT_WRITABLE;
    }
    /* Hangup and error are not asked for. wayland reports them regardless and
     * refuses a request for them, so naming them here would turn a caller
     * saying something already true into a watch that could not be made. */
    return mask;
}

static unsigned from_wl(uint32_t mask) {
    unsigned events = 0;

    if ((mask & WL_EVENT_READABLE) != 0) {
        events |= RECON_WATCH_READABLE;
    }
    if ((mask & WL_EVENT_WRITABLE) != 0) {
        events |= RECON_WATCH_WRITABLE;
    }
    if ((mask & WL_EVENT_HANGUP) != 0) {
        events |= RECON_WATCH_HANGUP;
    }
    if ((mask & WL_EVENT_ERROR) != 0) {
        events |= RECON_WATCH_ERROR;
    }
    return events;
}

static int timer_fired(void *data) {
    struct wl_timer *t = data;

    /*
     * Wayland disarms a timer before calling this, which is the behaviour
     * `recon_loop.h` promises and the reason a callback may re-arm itself.
     */
    t->fn(t->user);
    return 0;
}

struct recon_timer *recon_timer_create(struct recon_loop *loop,
        recon_timer_fn fn, void *user) {
    if (loop == NULL || fn == NULL) {
        return NULL;
    }

    struct wl_timer *t = calloc(1, sizeof(*t));

    if (t == NULL) {
        return NULL;
    }
    t->fn = fn;
    t->user = user;
    t->source = wl_event_loop_add_timer((struct wl_event_loop *)loop,
        timer_fired, t);

    if (t->source == NULL) {
        free(t);
        return NULL;
    }
    return (struct recon_timer *)t;
}

void recon_timer_after(struct recon_timer *timer, int milliseconds) {
    struct wl_timer *t = (struct wl_timer *)timer;

    if (t == NULL) {
        return;
    }
    /* Negative disarms here too, matching src/recon_loop.c. wayland would
     * take it as an enormous unsigned delay, which is "never" by accident
     * rather than on purpose. */
    wl_event_source_timer_update(t->source, milliseconds > 0 ? milliseconds : 0);
}

/*
 * Whether it is waiting.
 *
 * Tracked nowhere, because wayland does not say and the one honest answer
 * this file can give is the one below. Nothing in the desktop asks: the
 * function exists for `src/recon_loop.c`'s own tests, where the state is
 * visible. Said out loud rather than returning a guess.
 */
int recon_timer_is_armed(const struct recon_timer *timer) {
    (void)timer;
    return -1;
}

void recon_timer_destroy(struct recon_timer *timer) {
    struct wl_timer *t = (struct wl_timer *)timer;

    if (t == NULL) {
        return;
    }
    if (t->source != NULL) {
        wl_event_source_remove(t->source);
    }
    free(t);
}

static int watch_ready(int fd, uint32_t mask, void *data) {
    struct wl_watch *w = data;

    /*
     * Wayland's four, translated to ours one for one.
     *
     * This used to collapse all of them to "readable", with an argument that
     * a caller reads and finds out anyway. That was true of the one caller it
     * had. It is not true of `src/recon_net.c`, which has to tell a connect
     * that failed from one that is still in flight -- and on a socket that has
     * neither connected nor died, reading is exactly what tells you nothing.
     */
    w->fn(fd, from_wl(mask), w->user);
    return 0;
}

struct recon_watch *recon_watch_create(struct recon_loop *loop, int fd,
        unsigned events, recon_watch_fn fn, void *user) {
    if (loop == NULL || fn == NULL || fd < 0) {
        return NULL;
    }

    struct wl_watch *w = calloc(1, sizeof(*w));

    if (w == NULL) {
        return NULL;
    }
    w->fn = fn;
    w->user = user;
    w->fd = fd;
    w->source = wl_event_loop_add_fd((struct wl_event_loop *)loop, fd,
        to_wl(events), watch_ready, w);

    if (w->source == NULL) {
        free(w);
        return NULL;
    }
    return (struct recon_watch *)w;
}

bool recon_watch_wants(struct recon_watch *watch, unsigned events) {
    struct wl_watch *w = (struct wl_watch *)watch;

    if (w == NULL || w->source == NULL) {
        return false;
    }
    return wl_event_source_fd_update(w->source, to_wl(events)) == 0;
}

void recon_watch_destroy(struct recon_watch *watch) {
    struct wl_watch *w = (struct wl_watch *)watch;

    if (w == NULL) {
        return;
    }
    if (w->source != NULL) {
        wl_event_source_remove(w->source);
    }
    free(w);
}

/*
 * --- The driving half, which wayland owns ---
 *
 * `recon_loop_create`, `recon_loop_tick` and the rest are for whoever runs a
 * loop, and here that is the compositor: it has an epoll, it knows when a
 * descriptor is ready, and it decides when to sleep. Nothing in the desktop
 * should be telling it.
 *
 * Refused rather than left out, so that a caller which wandered onto the wrong
 * half gets a null pointer and a zero rather than a link error somebody
 * silences by adding a file.
 */
struct recon_loop *recon_loop_create(void) {
    return NULL;
}

void recon_loop_destroy(struct recon_loop *loop) {
    (void)loop;
}

int recon_loop_tick(struct recon_loop *loop, uint64_t now_ms) {
    (void)loop;
    (void)now_ms;
    return 0;
}

/*
 * Nothing, and that is the answer rather than a gap. See the header: the
 * compositor owns the waiting and polls its own descriptors, so an owner
 * asking what to wait on here is one about to poll what is already polled.
 */
int recon_loop_watch_count(const struct recon_loop *loop) {
    (void)loop;
    return 0;
}

bool recon_loop_watch_at(const struct recon_loop *loop, int index,
        int *fd, unsigned *events) {
    (void)loop;
    (void)index;
    (void)fd;
    (void)events;
    return false;
}

void recon_loop_ready(struct recon_loop *loop, int fd, unsigned events) {
    (void)loop;
    (void)fd;
    (void)events;
}

int recon_loop_next_deadline(const struct recon_loop *loop, uint64_t now_ms) {
    (void)loop;
    (void)now_ms;
    return -1;
}
