/*
 * Which picture, when. See include/recon_movie.h.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_codec.h"
#include "recon_movie.h"
#include "recon_mp4.h"
#include "recon_video.h"

/*
 * How many pictures one call to advance() will decode before giving up.
 *
 * Not a performance tuning knob -- a guarantee that the event loop comes back.
 * A seek can legitimately need to decode a hundred frames from a keyframe, and
 * a damaged file can ask for all of them forever. Fifteen hundred is far more
 * than any real distance between keyframes and far less than a hang somebody
 * would have to kill the desktop to escape.
 */
#define DECODE_LIMIT 1500

struct recon_movie {
    struct recon_mp4 *mp4;
    int track;

    const struct recon_codec_video *codec;
    struct recon_codec_video_reader *reader;
    char format[RECON_CODEC_NAME_MAX];

    int width, height;              /* the picture's own size */
    double duration;

    /* Read once. Asking the track for them per sample is a walk of the box
     * tree's worth of work to be told the same two numbers thirty times a
     * second. */
    const uint8_t *bytes;
    size_t byte_count;
    uint32_t timescale;

    int next_sample;
    int sample_count;
    bool fed_everything;            /* every sample has gone in */
    bool drained;                   /* and everything held has come out */

    /*
     * A picture that was decoded before it was due.
     *
     * Its planes point into the decoder and stay valid precisely because
     * nothing is decoded between storing it and using it. That is the one
     * invariant in this file worth stating out loud, and every early return
     * below is written to keep it.
     */
    struct recon_video_picture pending;
    bool has_pending;

    /* Frames earlier than this are decoded for their side effects only: they
     * are what the frame we actually want was predicted from. */
    double render_from;

    unsigned char *rgba;
    int out_width, out_height;
    bool has_picture;
};

static char g_error[192];

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_movie_last_error(void) {
    return g_error;
}

/* --- Opening --- */

struct recon_movie *recon_movie_open(const uint8_t *bytes, size_t length) {
    g_error[0] = '\0';

    struct recon_mp4 *mp4 = recon_mp4_open(bytes, length);
    if (mp4 == NULL) {
        fail("%s", recon_mp4_last_error());
        return NULL;
    }

    int track = recon_mp4_first(mp4, RECON_MP4_VIDEO);
    if (track < 0) {
        recon_mp4_close(mp4);
        fail("There is no picture in this file, only sound.");
        return NULL;
    }

    struct recon_mp4_track info;
    if (!recon_mp4_track_at(mp4, track, &info)) {
        recon_mp4_close(mp4);
        fail("This file's video track cannot be read.");
        return NULL;
    }

    /*
     * The decoder is asked for by name, and when it is missing the name is what
     * gets said. "There is no H.264 decoder installed" is something somebody
     * can act on; "cannot play this file" is not.
     */
    const struct recon_codec_video *codec = recon_codec_video_for(info.name);
    if (codec == NULL) {
        recon_mp4_close(mp4);
        fail("This is %s video, and there is no %s decoder installed. "
            "The codec pack adds one.", info.name, info.name);
        return NULL;
    }

    if (info.setup == NULL || info.setup_length == 0) {
        recon_mp4_close(mp4);
        fail("This file's video track carries no parameter sets, so nothing "
            "can start decoding it.");
        return NULL;
    }

    struct recon_movie *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        recon_mp4_close(mp4);
        fail("Out of memory.");
        return NULL;
    }

    m->mp4 = mp4;
    m->track = track;
    m->codec = codec;
    m->width = info.width;
    m->height = info.height;
    m->sample_count = info.sample_count;
    snprintf(m->format, sizeof(m->format), "%s", info.name);

    recon_mp4_bytes(mp4, &m->bytes, &m->byte_count);
    m->timescale = info.timescale;

    m->duration = (info.timescale > 0) ?
        (double)info.duration / (double)info.timescale :
        recon_mp4_duration(mp4);

    m->reader = codec->open(info.setup, info.setup_length,
        &m->width, &m->height);
    if (m->reader == NULL) {
        recon_mp4_close(mp4);
        free(m);
        fail("The %s decoder would not start on this file.", info.name);
        return NULL;
    }

    if (m->width <= 0 || m->height <= 0) {
        codec->close(m->reader);
        recon_mp4_close(mp4);
        free(m);
        fail("This file does not say how large its picture is.");
        return NULL;
    }
    return m;
}

