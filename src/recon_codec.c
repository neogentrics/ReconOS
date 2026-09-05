/*
 * Turning a file into sound. See include/recon_codec.h.
 *
 * The registry, and the two decoders ReconOS ships: WAV, written here because
 * it is a header and then the samples, and MP3 through minimp3 -- a format
 * parser, which is the half of THIRD_PARTY.md's line that permits it.
 *
 * Both go through the same interface as anything a module might add. That is
 * the point of doing it this way: if the built-in decoders took a shortcut the
 * registry did not offer, the registry would be decoration.
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* One translation unit gets the implementation, and this is it. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT_DISABLED
#include "minimp3_ex.h"

#include "recon_codec.h"

#define CODECS_MAX 16

static struct recon_codec g_codecs[CODECS_MAX];
static int g_count;

/* --- The registry --- */

bool recon_codec_register(const struct recon_codec *codec) {
    if (codec == NULL || codec->name[0] == '\0' || codec->open == NULL ||
            codec->read == NULL) {
        return false;
    }

    for (int i = 0; i < g_count; i++) {
        if (strcasecmp(g_codecs[i].name, codec->name) == 0) {
            /*
             * Refused rather than replaced. Two decoders answering to one name
             * is the collision this exists to prevent, and silently preferring
             * the newer would make which one runs depend on the order modules
             * happened to load in.
             */
            return false;
        }
    }
    if (g_count >= CODECS_MAX) {
        return false;
    }

    g_codecs[g_count++] = *codec;
    return true;
}

bool recon_codec_unregister(const char *name) {
    for (int i = 0; i < g_count; i++) {
        if (strcasecmp(g_codecs[i].name, name) == 0) {
            memmove(&g_codecs[i], &g_codecs[i + 1],
                sizeof(g_codecs[0]) * (size_t)(g_count - i - 1));
            g_count--;
            return true;
        }
    }
    return false;
}

int recon_codec_count(void) {
    return g_count;
}

bool recon_codec_at(int index, struct recon_codec *out) {
    if (index < 0 || index >= g_count || out == NULL) {
        return false;
    }
    *out = g_codecs[index];
    return true;
}

/* Does `extensions` -- a space-separated list of dotted extensions -- contain
 * the one on the end of `name`? */
