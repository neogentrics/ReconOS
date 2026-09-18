/*
 * Waiting, as something the desktop can ask for without knowing who is asking.
 *
 * --- Why this exists ---
 *
 * `scripts/port-blockers.sh` was written to answer "what stops each source that
 * cannot build with no Linux under it, and how much is it holding". Its first
 * run gave an answer nobody had guessed: **six files, 8,155 lines, and between
 * them they used exactly one thing from the compositor.** Not drawing, not
 * windows, not input -- the event loop.
 *
 * `recon_session.c` is three thousand lines about signing in and shutting down
 * and mentions wayland twice. `recon_player.c` is twelve hundred lines about
 * playing a film and mentions it not at all -- it only holds a `wl_event_source`
 * so that a frame can arrive on time.
 *
 * So this is the seam, and it is the fifth this project has drawn:
 * `recon_memory_source` for the allocator, `recon_panel_present` for the
 * screen, `recon_desktop_machine` for the kernel, and now waiting.
 *
 * --- What a timer is here, which is what it already was ---
 *
 * **One-shot.** A timer fires once, and the callback arms it again if it wants
 * another. That is not a simplification of wayland's -- it *is* wayland's, and
 * it is what every one of these six files already assumes: `on_tick` in
 * `recon_player.c` ends by re-arming itself, and so do the audio top-up, the
 * boot progress and the task manager's refresh.
 *
 * Making it repeating instead would be the quiet kind of wrong. Every caller
 * re-arms, so every rate would double, and what that looks like is a machine
 * that feels busy rather than a machine that is broken.
 *
 * **A delay of zero disarms it**, which is also what callers already pass:
 * `recon_session.c` stops its boot timer with `0`, and so does the task
 * manager when its window closes.
 *
 * --- Time is passed in, never read ---
 *
 * `recon_loop_tick` takes the time rather than asking for it. That is the same
 * choice `userland/init/screen.c` and `userland/desktop/shell_frame.c` make,
 * and for the same reason: a file that reads a clock cannot be tested against
 * one, and every interesting question about a timer -- does it fire late, does
 * it fire twice, does a callback that re-arms itself run away -- is a question
 * about *what time it is*.
 *
 * --- And readability is told, not polled ---
 *
 * There is no `poll` in this interface, and that is deliberate rather than
 * pending. ReconOS has no such call; putting one in the shape of this header
 * would be describing a mechanism that does not exist, and the first
 * implementation would have to invent it. Whoever owns the loop knows how it
 * finds out -- wayland has an epoll, and ReconOS will have whatever it has --
 * and says so with `recon_loop_readable`.
 */

#ifndef RECON_LOOP_H
#define RECON_LOOP_H

#include <stdbool.h>
#include <stdint.h>

struct recon_loop;
struct recon_timer;
struct recon_watch;

/* --- What a program asks for -------------------------------------------- */

/*
 * Called when the timer comes due. It has already been disarmed by then, so a
 * callback that wants another has to say so.
 */
typedef void (*recon_timer_fn)(void *user);

/*
 * --- What a watch waits for, and what it is told about ---
 *
 * The first two are asked for. The second two are **reported and never asked
 * for**: a caller cannot want a hangup, and a loop that only told you about
 * the things you asked for would leave a socket that died mid-request looking
 * like one that had gone quiet.
 *
 * A mask rather than four callbacks, because the callers already treat it as
 * one: `src/recon_net.c` opens a socket watching for *writable* to learn that
 * a connect finished, then switches to readable, then back to both while it
 * has something to send. One watch changing its mind, not four watches.
 */
enum {
    RECON_WATCH_READABLE = 1u << 0,
    RECON_WATCH_WRITABLE = 1u << 1,
    RECON_WATCH_HANGUP   = 1u << 2,
    RECON_WATCH_ERROR    = 1u << 3,
};

/*
 * Called when one of those has happened on `fd`.
 *
 * The fd is passed even though the callback usually captured it, because the
 * one thing worse than an unused parameter is a callback that reads a
 * descriptor a caller has since closed and replaced.
 *
 * `events` is what happened, which is not the same as what was asked for --
 * see above.
 */
typedef void (*recon_watch_fn)(int fd, unsigned events, void *user);

/*
 * A timer, created **disarmed**.
 *
 * Two steps rather than one, because that is how every caller already uses it:
 * the thing is made when a window opens and armed when something starts
 * happening, and those are rarely the same moment. NULL if it cannot be made.
 */
