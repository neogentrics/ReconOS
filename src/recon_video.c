/*
 * Planes of brightness and colour into pixels. See include/recon_video.h.
 *
 * All of it is fixed point. Not for speed in the abstract -- for the specific
 * reason that this runs per pixel per frame, which at a modest 640x360 and
 * thirty frames a second is seven million times a second, and a float multiply
 * chain there is the difference between video that plays and video that
 * stutters. Sixteen fractional bits is far more precision than eight-bit output
 * can show.
 */

#include <stdlib.h>
#include <string.h>

#include "recon_video.h"

/* 16.16 fixed point throughout. */
#define FRAC_BITS 16
#define ONE (1 << FRAC_BITS)
#define HALF (ONE / 2)

/*
 * The four coefficient sets, as published.
 *
 * Studio range carries a scale on Y as well as an offset: the 16-235 range has
 * to be stretched to 0-255, which is where the 1.164 comes from. Full range has
 * nothing to stretch, so its Y scale is exactly one and the offset is zero --
 * and writing that out rather than special-casing it means one loop below
 * instead of two.
 */
struct coefficients {
    int32_t y_offset;   /* subtracted from Y before scaling */
    int32_t y_scale;    /* 16.16 */
    int32_t r_v;        /* 16.16, applied to (V - 128) */
    int32_t g_v;
    int32_t g_u;
    int32_t b_u;        /* 16.16, applied to (U - 128) */
};

static struct coefficients coefficients_for(enum recon_video_matrix matrix,
        bool full_range) {
    if (full_range) {
        if (matrix == RECON_VIDEO_BT709) {
            /* 1.5748, 0.4681, 0.1873, 1.8556 */
            struct coefficients c = { 0, ONE, 103206, 30679, 12276, 121608 };
            return c;
        }
        /* BT.601 full range -- what JPEG uses: 1.402, 0.714, 0.344, 1.772 */
        struct coefficients c = { 0, ONE, 91881, 46802, 22554, 116130 };
        return c;
    }

    if (matrix == RECON_VIDEO_BT709) {
        /* 1.164, 1.793, 0.534, 0.213, 2.115 */
        struct coefficients c = { 16, 76309, 117501, 34996, 13959, 138593 };
        return c;
    }
    /* BT.601 studio range: 1.164, 1.596, 0.813, 0.391, 2.018 */
    struct coefficients c = { 16, 76309, 104597, 53279, 25624, 132251 };
    return c;
}

