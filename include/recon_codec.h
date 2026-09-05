/*
 * Turning a file into sound.
 *
 * --- What a codec is here, and why this is a registry ---
 *
 * A format is a thing that can be *added*. A decoder registers itself, saying
 * which extensions it handles and how to recognise its files, and everything
 * above asks this rather than knowing a list. That is the whole point of the
 * shape: a module can bring a decoder, and nothing in the player changes.
 *
 * ReconOS ships the ones it can honestly write. WAV is here because it is a
 * header and then the samples, and writing it is an afternoon. Everything else
 * that matters -- MP3, AAC, Vorbis, FLAC, Opus -- is somebody's life's work in
 * a way that is not a figure of speech: the MP3 specification alone is a
 * hundred pages of psychoacoustics, and a decoder that is 95% correct sounds
 * broken rather than nearly right.
 *
 * So the honest position is: this is the mechanism, and the mechanism is the
 * deliverable. A decoder for a format ReconOS does not ship is a `.rts` that
 * calls recon_codec_register, and THIRD_PARTY.md's line -- "libraries may
 * parse formats" -- is what makes wrapping an existing one legitimate rather
 * than a compromise.
 *
 * --- Video ---
 *
 * There are three registries here, not one, and the split is the whole design.
 *
 * A **file codec** is handed a whole file and finds its own way through it.
 * A **frame codec** is handed one compressed audio frame at a time by
 * something that already knows where the frames are. A **video codec** is the
 * same arrangement for pictures, and it needs its own shape rather than a flag
 * on the audio one: what comes out is not a number of samples but a picture,
 * with a size, a colour space, and a moment it belongs at.
 *
 * This file used to say there was no video and explain why at length. What it
 * said was that decoding is the small half -- the large half is a container
 * that interleaves two streams, a clock that keeps them together, and turning
 * thirty megabytes a second of planes into something the screen can show. That
 * was true and it is still true; those three are now written, in recon_mp4,
 * recon_movie and recon_video respectively, and all three are ReconOS's own.
 *
 * What is *not* written here is H.264 itself, for the same reason AAC is not:
 * seven hundred pages, where ninety-five per cent correct looks broken rather
 * than nearly right. So a module brings it, and when no module has, ReconOS
 * says which decoder is missing instead of showing a black rectangle.
 */

#ifndef RECON_CODEC_H
#define RECON_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Enough for "MPEG-1 Audio Layer III" and the like. */
#define RECON_CODEC_NAME_MAX 48
#define RECON_CODEC_EXTENSIONS_MAX 96

/*
 * What a decoder produces.
 *
 * Sixteen-bit interleaved, which is what recon_audio takes, so a decoder that
 * produces anything else converts on its way out rather than making everything
 * downstream ask. One format in the middle instead of a conversion matrix.
 */
struct recon_codec_format {
    int rate;
    int channels;
    /* Total frames, or 0 when the format does not say. A stream that cannot
     * report its length shows elapsed time and no bar, rather than a bar that
     * lies about where the end is. */
    uint64_t frames;
};

struct recon_codec_reader;

/*
 * One decoder.
 *
 * `open` is handed the whole file. That is a real constraint and a deliberate
 * one: it caps what can be played at what fits in memory, and it removes
 * seeking-in-a-stream from every decoder that would otherwise have to
 * implement it. A media player that can open a forty-minute recording and not
 * a four-hour one is a limit worth having stated.
 */
struct recon_codec {
    char name[RECON_CODEC_NAME_MAX];
    /* Space-separated, with dots: ".wav .wave" */
    char extensions[RECON_CODEC_EXTENSIONS_MAX];

    /*
     * Does this decoder recognise these bytes?
     *
     * Asked *before* the extension, because a file's contents are what it is
     * and its name is what somebody called it. A WAV named .mp3 should play.
     */
    bool (*sniff)(const uint8_t *bytes, size_t length);