void recon_movie_close(struct recon_movie *m) {
    if (m == NULL) {
        return;
    }
    if (m->reader != NULL && m->codec != NULL) {
        m->codec->close(m->reader);
    }
    recon_mp4_close(m->mp4);
    free(m->rgba);
    free(m);
}

void recon_movie_size(const struct recon_movie *m, int *width, int *height) {
    if (width != NULL) {
        *width = (m != NULL) ? m->width : 0;
    }
    if (height != NULL) {
        *height = (m != NULL) ? m->height : 0;
    }
}

double recon_movie_duration(const struct recon_movie *m) {
    return (m != NULL) ? m->duration : 0.0;
}

const char *recon_movie_format(const struct recon_movie *m) {
    return (m != NULL) ? m->format : "";
}

bool recon_movie_finished(const struct recon_movie *m) {
    return (m != NULL) && m->drained && !m->has_pending;
}

/* --- The buffer pictures are converted into --- */

bool recon_movie_set_size(struct recon_movie *m, int width, int height) {
    if (m == NULL || width <= 0 || height <= 0) {
        return false;
    }
    if (m->rgba != NULL && width == m->out_width && height == m->out_height) {
        return true;
    }

    size_t bytes = (size_t)width * (size_t)height * 4;
    unsigned char *fresh = calloc(1, bytes);
    if (fresh == NULL) {
        return false;
    }

    free(m->rgba);
    m->rgba = fresh;
    m->out_width = width;
    m->out_height = height;

    /*
     * The picture that was there is the wrong size now, and there is no way to
     * get it back: the planes it was made from belong to the decoder and have
     * been reused several times since. So the window shows nothing until the
     * next frame is due -- at most a fortieth of a second while playing, and
     * for as long as it stays paused otherwise.
     *
     * Stretching the old pixels to the new size would hide that, and would look
     * exactly like a video that had gone soft.
     */
    m->has_picture = false;
    return true;
}

const unsigned char *recon_movie_pixels(const struct recon_movie *m) {
    if (m == NULL || !m->has_picture) {
        return NULL;
    }
    return m->rgba;
}

void recon_movie_pixel_size(const struct recon_movie *m,
        int *width, int *height) {
    if (width != NULL) {
        *width = (m != NULL) ? m->out_width : 0;
    }
    if (height != NULL) {
        *height = (m != NULL) ? m->out_height : 0;
    }
}

static bool render(struct recon_movie *m,
        const struct recon_video_picture *picture) {
    if (m->rgba == NULL || m->out_width <= 0 || m->out_height <= 0) {
        return false;
    }
    if (!recon_video_render(picture, m->rgba, m->out_width, m->out_height)) {
        return false;
    }
    m->has_picture = true;
    return true;
}

/* --- Decoding --- */

/*
 * One picture out of the decoder, or false when there will be no more.
 *
 * Samples go in until they run out, and then the decoder is drained: several
 * pictures are usually still inside it, held back because the order they are
 * stored in is not the order they are shown in. Skipping the drain loses the
 * last handful of frames of every file, which looks like a video that stops
 * slightly early rather than like a bug.
 */