static inline uint8_t clamp_byte(int32_t value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

/* How much smaller each colour plane is than the brightness one. */
static void chroma_shift(enum recon_video_format format, int *x, int *y) {
    switch (format) {
    case RECON_VIDEO_YUV444: *x = 0; *y = 0; break;
    case RECON_VIDEO_YUV422: *x = 1; *y = 0; break;
    case RECON_VIDEO_YUV420:
    default:                 *x = 1; *y = 1; break;
    }
}

/*
 * Where one output column or row lands in a source plane, as a whole part and
 * a fraction.
 *
 * The half-pixel terms are the difference between a correct resize and one
 * that drifts. A pixel is a sample of the middle of a little square, not of its
 * corner, so mapping output *centres* to input *centres* means adding a half
 * before scaling and taking one off after. Leaving them out shifts the whole
 * picture by up to half a source pixel and, worse, shifts it by a different
 * amount at each edge -- which reads as a picture very slightly sheared.
 */
static inline void sample_at(int index, int32_t step, int limit,
        int *whole, int *next, int32_t *fraction) {
    int32_t at = index * step + (step / 2) - HALF;
    if (at < 0) {
        at = 0;
    }
    int32_t last = (limit - 1) * ONE;
    if (at > last) {
        at = last;
    }
    *whole = at >> FRAC_BITS;
    *fraction = at & (ONE - 1);
    *next = (*whole + 1 < limit) ? *whole + 1 : *whole;
}

/* One plane sampled between four neighbours. */
static inline int32_t bilinear(const uint8_t *plane, int stride,
        int x0, int x1, int32_t fx, int y0, int y1, int32_t fy) {
    const uint8_t *top = plane + (size_t)y0 * stride;
    const uint8_t *bottom = plane + (size_t)y1 * stride;

    int32_t a = top[x0] + (((top[x1] - top[x0]) * fx) >> FRAC_BITS);
    int32_t b = bottom[x0] + (((bottom[x1] - bottom[x0]) * fx) >> FRAC_BITS);
    return a + (((b - a) * fy) >> FRAC_BITS);
}

/*
 * The span of source pixels one output pixel covers, as a half-open range.
 *
 * Integer division of the edges rather than a scaled centre, because this has
 * to *partition* the source: every source pixel belongs to exactly one output
 * pixel, with no gaps and no overlaps. Anything approximate here shows up as a
 * faint grid over flat colour, which is the kind of artefact that gets blamed
 * on the video.
 */
static inline void span(int index, int out_size, int in_size,
        int *from, int *to) {
    *from = (int)(((int64_t)index * in_size) / out_size);
    *to = (int)(((int64_t)(index + 1) * in_size) / out_size);
    if (*to <= *from) {
        *to = *from + 1;
    }
    if (*to > in_size) {
        *to = in_size;
    }
}

/* The mean of a rectangle of one plane. */
static inline int32_t area_average(const uint8_t *plane, int stride,
        int x0, int x1, int y0, int y1) {
    int32_t total = 0;
    for (int y = y0; y < y1; y++) {
        const uint8_t *row = plane + (size_t)y * stride;
        for (int x = x0; x < x1; x++) {
            total += row[x];
        }
    }
    int count = (x1 - x0) * (y1 - y0);
    return (count > 0) ? (total + count / 2) / count : 0;
}

bool recon_video_render(const struct recon_video_picture *picture,
        unsigned char *rgba, int out_width, int out_height) {
    if (picture == NULL || rgba == NULL || out_width <= 0 || out_height <= 0) {
        return false;
    }
    if (picture->width <= 0 || picture->height <= 0) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (picture->plane[i] == NULL || picture->stride[i] <= 0) {
            return false;
        }
    }

    int shift_x = 0, shift_y = 0;
    chroma_shift(picture->format, &shift_x, &shift_y);

    /*
     * Rounded up, not down.
     *
     * An odd-width 4:2:0 picture has (width + 1) / 2 colour samples, because
     * the last column still needs one of its own. Rounding down loses it and
     * reads one sample short of the row for every pixel in that column, which
     * looks like a coloured stripe down one edge -- on odd-sized video only,
     * which is why it survives a lot of testing.
     */
    int chroma_width = (picture->width + (1 << shift_x) - 1) >> shift_x;
    int chroma_height = (picture->height + (1 << shift_y) - 1) >> shift_y;
    if (chroma_width <= 0 || chroma_height <= 0) {
        return false;
    }

    struct coefficients c = coefficients_for(picture->matrix,
        picture->full_range);

    int32_t step_x = (int32_t)(((int64_t)picture->width * ONE) / out_width);
    int32_t step_y = (int32_t)(((int64_t)picture->height * ONE) / out_height);
    int32_t cstep_x = (int32_t)(((int64_t)chroma_width * ONE) / out_width);
    int32_t cstep_y = (int32_t)(((int64_t)chroma_height * ONE) / out_height);

    /*
     * Which of the two resamplers to use, and why there are two.
     *
     * Bilinear reads four source pixels. That is the right answer when the
     * picture is being *enlarged*, where there are more output pixels than
     * input and the job is to interpolate between them. It is the wrong answer
     * when the picture is being reduced by more than a factor of two, because
     * four pixels out of every sixteen -- or every hundred -- means most of the
     * source is never read at all. What is thrown away does not vanish quietly:
     * it comes back as aliasing, and on video it moves, so fine detail crawls
     * and shimmers from frame to frame.
     *
     * Measured rather than argued. Against ffmpeg on a real file, at the
     * picture's own size the mean per-channel difference was 1.15 -- two
     * correct implementations rounding differently. At a third of the size,
     * with the same colour conversion, it was 5.05. The extra four units were
     * all resampling, and they were all this.
     *
     * So reducing averages every source pixel that lands in an output pixel,
     * and enlarging interpolates. The decision is made once for both axes
     * because recon_video_fit keeps the aspect ratio, which makes the two
     * scale factors the same to within a pixel.
     */
    bool reducing = (step_x > ONE) || (step_y > ONE);

    for (int dy = 0; dy < out_height; dy++) {
        int y0, y1, cy0, cy1;
        int32_t fy = 0, cfy = 0;

        if (reducing) {
            span(dy, out_height, picture->height, &y0, &y1);
            span(dy, out_height, chroma_height, &cy0, &cy1);
        } else {
            sample_at(dy, step_y, picture->height, &y0, &y1, &fy);
            sample_at(dy, cstep_y, chroma_height, &cy0, &cy1, &cfy);
        }

        unsigned char *row = rgba + (size_t)dy * out_width * 4;

        for (int dx = 0; dx < out_width; dx++) {
            int x0, x1, cx0, cx1;
            int32_t fx = 0, cfx = 0;
            int32_t y, u, v;

            if (reducing) {
                span(dx, out_width, picture->width, &x0, &x1);
                span(dx, out_width, chroma_width, &cx0, &cx1);

                y = area_average(picture->plane[0], picture->stride[0],
                    x0, x1, y0, y1);
                u = area_average(picture->plane[1], picture->stride[1],
                    cx0, cx1, cy0, cy1) - 128;
                v = area_average(picture->plane[2], picture->stride[2],
                    cx0, cx1, cy0, cy1) - 128;
            } else {
                sample_at(dx, step_x, picture->width, &x0, &x1, &fx);
                sample_at(dx, cstep_x, chroma_width, &cx0, &cx1, &cfx);

                y = bilinear(picture->plane[0], picture->stride[0],
                    x0, x1, fx, y0, y1, fy);
                u = bilinear(picture->plane[1], picture->stride[1],
                    cx0, cx1, cfx, cy0, cy1, cfy) - 128;
                v = bilinear(picture->plane[2], picture->stride[2],
                    cx0, cx1, cfx, cy0, cy1, cfy) - 128;
            }

            int32_t base = (y - c.y_offset) * c.y_scale;

            unsigned char *out = row + (size_t)dx * 4;
            out[0] = clamp_byte((base + c.r_v * v) >> FRAC_BITS);
            out[1] = clamp_byte((base - c.g_v * v - c.g_u * u) >> FRAC_BITS);
            out[2] = clamp_byte((base + c.b_u * u) >> FRAC_BITS);
            out[3] = 255;
        }
    }
    return true;
}

void recon_video_fit(int picture_width, int picture_height,
        int box_width, int box_height, int *out_width, int *out_height) {
    if (out_width == NULL || out_height == NULL) {
        return;
    }
    if (picture_width <= 0 || picture_height <= 0 ||
            box_width <= 0 || box_height <= 0) {
        *out_width = 0;
        *out_height = 0;
        return;
    }

    /*
     * Compared as a cross-multiply rather than as two ratios, because the
     * ratios are the thing being compared and doing it in integers avoids
     * asking whether 1.7777 and 1.7778 are the same number.
     */
    int64_t wide = (int64_t)picture_width * box_height;
    int64_t tall = (int64_t)box_width * picture_height;

    if (wide > tall) {
        /* Limited by width. */
        *out_width = box_width;
        *out_height = (int)((int64_t)box_width * picture_height /
            picture_width);
    } else {
        *out_height = box_height;
        *out_width = (int)((int64_t)box_height * picture_width /
            picture_height);
    }

    /* A window can be dragged narrow enough that the other side rounds to
     * nothing, and a zero-sized destination is a division by zero downstream
     * rather than a very small picture. */
    if (*out_width < 1) {
        *out_width = 1;
    }
    if (*out_height < 1) {
        *out_height = 1;
    }
}
