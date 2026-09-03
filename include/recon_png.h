/*
 * Writing PNG files.
 *
 * ReconOS can read PNG -- stb_image does that -- and could not write one,
 * which was fine until it needed to save a picture of its own screen.
 *
 * This is a deliberately small encoder: 8-bit RGB or RGBA, no interlacing, no
 * palette, one filter mode. That is all a screenshot needs, and every feature
 * left out is one that cannot be got wrong. The compression is zlib's, which
 * is already linked for the icon decoder.
 *
 * Written rather than pulled in because the alternative was a second image
 * library for one direction of one format.
 */

#ifndef RECON_PNG_H
#define RECON_PNG_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Encode pixels into a PNG in memory.
 *
 * `pixels` is `width * height` of 0xAARRGGBB, which is what the whole UI
 * layer uses. Alpha is written out when `with_alpha` is set and dropped
 * otherwise -- a screenshot has nothing behind it, so keeping a channel that
 * is 255 everywhere would be a third more file for no more picture.
 *
 * Returns a buffer the caller frees, and puts its length in `size_out`. NULL
 * if there was no memory or the arguments make no sense.
 */
unsigned char *recon_png_encode(const unsigned int *pixels, int width,
    int height, bool with_alpha, size_t *size_out);

/*
 * Encode and write into the ReconOS filesystem, at a ReconOS path.
 *
 * Goes through recon_fs like everything else, so a screenshot lands inside
 * the system rather than on the host, and the account rules apply to it.
 */
bool recon_png_write(const char *path, const unsigned int *pixels, int width,
    int height, bool with_alpha);

const char *recon_png_last_error(void);

#endif
