/*
 * Taking an MP4 apart. See include/recon_mp4.h.
 *
 * Everything here is reading. Nothing is decoded, nothing is copied, and the
 * sample tables are pointed into rather than expanded -- a two-hour file has a
 * few hundred thousand samples, and turning three run-length tables into three
 * flat arrays would cost more memory than the audio does.
 *
 * The cost of not expanding them is that finding sample N means walking the
 * runs. That is done once per sample in order, which is how a player reads
 * them, so the walk is amortised by remembering where the last one left off.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_mp4.h"

#define TRACKS_MAX 8

static char g_error[192];

const char *recon_mp4_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* --- Reading numbers --- */
/*
 * Big-endian, throughout. MP4 comes from QuickTime, which came from a Motorola
 * machine, and the format kept the byte order -- so every number in it is the
 * opposite way round from every number in a WAV.
 */

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

/* --- One track's tables --- */

struct table {
    const uint8_t *entries;
    uint32_t count;
};

struct track {
    struct recon_mp4_track info;

    struct table stts;      /* count, duration */
    struct table stsc;      /* first chunk, samples per chunk, description */
    struct table stsz;      /* one size each, or one size for all */
    uint32_t uniform_size;  /* when stsz says every sample is the same size */
    struct table stco;      /* chunk offsets */
    bool wide_offsets;      /* co64 rather than stco */

    /*
     * Where the last lookup got to.
     *
     * A player asks for sample 0, then 1, then 2. Remembering the walk turns
     * every lookup after the first into a step, which is the difference
     * between reading a long file and quadratic time.
     */
    int cached_index;
    uint32_t cached_chunk;          /* which chunk it is in */
    uint32_t cached_first_in_chunk; /* index of that chunk's first sample */
    size_t cached_offset;           /* where that sample starts */
    uint64_t cached_time;           /* its start, in the track's timescale */
};

struct recon_mp4 {
    const uint8_t *bytes;
    size_t length;

    struct track tracks[TRACKS_MAX];
    int count;

    uint32_t movie_timescale;
    uint64_t movie_duration;
};

/* --- Walking the boxes --- */

/*
 * Call `visit` for every box directly inside [at, end).
 *
 * Sizes are checked against the end rather than trusted: a truncated file has
 * a box claiming more than is there, and following it would read past the
 * buffer. A box that does not fit ends the walk rather than failing the file,
 * because everything before it was still readable.
 */
typedef void (*box_fn)(struct recon_mp4 *mp4, struct track *track,
    const char *name, const uint8_t *body, size_t length);

static void walk(struct recon_mp4 *mp4, struct track *track,
        const uint8_t *at, const uint8_t *end, box_fn visit) {
    while (at + 8 <= end) {
        uint64_t size = be32(at);
        const char *name = (const char *)(at + 4);
        const uint8_t *body = at + 8;

        if (size == 1) {
            /* A 64-bit size, for a box larger than four gigabytes -- which in
             * practice means mdat in a long recording. */
            if (at + 16 > end) {
                return;
            }
            size = be64(at + 8);
            body = at + 16;
        } else if (size == 0) {
            size = (uint64_t)(end - at);   /* to the end of its parent */
        }

        if (size < (uint64_t)(body - at) || at + size > end) {
            return;
        }

        visit(mp4, track, name, body, (size_t)(at + size - body));
        at += size;
    }
}

/* --- The pieces of a track --- */

/* A version-and-flags header, which most of these boxes begin with. */
static const uint8_t *skip_full(const uint8_t *body, size_t length,
        uint8_t *version_out) {
    if (length < 4) {
        return NULL;
    }
    if (version_out != NULL) {
        *version_out = body[0];
    }
    return body + 4;
}

/*
 * An AAC track's setup data, out of the descriptor chain inside `esds`.
 *
 * The chain is MPEG-4's own tagged format: a tag byte, a length written seven
 * bits at a time, and a body. Three nested descriptors deep is where the two
 * bytes a decoder needs actually live, and there is no shortcut to them.
 */
