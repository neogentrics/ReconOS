/*
 * Taking an MP4 apart.
 *
 * A container is not a codec, and keeping the two apart is most of the point
 * of this file. An MP4 says *where* the compressed pieces are and what they
 * are; it does not say what the numbers inside them mean. So this is written
 * here -- it is structure, and structure is the kind of thing ReconOS writes
 * for itself -- while turning those pieces into sound or pictures is a
 * decoder's job and may be somebody else's code.
 *
 * The practical consequence: this can open a video file, tell you exactly what
 * is in it, and hand out its audio frames one at a time, on a system that
 * cannot decode a single one of them. That is a useful thing to be able to do.
 * It is the difference between "this file will not play" and "this file is
 * H.264 video and AAC audio, and there is no AAC decoder installed".
 *
 * --- The shape of the format ---
 *
 * An MP4 is a tree of boxes. Each is a 32-bit length, a four-character name,
 * and a body that is either data or more boxes. The ones that matter here:
 *
 *   ftyp                  what kind of MP4 this is
 *   moov                  everything about the file except the media itself
 *     trak                one stream -- a video track, an audio track
 *       mdia/mdhd         its timescale and duration
 *       mdia/hdlr         whether it is sound or pictures
 *       mdia/minf/stbl    the tables saying where its pieces are:
 *         stsd            what codec, and its setup data
 *         stts            how long each piece lasts
 *         stsc            how pieces are grouped into chunks
 *         stsz            how big each piece is
 *         stco / co64     where each chunk starts in the file
 *   mdat                  the pieces themselves, in one lump
 *
 * Reassembling a sample's position out of stsc, stsz and stco is the whole
 * job, and it is fiddly rather than difficult: the tables are compressed by
 * run-length in three different ways and none of them agree about indexing
 * from zero.
 */

#ifndef RECON_MP4_H
#define RECON_MP4_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a track carries. */
enum recon_mp4_kind {
    RECON_MP4_OTHER,
    RECON_MP4_AUDIO,
    RECON_MP4_VIDEO,
};

/* Long enough for "mp4a" and a readable name for it. */
#define RECON_MP4_CODEC_MAX 32

struct recon_mp4_track {
    enum recon_mp4_kind kind;

    /* The four-character code as written, and what it is in words. A file
     * that cannot be played should be able to say what it is. */
    char format[8];
    char name[RECON_MP4_CODEC_MAX];

    /* Sound. Zero for a video track. */
    int rate;
    int channels;

    /* Pictures. Zero for an audio track. */
    int width;
    int height;

    uint32_t timescale;        /* units per second for this track's clock */
    uint64_t duration;         /* in those units */
    int sample_count;

    /*
     * The codec's setup data, when the format carries any.
     *
     * AAC keeps two bytes here that say its sample rate and channel count,
     * and a decoder cannot start without them. H.264 keeps its parameter sets
     * here. Points into the file the caller supplied; it is not copied.
     */
    const uint8_t *setup;
    size_t setup_length;
};

struct recon_mp4;

/*
 * Read the structure of an MP4.
 *
 * `bytes` is borrowed and must outlive the returned handle: the sample tables
 * are large and this points into them rather than copying, which is what makes
 * opening a two-hour file cost the same as opening a two-minute one.
 *
 * NULL when it is not an MP4, or when its tables are inconsistent -- which
 * happens with a file that was still being written when it was copied.
 */
struct recon_mp4 *recon_mp4_open(const uint8_t *bytes, size_t length);
void recon_mp4_close(struct recon_mp4 *mp4);

int recon_mp4_track_count(const struct recon_mp4 *mp4);
bool recon_mp4_track_at(const struct recon_mp4 *mp4, int index,
    struct recon_mp4_track *out);

/* The first track of a kind, or -1. Almost every file has one of each. */
int recon_mp4_first(const struct recon_mp4 *mp4, enum recon_mp4_kind kind);

/*
 * Where one sample of a track lives, and how long it lasts.
 *
 * A "sample" here is the format's word, not a sound sample: it is one
 * compressed piece -- an AAC frame, a video picture. `offset` is into the
 * bytes recon_mp4_open was given.
 *
 * False past the end of the track.
 */
bool recon_mp4_sample(const struct recon_mp4 *mp4, int track, int index,
    size_t *offset, size_t *length, uint64_t *start_in_timescale);

/*
 * The last sample at or before `index` that can be decoded on its own.
 *
 * Seeking video is not seeking sound, and this is the difference. An audio
 * frame decodes by itself; a video frame is mostly a description of how it
 * differs from earlier ones, so landing on an arbitrary frame and decoding
 * forward gives several seconds of coloured smears before the picture assembles
 * itself. The answer is to start at the last frame that stands alone and decode
 * up to where the caller actually asked for.
 *
 * A track with no sync table has every sample answer for itself -- which is
 * what the format says, and is true of all audio.
 *
 * -1 when the track or index is not there.
 */
int recon_mp4_sync_sample(const struct recon_mp4 *mp4, int track, int index);

/* Which sample covers this moment. -1 when the track cannot say. */
int recon_mp4_sample_at_time(const struct recon_mp4 *mp4, int track,
    double seconds);

/*
 * The bytes this was opened over.
 *
 * A caller that has a sample's offset needs somewhere to add it to, and asking
 * it to keep its own copy of the pointer alongside the handle is asking it to
 * keep two things in step that are already one thing.
 */
void recon_mp4_bytes(const struct recon_mp4 *mp4, const uint8_t **bytes,
    size_t *length);

/* How long the whole thing runs, in seconds. Zero when the file does not
 * say, which a badly-written one does not. */
double recon_mp4_duration(const struct recon_mp4 *mp4);

const char *recon_mp4_last_error(void);

#endif /* RECON_MP4_H */