    /* NULL when the file is not really this format after all. */
    struct recon_codec_reader *(*open)(const uint8_t *bytes, size_t length,
        struct recon_codec_format *format);

    /* Fill `out` with up to `frames` frames; return how many. Zero at the end.
     * Called from the event loop and must not block. */
    int (*read)(struct recon_codec_reader *reader, int16_t *out, int frames);

    /* Move to a frame. False when the format cannot seek, which leaves the
     * position where it was rather than somewhere approximate. */
    bool (*seek)(struct recon_codec_reader *reader, uint64_t frame);

    void (*close)(struct recon_codec_reader *reader);
};

/*
 * Add a decoder. The struct is copied, so a module may keep its own on the
 * stack.
 *
 * False when the name is taken or there is no room. Registering the same name
 * twice is refused rather than replacing: two decoders answering to one name
 * is the collision this exists to prevent, and silently preferring the newer
 * would make which one runs depend on load order.
 */
bool recon_codec_register(const struct recon_codec *codec);
bool recon_codec_unregister(const char *name);

/*
 * --- Decoders that live inside a container ---
 *
 * A different shape, for a different situation. A file codec is handed a whole
 * file and finds its own way through it. A frame codec is handed one
 * compressed frame at a time by something that already knows where the frames
 * are -- a demuxer -- plus the setup bytes the container holds and the frames
 * do not.
 *
 * Forcing both through one interface would mean the container synthesising a
 * file for the decoder to parse: wrapping every AAC frame in an ADTS header so
 * a decoder can find a frame it was just handed. That is inventing a format to
 * feed a decoder that already has what it needs.
 *
 * This is where a codec pack plugs in. AAC and H.264 are not written here --
 * they are a hundred and seven hundred pages of specification respectively --
 * so a module registers them, and ReconOS says plainly which one is missing
 * when it cannot play something.
 */

struct recon_codec_frame_reader;

struct recon_codec_frames {
    /* Matched against the name a demuxer reports: "AAC", "H.264". */
    char name[RECON_CODEC_NAME_MAX];

    /*
     * `setup` is the container's own setup data -- AAC's two bytes saying its
     * real rate and channel count, H.264's parameter sets. Most decoders
     * cannot start without it, which is why it is not optional here.
     *
     * `rate` and `channels` are what the container *claims*; a decoder that
     * learns better from the setup data should write the truth back.
     */
    struct recon_codec_frame_reader *(*open)(const uint8_t *setup,
        size_t setup_length, int *rate, int *channels);

    /*
     * Decode one compressed frame into `out`, returning frames written.
     *
     * Zero is not an error: several formats begin with frames that prime the
     * decoder and produce no sound, and a caller that treated that as the end
     * would play nothing.
     */
    int (*decode)(struct recon_codec_frame_reader *reader,
        const uint8_t *frame, size_t length, int16_t *out, int max_frames);

    /* Throw away what the decoder is carrying between frames, after a seek --
     * without it the first frames after a jump are decoded against the state
     * of somewhere else entirely. May be NULL. */
    void (*reset)(struct recon_codec_frame_reader *reader);

    void (*close)(struct recon_codec_frame_reader *reader);
};

bool recon_codec_register_frames(const struct recon_codec_frames *codec);
bool recon_codec_unregister_frames(const char *name);

/*
 * --- Decoders that produce pictures ---
 *
 * The same arrangement again, and deliberately not the same interface.
 *
 * An audio frame decodes to a count of samples, and the caller knows what to do
 * with them because there is only one thing to do with them. A video frame
 * decodes to a picture that has a size the container may have lied about, a
 * colour space that has to be carried or the colours come out wrong, and a
 * moment it belongs at that is not the moment it was handed over.
 *
 * That last one is why `decode` reports whether a picture came out rather than
 * returning one every time. Modern video is not in order: a decoder is fed
 * frames in the order they are *stored*, which is not the order they are shown,
 * and it holds several while it waits for the one that comes first. A caller
 * that assumed one frame in meant one picture out would show the file scrambled
 * -- and would look, at a glance, like a decoder bug rather than a caller bug.
 */

