/*
 * Making a sound.
 *
 * The same bargain `recon_fs` and `recon_net` make, for the same reason: this
 * is the one file that knows how audio reaches hardware on the machine
 * underneath, and nothing above it does. When ReconOS has a driver of its own,
 * this is what changes and nothing else does.
 *
 * ALSA is what it speaks today, and that is a deliberate choice rather than
 * the easiest one. It is the lowest thing on Linux that is still an interface
 * -- a thin layer over the ioctls a driver exposes -- so the shape of this
 * header is close to the shape a real driver has, and replacing it later is
 * replacing a file rather than rethinking an abstraction. A sound server would
 * have been easier to get working and would have put a daemon, a protocol and
 * a mixing policy inside the thing being replaced.
 *
 * THIRD_PARTY.md's line is "libraries may parse formats and talk to hardware".
 * This is the second half of that sentence.
 *
 * --- Pulled, not pushed ---
 *
 * The caller does not hand samples over; it is asked for them. A device runs
 * at its own rate and wants a fixed number of frames at fixed moments, and an
 * interface where the caller decides when to write is one where the caller is
 * responsible for a clock it does not own. Every underrun would then be
 * somebody else's bug.
 *
 * So: open a stream with a function that fills a buffer, and it is called when
 * there is room. It runs from the event loop, not from a thread -- ReconOS is
 * one process with one loop, and audio is not a good enough reason to change
 * that.
 */

#ifndef RECON_AUDIO_H
#define RECON_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_event_loop;

/*
 * Sixteen-bit signed samples, interleaved, and nothing else.
 *
 * Every decoder here produces them and the device is asked for them, so there
 * is one format in the middle rather than a conversion matrix. It is what a CD
 * is, it is what everything can play, and the difference between it and
 * anything wider is inaudible on the hardware a desktop has.
 */
#define RECON_AUDIO_BITS 16

/* What a stream will be asked for at once, at most. Sets the latency: at
 * 44100 this is about a fortieth of a second, which is short enough that a
 * pause sounds immediate and long enough not to wake the loop constantly. */
#define RECON_AUDIO_FRAMES_MAX 1024

struct recon_audio_stream;

/*
 * Fill `out` with up to `frames` frames and return how many were written.
 *
 * A frame is one sample per channel, so a stereo frame is two int16_t. Fewer
 * than asked for means the source has run out; zero means it is finished, and
 * the stream stops itself and calls `finished`.
 *
 * This runs from the event loop. It must not block: everything else on the
 * desktop is waiting behind it.
 */
typedef int (*recon_audio_fill_fn)(void *user, int16_t *out, int frames);

/* Called once, from the loop, after the source has run out and everything
 * queued has actually been played. Not when the last sample was *handed over*,
 * which is up to a buffer earlier and is where a player that advances to the
 * next track too soon gets it wrong. */
typedef void (*recon_audio_done_fn)(void *user);

/*
 * Bring audio up. Safe to call when there is no sound hardware.
 *
 * The event loop is where the topping-up happens, so it is needed here rather
 * than at every open.
 */
void recon_audio_init(struct wl_event_loop *loop);
void recon_audio_finish(void);

/*
 * Whether a sound could be made at all, and why not when it could not.
 *
 * Asked before offering to play something, so a machine with no sound card
 * says so up front instead of failing at the moment somebody presses play.
 * A build with no audio library at all answers this too, and says that.
 */
bool recon_audio_available(char *why_out, size_t why_size);

/* What the device is called, for a settings page. "" when there is none. */
const char *recon_audio_device_name(void);

/*
 * Start playing.
 *
 * `rate` is frames per second and `channels` is 1 or 2. The device may not
 * support what was asked for; what it settled on is readable afterwards, and
 * a caller that cares has to resample -- this does not, because resampling
 * badly is worse than saying what the rate is.
 *
 * NULL when a stream could not be started, with recon_audio_last_error saying
 * why. Only one stream at a time: mixing is a policy, and a system with one
 * media player does not need one yet.
 */
struct recon_audio_stream *recon_audio_play(int rate, int channels,
    recon_audio_fill_fn fill, recon_audio_done_fn done, void *user);

/* What the device actually settled on, which may not be what was asked. */
int recon_audio_rate(struct recon_audio_stream *stream);
int recon_audio_channels(struct recon_audio_stream *stream);

/*
 * Stop asking for samples without closing the device.
 *
 * Paused rather than stopped, because the difference matters to the hardware:
 * closing and reopening a device makes an audible click and takes long enough
 * to hear.
 */
void recon_audio_pause(struct recon_audio_stream *stream, bool paused);
bool recon_audio_paused(struct recon_audio_stream *stream);

/*
 * How many frames have actually reached the hardware.
 *
 * What a position display should show. The number of frames *handed over* is
 * up to a buffer ahead of what a person is hearing, and a progress bar built
 * on it runs visibly early.
 */
uint64_t recon_audio_frames_played(struct recon_audio_stream *stream);

void recon_audio_stop(struct recon_audio_stream *stream);

const char *recon_audio_last_error(void);

#endif /* RECON_AUDIO_H */
