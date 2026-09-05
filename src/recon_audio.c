/*
 * Making a sound. See include/recon_audio.h.
 *
 * Two builds in one file. With ALSA present this drives a real device; without
 * it, every function is here and answers honestly that there is no audio on
 * this machine. That is not a stub for convenience -- a desktop on a machine
 * with no sound card takes exactly the same path, and it is the path that gets
 * tested least, so it is better that it is the same code.
 *
 * RECON_HAVE_ALSA is set by CMake when the library is found.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>

#include "recon_audio.h"

#ifdef RECON_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

/*
 * How often the device is topped up.
 *
 * Short enough that the buffer never runs dry between visits, long enough that
 * the loop is not woken constantly. The buffer holds about four of these, so
 * three can be missed -- by a slow redraw, by a page fault -- before anything
 * is audible.
 */
#define TOP_UP_MS 10

static char g_error[256];

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *recon_audio_last_error(void) {
    return g_error[0] != '\0' ? g_error : "no error";
}

/* --- Without a sound library --- */

#ifndef RECON_HAVE_ALSA

void recon_audio_init(struct wl_event_loop *loop) { (void)loop; }
void recon_audio_finish(void) { }

bool recon_audio_available(char *why_out, size_t why_size) {
    if (why_out != NULL && why_size > 0) {
        snprintf(why_out, why_size,
            "this build has no audio library, so nothing can be played");
    }
    return false;
}

const char *recon_audio_device_name(void) { return ""; }

struct recon_audio_stream *recon_audio_play(int rate, int channels,
        recon_audio_fill_fn fill, recon_audio_done_fn done, void *user) {
    (void)rate; (void)channels; (void)fill; (void)done; (void)user;
    set_error("this build has no audio library");
    return NULL;
}

int recon_audio_rate(struct recon_audio_stream *s) { (void)s; return 0; }
int recon_audio_channels(struct recon_audio_stream *s) { (void)s; return 0; }
void recon_audio_pause(struct recon_audio_stream *s, bool p) { (void)s; (void)p; }
bool recon_audio_paused(struct recon_audio_stream *s) { (void)s; return false; }
uint64_t recon_audio_frames_played(struct recon_audio_stream *s) {
    (void)s;
    return 0;
}
void recon_audio_stop(struct recon_audio_stream *s) { (void)s; }

#else /* RECON_HAVE_ALSA */

struct recon_audio_stream {
    snd_pcm_t *pcm;
    int rate;
    int channels;
    bool paused;
    bool source_finished;

    recon_audio_fill_fn fill;
    recon_audio_done_fn done;
    void *user;

    /* Handed to the device. What has actually been *played* is this minus
     * whatever is still queued, which is what a position display wants. */
    uint64_t frames_written;

    int16_t buffer[RECON_AUDIO_FRAMES_MAX * 2];
};

static struct wl_event_loop *g_loop;
static struct wl_event_source *g_timer;
static struct recon_audio_stream *g_stream;
static char g_device[128];
static bool g_looked;
static bool g_have_device;

void recon_audio_init(struct wl_event_loop *loop) {
    g_loop = loop;
}

/*
 * Is there anything to play through?
 *
 * Answered by opening the default device and closing it again, because that is
 * the only question that matters and every cheaper test lies. Enumerating
 * cards finds hardware that may be in use; reading a configuration file finds
 * what somebody wrote down. Opening it finds out.
 *
 * Done once and remembered: this is asked every time a window that can play
 * something is drawn, and opening a device is not free.
 */
bool recon_audio_available(char *why_out, size_t why_size) {
    if (!g_looked) {
        g_looked = true;

        snd_pcm_t *pcm = NULL;
        int rc = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK,
            SND_PCM_NONBLOCK);
        if (rc >= 0) {
            g_have_device = true;
            snprintf(g_device, sizeof(g_device), "%s", snd_pcm_name(pcm));
            snd_pcm_close(pcm);
        } else {
            snprintf(g_device, sizeof(g_device), "%s", "");
            set_error("no sound device: %s", snd_strerror(rc));
        }
    }

    if (!g_have_device && why_out != NULL && why_size > 0) {
        snprintf(why_out, why_size,
            "there is no sound device on this machine (%s)", g_error);
    }
    return g_have_device;
}