static bool decode_one(struct recon_movie *m,
        struct recon_video_picture *out) {
    while (!m->fed_everything) {
        if (m->next_sample >= m->sample_count) {
            m->fed_everything = true;
            break;
        }

        size_t offset = 0, length = 0;
        uint64_t start = 0;
        int index = m->next_sample++;

        if (!recon_mp4_sample(m->mp4, m->track, index, &offset, &length,
                &start)) {
            m->fed_everything = true;
            break;
        }

        if (m->bytes == NULL || m->timescale == 0 ||
                offset + length > m->byte_count) {
            m->fed_everything = true;
            break;
        }

        double when = (double)start / (double)m->timescale;
        int rc = m->codec->decode(m->reader, m->bytes + offset, length, when,
            out);
        if (rc > 0) {
            return true;
        }
        /*
         * Zero means the decoder kept it, which is ordinary and not an end.
         * Negative means this one frame would not decode -- a damaged sample --
         * and the right answer is the next frame rather than the end of the
         * file, because one bad frame in the middle of a good file is exactly
         * what a partial download looks like.
         */
    }

    if (m->drained || m->codec->flush == NULL) {
        m->drained = true;
        return false;
    }
    if (m->codec->flush(m->reader, out) > 0) {
        return true;
    }
    m->drained = true;
    return false;
}

bool recon_movie_advance(struct recon_movie *m, double seconds) {
    if (m == NULL || m->reader == NULL) {
        return false;
    }

    bool changed = false;

    for (int guard = 0; guard < DECODE_LIMIT; guard++) {
        /* Something held back from an earlier call, now due? Its planes are
         * still the decoder's and are still valid, because reaching here means
         * nothing has been decoded since it was stored. */
        if (m->has_pending) {
            if (m->pending.time > seconds) {
                break;
            }
            if (m->pending.time >= m->render_from && render(m, &m->pending)) {
                changed = true;
            }
            m->has_pending = false;
            continue;
        }

        struct recon_video_picture picture;
        memset(&picture, 0, sizeof(picture));
        if (!decode_one(m, &picture)) {
            break;
        }

        if (picture.time > seconds) {
            /* Early. Keep it, and stop -- decoding anything else would throw
             * away the planes this one points at. */
            m->pending = picture;
            m->has_pending = true;
            break;
        }

        if (picture.time >= m->render_from && render(m, &picture)) {
            changed = true;
        }
    }

    return changed;
}

bool recon_movie_seek(struct recon_movie *m, double seconds) {
    if (m == NULL || m->reader == NULL) {
        return false;
    }
    if (seconds < 0.0) {
        seconds = 0.0;
    }

    int want = recon_mp4_sample_at_time(m->mp4, m->track, seconds);
    if (want < 0) {
        return false;
    }
    int from = recon_mp4_sync_sample(m->mp4, m->track, want);
    if (from < 0) {
        return false;
    }

    if (m->codec->reset != NULL) {
        m->codec->reset(m->reader);
    }

    /*
     * The pending picture is dropped rather than kept.
     *
     * reset() has just thrown away the decoder's internal buffers, so whatever
     * that picture's planes pointed at is gone. Keeping the struct because its
     * numbers still look reasonable would leave a pointer into freed memory
     * that only gets read when somebody seeks and then waits -- which is to say
     * almost never, and never while anybody is looking for it.
     */
    m->has_pending = false;
    m->next_sample = from;
    m->fed_everything = false;
    m->drained = false;

    /*
     * Everything between the keyframe and the target is decoded for what it
     * teaches the decoder, and not shown. The line between the two is the
     * target frame's *own* start time, not the time that was asked for.
     *
     * Those are different and the difference is a bug this had. A frame covers
     * an interval, so the frame containing 33.7 seconds starts at, say, 33.667.
     * Refusing to show anything before 33.7 refuses that frame -- and the next
     * one does not start until 33.7 either, so the seek produced no picture at
     * all. It worked perfectly for every whole number of seconds, because those
     * happen to land on frame boundaries, and a test written with round numbers
     * in it never saw the fault once.
     */
    uint64_t start = 0;
    if (recon_mp4_sample(m->mp4, m->track, want, NULL, NULL, &start) &&
            m->timescale > 0) {
        m->render_from = (double)start / (double)m->timescale;
    } else {
        m->render_from = seconds;
    }
    return true;
}