static void read_esds(struct track *track, const uint8_t *body, size_t length) {
    const uint8_t *at = skip_full(body, length, NULL);
    if (at == NULL) {
        return;
    }
    const uint8_t *end = body + length;

    while (at < end) {
        uint8_t tag = *at++;

        /* The length, seven bits per byte, high bit meaning "another
         * follows". Capped at four bytes, which is the format's own limit. */
        size_t size = 0;
        for (int i = 0; i < 4 && at < end; i++) {
            uint8_t b = *at++;
            size = (size << 7) | (b & 0x7F);
            if ((b & 0x80) == 0) {
                break;
            }
        }
        if (at + size > end) {
            return;
        }

        if (tag == 0x03) {              /* ES_Descriptor */
            /* An id, then flags whose bits say which optional fields follow.
             * Stepping over them wrongly lands in the middle of the next
             * descriptor and reads noise as a length. */
            if (at + 3 > end) {
                return;
            }
            const uint8_t *inner = at + 2;
            uint8_t flags = inner[0];
            inner++;
            if ((flags & 0x80) != 0) {
                inner += 2;             /* depends on another stream */
            }
            if ((flags & 0x40) != 0 && inner < end) {
                inner += 1 + *inner;    /* a URL */
            }
            if ((flags & 0x20) != 0) {
                inner += 2;             /* OCR stream */
            }
            at = inner;
            continue;                   /* its children follow in place */
        }

        if (tag == 0x04) {              /* DecoderConfigDescriptor */
            /* One byte of object type, then twelve of buffer sizes and
             * bitrates that nothing here needs. */
            at += 13;
            continue;
        }

        if (tag == 0x05) {              /* DecoderSpecificInfo -- the prize */
            track->info.setup = at;
            track->info.setup_length = size;
            return;
        }

        at += size;
    }
}

static void read_stsd(struct recon_mp4 *mp4, struct track *track,
        const uint8_t *body, size_t length);

static void on_box(struct recon_mp4 *mp4, struct track *track,
        const char *name, const uint8_t *body, size_t length) {
    /* Containers: recurse. */
    if (memcmp(name, "moov", 4) == 0 || memcmp(name, "trak", 4) == 0 ||
            memcmp(name, "mdia", 4) == 0 || memcmp(name, "minf", 4) == 0 ||
            memcmp(name, "stbl", 4) == 0) {
        if (memcmp(name, "trak", 4) == 0) {
            if (mp4->count >= TRACKS_MAX) {
                return;
            }
            track = &mp4->tracks[mp4->count++];
            memset(track, 0, sizeof(*track));
            track->cached_index = -1;
        }
        walk(mp4, track, body, body + length, on_box);
        return;
    }

    if (memcmp(name, "mvhd", 4) == 0) {
        uint8_t version = 0;
        const uint8_t *at = skip_full(body, length, &version);
        if (at == NULL) {
            return;
        }
        if (version == 1 && length >= 32) {
            mp4->movie_timescale = be32(at + 16);
            mp4->movie_duration = be64(at + 20);
        } else if (length >= 20) {
            mp4->movie_timescale = be32(at + 8);
            mp4->movie_duration = be32(at + 12);
        }
        return;
    }

    if (track == NULL) {
        return;
    }

    if (memcmp(name, "mdhd", 4) == 0) {
        uint8_t version = 0;
        const uint8_t *at = skip_full(body, length, &version);
        if (at == NULL) {
            return;
        }
        if (version == 1 && length >= 32) {
            track->info.timescale = be32(at + 16);
            track->info.duration = be64(at + 20);
        } else if (length >= 20) {
            track->info.timescale = be32(at + 8);
            track->info.duration = be32(at + 12);
        }
        return;
    }

    if (memcmp(name, "hdlr", 4) == 0) {
        const uint8_t *at = skip_full(body, length, NULL);
        if (at == NULL || length < 12) {
            return;
        }
        /* Four bytes reserved, then the handler's four-character type. */
        if (memcmp(at + 4, "soun", 4) == 0) {
            track->info.kind = RECON_MP4_AUDIO;
        } else if (memcmp(at + 4, "vide", 4) == 0) {
            track->info.kind = RECON_MP4_VIDEO;
        }
        return;
    }

    if (memcmp(name, "stsd", 4) == 0) {
        read_stsd(mp4, track, body, length);
        return;
    }

    /* --- The tables --- */

    if (memcmp(name, "stts", 4) == 0 || memcmp(name, "stsc", 4) == 0 ||
            memcmp(name, "stco", 4) == 0 || memcmp(name, "co64", 4) == 0) {
        const uint8_t *at = skip_full(body, length, NULL);
        if (at == NULL || length < 8) {
            return;
        }
        struct table t = { .entries = at + 4, .count = be32(at) };

        if (memcmp(name, "stts", 4) == 0) {
            track->stts = t;
        } else if (memcmp(name, "stsc", 4) == 0) {
            track->stsc = t;
        } else {
            track->stco = t;
            track->wide_offsets = (memcmp(name, "co64", 4) == 0);
        }
        return;
    }

    if (memcmp(name, "stsz", 4) == 0) {
        const uint8_t *at = skip_full(body, length, NULL);
        if (at == NULL || length < 12) {
            return;
        }
        /*
         * One size for everything, or one per sample. Constant-size tracks
         * write a single number and no table at all, which is most of why
         * this box has a shape of its own.
         */
        track->uniform_size = be32(at);
        track->stsz.count = be32(at + 4);
        track->stsz.entries = (track->uniform_size == 0) ? at + 8 : NULL;
        track->info.sample_count = (int)track->stsz.count;
        return;
    }
}

