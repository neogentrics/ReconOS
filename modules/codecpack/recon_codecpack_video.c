/*
 * H.264 and H.265, through libavcodec. The other half of the codec pack.
 *
 * --- What is borrowed and what is not ---
 *
 * This file does exactly one thing: compressed bytes in, three planes out. It
 * does not convert colour, it does not scale, it does not decide when a picture
 * should be shown, and it does not read the container. libavcodec can do all
 * four and none of them are used, because all four are arithmetic or structure
 * and both of those are things ReconOS writes for itself.
 *
 * That is not fastidiousness for its own sake. The reason to keep a borrowed
 * dependency at exactly the size of the thing that justifies it is that the
 * justification has to stay checkable. "We use libavcodec because H.264 is
 * seven hundred pages" is a claim somebody can weigh. "We use libavcodec"
 * is not.
 *
 * --- Why there is a queue in here ---
 *
 * The interface above hands over one frame and takes back one picture. A
 * decoder does not work that way: it can swallow six frames and then produce
 * six pictures at once, because the order they are stored in is not the order
 * they are shown in and it has to have seen the later ones to emit the earlier.
 *
 * So every packet is sent, everything the decoder will give back is taken
 * immediately, and the results are handed out one at a time in order. The
 * alternative -- returning early while a packet is still unsent -- silently
 * drops frames, and drops them only on files with B-frames, which is to say on
 * files from a real encoder and not on any test clip anybody makes by hand.
 */

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>

#include "recon_codec.h"
#include "recon_codecpack.h"
#include "recon_video.h"

/*
 * More than any H.264 packet can produce, which is the property that matters.
 *
 * Draining completely after every send is what keeps avcodec_send_packet from
 * ever refusing a packet, and it only stays complete while the queue is larger
 * than the largest burst. A packet decodes to one picture, or to two for
 * field-coded content; sixteen is not a tuning choice, it is headroom.
 */
#define QUEUE_MAX 16

/* Timestamps go through libavcodec in microseconds. Integers, because a
 * timebase is a ratio of integers and handing it a rounded double back and
 * forth accumulates exactly the drift this is meant to avoid. */
#define TIME_BASE 1000000

struct recon_codec_video_reader {
    AVCodecContext *context;
    AVPacket *packet;

    AVFrame *queue[QUEUE_MAX];
    int head;
    int count;

    /* Handed out last time. Freed at the start of the next call, which is what
     * makes the "valid until the next call" promise in recon_codec.h true
     * rather than merely usual. */
    AVFrame *current;

    bool flushed;                   /* the end-of-stream packet has gone in */
};

/* --- Describing what came out --- */

static bool plane_format(int pix_fmt, enum recon_video_format *out,
        bool *full_range) {
    switch (pix_fmt) {
    case AV_PIX_FMT_YUVJ420P:
        *full_range = true;
        *out = RECON_VIDEO_YUV420;
        return true;
    case AV_PIX_FMT_YUV420P:
        *out = RECON_VIDEO_YUV420;
        return true;
    case AV_PIX_FMT_YUVJ422P:
        *full_range = true;
        *out = RECON_VIDEO_YUV422;
        return true;
    case AV_PIX_FMT_YUV422P:
        *out = RECON_VIDEO_YUV422;
        return true;
    case AV_PIX_FMT_YUVJ444P:
        *full_range = true;
        *out = RECON_VIDEO_YUV444;
        return true;
    case AV_PIX_FMT_YUV444P:
        *out = RECON_VIDEO_YUV444;
        return true;
    default:
        /*
         * Ten-bit and twelve-bit video lands here, and so does anything with
         * the colour planes interleaved. Converting it would mean either a
         * second plane format everywhere above or pulling in a scaling library
         * -- and the second is how a dependency kept deliberately small stops
         * being small.
         *
         * Refusing is honest and the refusal is specific: recon_movie says what
         * the file is, and this is a 4:2:0 8-bit player.
         */
        return false;
    }
}

/*
 * Which coefficients to convert with.
 *
 * The file is asked first, because the file may know. When it does not -- and
 * a great many do not, since encoders leave it unspecified and players guess --
 * the guess everybody makes is by size: standard-definition material is BT.601
 * and high-definition is BT.709, and 720 lines is where the world drew the
 * line. Guessing is unavoidable here; guessing the same way as everything else
 * is what keeps a file from looking different in this player than in another.
 */
static enum recon_video_matrix matrix_for(const AVFrame *frame) {
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        return RECON_VIDEO_BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
        return RECON_VIDEO_BT601;
    default:
        return (frame->height >= 720) ? RECON_VIDEO_BT709 : RECON_VIDEO_BT601;
    }
}

