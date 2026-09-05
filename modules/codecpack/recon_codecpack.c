/*
 * A codec pack: decoders ReconOS does not write, in a module it can do
 * without.
 *
 * --- Why this is a module and not part of the system ---
 *
 * ReconOS writes its own containers and the formats it can honestly write --
 * WAV because it is a header and then the samples, and the MP4 demuxer because
 * a container is structure. What it does not write is the decoding: AAC is a
 * hundred pages of specification and H.264 is seven hundred, and a decoder that
 * is ninety-five per cent correct does not sound or look nearly right, it
 * sounds and looks broken.
 *
 * So this wraps libavcodec, which does. And it is a module rather than part of
 * the core for three reasons, in order of how much they matter:
 *
 *   1. Somebody who never plays video should not carry the dependency. The
 *      desktop builds, runs and plays WAV and MP3 with this file absent.
 *   2. It can be removed. A codec pack that cannot be taken off again is not a
 *      pack, it is a decision somebody made for you.
 *   3. It keeps the borrowed part exactly the size it needs to be. libavcodec
 *      can demux, scale, filter and convert; none of that is used here. It is
 *      handed a compressed frame and asked for samples, and everything else is
 *      ReconOS's own.
 *
 * THIRD_PARTY.md's line is "libraries may parse formats and talk to hardware".
 * This is the first half of that sentence, kept to one file so the line is
 * visible rather than assumed.
 */

#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>

#include "recon_codec.h"
#include "recon_module.h"

#define CODECPACK_VERSION "1.0.0"

/* --- One decoder --- */

struct recon_codec_frame_reader {
    AVCodecContext *context;
    AVPacket *packet;
    AVFrame *frame;
    int channels;
};

/*
 * Open a decoder for a format, with the container's setup data.
 *
 * `extradata` is where AAC's two bytes and H.264's parameter sets go, and
 * libavcodec will not start an AAC decoder without them: the frames themselves
 * carry no rate or channel count, because in an MP4 the container is supposed
 * to have said.
 */
static struct recon_codec_frame_reader *open_decoder(enum AVCodecID id,
        const uint8_t *setup, size_t setup_length, int *rate, int *channels) {
    const AVCodec *codec = avcodec_find_decoder(id);
    if (codec == NULL) {
        return NULL;
    }

    struct recon_codec_frame_reader *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }

    r->context = avcodec_alloc_context3(codec);
    r->packet = av_packet_alloc();
    r->frame = av_frame_alloc();
    if (r->context == NULL || r->packet == NULL || r->frame == NULL) {
        goto failed;
    }

    if (setup != NULL && setup_length > 0) {
        /*
         * Allocated with libavcodec's own allocator and padded, which it
         * requires: its bitstream readers read a few bytes past the end on
         * purpose, and a plain malloc here is a buffer overrun that shows up
         * as a crash in a library rather than as a bug in this file.
         */
        r->context->extradata = av_mallocz(setup_length +
            AV_INPUT_BUFFER_PADDING_SIZE);
        if (r->context->extradata == NULL) {
            goto failed;
        }
        memcpy(r->context->extradata, setup, setup_length);
        r->context->extradata_size = (int)setup_length;
    }

    if (rate != NULL && *rate > 0) {
        r->context->sample_rate = *rate;
    }
    if (channels != NULL && *channels > 0) {
        av_channel_layout_default(&r->context->ch_layout, *channels);
    }

    if (avcodec_open2(r->context, codec, NULL) < 0) {
        goto failed;
    }

    /*
     * What the decoder settled on, written back.
     *
     * The container's numbers are a claim and the setup data is the truth --
     * an AAC track's sample rate is in its AudioSpecificConfig, and the box
     * above it holds a 16-bit field that cannot represent every rate the
     * format allows. Believing the box gives a file that plays at the wrong
     * speed, which sounds like a broken decoder rather than a wrong number.
     */
    if (rate != NULL && r->context->sample_rate > 0) {
        *rate = r->context->sample_rate;
    }
    if (channels != NULL && r->context->ch_layout.nb_channels > 0) {
        *channels = r->context->ch_layout.nb_channels;
    }
    r->channels = (channels != NULL) ? *channels : 2;
    return r;

failed:
    if (r->context != NULL) {
        avcodec_free_context(&r->context);
    }
    av_packet_free(&r->packet);
    av_frame_free(&r->frame);
    free(r);
    return NULL;
}

static struct recon_codec_frame_reader *aac_open(const uint8_t *setup,
        size_t setup_length, int *rate, int *channels) {
    return open_decoder(AV_CODEC_ID_AAC, setup, setup_length, rate, channels);
}

/*
 * One sample, whatever the decoder produced it as, as sixteen bits.
 *
 * libavcodec's AAC decoder produces planar floats -- one array per channel,
 * not interleaved -- so this is a conversion and an interleave at once.
 * Rounded and clamped for the reason recon_codec.c's WAV path gives at
 * length: truncating biases every sample towards silence, and the negative
 * side of sixteen-bit signed is one step wider than the positive.
 */