const char *recon_audio_device_name(void) {
    return g_device;
}

/* --- Topping up --- */

/*
 * Hand the device as much as it will take, and no more.
 *
 * `snd_pcm_avail_update` says how much room there is. Writing more than that
 * on a non-blocking device returns EAGAIN, which is fine; writing on a
 * blocking one would stop the desktop until the speaker caught up.
 */
static void top_up(struct recon_audio_stream *s) {
    if (s->paused) {
        return;
    }

    for (;;) {
        snd_pcm_sframes_t room = snd_pcm_avail_update(s->pcm);

        if (room < 0) {
            /*
             * An underrun. The device ran dry before we got back to it, which
             * on a desktop means something took too long -- and the answer is
             * to recover and carry on rather than to stop, because a click is
             * better than silence and a report nobody reads.
             */
            int rc = snd_pcm_recover(s->pcm, (int)room, 1 /* silent */);
            if (rc < 0) {
                set_error("the sound device stopped: %s", snd_strerror(rc));
                return;
            }
            continue;
        }

        if (room < 64) {
            return;                         /* full enough; come back later */
        }

        int want = (int)room;
        if (want > RECON_AUDIO_FRAMES_MAX) {
            want = RECON_AUDIO_FRAMES_MAX;
        }

        int got = 0;
        if (!s->source_finished && s->fill != NULL) {
            got = s->fill(s->user, s->buffer, want);
        }

        if (got <= 0) {
            /*
             * The source has run out. `drain` is not called: it blocks until
             * the device has played everything, and blocking the loop to wait
             * for a speaker is the thing this design exists to avoid. Instead
             * the stream stays open and empties itself, and `done` fires when
             * the last frame has actually been heard.
             */
            s->source_finished = true;

            snd_pcm_sframes_t delay = 0;
            if (snd_pcm_delay(s->pcm, &delay) == 0 && delay <= 0) {
                if (s->done != NULL) {
                    recon_audio_done_fn finished = s->done;
                    void *user = s->user;
                    s->done = NULL;
                    finished(user);
                }
            }
            return;
        }

        snd_pcm_sframes_t wrote = snd_pcm_writei(s->pcm, s->buffer, got);
        if (wrote < 0) {
            if (snd_pcm_recover(s->pcm, (int)wrote, 1) < 0) {
                set_error("the sound device stopped: %s",
                    snd_strerror((int)wrote));
                return;
            }
            continue;
        }

        s->frames_written += (uint64_t)wrote;
        if (wrote < got) {
            return;                    /* it took what it could; that is all */
        }
    }
}

static int on_timer(void *data) {
    (void)data;
    if (g_stream != NULL) {
        top_up(g_stream);
    }
    if (g_timer != NULL) {
        wl_event_source_timer_update(g_timer, TOP_UP_MS);
    }
    return 0;
}

/* --- Starting one --- */

