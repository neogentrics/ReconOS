/*
 * Resampling, checked against properties rather than against a fixture.
 *
 * A fixture for a scaler is a picture somebody produced with the scaler, which
 * proves the scaler agrees with itself. What can be checked without one is
 * everything that has to be true of any correct resampler: the size asked for,
 * a flat colour surviving, a gradient staying a gradient, alpha coming along,
 * and scaling by one changing nothing at all.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_image.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what) {
    checks++;
    if (ok) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static unsigned char *flat(int w, int h, int r, int g, int b, int a) {
    unsigned char *px = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        px[i * 4 + 0] = (unsigned char)r;
        px[i * 4 + 1] = (unsigned char)g;
        px[i * 4 + 2] = (unsigned char)b;
        px[i * 4 + 3] = (unsigned char)a;
    }
    return px;
}

/* Dark on the left, light on the right, and nothing else. */
static unsigned char *ramp(int w, int h) {
    unsigned char *px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char v = (unsigned char)(w > 1 ? x * 255 / (w - 1) : 0);
            unsigned char *p = px + ((size_t)y * w + x) * 4;
            p[0] = p[1] = p[2] = v;
            p[3] = 255;
        }
    }
    return px;
}

static void test_refusals(void) {
    printf("What it will not do\n");

    unsigned char one[4] = { 1, 2, 3, 4 };
    unsigned char out[4];

    check(!recon_image_scale(NULL, 1, 1, out, 1, 1), "no source is refused");
    check(!recon_image_scale(one, 1, 1, NULL, 1, 1),
        "no destination is refused");
    check(!recon_image_scale(one, 0, 1, out, 1, 1), "a source with no width");
    check(!recon_image_scale(one, 1, 1, out, 1, 0), "an output with no height");
}

static void test_same_size(void) {
    printf("Scaling by one\n");

    int w = 17, h = 11;
    unsigned char *src = ramp(w, h);
    unsigned char *dst = malloc((size_t)w * h * 4);

    check(recon_image_scale(src, w, h, dst, w, h),
        "a picture scales to its own size");

    /*
     * Identical, not merely close.
     *
     * Neither branch is chosen at equal sizes -- shrinking is tested as
     * "smaller than" and growing is its complement -- so this is the check
     * that the boundary between them lands on doing nothing, rather than on a
     * resample that happens to come out nearly right.
     */
    check(memcmp(src, dst, (size_t)w * h * 4) == 0,
        "and comes out identical, byte for byte");

    free(src);
    free(dst);
}

static void test_flat_stays_flat(void) {
    printf("A flat colour\n");

    /*
     * Every combination of bigger and smaller on each axis, because the two
     * axes choose their resampler separately and a picture can go both ways
     * at once.
     */
    struct { int w, h; const char *what; } sizes[] = {
        { 40, 40, "enlarged both ways" },
        { 5, 5, "reduced both ways" },
        { 40, 5, "wider and shorter at the same time" },
        { 5, 40, "narrower and taller at the same time" },
        { 1, 1, "down to a single pixel" },
    };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        unsigned char *src = flat(16, 16, 200, 100, 50, 255);
        unsigned char *dst = malloc((size_t)sizes[i].w * sizes[i].h * 4);
        recon_image_scale(src, 16, 16, dst, sizes[i].w, sizes[i].h);

        bool same = true;
        for (int j = 0; j < sizes[i].w * sizes[i].h; j++) {
            if (dst[j * 4] != 200 || dst[j * 4 + 1] != 100 ||
                    dst[j * 4 + 2] != 50 || dst[j * 4 + 3] != 255) {
                same = false;
                break;
            }
        }
        check(same, sizes[i].what);

        free(src);
        free(dst);
    }
}

static void test_a_gradient_stays_a_gradient(void) {
    printf("A gradient\n");

    unsigned char *src = ramp(64, 8);

    /*
     * Enlarged: still dark to light and still never going backwards. Repeating
     * pixels instead of interpolating would also be monotonic, so the number
     * of distinct values is checked as well -- that is the difference between
     * a larger picture and a picture of squares.
     */
    int w = 200;
    unsigned char *big = malloc((size_t)w * 8 * 4);
    recon_image_scale(src, 64, 8, big, w, 8);

    bool rising = true;
    int distinct = 1;
    for (int x = 1; x < w; x++) {
        int a = big[(x - 1) * 4];
        int b = big[x * 4];
        if (b < a) {
            rising = false;
        }
        if (b != a) {
            distinct++;
        }
    }
    check(rising, "enlarged, it never goes backwards");
    check(distinct > 64, "and has more steps than the picture it came from");

    /*
     * Reduced: an averaging resampler pulls the extremes in a little, so what
     * is checked is the direction and the span rather than exact values.
     */
    unsigned char small[16 * 8 * 4];
    recon_image_scale(src, 64, 8, small, 16, 8);
    check(small[0] < small[15 * 4], "reduced, it still runs dark to light");
    check(small[15 * 4] - small[0] > 200, "and still covers most of the range");

    free(src);
    free(big);
}

static void test_alpha_comes_along(void) {
    printf("Alpha\n");

    /* Clear on the left, solid on the right. */
    int w = 32, h = 4;
    unsigned char *src = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char *p = src + ((size_t)y * w + x) * 4;
            p[0] = p[1] = p[2] = 128;
            p[3] = (unsigned char)(x * 255 / (w - 1));
        }
    }

    unsigned char out[8 * 4 * 4];
    recon_image_scale(src, w, h, out, 8, 4);

    check(out[3] < out[7 * 4 + 3],
        "a picture that fades from clear to solid still does");
    check(out[3] < 60 && out[7 * 4 + 3] > 195,
        "and both ends are still near where they were");

    free(src);
}

static void test_fitting(void) {
    printf("Fitting inside a box\n");

    int w = 0, h = 0;

    recon_image_fit(1000, 500, 100, 100, &w, &h);
    check(w == 100 && h == 50, "a wide picture fits by its width");

    recon_image_fit(500, 1000, 100, 100, &w, &h);
    check(w == 50 && h == 100, "a tall one fits by its height");

    recon_image_fit(100, 100, 40, 80, &w, &h);
    check(w == 40 && h == 40, "a square in a tall box stays square");

    recon_image_fit(1000, 3, 100, 100, &w, &h);
    check(w == 100 && h >= 1, "and nothing is ever fitted to nothing");

    recon_image_fit(0, 5, 10, 10, &w, &h);
    check(w == 0 && h == 0, "a picture with no width fits nowhere");
}

int main(void) {
    printf("Resampling\n\n");

    test_refusals();
    test_same_size();
    test_flat_stays_flat();
    test_a_gradient_stays_a_gradient();
    test_alpha_comes_along();
    test_fitting();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
