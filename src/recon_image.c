/*
 * Resampling RGBA. See include/recon_image.h.
 */

#include <stdint.h>
#include <stdlib.h>

#include "recon_image.h"

/*
 * One output pixel, averaged over every source pixel that lands on it.
 *
 * For making a picture smaller, where several source pixels share one output
 * pixel and dropping all but one of them is dropping detail.
 */
static void shrink_pixel(const unsigned char *src, int src_w,
        int sx0, int sx1, int sy0, int sy1, unsigned char *out) {
    uint32_t total[4] = { 0, 0, 0, 0 };
    uint32_t n = 0;

    for (int sy = sy0; sy < sy1; sy++) {
        const unsigned char *px = src + ((size_t)sy * src_w + sx0) * 4;
        for (int sx = sx0; sx < sx1; sx++) {
            total[0] += px[0];
            total[1] += px[1];
            total[2] += px[2];
            total[3] += px[3];
            px += 4;
            n++;
        }
    }

    if (n == 0) {
        n = 1;
    }
    for (int i = 0; i < 4; i++) {
        out[i] = (unsigned char)(total[i] / n);
    }
}

/*
 * One output pixel, interpolated between the four source pixels around it.
 *
 * For making a picture larger, where one source pixel covers several output
 * ones and there is nothing to average. The alternative is repeating pixels,
 * which makes a picture of squares rather than a larger picture.
 *
 * Positions are in sixteenths of a source pixel, so the weights are integers
 * and the arithmetic is exact.
 */
static void grow_pixel(const unsigned char *src, int src_w, int src_h,
        int fx16, int fy16, unsigned char *out) {
    int x0 = fx16 >> 4;
    int y0 = fy16 >> 4;
    int wx = fx16 & 15;
    int wy = fy16 & 15;

    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > src_w - 1) { x1 = src_w - 1; }
    if (y1 > src_h - 1) { y1 = src_h - 1; }
    if (x0 > src_w - 1) { x0 = src_w - 1; }
    if (y0 > src_h - 1) { y0 = src_h - 1; }

    const unsigned char *a = src + ((size_t)y0 * src_w + x0) * 4;
    const unsigned char *b = src + ((size_t)y0 * src_w + x1) * 4;
    const unsigned char *c = src + ((size_t)y1 * src_w + x0) * 4;
    const unsigned char *d = src + ((size_t)y1 * src_w + x1) * 4;

    for (int i = 0; i < 4; i++) {
        int top = a[i] * (16 - wx) + b[i] * wx;
        int bottom = c[i] * (16 - wx) + d[i] * wx;
        out[i] = (unsigned char)((top * (16 - wy) + bottom * wy) / 256);
    }
}

bool recon_image_scale(const unsigned char *src, int src_w, int src_h,
        unsigned char *dst, int dst_w, int dst_h) {
    if (src == NULL || dst == NULL ||
            src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return false;
    }

    /*
     * Chosen per axis rather than once for the picture, because the two can
     * disagree: a picture made narrower and taller at the same time wants an
     * average across and an interpolation down, and picking one rule for both
     * gets one of them wrong.
     */
    bool shrink_x = dst_w < src_w;
    bool shrink_y = dst_h < src_h;

    for (int y = 0; y < dst_h; y++) {
        /*
         * The source band this output row covers, for the shrinking case, and
         * the source position it sits at, for the growing one.
         *
         * The growing position is offset to the CENTRE of the output pixel.
         * Sampling at its leading edge shifts the whole picture half an output
         * pixel up and left, which is invisible on one enlargement and
         * accumulates into a visible drift if anything ever chains them.
         */
        int sy0 = (int)((int64_t)y * src_h / dst_h);
        int sy1 = (int)((int64_t)(y + 1) * src_h / dst_h);
        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }
        if (sy1 > src_h) {
            sy1 = src_h;
        }

        int fy16 = dst_h > 1
            ? (int)(((int64_t)(2 * y + 1) * src_h * 16 / (2 * dst_h)) - 8)
            : 0;

        for (int x = 0; x < dst_w; x++) {
            int sx0 = (int)((int64_t)x * src_w / dst_w);
            int sx1 = (int)((int64_t)(x + 1) * src_w / dst_w);
            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }
            if (sx1 > src_w) {
                sx1 = src_w;
            }

            int fx16 = dst_w > 1
                ? (int)(((int64_t)(2 * x + 1) * src_w * 16 / (2 * dst_w)) - 8)
                : 0;

            unsigned char *out = dst + ((size_t)y * dst_w + x) * 4;

            if (shrink_x && shrink_y) {
                shrink_pixel(src, src_w, sx0, sx1, sy0, sy1, out);
            } else if (!shrink_x && !shrink_y) {
                grow_pixel(src, src_w, src_h, fx16, fy16, out);
            } else {
                /*
                 * One axis each way. Averaged along the axis being reduced and
                 * interpolated along the other, which is what each of them
                 * needs -- and cheaper to say than to build a separable
                 * two-pass resampler for a case that is mostly a thumbnail
                 * being made from a panorama.
                 */
                int ax0 = shrink_x ? sx0 : (fx16 >> 4);
                int ax1 = shrink_x ? sx1 : ax0 + 1;
                int ay0 = shrink_y ? sy0 : (fy16 >> 4);
                int ay1 = shrink_y ? sy1 : ay0 + 1;
                if (ax0 < 0) { ax0 = 0; }
                if (ay0 < 0) { ay0 = 0; }
                if (ax1 > src_w) { ax1 = src_w; }
                if (ay1 > src_h) { ay1 = src_h; }
                if (ax1 <= ax0) { ax1 = ax0 + 1; }
                if (ay1 <= ay0) { ay1 = ay0 + 1; }
                shrink_pixel(src, src_w, ax0, ax1, ay0, ay1, out);
            }
        }
    }

    return true;
}

void recon_image_fit(int src_w, int src_h, int box_w, int box_h,
        int *out_w, int *out_h) {
    if (src_w <= 0 || src_h <= 0 || box_w <= 0 || box_h <= 0) {
        if (out_w != NULL) { *out_w = 0; }
        if (out_h != NULL) { *out_h = 0; }
        return;
    }

    /* Whichever side runs out first decides, which is what "fits inside"
     * means. Compared as a cross-multiplication so there is no division and
     * no rounding until the end. */
    int w, h;
    if ((int64_t)src_w * box_h > (int64_t)box_w * src_h) {
        w = box_w;
        h = (int)(((int64_t)src_h * box_w + src_w / 2) / src_w);
    } else {
        h = box_h;
        w = (int)(((int64_t)src_w * box_h + src_h / 2) / src_h);
    }

    if (w < 1) { w = 1; }
    if (h < 1) { h = 1; }
    if (out_w != NULL) { *out_w = w; }
    if (out_h != NULL) { *out_h = h; }
}