static bool claims(const char *extensions, const char *name) {
    const char *dot = (name != NULL) ? strrchr(name, '.') : NULL;
    if (dot == NULL || dot[1] == '\0') {
        return false;
    }

    size_t length = strlen(dot);
    for (const char *at = extensions; *at != '\0'; ) {
        while (*at == ' ') {
            at++;
        }
        const char *end = strchr(at, ' ');
        size_t span = (end != NULL) ? (size_t)(end - at) : strlen(at);

        if (span == length && strncasecmp(at, dot, span) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        at = end;
    }
    return false;
}

bool recon_codec_handles_extension(const char *name) {
    for (int i = 0; i < g_count; i++) {
        if (claims(g_codecs[i].extensions, name)) {
            return true;
        }
    }
    return false;
}

const struct recon_codec *recon_codec_for(const char *name,
        const uint8_t *bytes, size_t length) {
    /*
     * The contents first, and the name only after.
     *
     * A file's bytes are what it is; its name is what somebody called it. A
     * WAV named .mp3 should play, and a .wav that is really an MP3 should
     * play as one -- both happen, and both come from somebody renaming a file
     * rather than converting it.
     */
    if (bytes != NULL && length > 0) {
        for (int i = 0; i < g_count; i++) {
            if (g_codecs[i].sniff != NULL &&
                    g_codecs[i].sniff(bytes, length)) {
                return &g_codecs[i];
            }
        }
    }

    for (int i = 0; i < g_count; i++) {
        if (claims(g_codecs[i].extensions, name)) {
            return &g_codecs[i];
        }
    }
    return NULL;
}

/* --- WAV --- */

/*
 * RIFF: a header, then chunks, each a four-character tag and a length.
 *
 * The chunks are walked rather than assumed to be in order. "fmt " before
 * "data" is what every file has and is not what the format guarantees, and a
 * reader that assumes it fails on a file with a LIST chunk in the middle --
 * which anything that has been through an editor has.
 */

struct wav_reader {
    const uint8_t *samples;      /* into the caller's buffer */
    size_t frames;
    int channels;
    int bits;
    bool is_float;
    size_t at;                   /* frames consumed */
};

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static bool wav_sniff(const uint8_t *bytes, size_t length) {
    return length >= 12 && memcmp(bytes, "RIFF", 4) == 0 &&
        memcmp(bytes + 8, "WAVE", 4) == 0;
}

static struct recon_codec_reader *wav_open(const uint8_t *bytes, size_t length,
        struct recon_codec_format *format) {
    if (!wav_sniff(bytes, length)) {
        return NULL;
    }

    int channels = 0, bits = 0;
    unsigned rate = 0;
    bool is_float = false;
    const uint8_t *data = NULL;
    size_t data_length = 0;

    size_t at = 12;
    while (at + 8 <= length) {
        const uint8_t *tag = bytes + at;
        uint32_t size = le32(bytes + at + 4);
        size_t body = at + 8;

        if (body + size > length) {
            /* A chunk that claims more than the file holds. Truncated
             * download, or a length written before the data was. Take what is
             * actually there rather than refusing the file. */
            size = (uint32_t)(length - body);
        }

        if (memcmp(tag, "fmt ", 4) == 0 && size >= 16) {
            uint16_t kind = le16(bytes + body);
            channels = le16(bytes + body + 2);
            rate = le32(bytes + body + 4);
            bits = le16(bytes + body + 14);

            /*
             * 1 is PCM and 3 is float. 0xFFFE is "extensible", where the real
             * format is a GUID later in the chunk -- and the first two bytes
             * of that GUID are the same 1 or 3, which is the only part of the
             * extensible format worth reading.
             */
            if (kind == 0xFFFE && size >= 26) {
                kind = le16(bytes + body + 24);
            }
            is_float = (kind == 3);
            if (kind != 1 && kind != 3) {
                return NULL;            /* compressed WAV; not this decoder */
            }
        } else if (memcmp(tag, "data", 4) == 0) {
            data = bytes + body;
            data_length = size;
        }

        /* Chunks are padded to an even length, and the pad is not counted. */
        at = body + size + (size & 1);
    }

    if (data == NULL || channels < 1 || channels > 2 || rate == 0) {
        return NULL;
    }
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
        return NULL;
    }

    size_t frame_bytes = (size_t)channels * (size_t)(bits / 8);
    if (frame_bytes == 0) {
        return NULL;
    }

    struct wav_reader *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }

    r->samples = data;
    r->frames = data_length / frame_bytes;
    r->channels = channels;
    r->bits = bits;
    r->is_float = is_float;

    format->rate = (int)rate;
    format->channels = channels;
    format->frames = r->frames;
    return (struct recon_codec_reader *)r;
}

/* One sample, whatever it was stored as, as sixteen bits. */
static int16_t wav_sample(const struct wav_reader *r, const uint8_t *p) {
    if (r->is_float) {
        /*
         * 32-bit float, nominally -1.0 to 1.0.
         *
         * Two details, both of which this got wrong first time and which
         * comparing against ffmpeg found. The scale is 32768, not 32767:
         * signed sixteen-bit runs from -32768 to 32767, so the negative side
         * is one step wider and scaling by 32767 makes everything quieter than
         * it should be. And it rounds rather than truncating -- a cast towards
         * zero biases every sample towards silence, which across a whole file
         * is a systematic error rather than noise.
         *
         * Clamped after scaling, because a file mixed too hot contains values
         * past 1.0 and wrapping them turns a loud passage into a burst of
         * noise. Clamping to the asymmetric range is what makes the 32768
         * scale safe.
         */
        uint32_t raw = le32(p);
        float value;
        memcpy(&value, &raw, sizeof(value));

        float scaled = value * 32768.0f;
        if (scaled > 32767.0f) {
            scaled = 32767.0f;
        }
        if (scaled < -32768.0f) {
            scaled = -32768.0f;
        }
        return (int16_t)lrintf(scaled);
    }

    switch (r->bits) {
    case 8:
        /* Eight-bit WAV is *unsigned*, alone among the widths. */
        return (int16_t)(((int)p[0] - 128) << 8);
    case 16:
        return (int16_t)le16(p);
    case 24:
        /* The top two bytes of three. */
        return (int16_t)((int16_t)p[1] | (int16_t)((int8_t)p[2] << 8));
    case 32:
        return (int16_t)(le32(p) >> 16);
    default:
        return 0;
    }
}