struct recon_timer *recon_timer_create(struct recon_loop *loop,
    recon_timer_fn fn, void *user);

/*
 * Come due in `milliseconds`, counted from now.
 *
 * **Zero disarms it.** Re-arming an armed timer replaces the delay rather than
 * adding to it, which is the behaviour `recon_player.c` relies on when a film
 * is paused and resumed.
 */
void recon_timer_after(struct recon_timer *timer, int milliseconds);

/* Whether it is waiting to come due. */
int recon_timer_is_armed(const struct recon_timer *timer);

void recon_timer_destroy(struct recon_timer *timer);

/*
 * Tell me when one of `events` happens on `fd`.
 *
 * The loop does not own the descriptor and never closes it. NULL if it cannot
 * be watched.
 *
 * Asking for a hangup or an error is neither refused nor honoured: they arrive
 * regardless, and a caller that named them has said something that was already
 * true.
 */
struct recon_watch *recon_watch_create(struct recon_loop *loop, int fd,
    unsigned events, recon_watch_fn fn, void *user);

/*
 * Change what it is waiting for, without taking it down and putting it back.
 *
 * The difference matters on a connecting socket. Destroying the watch and
 * making another one has a window in it where the descriptor is watched by
 * nothing -- and what arrives in that window is the reply to the request that
 * was just sent. False when it could not be changed, which leaves it watching
 * what it was.
 */
bool recon_watch_wants(struct recon_watch *watch, unsigned events);

void recon_watch_destroy(struct recon_watch *watch);

/* --- What whoever owns the loop does ------------------------------------ */

/*
 * A loop of this file's own. `src/recon_loop.c`, and it makes no system call.
 *
 * The wayland one is not made this way: `recon_loop_from_wl` in
 * `src/recon_loop_wl.c` wraps a loop the compositor already has.
 */
struct recon_loop *recon_loop_create(void);
void recon_loop_destroy(struct recon_loop *loop);

/*
 * The time has moved on. Fire whatever is due.
 *
 * `now_ms` is a monotonic millisecond count and its zero point does not
 * matter; what matters is that it never goes backwards. A clock that does is
 * handled rather than trusted -- see `src/recon_loop.c`.
 *
 * A callback may create, arm, disarm or destroy timers, including its own.
 * That is not a convenience: it is what one-shot timers *mean*, since the
 * ordinary use is a callback re-arming itself.
 *
 * Answers how many came due, which is only interesting when it is a number
 * nobody expected.
 */
int recon_loop_tick(struct recon_loop *loop, uint64_t now_ms);

/*
 * One of `events` has happened on `fd`. Tell whoever asked.
 *
 * Called by whatever found out, which this file deliberately is not.
 */
void recon_loop_ready(struct recon_loop *loop, int fd, unsigned events);

/*
 * --- What this loop is waiting on, so an owner can find out ---
 *
 * The header above says "whoever owns the loop knows how it finds out", and
 * that was half an interface: it told an owner to go and find out, and never
 * said **what about**. A loop keeps the list of descriptors and what each one
 * is waiting for, and without a way to read it back an owner has to keep a
 * second copy of the same list -- which is a second thing to keep in step, and
 * the one that drifts is the one that decides whether a socket is polled.
 *
 * So: ask the loop. Build a poll set from it, wait, and hand the answers back
 * through `recon_loop_ready`. That is the whole of what an owner does, and it
 * is what `userland/` will do on ReconOS with whatever call it ends up having.
 *
 * Under a compositor both of these answer **zero**, and that is not a gap: the
 * compositor owns the waiting and polls its own descriptors. An owner asking
 * this of a wayland loop is an owner about to poll something already being
 * polled, and the honest answer to "what should I wait on" is "nothing, I have
 * it".
 *
 * The order is not promised and a watch may move between calls, so the pair is
 * read in one pass rather than held across one.
 */
int recon_loop_watch_count(const struct recon_loop *loop);

bool recon_loop_watch_at(const struct recon_loop *loop, int index,
    int *fd, unsigned *events);

/*
 * How long until the next timer comes due, in milliseconds, or **-1 when
 * nothing is armed**.
 *
 * What it is for is sleeping. A driver that ticks in a tight loop is a machine
 * with a processor pinned for nothing, and a driver that sleeps a fixed amount
 * is a timer that is late by up to that amount. Zero when something is already
 * overdue.
 */
int recon_loop_next_deadline(const struct recon_loop *loop, uint64_t now_ms);

#endif /* RECON_LOOP_H */
