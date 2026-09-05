/*
 * Tests for turning planes into pixels.
 *
 * This half of video playback is the half that can be tested without a decoder,
 * a file or a screen: it is arithmetic with a known right answer. Feed it a
 * colour and check the colour that comes out.
 *
 * That matters more than it sounds. The decoder was verified against ffmpeg on
 * a real film, which is the right way to check a decoder and the wrong way to
 * check this -- a comparison against another implementation says the two agree
 * and cannot say either is right, and it needs a library, a file and a
 * subprocess to say even that. What is here needs none of them and runs on a
 * machine with no sound card, no display and no codec pack.
 *
 * The four things worth pinning down:
 *
 *   - Black is black and white is white, in both ranges. Studio-range video
 *     stores black as 16 and white as 235, and treating those as 0 and 255
 *     gives a picture with grey blacks that looks like a bad transfer rather
 *     than like a bug.
 *   - A neutral grey has no colour in it. Any error in the U and V terms shows
 *     up here as a tint, and a tint on grey is the single most visible way to
 *     get the matrix wrong.
 *   - Primaries land where they should. Swapping two planes still produces a
 *     plausible picture, and it is plausible right up until somebody notices
 *     everybody is blue.
 *   - Scaling preserves a flat colour exactly, at every size, in both
 *     directions. A resampler that drifts by a unit is invisible on a
 *     photograph and obvious on a test that says what it expects.
 *
 * Run with: cmake --build build && ./build/recon_video_tests
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_video.h"

static int g_failures;
static int g_checks;

static void check(bool condition, const char *what) {
    g_checks++;
    if (!condition) {
        g_failures++;
        printf("  FAIL: %s\n", what);
    }
}

/*
 * A picture of one colour, in planes this owns.
 *
 * Sized generously and with a stride that is deliberately *wider* than the
 * picture. Real decoders hand back padded rows -- libavcodec aligns them for
 * vector loads -- and code that assumes stride equals width works perfectly
 * on anything it allocates itself and skews every frame that comes out of a
 * decoder. It is the first thing to get wrong and the last thing to notice.
 */
#define PLANE_MAX 64
#define STRIDE (PLANE_MAX + 13)

struct flat {
    uint8_t y[STRIDE * PLANE_MAX];
    uint8_t u[STRIDE * PLANE_MAX];
    uint8_t v[STRIDE * PLANE_MAX];
    struct recon_video_picture picture;
};

static void make_flat(struct flat *f, int width, int height,
        enum recon_video_format format, enum recon_video_matrix matrix,
        bool full_range, int y, int u, int v) {
    memset(f->y, y, sizeof(f->y));
    memset(f->u, u, sizeof(f->u));
    memset(f->v, v, sizeof(f->v));

    memset(&f->picture, 0, sizeof(f->picture));
    f->picture.width = width;
    f->picture.height = height;
    f->picture.format = format;
    f->picture.matrix = matrix;
    f->picture.full_range = full_range;
    f->picture.plane[0] = f->y;
    f->picture.plane[1] = f->u;
    f->picture.plane[2] = f->v;
    f->picture.stride[0] = STRIDE;
    f->picture.stride[1] = STRIDE;
    f->picture.stride[2] = STRIDE;
}

/* Near enough, since the conversion is fixed point and the coefficients are
 * rounded decimals. Two units is under one part in a hundred and is invisible;
 * anything this is meant to catch is off by tens. */
static bool near(int a, int b, int slack) {
    int d = (a > b) ? a - b : b - a;
    return d <= slack;
}

static void colour_is(enum recon_video_matrix matrix, bool full_range,
        int y, int u, int v, int r, int g, int b, const char *what) {
    struct flat f;
    unsigned char out[8 * 8 * 4];

    make_flat(&f, 8, 8, RECON_VIDEO_YUV420, matrix, full_range, y, u, v);

    char label[160];
    if (!recon_video_render(&f.picture, out, 8, 8)) {
        snprintf(label, sizeof(label), "%s renders at all", what);
        check(false, label);
        return;
    }

    /* The middle pixel rather than the first: an edge pixel is the one place a
     * sampler can be wrong and still look right, because it clamps. */
    const unsigned char *p = out + (4 * 8 + 4) * 4;

    snprintf(label, sizeof(label), "%s gives r=%d g=%d b=%d (got %d %d %d)",
        what, r, g, b, p[0], p[1], p[2]);
    check(near(p[0], r, 3) && near(p[1], g, 3) && near(p[2], b, 3), label);

    snprintf(label, sizeof(label), "%s is opaque", what);
    check(p[3] == 255, label);
}

