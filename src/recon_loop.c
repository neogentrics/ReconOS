/*
 * Waiting, with nothing underneath. See include/recon_loop.h.
 *
 * No system call in this file, and no clock read: the time arrives as an
 * argument. That is what lets every question about a timer be asked from a
 * test -- does it fire late, does it fire twice, does a callback that re-arms
 * itself run away, what happens when the clock jumps.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "recon_loop.h"

/*
 * How many of each a loop holds.
 *
 * Fixed rather than grown, which is the discipline the rest of this tree keeps
 * for the same reason: a list that grows on demand is a list whose worst case
 * is decided by whatever asked last. Sixteen timers is four times what the six
 * files using this create between them, and a program wanting more than that
 * is a program doing something this interface was not shaped for.
 */
#define TIMERS_MAX 16
#define WATCHES_MAX 8

struct recon_timer {
    struct recon_loop *loop;
    recon_timer_fn fn;
    void *user;

    bool live;              /* this slot is in use */
    bool armed;
    uint64_t due_ms;
};

struct recon_watch {
    struct recon_loop *loop;
    recon_watch_fn fn;
    unsigned events;
    void *user;

    bool live;
    int fd;
};

struct recon_loop {
    struct recon_timer timers[TIMERS_MAX];
    struct recon_watch watches[WATCHES_MAX];

    /*
     * The last time anybody said it was.
     *
     * Kept so that arming can be "in n milliseconds" without reading a clock.
     * A timer armed before the first tick is due at `n` from zero, which is
     * the right answer as long as nothing fires before the first tick -- and
     * nothing can, because firing is what a tick does.
     */
    uint64_t now_ms;
};

struct recon_loop *recon_loop_create(void) {
    return calloc(1, sizeof(struct recon_loop));
}

void recon_loop_destroy(struct recon_loop *loop) {
    free(loop);
}

struct recon_timer *recon_timer_create(struct recon_loop *loop,
        recon_timer_fn fn, void *user) {
    if (loop == NULL || fn == NULL) {
        return NULL;
    }

    for (int i = 0; i < TIMERS_MAX; i++) {
        struct recon_timer *t = &loop->timers[i];

        if (t->live) {
            continue;
        }
        t->loop = loop;
        t->fn = fn;
        t->user = user;
        t->live = true;
        t->armed = false;   /* created disarmed, which is what callers expect */
        t->due_ms = 0;
        return t;
    }
    return NULL;
}

void recon_timer_after(struct recon_timer *timer, int milliseconds) {
    if (timer == NULL || !timer->live) {
        return;
    }

    /*
     * Zero disarms, and so does a negative -- which is not a case any caller
     * passes deliberately. It is what a caller passes by accident, having
     * computed a delay from two times and got them the wrong way round, and
     * the choice is between "never" and "immediately, for ever". A timer that
     * fires in a tight loop because a subtraction went backwards is a machine
     * that stops responding; one that does not fire is a feature that does not
     * work. The second is diagnosable.
     */
    if (milliseconds <= 0) {
        timer->armed = false;
        return;
    }

    timer->armed = true;
    timer->due_ms = timer->loop->now_ms + (uint64_t)milliseconds;
}

int recon_timer_is_armed(const struct recon_timer *timer) {
    return timer != NULL && timer->live && timer->armed;
}

void recon_timer_destroy(struct recon_timer *timer) {
    if (timer == NULL) {
        return;
    }
    timer->live = false;
    timer->armed = false;
    timer->fn = NULL;
    timer->user = NULL;
}

struct recon_watch *recon_watch_create(struct recon_loop *loop, int fd,
        unsigned events, recon_watch_fn fn, void *user) {
    if (loop == NULL || fn == NULL || fd < 0) {
        return NULL;
    }

    for (int i = 0; i < WATCHES_MAX; i++) {
        struct recon_watch *w = &loop->watches[i];

        if (w->live) {
            continue;
        }
        w->loop = loop;
        w->fn = fn;
        w->user = user;
        w->fd = fd;
        w->events = events;
        w->live = true;
        return w;
    }
    return NULL;
}

bool recon_watch_wants(struct recon_watch *watch, unsigned events) {
    if (watch == NULL || !watch->live) {
        return false;
    }
    watch->events = events;
    return true;
}