static bool describe(const AVFrame *frame, struct recon_video_picture *out) {
    memset(out, 0, sizeof(*out));

    bool full_range = (frame->color_range == AVCOL_RANGE_JPEG);
    enum recon_video_format format;
    if (!plane_format(frame->format, &format, &full_range)) {
        return false;
    }

    out->width = frame->width;
    out->height = frame->height;
    out->format = format;
    out->matrix = matrix_for(frame);
    out->full_range = full_range;

    for (int i = 0; i < 3; i++) {
        out->plane[i] = frame->data[i];
        out->stride[i] = frame->linesize[i];
    }

    /*
     * best_effort_timestamp rather than pts, because it is the one that is
     * filled in when the container's timestamps have gaps -- which they do
     * whenever a file was cut or concatenated. A picture with no time at all
     * is shown immediately rather than dropped: a frame with a broken timestamp
     * is still a frame, and dropping it makes a jump where a repeat would do.
     */
    int64_t stamp = frame->best_effort_timestamp;
    if (stamp == AV_NOPTS_VALUE) {
        stamp = frame->pts;
    }
    out->time = (stamp == AV_NOPTS_VALUE) ?
        0.0 : (double)stamp / (double)TIME_BASE;

    return out->width > 0 && out->height > 0 &&
        out->plane[0] != NULL && out->plane[1] != NULL && out->plane[2] != NULL;
}

/* --- The queue --- */

static void release_current(struct recon_codec_video_reader *r) {
    if (r->current != NULL) {
        av_frame_free(&r->current);
    }
}

static void drain(struct recon_codec_video_reader *r) {
    while (r->count < QUEUE_MAX) {
        AVFrame *frame = av_frame_alloc();
        if (frame == NULL) {
            return;
        }
        if (avcodec_receive_frame(r->context, frame) < 0) {
            av_frame_free(&frame);
            return;
        }
        r->queue[(r->head + r->count) % QUEUE_MAX] = frame;
        r->count++;
    }
}

static void empty_queue(struct recon_codec_video_reader *r) {
    while (r->count > 0) {
        av_frame_free(&r->queue[r->head]);
        r->head = (r->head + 1) % QUEUE_MAX;
        r->count--;
    }
    r->head = 0;
}

static int take(struct recon_codec_video_reader *r,
        struct recon_video_picture *out) {
    while (r->count > 0) {
        AVFrame *frame = r->queue[r->head];
        r->head = (r->head + 1) % QUEUE_MAX;
        r->count--;

        if (describe(frame, out)) {
            r->current = frame;
            return 1;
        }
        /* A picture in a plane format this cannot express. Dropped rather than
         * fatal: one frame missing is better than a file that stops. */
        av_frame_free(&frame);
    }
    return 0;
}

/* --- The decoder --- */

static struct recon_codec_video_reader *open_video(enum AVCodecID id,
        const uint8_t *setup, size_t setup_length, int *width, int *height) {
    const AVCodec *codec = avcodec_find_decoder(id);
    if (codec == NULL) {
        return NULL;
    }

    struct recon_codec_video_reader *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }

    r->context = avcodec_alloc_context3(codec);
    r->packet = av_packet_alloc();
    if (r->context == NULL || r->packet == NULL) {
        goto failed;
    }

    if (setup != NULL && setup_length > 0) {
        /*
         * The avcC box, verbatim.
         *
         * Handing it over whole is also what tells libavcodec that the frames
         * are length-prefixed rather than separated by start codes -- avcC
         * begins with a version byte of 1, and the decoder switches on it. Strip
         * it down to the parameter sets instead and every frame decodes to
         * nothing, with no error anywhere: the decoder is looking for start
         * codes in a stream that has none.
         *
         * Padded and allocated with libavcodec's own allocator because its
         * bitstream readers deliberately read a few bytes past the end.
         */
        r->context->extradata = av_mallocz(setup_length +
            AV_INPUT_BUFFER_PADDING_SIZE);
        if (r->context->extradata == NULL) {
            goto failed;
        }
        memcpy(r->context->extradata, setup, setup_length);
        r->context->extradata_size = (int)setup_length;
    }

    /* So the times handed in come back out attached to the right pictures,
     * through a reorder this file is not otherwise involved in. */
    r->context->pkt_timebase = (AVRational){ 1, TIME_BASE };
    r->context->time_base = (AVRational){ 1, TIME_BASE };

    if (width != NULL && *width > 0) {
        r->context->width = *width;
    }
    if (height != NULL && *height > 0) {
        r->context->height = *height;
    }

    if (avcodec_open2(r->context, codec, NULL) < 0) {
        goto failed;
    }

    /*
     * What the decoder found, written back over what the container claimed.
     *
     * These disagree more often than they should. A container records the
     * display size, which may have been cropped from a coded size that is
     * rounded up to whole macroblocks -- 1080 becomes 1088 in the bitstream --
     * and believing the wrong one puts eight rows of somebody else's memory
     * along the bottom edge of every frame.
     */
    if (width != NULL && r->context->width > 0) {
        *width = r->context->width;
    }
    if (height != NULL && r->context->height > 0) {
        *height = r->context->height;
    }
    return r;