static int16_t as_s16(const AVFrame *frame, int channel, int index) {
    switch (frame->format) {
    case AV_SAMPLE_FMT_FLTP: {
        float value = ((const float *)frame->extended_data[channel])[index];
        float scaled = value * 32768.0f;
        if (scaled > 32767.0f) {
            scaled = 32767.0f;
        }
        if (scaled < -32768.0f) {
            scaled = -32768.0f;
        }
        return (int16_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
    }
    case AV_SAMPLE_FMT_FLT: {
        const float *at = (const float *)frame->extended_data[0];
        float value = at[index * frame->ch_layout.nb_channels + channel];
        float scaled = value * 32768.0f;
        if (scaled > 32767.0f) {
            scaled = 32767.0f;
        }
        if (scaled < -32768.0f) {
            scaled = -32768.0f;
        }
        return (int16_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
    }
    case AV_SAMPLE_FMT_S16P:
        return ((const int16_t *)frame->extended_data[channel])[index];
    case AV_SAMPLE_FMT_S16: {
        const int16_t *at = (const int16_t *)frame->extended_data[0];
        return at[index * frame->ch_layout.nb_channels + channel];
    }
    case AV_SAMPLE_FMT_S32P:
        return (int16_t)(((const int32_t *)
            frame->extended_data[channel])[index] >> 16);
    default:
        return 0;
    }
}

static int decode_frame(struct recon_codec_frame_reader *r,
        const uint8_t *bytes, size_t length, int16_t *out, int max_frames) {
    if (r == NULL || bytes == NULL || length == 0) {
        return 0;
    }

    /*
     * The packet points at the caller's memory rather than copying it.
     *
     * That is safe here and worth saying why: the packet is submitted and
     * fully consumed before this function returns, so the borrowed bytes
     * outlive the only use libavcodec makes of them. It would not be safe with
     * a decoder that queued packets.
     */
    r->packet->data = (uint8_t *)bytes;
    r->packet->size = (int)length;

    if (avcodec_send_packet(r->context, r->packet) < 0) {
        return 0;
    }

    int written = 0;
    while (written < max_frames) {
        int rc = avcodec_receive_frame(r->context, r->frame);
        if (rc < 0) {
            break;                       /* wants more input, or is finished */
        }

        int channels = r->frame->ch_layout.nb_channels;
        if (channels > r->channels) {
            channels = r->channels;
        }

        int count = r->frame->nb_samples;
        if (written + count > max_frames) {
            count = max_frames - written;
        }

        for (int i = 0; i < count; i++) {
            for (int c = 0; c < r->channels; c++) {
                /* A mono frame played into two channels goes to both, rather
                 * than to the left one and silence on the right. */
                int from = (c < channels) ? c : 0;
                out[(size_t)(written + i) * r->channels + c] =
                    as_s16(r->frame, from, i);
            }
        }
        written += count;
        av_frame_unref(r->frame);
    }

    return written;
}

static void reset_decoder(struct recon_codec_frame_reader *r) {
    if (r != NULL && r->context != NULL) {
        /* Everything the decoder was carrying from the last frame. Without
         * this the first frames after a seek are decoded against the state of
         * somewhere else in the file. */
        avcodec_flush_buffers(r->context);
    }
}

static void close_decoder(struct recon_codec_frame_reader *r) {
    if (r == NULL) {
        return;
    }
    avcodec_free_context(&r->context);
    av_packet_free(&r->packet);
    av_frame_free(&r->frame);
    free(r);
}

/* --- What this pack brings --- */

static bool load(void) {
    static const struct recon_codec_frames AAC = {
        .name = "AAC",
        .open = aac_open,
        .decode = decode_frame,
        .reset = reset_decoder,
        .close = close_decoder,
    };

    /*
     * AAC only, for now, and deliberately not a list of everything libavcodec
     * can do.
     *
     * Registering a decoder for a format nothing here can demux would be a
     * claim the system cannot keep: ReconOS reads MP4 and nothing else, so
     * Vorbis-in-Matroska has no path to this decoder however willing it is.
     * A registry entry that can never be reached is worse than a gap, because
     * a settings page would list it as installed.
     *
     * Video is not here either. Decoding H.264 is one line more than this;
     * *showing* it needs colour conversion, scaling and a clock keeping two
     * streams together, and none of that exists yet. A video decoder with
     * nowhere to send its pictures would be the same empty claim.
     */
    if (!recon_codec_register_frames(&AAC)) {
        return false;
    }
    return true;
}

static void unload(void) {
    recon_codec_unregister_frames("AAC");
}

RECON_MODULE(
    .name = "Codec Pack",
    .version = CODECPACK_VERSION,
    .description = "AAC decoding, through libavcodec",
    .load = load,
    .unload = unload,
);