struct recon_codec_video_reader;
struct recon_video_picture;

struct recon_codec_video {
    /* Matched against the name a demuxer reports: "H.264", "H.265". */
    char name[RECON_CODEC_NAME_MAX];

    /*
     * `setup` is the container's parameter sets -- H.264's avcC box. A decoder
     * cannot start without them: they carry the picture size, the profile and
     * the entropy coder, none of which the frames themselves repeat.
     *
     * `width` and `height` are what the container claims, and are written back
     * with what the decoder actually found. Those disagree more often than they
     * should, and the decoder is the one that is right.
     */
    struct recon_codec_video_reader *(*open)(const uint8_t *setup,
        size_t setup_length, int *width, int *height);

    /*
     * Feed one compressed frame. Returns 1 when a picture came out, 0 when the
     * decoder kept it for later, and -1 when the frame could not be decoded.
     *
     * `out` borrows the decoder's own memory and stays valid until the next
     * call. Nothing copies it -- a 1080p picture is three megabytes, and
     * copying one per frame costs more than every sum performed on it.
     */
    int (*decode)(struct recon_codec_video_reader *reader,
        const uint8_t *frame, size_t length, double time,
        struct recon_video_picture *out);

    /*
     * Pictures the decoder is still holding, one call at a time, after the last
     * frame has been fed. Returns 1 while there are more, 0 when there are not.
     *
     * Without this the last few pictures of every file are never shown, which
     * looks like a video that ends slightly early -- small enough to be blamed
     * on the file. May be NULL for a decoder that holds nothing.
     */
    int (*flush)(struct recon_codec_video_reader *reader,
        struct recon_video_picture *out);

    /* Throw away what is held between frames, after a seek. May be NULL. */
    void (*reset)(struct recon_codec_video_reader *reader);

    void (*close)(struct recon_codec_video_reader *reader);
};

bool recon_codec_register_video(const struct recon_codec_video *codec);
bool recon_codec_unregister_video(const char *name);

/* The picture decoder for a format, or NULL when none is installed. */
const struct recon_codec_video *recon_codec_video_for(const char *name);

int recon_codec_video_count(void);
bool recon_codec_video_at(int index, struct recon_codec_video *out);

/* The frame decoder for a format, or NULL when none is installed. The caller
 * says which one is missing, by name -- "there is no AAC decoder installed" is
 * something to act on. */
const struct recon_codec_frames *recon_codec_frames_for(const char *name);

int recon_codec_frames_count(void);
bool recon_codec_frames_at(int index, struct recon_codec_frames *out);

/*
 * Why the last open() failed, in a sentence.
 *
 * A decoder returns NULL for reasons that are nothing like each other -- the
 * file is not that format, the file is damaged, or the file is fine and the
 * decoder for what is inside it is not installed. A caller that cannot tell
 * them apart has to guess, and the guess it reached for was "or it is
 * damaged", which accused somebody's perfectly good video of being broken.
 */
const char *recon_codec_last_error(void);

/* Everything ReconOS itself can decode. Called once at startup. */
void recon_codec_init_builtin(void);

/*
 * The decoder for a file, by looking at it and then at its name.
 *
 * NULL when nothing here understands it, which is the common case and is not
 * an error -- the caller says so in words rather than showing an empty player.
 */
const struct recon_codec *recon_codec_for(const char *name,
    const uint8_t *bytes, size_t length);

/* What is installed, for a settings page: somebody wondering why a file will
 * not play should be able to see the list rather than guess at it. */
int recon_codec_count(void);
bool recon_codec_at(int index, struct recon_codec *out);

/*
 * Whether any decoder claims this extension, without reading the file.
 *
 * For the file list, which wants to know whether to offer to play something
 * before anybody has opened it.
 */
bool recon_codec_handles_extension(const char *name);

#endif /* RECON_CODEC_H */