static void test_colours(void) {
    printf("Colours\n");

    /*
     * Studio range: black is 16, white is 235. Getting this wrong is the
     * commonest fault in a home-made player and it does not look like a fault
     * -- it looks like a washed-out video.
     */
    colour_is(RECON_VIDEO_BT601, false, 16, 128, 128, 0, 0, 0,
        "BT.601 studio black");
    colour_is(RECON_VIDEO_BT601, false, 235, 128, 128, 255, 255, 255,
        "BT.601 studio white");
    colour_is(RECON_VIDEO_BT709, false, 16, 128, 128, 0, 0, 0,
        "BT.709 studio black");
    colour_is(RECON_VIDEO_BT709, false, 235, 128, 128, 255, 255, 255,
        "BT.709 studio white");

    /* Full range: black is 0 and white is 255, with nothing to stretch. */
    colour_is(RECON_VIDEO_BT601, true, 0, 128, 128, 0, 0, 0,
        "full-range black");
    colour_is(RECON_VIDEO_BT601, true, 255, 128, 128, 255, 255, 255,
        "full-range white");

    /* Grey has no colour in it. Any error in the chroma terms is a tint here,
     * and a tint on grey is the most visible way to be wrong. */
    colour_is(RECON_VIDEO_BT601, true, 128, 128, 128, 128, 128, 128,
        "full-range mid grey stays grey");
    colour_is(RECON_VIDEO_BT709, true, 200, 128, 128, 200, 200, 200,
        "full-range light grey stays grey");

    /*
     * The primaries, which is where a swapped plane shows itself. U carries
     * blue and V carries red; exchanging them turns every warm colour cold and
     * still produces a picture somebody could look at without immediately
     * calling it broken.
     */
    struct flat f;
    unsigned char out[8 * 8 * 4];

    /*
     * Stated as dominance rather than as a threshold on each channel, because
     * the first version of this test asserted that blue went *down* when V went
     * up -- and it does not. With U left neutral at 128 the blue term is
     * exactly zero, so blue stays at whatever the brightness says. Two
     * failures, both of them the test being wrong about the arithmetic rather
     * than the arithmetic being wrong.
     *
     * What is actually true, and is the thing worth pinning: V drives red, U
     * drives blue, and each leaves the other where it was.
     */
    const unsigned char *p = out + (4 * 8 + 4) * 4;

    make_flat(&f, 8, 8, RECON_VIDEO_YUV420, RECON_VIDEO_BT601, true,
        128, 128, 255);
    check(recon_video_render(&f.picture, out, 8, 8), "V high renders");
    check(p[0] > 240 && p[0] > p[2] + 100, "V high is red, not blue");
    check(near(p[2], 128, 3), "V high leaves blue alone");

    make_flat(&f, 8, 8, RECON_VIDEO_YUV420, RECON_VIDEO_BT601, true,
        128, 255, 128);
    check(recon_video_render(&f.picture, out, 8, 8), "U high renders");
    check(p[2] > 240 && p[2] > p[0] + 100, "U high is blue, not red");
    check(near(p[0], 128, 3), "U high leaves red alone");
}

/* --- Scaling --- */

/*
 * A flat colour must survive any scale factor exactly.
 *
 * This is the property that catches a resampler reading outside its plane: the
 * padding beyond a row is not the colour, so a sampler that runs one pixel past
 * the edge produces a wrong pixel on exactly one column -- and only on the
 * column at the edge, which is the one place a person's eye skips.
 */
static void flat_survives(enum recon_video_format format, int in_w, int in_h,
        int out_w, int out_h) {
    struct flat f;
    make_flat(&f, in_w, in_h, format, RECON_VIDEO_BT601, true, 128, 128, 128);

    unsigned char *out = malloc((size_t)out_w * out_h * 4);
    char what[160];
    snprintf(what, sizeof(what), "%dx%d to %dx%d stays one flat colour",
        in_w, in_h, out_w, out_h);

    if (out == NULL || !recon_video_render(&f.picture, out, out_w, out_h)) {
        check(false, what);
        free(out);
        return;
    }

    bool same = true;
    int worst = 0;
    for (int i = 0; i < out_w * out_h; i++) {
        for (int c = 0; c < 3; c++) {
            int d = out[(size_t)i * 4 + c] - 128;
            if (d < 0) {
                d = -d;
            }
            if (d > worst) {
                worst = d;
            }
            if (d > 2) {
                same = false;
            }
        }
    }
    if (!same) {
        snprintf(what, sizeof(what),
            "%dx%d to %dx%d stays flat (worst pixel off by %d)",
            in_w, in_h, out_w, out_h, worst);
    }
    check(same, what);
    free(out);
}

