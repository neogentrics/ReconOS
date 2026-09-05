/*
 * The picture half of a video file.
 *
 * recon_mp4 says where the compressed pieces are. A codec module turns one of
 * them into planes. recon_video turns planes into pixels. This is the thing in
 * the middle that decides *which* piece, *when* -- and it is the part that has
 * nothing to do with any format at all.
 *
 * --- Why the clock is not in here ---
 *
 * It is handed in. `recon_movie_advance` is told what time it is and finds the
 * right picture for it; it never asks.
 *
 * That is the whole trick of playing sound and pictures together. The sound
 * card is a real clock -- it consumes samples at a fixed rate whether anybody
 * is watching or not, and asking how many it has actually played is a
 * measurement rather than an estimate. Pictures have no such thing. So the
 * sound leads and the pictures follow, and a frame that arrives late is
 * *dropped* rather than shown late, because a video half a second behind its
 * own soundtrack is unwatchable in a way that a missing frame is not.
 *
 * A file with no sound track has no such clock, and the caller passes wall time
 * instead. That is a worse clock and it is the right one to use, because the
 * alternative -- counting frames and assuming they took as long as they should
 * have -- drifts and never notices.
 *
 * --- Why the pixels are kept here ---
 *
 * A decoder's picture is borrowed and dies at the next call. That makes
 * "decode ahead, then decide what to show" impossible without copying three
 * megabytes a frame. So this converts to pixels the moment a picture is due,
 * into a buffer it owns, at the size the caller asked for -- which also means
 * the expensive part happens once per picture *shown* rather than once per
 * picture decoded.
 */

#ifndef RECON_MOVIE_H
#define RECON_MOVIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct recon_movie;

/*
 * Open the video track of a file.
 *
 * `bytes` is borrowed and must outlive the handle, exactly as recon_mp4_open
 * requires -- the sample tables are read in place rather than copied.
 *
 * NULL when there is no video track, or when nothing installed can decode the
 * one there is. Those two are not the same thing and recon_movie_last_error
 * tells them apart: one is a file that has no picture, the other is a file
 * whose picture needs a codec pack.
 */
struct recon_movie *recon_movie_open(const uint8_t *bytes, size_t length);
void recon_movie_close(struct recon_movie *movie);

/* The picture's own size, which is what to keep the shape of. */
void recon_movie_size(const struct recon_movie *movie, int *width, int *height);

/* How long the picture track runs, in seconds. */
double recon_movie_duration(const struct recon_movie *movie);

/* What it is, in words: "H.264". For a status line, and for saying what a
 * missing decoder was missing. */
const char *recon_movie_format(const struct recon_movie *movie);

/*
 * The size pictures should be produced at -- the window's, not the file's.
 *
 * Changing it is cheap and takes effect at the next picture. Scaling happens
 * during colour conversion rather than after it, so a small window is genuinely
 * less work rather than the same work followed by a shrink.
 */
bool recon_movie_set_size(struct recon_movie *movie, int width, int height);

/*
 * Move to `seconds`, decoding whatever that takes.
 *
 * True when the picture changed and the window should be redrawn. False is the
 * ordinary case between frames -- a video at 25 frames a second asked 60 times
 * a second says no more often than yes, and a caller that redrew anyway would
 * spend most of its effort compositing a picture nobody changed.
 */
bool recon_movie_advance(struct recon_movie *movie, double seconds);

/*
 * The current picture, as RGBA at the size set. NULL before the first one has
 * been decoded, which is a real state: it is what a window shows in the moment
 * between pressing play and the first frame arriving.
 */
const unsigned char *recon_movie_pixels(const struct recon_movie *movie);
void recon_movie_pixel_size(const struct recon_movie *movie,
    int *width, int *height);

/*
 * Jump.
 *
 * Decoding resumes from the last frame that stands on its own, and the frames
 * between there and here are decoded without being shown -- they exist only so
 * the one that is shown has something to have been predicted from. Skipping
 * them is what produces the coloured smears that a home-made player shows for a
 * second after every seek.
 */
bool recon_movie_seek(struct recon_movie *movie, double seconds);

/* Whether the track has run out. */
bool recon_movie_finished(const struct recon_movie *movie);

const char *recon_movie_last_error(void);

#endif /* RECON_MOVIE_H */
