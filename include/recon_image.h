/*
 * Making a picture a different size.
 *
 * Split out because three places wanted it and each had its own answer or none.
 * The wallpaper loader carried a private area-averaging shrinker; the video
 * path had a good one welded to YUV input, so it could not be pointed at a
 * photograph; and Photos, which is the application where somebody would
 * actually ask for this, had nothing.
 *
 * The rule about which resampler to use is the same one recon_video measured
 * its way to, and it is worth stating rather than re-deriving:
 *
 *   Making a picture SMALLER means several source pixels land on one output
 *   pixel, so every one of them has to be looked at. Sampling instead of
 *   averaging drops most of them, and what is dropped is detail -- on a picture
 *   full of one-pixel lines it turns them into dotted ones.
 *
 *   Making a picture LARGER means one source pixel covers several output ones,
 *   so there is nothing to average. Interpolating between neighbours is the
 *   only thing left that is not simply repeating pixels.
 *
 * So both, chosen per axis. A picture can be made narrower and taller at once.
 */

#ifndef RECON_IMAGE_H
#define RECON_IMAGE_H

#include <stdbool.h>

/*
 * Resample RGBA into a buffer of exactly the size given.
 *
 * `src` is src_w * src_h * 4 bytes and `dst` is dst_w * dst_h * 4. Alpha is
 * resampled with the colours rather than being carried across untouched -- a
 * picture with a transparent corner has an edge to that transparency, and an
 * edge is exactly what resampling is about.
 *
 * False when a size is not positive or a pointer is missing.
 */
bool recon_image_scale(const unsigned char *src, int src_w, int src_h,
    unsigned char *dst, int dst_w, int dst_h);

/*
 * The largest box of the picture's shape that fits inside the given one.
 *
 * Aspect ratio is kept, always. There is no setting for stretching, because a
 * picture stretched to fill a box is a picture of the wrong shape and nobody
 * has ever wanted one.
 */
void recon_image_fit(int src_w, int src_h, int box_w, int box_h,
    int *out_w, int *out_h);

#endif /* RECON_IMAGE_H */