/* What a four-character format code is, in words. */
static const char *format_name(const char *code) {
    if (memcmp(code, "mp4a", 4) == 0) { return "AAC"; }
    if (memcmp(code, "avc1", 4) == 0 || memcmp(code, "avc3", 4) == 0) {
        return "H.264";
    }
    if (memcmp(code, "hvc1", 4) == 0 || memcmp(code, "hev1", 4) == 0) {
        return "H.265";
    }
    if (memcmp(code, "av01", 4) == 0) { return "AV1"; }
    if (memcmp(code, "vp09", 4) == 0) { return "VP9"; }
    if (memcmp(code, "alac", 4) == 0) { return "ALAC"; }
    if (memcmp(code, "Opus", 4) == 0) { return "Opus"; }
    if (memcmp(code, ".mp3", 4) == 0 || memcmp(code, "mp3 ", 4) == 0) {
        return "MP3";
    }
    if (memcmp(code, "twos", 4) == 0 || memcmp(code, "sowt", 4) == 0 ||
            memcmp(code, "lpcm", 4) == 0) {
        return "PCM";
    }
    return "";
}

/*
 * The sample description: what codec, and the setup it needs.
 *
 * The layout depends on whether the track is sound or pictures, which is why
 * this is not part of the generic box walk -- the same offsets mean different
 * things, and reading a video box as an audio one produces a plausible sample
 * rate out of a picture's width.
 */
static void read_stsd(struct recon_mp4 *mp4, struct track *track,
        const uint8_t *body, size_t length) {
    (void)mp4;

    const uint8_t *at = skip_full(body, length, NULL);
    if (at == NULL || length < 16) {
        return;
    }

    /* An entry count, then the first entry: a size, a format code, six
     * reserved bytes and a data-reference index. */
    const uint8_t *entry = at + 4;
    const uint8_t *end = body + length;
    if (entry + 16 > end) {
        return;
    }

    uint32_t entry_size = be32(entry);
    memcpy(track->info.format, entry + 4, 4);
    track->info.format[4] = '\0';
    snprintf(track->info.name, sizeof(track->info.name), "%s",
        format_name(track->info.format));

    const uint8_t *rest = entry + 16;
    const uint8_t *entry_end = entry + entry_size;
    if (entry_end > end) {
        entry_end = end;
    }

    if (track->info.kind == RECON_MP4_AUDIO && rest + 20 <= entry_end) {
        /* Eight reserved bytes, channels, sample size, two more reserved
         * fields, then the rate as 16.16 fixed point -- of which the whole
         * part is the only half anybody uses. */
        track->info.channels = be16(rest + 8);
        track->info.rate = be16(rest + 16);
        rest += 20;
    } else if (track->info.kind == RECON_MP4_VIDEO && rest + 70 <= entry_end) {
        track->info.width = be16(rest + 16);
        track->info.height = be16(rest + 18);

        /*
         * Seventy bytes, counted rather than remembered: two pre-defined,
         * two reserved, twelve more pre-defined, width, height, two
         * resolutions of four bytes each, four reserved, a frame count, a
         * thirty-two byte compressor name, a depth and a final pre-defined.
         *
         * It was 78 here, which is eight too many -- and eight bytes past the
         * fixed part is the middle of the avcC box that follows, so the scan
         * below found nothing and the track reported no setup data at all.
         * A video track with no avcC has no parameter sets, and no decoder can
         * start without them: it would have looked like a demuxer that worked
         * right up until the moment anything tried to decode.
         */
        rest += 70;
    }

    /* Whatever follows the fixed part is more boxes: esds for AAC, avcC for
     * H.264. Both hold the setup a decoder cannot start without. */
    while (rest + 8 <= entry_end) {
        uint32_t size = be32(rest);
        const char *name = (const char *)(rest + 4);
        if (size < 8 || rest + size > entry_end) {
            break;
        }

        if (memcmp(name, "esds", 4) == 0) {
            read_esds(track, rest + 8, size - 8);
            /* An AAC track's real rate is in the setup data, not in the box
             * above -- the box holds a 16-bit field, and 48000 does not fit
             * in it the way the format expects for high-rate audio. */
        } else if (memcmp(name, "avcC", 4) == 0 ||
                memcmp(name, "hvcC", 4) == 0 || memcmp(name, "av1C", 4) == 0) {
            track->info.setup = rest + 8;
            track->info.setup_length = size - 8;
        }
        rest += size;
    }
}