failed:
    if (r->context != NULL) {
        avcodec_free_context(&r->context);
    }
    av_packet_free(&r->packet);
    free(r);
    return NULL;
}

static struct recon_codec_video_reader *h264_open(const uint8_t *setup,
        size_t setup_length, int *width, int *height) {
    return open_video(AV_CODEC_ID_H264, setup, setup_length, width, height);
}

static struct recon_codec_video_reader *h265_open(const uint8_t *setup,
        size_t setup_length, int *width, int *height) {
    return open_video(AV_CODEC_ID_HEVC, setup, setup_length, width, height);
}

static int decode_picture(struct recon_codec_video_reader *r,
        const uint8_t *bytes, size_t length, double time,
        struct recon_video_picture *out) {
    if (r == NULL || out == NULL) {
        return -1;
    }
    release_current(r);

    if (bytes != NULL && length > 0) {
        /*
         * Points at the caller's memory rather than copying it, which is safe
         * for the same reason the audio side gives: the packet is fully
         * consumed by send_packet before this returns. libavcodec copies what
         * it needs to keep.
         */
        r->packet->data = (uint8_t *)bytes;
        r->packet->size = (int)length;
        r->packet->pts = (int64_t)(time * (double)TIME_BASE);
        r->packet->dts = AV_NOPTS_VALUE;

        int rc = avcodec_send_packet(r->context, r->packet);
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            /* One frame that will not decode. The caller moves on to the next
             * rather than stopping, because a damaged sample in the middle of
             * a sound file is what an interrupted download looks like. */
            drain(r);
            return (take(r, out) > 0) ? 1 : -1;
        }
        drain(r);
    }

    return take(r, out);
}

static int flush_pictures(struct recon_codec_video_reader *r,
        struct recon_video_picture *out) {
    if (r == NULL || out == NULL) {
        return 0;
    }
    release_current(r);

    if (!r->flushed) {
        /*
         * A null packet is how a decoder is told there is no more coming, and
         * it is what makes it give up the frames it has been holding for
         * reordering. Sent exactly once: sending it twice is an error the
         * decoder reports by returning nothing, which looks identical to being
         * finished and so hides itself.
         */
        avcodec_send_packet(r->context, NULL);
        r->flushed = true;
        drain(r);
    }
    return take(r, out);
}

static void reset_video(struct recon_codec_video_reader *r) {
    if (r == NULL) {
        return;
    }
    release_current(r);
    empty_queue(r);

    if (r->context != NULL) {
        avcodec_flush_buffers(r->context);
    }
    /* After a seek the stream is live again, so the end-of-stream packet has
     * to be allowed a second time. Without this, seeking backwards from the end
     * of a file leaves a decoder that will never emit another picture. */
    r->flushed = false;
}

static void close_video(struct recon_codec_video_reader *r) {
    if (r == NULL) {
        return;
    }
    release_current(r);
    empty_queue(r);
    avcodec_free_context(&r->context);
    av_packet_free(&r->packet);
    free(r);
}

/* --- What this half of the pack brings --- */

bool recon_codecpack_video_load(void) {
    static const struct recon_codec_video H264 = {
        .name = "H.264",
        .open = h264_open,
        .decode = decode_picture,
        .flush = flush_pictures,
        .reset = reset_video,
        .close = close_video,
    };
    static const struct recon_codec_video H265 = {
        .name = "H.265",
        .open = h265_open,
        .decode = decode_picture,
        .flush = flush_pictures,
        .reset = reset_video,
        .close = close_video,
    };

    if (!recon_codec_register_video(&H264)) {
        return false;
    }

    /*
     * H.265 is registered too, and only because recon_mp4 already recognises
     * hvc1 and hev1 and names them. A decoder registered for a format nothing
     * can demux would be a claim the system cannot keep -- which is the rule
     * this pack applies to itself for Vorbis and Opus, and it has to apply the
     * same way when the answer comes out yes.
     *
     * Its failure is not fatal: a build of libavcodec without HEVC is a real
     * thing, and it is not a reason to have no H.264 either.
     */
    recon_codec_register_video(&H265);
    return true;
}

void recon_codecpack_video_unload(void) {
    recon_codec_unregister_video("H.264");
    recon_codec_unregister_video("H.265");
}