struct recon_audio_stream *recon_audio_play(int rate, int channels,
        recon_audio_fill_fn fill, recon_audio_done_fn done, void *user) {
    if (fill == NULL || rate <= 0 || channels < 1 || channels > 2) {
        set_error("a stream needs a rate, one or two channels, and a source");
        return NULL;
    }
    if (!recon_audio_available(NULL, 0)) {
        return NULL;
    }
    if (g_stream != NULL) {
        /* One at a time. Two streams need mixing, which is a policy about
         * whose sound wins, and nothing here has asked for one yet. */
        recon_audio_stop(g_stream);
    }

    struct recon_audio_stream *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        set_error("out of memory");
        return NULL;
    }

    int rc = snd_pcm_open(&s->pcm, "default", SND_PCM_STREAM_PLAYBACK,
        SND_PCM_NONBLOCK);
    if (rc < 0) {
        set_error("cannot open the sound device: %s", snd_strerror(rc));
        free(s);
        return NULL;
    }

    /*
     * Asked for, not assumed. snd_pcm_set_params takes what it can get and
     * reports what it settled on, which may be a different rate -- a device
     * that only does 48000 given 44100 will resample or refuse, and knowing
     * which is the caller's business.
     */
    unsigned int actual_rate = (unsigned int)rate;
    rc = snd_pcm_set_params(s->pcm, SND_PCM_FORMAT_S16, /* little-endian */
        SND_PCM_ACCESS_RW_INTERLEAVED, (unsigned int)channels, actual_rate,
        1 /* allow resampling */,
        /* Four top-ups of latency. Short enough that pausing sounds
         * immediate, long enough to survive a slow frame. */
        TOP_UP_MS * 4 * 1000);

    if (rc < 0) {
        set_error("the sound device would not take %d Hz, %d channel%s: %s",
            rate, channels, channels == 1 ? "" : "s", snd_strerror(rc));
        snd_pcm_close(s->pcm);
        free(s);
        return NULL;
    }

    s->rate = rate;
    s->channels = channels;
    s->fill = fill;
    s->done = done;
    s->user = user;

    g_stream = s;

    if (g_timer == NULL && g_loop != NULL) {
        g_timer = wl_event_loop_add_timer(g_loop, on_timer, NULL);
    }
    if (g_timer != NULL) {
        wl_event_source_timer_update(g_timer, TOP_UP_MS);
    }

    /* Filled once immediately, so the first sound is not a top-up away. */
    top_up(s);
    return s;
}

int recon_audio_rate(struct recon_audio_stream *s) {
    return s != NULL ? s->rate : 0;
}

int recon_audio_channels(struct recon_audio_stream *s) {
    return s != NULL ? s->channels : 0;
}

void recon_audio_pause(struct recon_audio_stream *s, bool paused) {
    if (s == NULL || s->paused == paused) {
        return;
    }
    s->paused = paused;

    /*
     * The device is told, rather than only stopping the top-ups. Left running,
     * it would play out whatever is already queued -- a fifth of a second of
     * sound after the button was pressed, which reads as the button not
     * working.
     */
    if (snd_pcm_pause(s->pcm, paused ? 1 : 0) < 0) {
        /* Not every device can pause. Dropping what is queued is the next
         * best thing and is what the person asked for. */
        if (paused) {
            snd_pcm_drop(s->pcm);
        } else {
            snd_pcm_prepare(s->pcm);
        }
    }
}

bool recon_audio_paused(struct recon_audio_stream *s) {
    return s != NULL && s->paused;
}

uint64_t recon_audio_frames_played(struct recon_audio_stream *s) {
    if (s == NULL) {
        return 0;
    }

    /* What has been handed over, less what has not come out yet. */
    snd_pcm_sframes_t queued = 0;
    if (snd_pcm_delay(s->pcm, &queued) < 0 || queued < 0) {
        queued = 0;
    }
    if ((uint64_t)queued > s->frames_written) {
        return 0;
    }
    return s->frames_written - (uint64_t)queued;
}

void recon_audio_stop(struct recon_audio_stream *s) {
    if (s == NULL) {
        return;
    }
    if (g_stream == s) {
        g_stream = NULL;
    }

    snd_pcm_drop(s->pcm);
    snd_pcm_close(s->pcm);
    free(s);
}

void recon_audio_finish(void) {
    if (g_stream != NULL) {
        recon_audio_stop(g_stream);
    }
    if (g_timer != NULL) {
        wl_event_source_remove(g_timer);
        g_timer = NULL;
    }
    g_loop = NULL;
}

#endif /* RECON_HAVE_ALSA */