void recon_watch_destroy(struct recon_watch *watch) {
    if (watch == NULL) {
        return;
    }
    watch->live = false;
    watch->fn = NULL;
    watch->user = NULL;
    watch->fd = -1;
}

int recon_loop_tick(struct recon_loop *loop, uint64_t now_ms) {
    if (loop == NULL) {
        return 0;
    }

    /*
     * Taken as given, including when it goes backwards.
     *
     * There used to be a clamp here, with a comment about unsigned arithmetic
     * wrapping and every timer becoming due in half a billion years. **That
     * failure cannot happen in this file**: every use of `now_ms` is an
     * addition or a comparison, and the one subtraction -- in
     * `recon_loop_next_deadline` -- is guarded by the check above it. A
     * mutation deleting the clamp changed no output, which is how it was
     * found.
     *
     * And the clamp cost something real. A timer armed *after* a backward
     * jump would be given a deadline relative to the old time and wait for
     * the clock to catch up -- fifty seconds late, for a fifty-second jump.
     * Taking the clock as given means a timer is armed relative to the clock
     * its caller is reading, which is what "in 100 milliseconds" means.
     */
    loop->now_ms = now_ms;

    /*
     * Which ones are due is decided **before** any of them run.
     *
     * A callback may arm timers, including its own -- that is the ordinary
     * use, since these are one-shot. Deciding as we go would let a callback
     * that re-arms itself for zero milliseconds fire again in the same tick,
     * and again, and the loop would not end. Taking the list first means a
     * timer fires at most once per tick however it behaves.
     */
    bool due[TIMERS_MAX];
    int count = 0;

    for (int i = 0; i < TIMERS_MAX; i++) {
        struct recon_timer *t = &loop->timers[i];

        due[i] = t->live && t->armed && t->due_ms <= loop->now_ms;
        if (due[i]) {
            count++;
        }
    }

    for (int i = 0; i < TIMERS_MAX; i++) {
        struct recon_timer *t = &loop->timers[i];

        if (!due[i]) {
            continue;
        }
        /*
         * Checked again, because an earlier callback in this same tick may
         * have destroyed this timer or disarmed it -- which a window closing
         * does to every timer it owns.
         */
        if (!t->live || !t->armed) {
            continue;
        }

        /*
         * Disarmed before it runs, not after. A callback that arms itself is
         * the common case, and disarming afterwards would throw that away --
         * which is a film that plays one frame and stops.
         */
        t->armed = false;
        t->fn(t->user);
    }

    return count;
}

void recon_loop_ready(struct recon_loop *loop, int fd, unsigned events) {
    if (loop == NULL || fd < 0 || events == 0) {
        return;
    }

    for (int i = 0; i < WATCHES_MAX; i++) {
        struct recon_watch *w = &loop->watches[i];

        /*
         * A hangup or an error reaches a watch whatever it asked for -- see
         * the enum in the header. Anything else only reaches one that wanted
         * it, so a socket that is readable does not wake a watch sitting on
         * it for writability alone.
         */
        unsigned told = events &
            (w->events | RECON_WATCH_HANGUP | RECON_WATCH_ERROR);

        if (w->live && w->fd == fd && told != 0) {
            /*
             * Not disarmed. A watch stays until it is destroyed, because a
             * descriptor with more to read is the ordinary case and a caller
             * that had to re-register after every read would drop whatever
             * arrived in between.
             */
            w->fn(w->fd, told, w->user);
        }
    }
}

int recon_loop_next_deadline(const struct recon_loop *loop, uint64_t now_ms) {
    if (loop == NULL) {
        return -1;
    }

    uint64_t at = now_ms;
    bool found = false;
    uint64_t soonest = 0;

    for (int i = 0; i < TIMERS_MAX; i++) {
        const struct recon_timer *t = &loop->timers[i];

        if (!t->live || !t->armed) {
            continue;
        }
        if (!found || t->due_ms < soonest) {
            soonest = t->due_ms;
            found = true;
        }
    }

    if (!found) {
        return -1;
    }
    if (soonest <= at) {
        return 0;       /* already overdue */
    }

    uint64_t wait = soonest - at;

    /*
     * Clamped to what an int holds. A caller asked how long to sleep and a
     * negative answer would be read as "nothing armed", which is the opposite
     * of the truth.
     */
    return wait > 0x7fffffffu ? 0x7fffffff : (int)wait;
}