static void test_scaling(void) {
    printf("Scaling\n");

    /* Reduced, enlarged, unchanged, and by ratios that do not divide -- the
     * last is where an off-by-one in the span arithmetic hides, because every
     * whole-number ratio happens to come out right anyway. */
    flat_survives(RECON_VIDEO_YUV420, 32, 32, 32, 32);
    flat_survives(RECON_VIDEO_YUV420, 32, 32, 16, 16);
    flat_survives(RECON_VIDEO_YUV420, 32, 32, 64, 64);
    flat_survives(RECON_VIDEO_YUV420, 32, 32, 7, 5);
    flat_survives(RECON_VIDEO_YUV420, 32, 32, 45, 13);
    flat_survives(RECON_VIDEO_YUV420, 32, 32, 1, 1);

    /* Odd sizes, where 4:2:0 has a half-used colour sample in the last column
     * and the last row. Rounding the chroma plane down loses it and reads one
     * sample short for every pixel in that column. */
    flat_survives(RECON_VIDEO_YUV420, 31, 17, 15, 9);
    flat_survives(RECON_VIDEO_YUV420, 5, 3, 20, 12);

    /* The other two subsamplings, which differ only by a shift -- and a shift
     * applied to the wrong axis is a picture that is right in one direction. */
    flat_survives(RECON_VIDEO_YUV422, 32, 32, 20, 20);
    flat_survives(RECON_VIDEO_YUV444, 32, 32, 20, 20);
    flat_survives(RECON_VIDEO_YUV422, 31, 31, 63, 63);
    flat_survives(RECON_VIDEO_YUV444, 31, 31, 63, 63);
}

/* --- Fitting --- */

static void fits(int pw, int ph, int bw, int bh, int want_w, int want_h) {
    int w = 0, h = 0;
    recon_video_fit(pw, ph, bw, bh, &w, &h);

    char what[160];
    snprintf(what, sizeof(what), "%dx%d in %dx%d is %dx%d (got %dx%d)",
        pw, ph, bw, bh, want_w, want_h, w, h);
    check(w == want_w && h == want_h, what);
}

static void test_fit(void) {
    printf("Fitting\n");

    /* Wider than the box: width decides. */
    fits(1920, 1080, 640, 640, 640, 360);
    /* Taller than the box: height decides. */
    fits(1080, 1920, 640, 640, 360, 640);
    /* Square in square. */
    fits(1080, 1080, 400, 400, 400, 400);
    /* Exactly the box's shape, which must not be nudged by rounding. */
    fits(1920, 1080, 960, 540, 960, 540);

    /*
     * A box so thin that keeping the ratio would round the other side to
     * nothing. One pixel rather than zero, because zero is a division by zero
     * further down rather than a very small picture -- and a window can be
     * dragged this narrow by accident.
     */
    int w = 0, h = 0;
    recon_video_fit(1920, 1080, 1, 400, &w, &h);
    check(w >= 1 && h >= 1, "a box one pixel wide still gives a size");

    /* Nothing to fit is not a crash. */
    recon_video_fit(0, 0, 100, 100, &w, &h);
    check(w == 0 && h == 0, "no picture fits to nothing");
}

/* --- What must be refused --- */

static void test_refusals(void) {
    printf("Refusals\n");

    struct flat f;
    unsigned char out[16 * 16 * 4];
    make_flat(&f, 8, 8, RECON_VIDEO_YUV420, RECON_VIDEO_BT601, true,
        128, 128, 128);

    check(!recon_video_render(NULL, out, 8, 8), "no picture is refused");
    check(!recon_video_render(&f.picture, NULL, 8, 8), "no buffer is refused");
    check(!recon_video_render(&f.picture, out, 0, 8), "zero width is refused");
    check(!recon_video_render(&f.picture, out, 8, -1),
        "negative height is refused");

    /*
     * A picture missing a plane, which is what a decoder hands back when it
     * produced a format this cannot express. Refusing is the point: rendering
     * it would read from a null pointer, and the alternative to reading from a
     * null pointer is not "render it anyway".
     */
    f.picture.plane[1] = NULL;
    check(!recon_video_render(&f.picture, out, 8, 8),
        "a missing colour plane is refused");

    make_flat(&f, 8, 8, RECON_VIDEO_YUV420, RECON_VIDEO_BT601, true,
        128, 128, 128);
    f.picture.stride[2] = 0;
    check(!recon_video_render(&f.picture, out, 8, 8),
        "a plane with no stride is refused");

    make_flat(&f, 0, 0, RECON_VIDEO_YUV420, RECON_VIDEO_BT601, true,
        128, 128, 128);
    check(!recon_video_render(&f.picture, out, 8, 8),
        "a picture with no size is refused");
}

int main(void) {
    printf("Video tests\n\n");

    test_colours();
    test_scaling();
    test_fit();
    test_refusals();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
