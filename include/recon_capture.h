/*
 * Screen capture.
 *
 * A picture of what is actually on the screen, saved into the user's Pictures
 * folder as a PNG.
 *
 * It works by asking for the *next* frame rather than by grabbing the last
 * one. A compositor does not keep a copy of what it drew -- the pixels go to
 * the display and the buffer is handed back to be drawn over -- so "capture
 * now" means "render one more frame, read it, and then show it". That is a
 * few milliseconds away rather than instant, and it is the difference between
 * a real picture and whatever happened to still be in a buffer.
 *
 * Which is also why this is asynchronous: the request is recorded, the next
 * frame satisfies it, and the caller is told afterwards.
 */

#ifndef RECON_CAPTURE_H
#define RECON_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>

struct recon_server;
struct wlr_buffer;

/*
 * Ask for a picture of the screen.
 *
 * `path` is a ReconOS path, or NULL to have one chosen in the signed-in
 * account's Pictures folder. False if a capture is already pending -- one at
 * a time, because two would be pictures of the same frame under two names.
 */
bool recon_capture_request(struct recon_server *server, const char *path);

/* Whether one is waiting for a frame. */
bool recon_capture_pending(void);

/*
 * Take the picture, from a buffer holding a frame that is about to be shown.
 *
 * Called from the frame path and nowhere else. Does nothing unless a capture
 * was asked for, so the cost on an ordinary frame is one test.
 */
void recon_capture_take(struct wlr_buffer *buffer);

/*
 * Where the last picture went, and whether it worked.
 *
 * Kept for the same reason the network keeps its last result: the answer
 * arrives a frame after the command that asked for it has returned.
 */
bool recon_capture_last(char *path, size_t size, bool *ok);

const char *recon_capture_last_error(void);

#endif