/* --- Opening --- */

struct recon_mp4 *recon_mp4_open(const uint8_t *bytes, size_t length) {
    g_error[0] = '\0';

    if (bytes == NULL || length < 16) {
        snprintf(g_error, sizeof(g_error), "that file is too short to be an MP4");
        return NULL;
    }

    /*
     * The first box should be ftyp. Some files begin with a free or skip box,
     * or with mdat, so the check is that *a* recognisable box is there rather
     * than that ftyp is first.
     */
    uint32_t first = be32(bytes);
    if (first < 8 || first > length) {
        snprintf(g_error, sizeof(g_error), "that is not an MP4");
        return NULL;
    }

    struct recon_mp4 *mp4 = calloc(1, sizeof(*mp4));
    if (mp4 == NULL) {
        snprintf(g_error, sizeof(g_error), "out of memory");
        return NULL;
    }
    mp4->bytes = bytes;
    mp4->length = length;

    walk(mp4, NULL, bytes, bytes + length, on_box);

    if (mp4->count == 0) {
        snprintf(g_error, sizeof(g_error),
            "that MP4 has no tracks in it, or its index is missing");
        free(mp4);
        return NULL;
    }
    return mp4;
}

void recon_mp4_close(struct recon_mp4 *mp4) {
    free(mp4);
}

int recon_mp4_track_count(const struct recon_mp4 *mp4) {
    return mp4 != NULL ? mp4->count : 0;
}

bool recon_mp4_track_at(const struct recon_mp4 *mp4, int index,
        struct recon_mp4_track *out) {
    if (mp4 == NULL || index < 0 || index >= mp4->count || out == NULL) {
        return false;
    }
    *out = mp4->tracks[index].info;
    return true;
}

int recon_mp4_first(const struct recon_mp4 *mp4, enum recon_mp4_kind kind) {
    if (mp4 == NULL) {
        return -1;
    }
    for (int i = 0; i < mp4->count; i++) {
        if (mp4->tracks[i].info.kind == kind) {
            return i;
        }
    }
    return -1;
}

double recon_mp4_duration(const struct recon_mp4 *mp4) {
    if (mp4 == NULL || mp4->movie_timescale == 0) {
        return 0.0;
    }
    return (double)mp4->movie_duration / (double)mp4->movie_timescale;
}

/* --- Finding a sample --- */

static uint32_t sample_size(const struct track *t, int index) {
    if (t->uniform_size != 0) {
        return t->uniform_size;
    }
    if (t->stsz.entries == NULL || (uint32_t)index >= t->stsz.count) {
        return 0;
    }
    return be32(t->stsz.entries + (size_t)index * 4);
}

static uint64_t chunk_offset(const struct track *t, uint32_t chunk) {
    if (chunk == 0 || chunk > t->stco.count) {
        return 0;
    }
    const uint8_t *p = t->stco.entries +
        (size_t)(chunk - 1) * (t->wide_offsets ? 8 : 4);
    return t->wide_offsets ? be64(p) : be32(p);
}

