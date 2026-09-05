/*
 * Turning a decoded picture into pixels somebody can look at.
 *
 * --- Why this is ours and the decoder is not ---
 *
 * THIRD_PARTY.md's line is "libraries may parse formats and talk to hardware",
 * and H.264 is a format: seven hundred pages of it, where a decoder that is
 * ninety-five per cent correct does not look nearly right, it looks broken. So
 * the decoding is borrowed, in a module that can be taken off again.
 *
 * What comes out of that decoder is not a picture yet. It is three planes of
 * brightness and colour at different resolutions, in a colour space that is not
 * the screen's, at a size that is not the window's. Turning that into something
 * to look at is *arithmetic*, not format parsing -- there is no specification to
 * get wrong, only sums -- and arithmetic is exactly the kind of thing ReconOS
 * writes for itself. Borrowing it as well would mean pulling in a scaling
 * library to avoid writing a page of multiplies.
 *
 * That is the line, and this file is the ReconOS side of it.
 *
 * --- Why converting and scaling are one pass ---
 *
 * The obvious shape is two steps: convert the frame to RGB, then scale the RGB.
 * It is also the wrong shape, and by a lot. A 1920x1080 frame shown in a
 * 640x360 window is two million pixels converted so that two hundred thousand
 * can be kept -- nine tenths of the work thrown away, thirty times a second.
 *
 * Going straight from the planes to the destination size does the colour
 * conversion once per pixel that will actually be *seen*. The scale factor stops
 * being a cost and starts being a saving, which is the right way round: a video
 * in a small window should be cheaper than the same video in a large one.
 */

#ifndef RECON_VIDEO_H
#define RECON_VIDEO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How the colour planes are sampled relative to the brightness plane.
 *
 * Human vision resolves brightness far better than colour, so every one of
 * these keeps the Y plane at full size and shrinks U and V. 4:2:0 -- half in
 * both directions, a quarter of the samples -- is what essentially all consumer
 * H.264 uses; the other two are here because supporting them is a shift rather
 * than a code path, and refusing a file over a one-line difference would be a
 * gap nobody could explain.
 */
enum recon_video_format {
    RECON_VIDEO_YUV420,     /* chroma half width, half height */
    RECON_VIDEO_YUV422,     /* chroma half width, full height */
    RECON_VIDEO_YUV444,     /* chroma at full resolution */
};

/*
 * Which set of coefficients turns Y, U and V back into red, green and blue.
 *
 * These are not interchangeable and the difference is visible: using the
 * standard-definition numbers on a high-definition picture pulls greens towards
 * yellow and makes skin tones ruddy. It is subtle enough to look like a
 * badly-graded video rather than a bug, which is exactly why it is worth
 * carrying the flag rather than picking one and hoping.
 */
enum recon_video_matrix {
    RECON_VIDEO_BT601,      /* standard definition, and most small video */
    RECON_VIDEO_BT709,      /* high definition */
};

/*
 * One decoded picture, borrowed.
 *
 * The planes point into the decoder's own memory and are only valid until the
 * next frame is asked for. Nothing here copies them: a 1080p frame is three
 * megabytes and copying it on the way past would cost more than the conversion
 * that follows.
 */
struct recon_video_picture {
    int width, height;
    enum recon_video_format format;
    enum recon_video_matrix matrix;

    /*
     * Whether Y uses the whole 0-255 range or the broadcast 16-235.
     *
     * Studio range exists because analogue equipment needed room to overshoot
     * at both ends. It is still the default in video files, and treating it as
     * full range gives a picture with grey blacks and washed-out whites -- the
     * commonest way a home-made video player looks subtly wrong.
     */
    bool full_range;

    const uint8_t *plane[3];    /* Y, U, V */
    int stride[3];              /* bytes per row, which is not width */

    /* When this picture should be shown, in seconds from the start. */
    double time;
};

/*
 * Convert and scale in one pass, into `rgba` at exactly the size given.
 *
 * `rgba` is out_width * out_height * 4 bytes, in the order recon_draw_image
 * takes, with alpha set to opaque throughout -- a video frame has nothing
 * behind it to show through.
 *
 * False when the picture is malformed or the output size is not positive.
 */
bool recon_video_render(const struct recon_video_picture *picture,
    unsigned char *rgba, int out_width, int out_height);

/*
 * The largest box of the picture's shape that fits inside the given one.
 *
 * Aspect ratio is kept, always. A video stretched to fill a window is a video
 * with everybody in it the wrong shape, and there is no setting for it because
 * there is no answer anybody wants.
 */
void recon_video_fit(int picture_width, int picture_height,
    int box_width, int box_height, int *out_width, int *out_height);

#endif /* RECON_VIDEO_H */
