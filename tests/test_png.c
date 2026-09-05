/*
 * The PNG writer, checked by reading its output back.
 *
 * Written when Photos gained a Save as PNG button, because that button makes a
 * claim -- that the picture comes out the other side unchanged -- and the
 * encoder had until then only ever been checked by a person looking at a
 * screenshot and finding it looked right. A picture can look right and have
 * every red and blue swapped, or an alpha channel quietly flattened, and a
 * person will not see it on a photograph of a desktop.
 *
 * So the check is a round trip: encode with the writer, decode with the reader
 * that the rest of the system uses on files it did not make, and compare every
 * pixel. Agreement between the two halves is the property that matters -- the
 * one that means a file saved here opens elsewhere as the same picture.
 *
 * stb_image is the decoder on the other end deliberately. It is a different
 * implementation by a different author, so agreeing with it is evidence about
 * the format rather than evidence that one piece of code is self-consistent.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "recon_png.h"

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

/* --- A picture with something in every channel --- */

/*
 * Deliberately not a flat colour or a grey ramp.
 *
 * A picture whose red, green and blue are equal survives having two of them
 * swapped, which is the mistake this file exists to catch. Every channel here
 * varies differently across the picture, and alpha varies too, so any pair
 * exchanged or any channel dropped shows up as a difference somewhere.
 */
static void paint(unsigned int *pixels, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned int r = (unsigned int)(x * 255 / (w - 1));
            unsigned int g = (unsigned int)(y * 255 / (h - 1));
            unsigned int b = (unsigned int)((x + y * 2) % 256);
            unsigned int a = (unsigned int)(255 - (x + y) % 256);
            pixels[y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

/* Encode, decode, and report the first pixel that came back different. */
static bool round_trip(const unsigned int *pixels, int w, int h,
        bool with_alpha, char *why, size_t why_size) {
    size_t size = 0;
    unsigned char *png = recon_png_encode(pixels, w, h, with_alpha, &size);
    if (png == NULL) {
        snprintf(why, why_size, "encode said no: %s", recon_png_last_error());
        return false;
    }

    int got_w = 0, got_h = 0, had = 0;
    unsigned char *back = stbi_load_from_memory(png, (int)size, &got_w, &got_h,
        &had, 4);
    free(png);

    if (back == NULL) {
        snprintf(why, why_size, "the decoder would not open it: %s",
            stbi_failure_reason());
        return false;
    }
    if (got_w != w || got_h != h) {
        snprintf(why, why_size, "came back %dx%d, not %dx%d", got_w, got_h, w,
            h);
        stbi_image_free(back);
        return false;
    }

    for (int i = 0; i < w * h; i++) {
        unsigned int want = pixels[i];
        unsigned int want_a = with_alpha ? (want >> 24) & 0xFF : 255;
        const unsigned char *px = back + i * 4;

        if (px[0] != ((want >> 16) & 0xFF) || px[1] != ((want >> 8) & 0xFF) ||
                px[2] != (want & 0xFF) || px[3] != want_a) {
            snprintf(why, why_size,
                "pixel %d,%d: wrote %02X%02X%02X%02X, read %02X%02X%02X%02X",
                i % w, i / w, want_a, (want >> 16) & 0xFF, (want >> 8) & 0xFF,
                want & 0xFF, px[3], px[0], px[1], px[2]);
            stbi_image_free(back);
            return false;
        }
    }

    stbi_image_free(back);
    return true;
}

static void test_round_trip(void) {
    printf("A picture written and read back\n");

    char why[256] = "";
    int w = 61, h = 37;              /* odd numbers: no lucky alignment */
    unsigned int *pixels = malloc((size_t)w * h * sizeof(*pixels));
    if (pixels == NULL) {
        check(false, "there was memory for a test picture");
        return;
    }
    paint(pixels, w, h);

    bool ok = round_trip(pixels, w, h, true, why, sizeof(why));
    if (!ok) {
        printf("        %s\n", why);
    }
    check(ok, "every pixel and its alpha survive the round trip");

    /*
     * And without alpha, which is what a screenshot is written as. The colours
     * have to be identical and the alpha has to come back opaque -- a channel
     * that was dropped on purpose, not one that was lost.
     */
    ok = round_trip(pixels, w, h, false, why, sizeof(why));
    if (!ok) {
        printf("        %s\n", why);
    }
    check(ok, "and without alpha the colours are the same and it reads opaque");

    free(pixels);
}

static void test_shapes(void) {
    printf("Awkward shapes\n");

    char why[256] = "";
    unsigned int one = 0xFF804020u;

    bool ok = round_trip(&one, 1, 1, true, why, sizeof(why));
    if (!ok) {
        printf("        %s\n", why);
    }
    check(ok, "a picture one pixel across");

    /*
     * A single row and a single column. Both are the cases where a filter that
     * looks at the pixel above or to the left has nothing to look at, and both
     * are shapes a person can produce by cropping.
     */
    unsigned int line[64];
    for (int i = 0; i < 64; i++) {
        line[i] = 0xFF000000u | (unsigned int)(i * 0x040201);
    }

    ok = round_trip(line, 64, 1, true, why, sizeof(why));
    if (!ok) {
        printf("        %s\n", why);
    }
    check(ok, "one row");

    ok = round_trip(line, 1, 64, true, why, sizeof(why));
    if (!ok) {
        printf("        %s\n", why);
    }
    check(ok, "one column");
}

static void test_refusals(void) {
    printf("What it will not do\n");

    unsigned int one = 0xFFFFFFFFu;
    size_t size = 1;

    check(recon_png_encode(NULL, 4, 4, true, &size) == NULL,
        "no pixels is refused");
    check(recon_png_encode(&one, 0, 4, true, &size) == NULL,
        "a picture no pixels wide is refused");
    check(recon_png_encode(&one, 4, -1, true, &size) == NULL,
        "a negative height is refused");

    /* Refused rather than written short. A caller that trusts size_out on a
     * failure would otherwise write whatever the last success left there. */
    check(size == 0, "and a refusal leaves no length behind");
}

int main(void) {
    printf("PNG\n\n");

    test_round_trip();
    test_shapes();
    test_refusals();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