/*
 * Which chunk a sample is in, and which sample starts that chunk.
 *
 * stsc is run-length: each entry says "from this chunk onwards, there are this
 * many samples per chunk", until the next entry says otherwise. So finding a
 * sample means walking the runs and counting -- there is no formula, because
 * the run lengths are data.
 */
static bool locate(const struct track *t, int index, uint32_t *chunk_out,
        uint32_t *first_out) {
    if (t->stsc.count == 0 || t->stco.count == 0) {
        return false;
    }

    uint32_t seen = 0;

    for (uint32_t i = 0; i < t->stsc.count; i++) {
        const uint8_t *entry = t->stsc.entries + (size_t)i * 12;
        uint32_t first_chunk = be32(entry);
        uint32_t per_chunk = be32(entry + 4);

        if (per_chunk == 0 || first_chunk == 0) {
            return false;
        }

        /* How many chunks this run covers: up to the next run's first chunk,
         * or to the end of the chunk table for the last run. */
        uint32_t next_first = (i + 1 < t->stsc.count)
            ? be32(t->stsc.entries + (size_t)(i + 1) * 12)
            : t->stco.count + 1;
        if (next_first <= first_chunk) {
            return false;
        }

        uint32_t chunks = next_first - first_chunk;
        uint64_t in_run = (uint64_t)chunks * per_chunk;

        if ((uint64_t)index < seen + in_run) {
            uint32_t into = (uint32_t)((uint64_t)index - seen);
            *chunk_out = first_chunk + into / per_chunk;
            *first_out = seen + (into / per_chunk) * per_chunk;
            return true;
        }
        seen += (uint32_t)in_run;
    }
    return false;
}

/* When a sample starts, by walking stts -- which is run-length in the same
 * way and for the same reason. */
static uint64_t sample_time(const struct track *t, int index) {
    uint64_t at = 0;
    uint32_t seen = 0;

    for (uint32_t i = 0; i < t->stts.count; i++) {
        const uint8_t *entry = t->stts.entries + (size_t)i * 8;
        uint32_t count = be32(entry);
        uint32_t duration = be32(entry + 4);

        if ((uint64_t)index < (uint64_t)seen + count) {
            return at + (uint64_t)((uint32_t)index - seen) * duration;
        }
        at += (uint64_t)count * duration;
        seen += count;
    }
    return at;
}

bool recon_mp4_sample(const struct recon_mp4 *mp4, int track_index, int index,
        size_t *offset, size_t *length, uint64_t *start) {
    if (mp4 == NULL || track_index < 0 || track_index >= mp4->count) {
        return false;
    }

    /* Cast away const: the cache is the only thing written, and it is a
     * memo of work already done rather than a change to what the file says. */
    struct track *t = (struct track *)&mp4->tracks[track_index];

    if (index < 0 || index >= t->info.sample_count) {
        return false;
    }

    uint32_t chunk = 0;
    uint32_t first_in_chunk = 0;
    size_t at = 0;

    /*
     * The step case: the next sample after the last one asked for, in the
     * same chunk. Adding its predecessor's size is the whole lookup, which is
     * what keeps reading a long file linear rather than quadratic.
     */
    if (t->cached_index >= 0 && index == t->cached_index + 1 &&
            locate(t, index, &chunk, &first_in_chunk) &&
            chunk == t->cached_chunk) {
        at = t->cached_offset + sample_size(t, t->cached_index);
    } else {
        if (!locate(t, index, &chunk, &first_in_chunk)) {
            return false;
        }
        uint64_t base = chunk_offset(t, chunk);
        if (base == 0) {
            return false;
        }
        at = (size_t)base;
        for (int i = (int)first_in_chunk; i < index; i++) {
            at += sample_size(t, i);
        }
    }

    size_t size = sample_size(t, index);
    if (size == 0 || at + size > mp4->length) {
        return false;
    }

    t->cached_index = index;
    t->cached_chunk = chunk;
    t->cached_first_in_chunk = first_in_chunk;
    t->cached_offset = at;
    t->cached_time = sample_time(t, index);

    if (offset != NULL) {
        *offset = at;
    }
    if (length != NULL) {
        *length = size;
    }
    if (start != NULL) {
        *start = t->cached_time;
    }
    return true;
}