static int wav_read(struct recon_codec_reader *reader, int16_t *out,
        int frames) {
    struct wav_reader *r = (struct wav_reader *)reader;

    size_t left = (r->at < r->frames) ? r->frames - r->at : 0;
    if (left == 0) {
        return 0;
    }
    if ((size_t)frames > left) {
        frames = (int)left;
    }

    size_t width = (size_t)(r->bits / 8);
    const uint8_t *p = r->samples +
        (r->at * (size_t)r->channels * width);

    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < r->channels; c++) {
            *out++ = wav_sample(r, p);
            p += width;
        }
    }

    r->at += (size_t)frames;
    return frames;
}

static bool wav_seek(struct recon_codec_reader *reader, uint64_t frame) {
    struct wav_reader *r = (struct wav_reader *)reader;
    r->at = (frame < r->frames) ? (size_t)frame : r->frames;
    return true;
}

static void wav_close(struct recon_codec_reader *reader) {
    free(reader);
}

/* --- MP3 --- */

/*
 * minimp3, which decodes and does not do anything else.
 *
 * The `_ex` layer is used rather than the frame-at-a-time one because it
 * builds an index on open, which is what makes seeking land on the right
 * sample. Without it, seeking in an MP3 means guessing from the average
 * bitrate -- which is right for a constant-bitrate file and wrong by seconds
 * for a variable one, and every file anybody has is variable.
 */

struct mp3_reader {
    mp3dec_ex_t dec;
};

static bool mp3_sniff(const uint8_t *bytes, size_t length) {
    if (length < 4) {
        return false;
    }
    /* An ID3v2 tag at the front, which almost every file has. */
    if (memcmp(bytes, "ID3", 3) == 0) {
        return true;
    }
    /* Or a frame header: eleven set bits, then a version that is not the
     * reserved one. Checking the version matters -- eleven set bits turn up
     * in other formats often enough to matter. */
    return bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0 &&
        (bytes[1] & 0x18) != 0x08;
}

static struct recon_codec_reader *mp3_open(const uint8_t *bytes, size_t length,
        struct recon_codec_format *format) {
    struct mp3_reader *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }

    if (mp3dec_ex_open_buf(&r->dec, bytes, length, MP3D_SEEK_TO_SAMPLE) != 0) {
        free(r);
        return NULL;
    }
    if (r->dec.info.hz <= 0 || r->dec.info.channels < 1 ||
            r->dec.info.channels > 2) {
        mp3dec_ex_close(&r->dec);
        free(r);
        return NULL;
    }

    format->rate = r->dec.info.hz;
    format->channels = r->dec.info.channels;
    /* minimp3 counts samples across all channels; a frame is one per
     * channel, and confusing the two halves or doubles every duration. */
    format->frames = r->dec.samples / (uint64_t)r->dec.info.channels;
    return (struct recon_codec_reader *)r;
}

static int mp3_read(struct recon_codec_reader *reader, int16_t *out,
        int frames) {
    struct mp3_reader *r = (struct mp3_reader *)reader;
    size_t want = (size_t)frames * (size_t)r->dec.info.channels;
    size_t got = mp3dec_ex_read(&r->dec, out, want);
    return (int)(got / (size_t)r->dec.info.channels);
}

static bool mp3_seek(struct recon_codec_reader *reader, uint64_t frame) {
    struct mp3_reader *r = (struct mp3_reader *)reader;
    return mp3dec_ex_seek(&r->dec,
        frame * (uint64_t)r->dec.info.channels) == 0;
}

static void mp3_close(struct recon_codec_reader *reader) {
    struct mp3_reader *r = (struct mp3_reader *)reader;
    mp3dec_ex_close(&r->dec);
    free(r);
}

/* --- The ones that ship --- */

void recon_codec_init_builtin(void) {
    static const struct recon_codec WAV = {
        .name = "WAV",
        .extensions = ".wav .wave",
        .sniff = wav_sniff,
        .open = wav_open,
        .read = wav_read,
        .seek = wav_seek,
        .close = wav_close,
    };
    static const struct recon_codec MP3 = {
        .name = "MP3",
        .extensions = ".mp3",
        .sniff = mp3_sniff,
        .open = mp3_open,
        .read = mp3_read,
        .seek = mp3_seek,
        .close = mp3_close,
    };

    recon_codec_register(&WAV);
    recon_codec_register(&MP3);
}
