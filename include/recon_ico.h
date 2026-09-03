/*
 * ICO decoding.
 *
 * An icon file holds several images at different sizes, each stored either as
 * PNG or as a Windows DIB. Both are handled, so icons made for Windows can be
 * used as they are rather than having to be converted first.
 */

#ifndef RECON_ICO_H
#define RECON_ICO_H

#include <stddef.h>

/*
 * Decode the image closest to preferred_size into RGBA pixels, which the
 * caller frees. Returns NULL if the data is not an icon or uses a form that
 * is not supported -- failing visibly rather than producing something wrong.
 */
unsigned char *recon_ico_decode(const unsigned char *data, size_t size,
    int preferred_size, int *width_out, int *height_out);

#endif
