/*
 * The Media Player window. See include/recon_player.h.
 *
 * The decoder is pulled from, not pushed to: recon_audio asks for samples and
 * this hands them over. Everything else -- the position, the transport, the
 * playlist -- hangs off that one callback, which is why there is no clock in
 * here. The device is the clock, and asking it how much it has actually played
 * is more truthful than counting.
 *
 * Video did not change that; it is the reason it matters. Pictures have no
 * clock of their own -- nothing consumes them at a fixed rate, so a player that
 * counts frames and assumes each took as long as it should have will drift away
 * from its own soundtrack and never notice. So the sound leads: the device is
 * asked what it has played, the picture for that moment is fetched, and a frame
 * that arrives late is dropped rather than shown late.
 *
 * A file with no sound has nothing to ask, and falls back to wall time. That is
 * a worse clock and it is still a real one, which counting is not.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_audio.h"
#include "recon_codec.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_movie.h"
#include "recon_server.h"
#include "recon_player.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_video.h"
#include "recon_users.h"

#define COLOR_BG THEME(SURFACE)
#define COLOR_PANEL THEME(SURFACE_ALT)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_DIM THEME(SURFACE_TEXT_DIM)
#define COLOR_SELECTED THEME(SELECTION)
#define COLOR_SELECTED_TEXT THEME(SELECTION_TEXT)
#define COLOR_BAR THEME(BAR)
#define COLOR_RULE THEME(MENU_SEPARATOR)
#define COLOR_ACCENT THEME(ACCENT)
#define COLOR_WARNING THEME(WARNING)

/* The name the network permission, the firewall and the Apps menu know this
 * by -- one string, for the reason recon_web.c gives at length. */
#define PLAYER_APPLICATION "Media Player"

#define TRACKS_MAX 512
#define TRANSPORT_HEIGHT 74
#define STATUS_HEIGHT 24
#define ROW_HEIGHT 22
#define PADDING 10
#define BUTTON 34
#define SCROLLBAR_WIDTH 7

/*
 * How often to ask the movie what should be on screen.
 *
 * Not the frame rate -- the rate at which the question is asked, which has to
 * be faster than any answer changes or frames are shown a tick late. Sixteen
 * milliseconds is a little over sixty times a second, and asking costs almost
 * nothing when the answer is no: recon_movie_advance returns false without
 * decoding anything until a frame is actually due.
 */
#define VIDEO_TICK_MS 16

/*
 * And how often to ask when there is no picture.
 *
 * The only thing that changes while sound plays is the position, which is shown
 * to the second, so a quarter of a second is four times finer than anything
 * anybody can see and sixteen times cheaper than the video rate. Asking at the
 * video rate for a file with no video would be redrawing a window sixty times a
 * second to move a bar once.
 */
#define AUDIO_TICK_MS 250

/* Room for the playlist under a picture. Four rows is enough to see what is
 * next without taking the window away from what it is for. */
#define LIST_ROWS_WITH_VIDEO 4

#define HIT_PLAY (RECON_APPWIN_HIT_USER + 1)
#define HIT_PREVIOUS (RECON_APPWIN_HIT_USER + 2)
#define HIT_NEXT (RECON_APPWIN_HIT_USER + 3)
#define HIT_STOP (RECON_APPWIN_HIT_USER + 4)
#define HIT_QUIETER (RECON_APPWIN_HIT_USER + 5)
#define HIT_LOUDER (RECON_APPWIN_HIT_USER + 6)
#define HIT_POSITION (RECON_APPWIN_HIT_USER + 7)
#define HIT_ROW_BASE (RECON_APPWIN_HIT_USER + 100)

struct track {
    char name[RECON_NAME_MAX];
    char path[RECON_PATH_MAX];
};

struct recon_player {
    struct recon_font *font;
    struct recon_appwin *win;

    struct track tracks[TRACKS_MAX];
    int count;
    int selected;
    int playing;               /* -1 when nothing is */
    int scroll;
    int rows_visible;

    /* The file, held while it plays. recon_codec hands the whole thing to a
     * decoder rather than streaming, so this stays until the track ends. */
    uint8_t *file;
    size_t file_length;

    const struct recon_codec *codec;
    struct recon_codec_reader *reader;
    struct recon_codec_format format;
    struct recon_audio_stream *stream;

    /*
     * The picture, when there is one. Null for a sound file, which is most of
     * them -- everything below is written so that the sound path does not
     * acquire a branch for the sake of the video one.
     */
    struct recon_movie *movie;
    struct wl_event_source *ticker;
    struct wl_event_loop *loop;

    /* Where the picture was drawn, so the size it is decoded at can follow the
     * window rather than the file. */
    int video_w, video_h;

    /*
     * Where the clock starts for a file with no sound.
     *
     * Kept in the same units the audio path uses -- seconds from the start of
     * the track -- so that everything downstream asks one question and does not
     * care which of the two answered it.
     */
    struct timespec wall_base;
    bool wall_running;

    /* The whole second the position was last drawn at, so the window is
     * redrawn when it changes and not sixty times a second while it does not. */
    int shown_second;

    /*
     * 0 to 16, and applied here rather than by the device.
     *
     * A device's mixer is the machine's, shared with everything else on it,
     * and a media player that turns the whole system down when somebody wants
     * this one track quieter has reached past its own edges.
     */
    int volume;

    /*
     * Where the current stream started in the track.
     *
     * recon_audio counts frames per stream, and seeking restarts the stream so
     * that what the device has already queued from the old place is dropped.
     * The track's position is therefore this plus what the stream has played;
     * without it, every seek reset the clock to zero and the next seek was
     * computed from the wrong place.
     */
    uint64_t base;

    /* Where the position bar was drawn, so a drag on it can be turned back
     * into a place in the track. */
    int bar_x, bar_y, bar_w;

    char status[192];
    bool status_is_error;
};

static void set_status(struct recon_player *p, bool error, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_status(struct recon_player *p, bool error, const char *fmt,
        ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(p->status, sizeof(p->status), fmt, args);
    va_end(args);
    p->status_is_error = error;
}

/* --- Playing --- */

/* The video clock and its timer, defined below next to each other because they
 * are one idea, and declared here because starting a track needs them. */
static void stop_ticking(struct recon_player *p);
static void start_ticking(struct recon_player *p);
static double clock_seconds(struct recon_player *p);

static void stop_playing(struct recon_player *p) {
    stop_ticking(p);
    if (p->movie != NULL) {
        recon_movie_close(p->movie);
        p->movie = NULL;
    }
    p->wall_running = false;
    p->shown_second = -1;
    p->video_w = 0;
    p->video_h = 0;

    if (p->stream != NULL) {
        recon_audio_stop(p->stream);
        p->stream = NULL;
    }
    if (p->reader != NULL && p->codec != NULL) {
        p->codec->close(p->reader);
        p->reader = NULL;
    }
    free(p->file);
    p->file = NULL;
    p->file_length = 0;
    p->codec = NULL;
    p->playing = -1;
    p->base = 0;
    memset(&p->format, 0, sizeof(p->format));
}

/*
 * Hand over the next samples.
 *
 * Runs from the event loop, inside recon_audio's top-up. It must not block and
 * must not touch anything that could tear down the stream it is being called
 * from -- which is why finishing a track is reported through `done` rather
 * than acted on here.
 */
static int on_fill(void *user, int16_t *out, int frames) {
    struct recon_player *p = user;
    if (p->reader == NULL || p->codec == NULL) {
        return 0;
    }

    int got = p->codec->read(p->reader, out, frames);
    if (got <= 0) {
        return 0;
    }

    /*
     * Volume, as a shift and a multiply on integers.
     *
     * 16 is unchanged, so the common case costs a comparison. Below that it is
     * value * volume / 16, which cannot overflow an int32 for any int16 input
     * and rounds towards zero -- inaudible at these magnitudes and much
     * cheaper than doing it in floating point for every sample of every track.
     */
    if (p->volume != 16) {
        int samples = got * p->format.channels;
        for (int i = 0; i < samples; i++) {
            out[i] = (int16_t)(((int32_t)out[i] * p->volume) / 16);
        }
    }
    return got;
}

static void play_index(struct recon_player *p, int index);

static void on_finished(void *user) {
    struct recon_player *p = user;

    /*
     * On to the next, which is what a player does. The last track stops
     * rather than wrapping: a playlist that starts again by itself is one
     * somebody has to notice and stop.
     */
    int next = p->playing + 1;
    if (next < p->count) {
        play_index(p, next);
    } else {
        stop_playing(p);
        set_status(p, false, "Finished.");
    }
    recon_appwin_refresh(p->win);
}

static void play_index(struct recon_player *p, int index) {
    if (index < 0 || index >= p->count) {
        return;
    }
    stop_playing(p);

    char why[160];
    if (!recon_audio_available(why, sizeof(why))) {
        set_status(p, true, "%s", why);
        return;
    }

    size_t length = 0;
    char *bytes = recon_fs_read("/", p->tracks[index].path, &length);
    if (bytes == NULL) {
        set_status(p, true, "%s", recon_fs_last_error());
        return;
    }

    /*
     * The picture first, because it decides what a failure to find sound means.
     *
     * A file with video and no decodable audio used to be a dead end. It is a
     * silent film now, which is the better answer -- and the reverse is
     * unchanged: a sound file simply has no movie and takes every path it took
     * before.
     */
    struct recon_movie *movie = recon_movie_open((const uint8_t *)bytes,
        length);

    const struct recon_codec *codec = recon_codec_for(p->tracks[index].name,
        (const uint8_t *)bytes, length);
    if (codec == NULL && movie == NULL) {
        free(bytes);
        set_status(p, true, "Nothing here can decode '%s'. %s",
            p->tracks[index].name, recon_movie_last_error());
        return;
    }

    struct recon_codec_format format;
    memset(&format, 0, sizeof(format));
    struct recon_codec_reader *reader = NULL;

    if (codec != NULL) {
        reader = codec->open((const uint8_t *)bytes, length, &format);
        if (reader == NULL && movie == NULL) {
            free(bytes);
            /*
             * The decoder's own words. It said "not a %s file after all, or it
             * is damaged" here, which is a guess -- and the guess was wrong for
             * the commonest case, a perfectly good video file with no decoder
             * for what is inside it.
             */
            set_status(p, true, "%s", recon_codec_last_error());
            return;
        }
    }

    p->file = (uint8_t *)bytes;
    p->file_length = length;
    p->codec = (reader != NULL) ? codec : NULL;
    p->reader = reader;
    p->format = format;
    p->movie = movie;

    if (reader != NULL) {
        p->stream = recon_audio_play(format.rate, format.channels, on_fill,
            on_finished, p);
        if (p->stream == NULL && movie == NULL) {
            set_status(p, true, "%s", recon_audio_last_error());
            stop_playing(p);
            return;
        }
    }

    p->base = 0;
    p->playing = index;
    p->selected = index;

    if (movie != NULL) {
        int vw = 0, vh = 0;
        recon_movie_size(movie, &vw, &vh);

        /*
         * A video with no sound runs on wall time, and the moment it starts is
         * recorded here rather than at the first tick. Starting the clock when
         * the first frame is asked for would make every decode delay part of
         * the timeline, so a slow first frame would put the whole file behind
         * and it would never catch up.
         */
        if (p->stream == NULL) {
            clock_gettime(CLOCK_MONOTONIC, &p->wall_base);
            p->wall_running = true;
        }
        if (p->stream != NULL) {
            set_status(p, false, "%s, %dx%d, with %s sound",
                recon_movie_format(movie), vw, vh, codec->name);
        } else {
            set_status(p, false, "%s, %dx%d, no sound track this can play",
                recon_movie_format(movie), vw, vh);
        }
    } else {
        set_status(p, false, "%s, %d Hz, %s", codec->name, format.rate,
            format.channels == 1 ? "mono" : "stereo");
    }

    /* Whatever is playing, something has to ask what time it is -- a position
     * bar that only moves when the window happens to be redrawn for some other
     * reason is one that looks broken exactly when it is being watched. */
    p->shown_second = -1;
    start_ticking(p);

    /* A title bar is not the place for a whole filename, so a long one is
     * cut. The list below shows it in full. */
    char title[160];
    snprintf(title, sizeof(title), "%.150s", p->tracks[index].name);
    recon_appwin_set_title(p->win, title);
}

/*
 * What time it is in this track, in seconds.
 *
 * The sound device when there is one, because it is the only thing here that
 * counts at a real rate: it consumes samples whether or not anything is
 * watching, and asking how many it has actually played is a measurement.
 * Wall time otherwise, which is what a file with no sound track has to use.
 *
 * These are the same units and mean the same thing, which is the point of
 * having one function: nothing above it has to know which clock answered.
 */
static double clock_seconds(struct recon_player *p) {
    if (p->stream != NULL && p->format.rate > 0) {
        return (double)(p->base + recon_audio_frames_played(p->stream)) /
            (double)p->format.rate;
    }

    if (!p->wall_running) {
        return (double)p->base;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)p->base +
        (double)(now.tv_sec - p->wall_base.tv_sec) +
        (double)(now.tv_nsec - p->wall_base.tv_nsec) / 1000000000.0;
}

/*
 * Ask the movie what should be on screen, and redraw only if it changed.
 *
 * The "only if" is the whole reason this is cheap. A video at twenty-five
 * frames a second asked sixty times a second says no more often than it says
 * yes, and a window that redrew on every tick would spend most of its effort
 * compositing a picture nobody had changed.
 */
static int on_tick(void *data) {
    struct recon_player *p = data;
    int again = (p->movie != NULL) ? VIDEO_TICK_MS : AUDIO_TICK_MS;

    if (p->stream == NULL && p->movie == NULL) {
        return 0;                      /* nothing is playing */
    }

    if (p->stream != NULL && recon_audio_paused(p->stream)) {
        /* Paused holds the picture where it is rather than freezing the clock
         * and letting it lurch forward on resume. */
        wl_event_source_timer_update(p->ticker, again);
        return 0;
    }

    bool changed = false;

    if (p->movie != NULL && recon_movie_advance(p->movie, clock_seconds(p))) {
        changed = true;
    }

    /*
     * And the position, whether or not there is a picture.
     *
     * Compared to the second because that is what the window shows. Redrawing
     * whenever the underlying number moved would redraw on every tick and
     * change nothing anybody can see -- the whole point of asking often is to
     * notice the moment it *does* change.
     */
    int shown = (int)clock_seconds(p);
    if (shown != p->shown_second) {
        p->shown_second = shown;
        changed = true;
    }

    if (changed) {
        recon_appwin_refresh(p->win);
    }
    wl_event_source_timer_update(p->ticker, again);
    return 0;
}

static void stop_ticking(struct recon_player *p) {
    if (p->ticker != NULL) {
        wl_event_source_remove(p->ticker);
        p->ticker = NULL;
    }
}

static void start_ticking(struct recon_player *p) {
    if (p->loop == NULL || p->ticker != NULL) {
        return;
    }
    p->ticker = wl_event_loop_add_timer(p->loop, on_tick, p);
    if (p->ticker != NULL) {
        wl_event_source_timer_update(p->ticker,
            (p->movie != NULL) ? VIDEO_TICK_MS : AUDIO_TICK_MS);
    }
}

/* Where the track has got to, in frames, asked of the device rather than
 * counted -- what has been handed over is up to a buffer ahead of what
 * somebody is hearing. */
static uint64_t position_frames(struct recon_player *p) {
    if (p->stream == NULL) {
        return p->base;
    }
    return p->base + recon_audio_frames_played(p->stream);
}

static void seek_to(struct recon_player *p, uint64_t frame) {
    /*
     * A video with no sound has no frames to count in, so its seeks arrive in
     * this function's units anyway -- the caller converts using the format's
     * rate, and a silent film borrows a nominal one. Handled first, because
     * everything below assumes there is a decoder to move.
     */
    if (p->reader == NULL || p->codec == NULL || p->codec->seek == NULL) {
        if (p->movie != NULL && p->format.rate > 0) {
            double when = (double)frame / (double)p->format.rate;
            if (recon_movie_seek(p->movie, when)) {
                p->base = frame;
                if (p->wall_running) {
                    clock_gettime(CLOCK_MONOTONIC, &p->wall_base);
                }
            }
        }
        return;
    }
    if (p->format.frames > 0 && frame >= p->format.frames) {
        frame = p->format.frames - 1;
    }

    /*
     * The stream is restarted rather than seeked underneath.
     *
     * Whatever the device already has queued was decoded from where the track
     * used to be, and letting it play out means a fifth of a second of the old
     * place after the new one was asked for. Stopping and starting drops it.
     */
    if (!p->codec->seek(p->reader, frame)) {
        set_status(p, true, "That file cannot be moved through.");
        return;
    }

    /* The new stream counts from zero, so this is where zero now is. */
    p->base = frame;

    if (p->stream != NULL) {
        recon_audio_stop(p->stream);
        p->stream = recon_audio_play(p->format.rate, p->format.channels,
            on_fill, on_finished, p);
    }

    /*
     * And the picture goes with it. Seeking the sound without seeking the
     * picture is the failure that looks most like a working player: everything
     * moves, the video simply carries on from where it was, and the two drift
     * apart by exactly however far the bar was dragged.
     */
    if (p->movie != NULL && p->format.rate > 0) {
        recon_movie_seek(p->movie, (double)frame / (double)p->format.rate);
        if (p->wall_running) {
            clock_gettime(CLOCK_MONOTONIC, &p->wall_base);
        }
    }
}

/* --- The playlist --- */

static void add_folder(struct recon_player *p, const char *folder) {
    struct recon_dirent entries[TRACKS_MAX];
    int count = recon_fs_list(folder, "", entries, TRACKS_MAX);

    for (int i = 0; i < count && p->count < TRACKS_MAX; i++) {
        if (entries[i].kind == RECON_FILE_DIRECTORY) {
            continue;
        }
        /* Offered only when something can decode it. A list full of names
         * that produce an error when pressed is worse than a short list. */
        if (!recon_codec_handles_extension(entries[i].name)) {
            continue;
        }

        struct track *track = &p->tracks[p->count++];
        snprintf(track->name, sizeof(track->name), "%s", entries[i].name);
        snprintf(track->path, sizeof(track->path), "%s/%s", folder,
            entries[i].name);
    }
}

static void refill_playlist(struct recon_player *p) {
    p->count = 0;

    const char *who = recon_users_current();
    char music[RECON_PATH_MAX];
    snprintf(music, sizeof(music), "/Users/%s/Music",
        (who != NULL && who[0] != '\0') ? who : "");
    add_folder(p, music);

    if (p->count == 0) {
        set_status(p, false, "There is nothing in your Music folder that this "
            "can play. It plays %s.", "WAV and MP3");
    } else {
        set_status(p, false, "%d track%s", p->count, p->count == 1 ? "" : "s");
    }
}

bool recon_player_open_path(struct recon_appwin *win, const char *path) {
    if (win == NULL || path == NULL) {
        return false;
    }
    struct recon_player *p = recon_appwin_user(win);
    if (p == NULL) {
        return false;
    }

    /* Already in the list? Play that rather than adding it twice. */
    for (int i = 0; i < p->count; i++) {
        if (strcmp(p->tracks[i].path, path) == 0) {
            play_index(p, i);
            recon_appwin_refresh(p->win);
            return true;
        }
    }

    if (p->count >= TRACKS_MAX) {
        set_status(p, true, "The list is full.");
        return false;
    }

    const char *leaf = strrchr(path, '/');
    struct track *track = &p->tracks[p->count];
    snprintf(track->name, sizeof(track->name), "%s",
        (leaf != NULL && leaf[1] != '\0') ? leaf + 1 : path);
    snprintf(track->path, sizeof(track->path), "%s", path);
    p->count++;

    play_index(p, p->count - 1);
    recon_appwin_refresh(p->win);
    return true;
}

/* --- Drawing --- */

/* Seconds as m:ss, which is what a track length is. */
static void as_time(uint64_t frames, int rate, char *out, size_t size) {
    if (rate <= 0) {
        snprintf(out, size, "--:--");
        return;
    }
    unsigned total = (unsigned)(frames / (uint64_t)rate);
    snprintf(out, size, "%u:%02u", total / 60, total % 60);
}

/* Which symbol a transport button carries. */
enum transport {
    TRANSPORT_PREVIOUS,
    TRANSPORT_PLAY,
    TRANSPORT_PAUSE,
    TRANSPORT_STOP,
    TRANSPORT_NEXT,
};

/*
 * A triangle pointing left or right, filled a row at a time.
 *
 * Exact rather than antialiased: at this size a play symbol with soft edges
 * looks blurred, not smooth.
 */
static void fill_triangle(struct recon_panel *panel, int x, int y, int w,
        int h, bool pointing_right, recon_color colour) {
    for (int row = 0; row < h; row++) {
        /* Distance from the vertical middle, as a fraction of half the
         * height -- the width of the row is what is left after that. */
        int from_middle = row - h / 2;
        if (from_middle < 0) {
            from_middle = -from_middle;
        }
        int span = w - (from_middle * w * 2) / h;
        if (span <= 0) {
            continue;
        }
        recon_fill_rect(panel, pointing_right ? x : x + (w - span),
            y + row, span, 1, colour);
    }
}

/*
 * The symbols, drawn rather than typed.
 *
 * They were the Unicode media control characters, which DejaVu Sans does not
 * have -- so every one of the four buttons drew as an empty box. Each is two
 * rectangles and a triangle, and this system draws its own icons, frames and
 * text; waiting on a typeface for a picture of a triangle was the odd one out.
 */
static void draw_symbol(struct recon_panel *panel, enum transport which,
        int x, int y, int size, recon_color ink) {
    int bar = size / 4;
    if (bar < 3) {
        bar = 3;
    }

    switch (which) {
    case TRANSPORT_PLAY:
        fill_triangle(panel, x + size / 4, y, size / 2, size, true, ink);
        break;

    case TRANSPORT_PAUSE: {
        int gap = bar;
        recon_fill_rect(panel, x + (size - bar * 2 - gap) / 2, y, bar, size,
            ink);
        recon_fill_rect(panel, x + (size - bar * 2 - gap) / 2 + bar + gap, y,
            bar, size, ink);
        break;
    }

    case TRANSPORT_STOP:
        recon_fill_rect(panel, x + size / 5, y + size / 5,
            size - size / 5 * 2, size - size / 5 * 2, ink);
        break;

    case TRANSPORT_PREVIOUS:
        /* A bar, then a triangle pointing at it. */
        recon_fill_rect(panel, x + size / 6, y, bar / 2 + 1, size, ink);
        fill_triangle(panel, x + size / 6 + bar / 2 + 2, y,
            size / 2, size, false, ink);
        break;

    case TRANSPORT_NEXT:
        fill_triangle(panel, x + size / 6, y, size / 2, size, true, ink);
        recon_fill_rect(panel, x + size / 6 + size / 2 + 1, y,
            bar / 2 + 1, size, ink);
        break;
    }
}

static void draw_button(struct recon_player *p, struct recon_panel *panel,
        int x, int y, enum transport which, uint32_t hit, bool on) {
    (void)p;

    recon_fill_rect(panel, x, y, BUTTON, BUTTON, COLOR_BAR);
    recon_draw_button_edge(panel, x, y, BUTTON, BUTTON, false, COLOR_BG);

    int size = BUTTON / 2;
    draw_symbol(panel, which, x + (BUTTON - size) / 2, y + (BUTTON - size) / 2,
        size, on ? COLOR_TEXT : COLOR_DIM);

    if (on) {
        recon_hit_add(panel, x, y, BUTTON, BUTTON, hit);
    }
}

/* The volume buttons keep their text: a minus and a plus are in every font
 * ever made, and drawing them would be inventing a problem. */
static void draw_text_button(struct recon_player *p, struct recon_panel *panel,
        int x, int y, const char *label, uint32_t hit, bool on) {
    int ascent = recon_font_ascent(p->font);

    recon_fill_rect(panel, x, y, BUTTON, BUTTON, COLOR_BAR);
    recon_draw_button_edge(panel, x, y, BUTTON, BUTTON, false, COLOR_BG);

    int w = recon_text_width(p->font, label);
    recon_draw_text(panel, p->font, x + (BUTTON - w) / 2,
        y + (BUTTON + ascent) / 2 - 2, BUTTON, label,
        on ? COLOR_TEXT : COLOR_DIM);

    if (on) {
        recon_hit_add(panel, x, y, BUTTON, BUTTON, hit);
    }
}

static void player_draw(void *user, struct recon_panel *panel, int x, int y,
        int w, int h) {
    struct recon_player *p = user;
    int ascent = recon_font_ascent(p->font);
    int line = recon_font_line_height(p->font);

    recon_fill_rect(panel, x, y, w, h, COLOR_BG);

    /* --- The status line --- */
    int status_y = y + h - STATUS_HEIGHT;
    recon_fill_rect(panel, x, status_y, w, STATUS_HEIGHT, COLOR_BAR);
    recon_fill_rect(panel, x, status_y, w, 1, COLOR_RULE);
    recon_draw_text(panel, p->font, x + 8,
        status_y + (STATUS_HEIGHT + ascent) / 2 - 1, w - 16, p->status,
        p->status_is_error ? COLOR_WARNING : COLOR_DIM);

    /* --- The transport, along the bottom --- */
    int transport_y = status_y - TRANSPORT_HEIGHT;
    recon_fill_rect(panel, x, transport_y, w, TRANSPORT_HEIGHT, COLOR_PANEL);
    recon_fill_rect(panel, x, transport_y, w, 1, COLOR_RULE);

    bool playing = p->stream != NULL;
    bool paused = playing && recon_audio_paused(p->stream);

    int bx = x + PADDING;
    int by = transport_y + 8;

    draw_button(p, panel, bx, by, TRANSPORT_PREVIOUS, HIT_PREVIOUS,
        p->playing > 0);
    bx += BUTTON + 4;
    /* The play button becomes a pause button while something is playing,
     * rather than sitting beside one: two buttons where only one ever does
     * anything is two chances to press the wrong one. */
    draw_button(p, panel, bx, by,
        (playing && !paused) ? TRANSPORT_PAUSE : TRANSPORT_PLAY, HIT_PLAY,
        p->count > 0);
    bx += BUTTON + 4;
    draw_button(p, panel, bx, by, TRANSPORT_STOP, HIT_STOP, playing);
    bx += BUTTON + 4;
    draw_button(p, panel, bx, by, TRANSPORT_NEXT, HIT_NEXT,
        p->playing >= 0 && p->playing + 1 < p->count);
    bx += BUTTON + 12;

    /* Volume, as two buttons and a number. A slider would be a drag target
     * three pixels wide at this size. */
    draw_text_button(p, panel, bx, by, "\xE2\x88\x92", HIT_QUIETER,
        p->volume > 0);
    bx += BUTTON + 2;

    char loud[16];
    snprintf(loud, sizeof(loud), "%d%%", p->volume * 100 / 16);
    int lw = recon_text_width(p->font, "100%");
    recon_draw_text(panel, p->font, bx, by + (BUTTON + ascent) / 2 - 2, lw + 8,
        loud, COLOR_DIM);
    bx += lw + 10;

    draw_text_button(p, panel, bx, by, "+", HIT_LOUDER, p->volume < 16);

    /* --- The position bar --- */
    uint64_t at = position_frames(p);
    uint64_t total = p->format.frames;

    /*
     * A file with pictures and no sound has no frames to count in, so it
     * borrows a rate and the movie's own duration. Fifty is not a sample rate
     * anybody uses, which is deliberate: it is a unit for the bar to divide,
     * not a claim about audio, and picking a plausible-looking 44100 would
     * invite somebody to read it as one.
     */
    if (p->movie != NULL && p->format.rate == 0) {
        p->format.rate = 50;
        p->format.frames =
            (uint64_t)(recon_movie_duration(p->movie) * 50.0);
        at = position_frames(p);
        total = p->format.frames;
    }
    if (p->movie != NULL && p->stream == NULL) {
        /* Wall time, in the same borrowed units. */
        at = (uint64_t)(clock_seconds(p) * (double)p->format.rate);
        if (total > 0 && at > total) {
            at = total;
        }
    }

    char now[16], end[16];
    as_time(at, p->format.rate, now, sizeof(now));
    as_time(total, p->format.rate, end, sizeof(end));

    int time_w = recon_text_width(p->font, "00:00") + 8;
    int bar_y = by + BUTTON + 8;

    recon_draw_text(panel, p->font, x + PADDING, bar_y + ascent, time_w, now,
        COLOR_DIM);
    recon_draw_text(panel, p->font, x + w - PADDING - time_w, bar_y + ascent,
        time_w, end, COLOR_DIM);

    p->bar_x = x + PADDING + time_w;
    p->bar_y = bar_y + 4;
    p->bar_w = w - PADDING * 2 - time_w * 2;

    if (p->bar_w > 0) {
        recon_fill_rect(panel, p->bar_x, p->bar_y, p->bar_w, 6, COLOR_BG);
        recon_stroke_rect(panel, p->bar_x, p->bar_y, p->bar_w, 6, COLOR_RULE);

        if (total > 0) {
            int filled = (int)((long long)p->bar_w * (long long)at /
                (long long)total);
            if (filled > p->bar_w) {
                filled = p->bar_w;
            }
            recon_fill_rect(panel, p->bar_x, p->bar_y, filled, 6, COLOR_ACCENT);
            recon_hit_add(panel, p->bar_x, p->bar_y - 6, p->bar_w, 18,
                HIT_POSITION);
        }
    }

    /* --- The picture --- */
    int top = y + PADDING;
    int bottom = transport_y - PADDING;

    if (p->movie != NULL) {
        /*
         * The picture takes what is left after a short strip of playlist.
         *
         * Not the whole area: a media player that hides what is playing next
         * the moment a video starts has stopped being a playlist, and the way
         * back is to stop the thing you are watching. Four rows costs about
         * ninety pixels and keeps both.
         */
        int reserved = LIST_ROWS_WITH_VIDEO * ROW_HEIGHT + PADDING;
        int box_h = bottom - top - reserved;
        int box_w = w - PADDING * 2;

        if (box_h > 40 && box_w > 40) {
            int pw = 0, ph = 0;
            int vw = 0, vh = 0;
            recon_movie_size(p->movie, &vw, &vh);
            recon_video_fit(vw, vh, box_w, box_h, &pw, &ph);

            /*
             * Told what size to decode at from here, which is the one place
             * that knows. Asking for the window's size means the colour
             * conversion happens once per pixel that will be seen rather than
             * once per pixel in the file -- a 1080p frame in a 400-pixel box is
             * nine tenths less work, every frame.
             */
            if (pw != p->video_w || ph != p->video_h) {
                if (recon_movie_set_size(p->movie, pw, ph)) {
                    p->video_w = pw;
                    p->video_h = ph;
                }
            }

            /* Centred, on black. Black rather than the window's colour because
             * a picture is a thing being looked at and the surround should get
             * out of its way. */
            int px = x + PADDING + (box_w - pw) / 2;
            int py = top + (box_h - ph) / 2;
            recon_fill_rect(panel, x + PADDING, top, box_w, box_h, 0xFF000000);

            const unsigned char *pixels = recon_movie_pixels(p->movie);
            if (pixels != NULL) {
                recon_draw_image(panel, px, py, pw, ph, pixels, pw, ph);
            }

            top += box_h + PADDING;
        }
    }

    /* --- The list --- */
    p->rows_visible = (bottom - top) / ROW_HEIGHT;
    if (p->rows_visible < 1) {
        p->rows_visible = 1;
    }

    if (p->count == 0) {
        recon_draw_text(panel, p->font, x + PADDING, top + ascent,
            w - PADDING * 2,
            "Nothing to play. Put a .wav, .mp3 or .mp4 in your Music folder.",
            COLOR_DIM);
        return;
    }

    if (p->scroll > p->count - p->rows_visible) {
        p->scroll = p->count - p->rows_visible;
    }
    if (p->scroll < 0) {
        p->scroll = 0;
    }

    int list_w = w - PADDING * 2 - SCROLLBAR_WIDTH;
    recon_fill_rect(panel, x + PADDING, top, list_w,
        p->rows_visible * ROW_HEIGHT, COLOR_PANEL);

    for (int row = 0; row < p->rows_visible; row++) {
        int i = p->scroll + row;
        if (i >= p->count) {
            break;
        }
        int ry = top + row * ROW_HEIGHT;

        if (i == p->selected) {
            recon_fill_rect(panel, x + PADDING, ry, list_w, ROW_HEIGHT,
                COLOR_SELECTED);
        }

        uint32_t ink = (i == p->selected) ? COLOR_SELECTED_TEXT : COLOR_TEXT;

        /* The one playing carries a mark, so it is findable in a long list
         * without reading the whole thing. */
        if (i == p->playing) {
            /* Drawn, for the same reason as the buttons. */
            draw_symbol(panel, TRANSPORT_PLAY, x + PADDING + 6,
                ry + (ROW_HEIGHT - 9) / 2, 9,
                (i == p->selected) ? ink : COLOR_ACCENT);
        }

        recon_draw_text(panel, p->font, x + PADDING + 22,
            ry + (ROW_HEIGHT + ascent) / 2 - 1, list_w - 28,
            p->tracks[i].name, ink);

        recon_hit_add(panel, x + PADDING, ry, list_w, ROW_HEIGHT,
            HIT_ROW_BASE + row);
    }

    /* A scrollbar, when there is more than fits. */
    if (p->count > p->rows_visible) {
        int sx = x + w - PADDING - SCROLLBAR_WIDTH;
        int sh = p->rows_visible * ROW_HEIGHT;
        recon_fill_rect(panel, sx, top, SCROLLBAR_WIDTH, sh, COLOR_BAR);

        int thumb = sh * p->rows_visible / p->count;
        if (thumb < 12) {
            thumb = 12;
        }
        int most = p->count - p->rows_visible;
        int thumb_at = (most > 0) ? (sh - thumb) * p->scroll / most : 0;
        recon_fill_rect(panel, sx + 1, top + thumb_at, SCROLLBAR_WIDTH - 2,
            thumb, COLOR_DIM);
    }
    (void)line;
}

/* --- Input --- */

static void toggle_play(struct recon_player *p) {
    if (p->stream != NULL) {
        recon_audio_pause(p->stream, !recon_audio_paused(p->stream));
        return;
    }
    if (p->count > 0) {
        play_index(p, p->selected >= 0 ? p->selected : 0);
    }
}

static bool player_click(void *user, uint32_t hit, int cx, int cy,
        bool pressed) {
    struct recon_player *p = user;
    (void)cy;

    if (!pressed || hit < RECON_APPWIN_HIT_USER) {
        return false;
    }

    if (hit >= HIT_ROW_BASE) {
        int i = p->scroll + (int)(hit - HIT_ROW_BASE);
        if (i < 0 || i >= p->count) {
            return true;
        }
        /* One click selects; a second on the same row plays it, the way File
         * Explorer opens a file. */
        if (p->selected == i) {
            play_index(p, i);
        } else {
            p->selected = i;
            set_status(p, false, "'%s' - click again to play",
                p->tracks[i].name);
        }
        return true;
    }

    switch (hit) {
    case HIT_PLAY:
        toggle_play(p);
        return true;
    case HIT_STOP:
        stop_playing(p);
        set_status(p, false, "Stopped.");
        return true;
    case HIT_PREVIOUS:
        if (p->playing > 0) {
            play_index(p, p->playing - 1);
        }
        return true;
    case HIT_NEXT:
        if (p->playing >= 0 && p->playing + 1 < p->count) {
            play_index(p, p->playing + 1);
        }
        return true;
    case HIT_QUIETER:
        if (p->volume > 0) {
            p->volume--;
        }
        return true;
    case HIT_LOUDER:
        if (p->volume < 16) {
            p->volume++;
        }
        return true;
    case HIT_POSITION: {
        if (p->format.frames == 0 || p->bar_w <= 0) {
            return true;
        }
        long long into = (long long)(cx - p->bar_x);
        if (into < 0) {
            into = 0;
        }
        if (into > p->bar_w) {
            into = p->bar_w;
        }
        seek_to(p, (uint64_t)((long long)p->format.frames * into / p->bar_w));
        return true;
    }
    default:
        return false;
    }
}

static bool player_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_player *p = user;
    (void)modifiers;

    switch (sym) {
    case XKB_KEY_space:
        toggle_play(p);
        return true;
    case XKB_KEY_Down:
        if (p->selected + 1 < p->count) {
            p->selected++;
        }
        return true;
    case XKB_KEY_Up:
        if (p->selected > 0) {
            p->selected--;
        }
        return true;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (p->selected >= 0) {
            play_index(p, p->selected);
        }
        return true;
    case XKB_KEY_Right:
    case XKB_KEY_Left: {
        /* Ten seconds, which is the step every player uses because it is long
         * enough to be worth pressing and short enough to press twice. */
        if (p->stream == NULL || p->format.rate <= 0) {
            return true;
        }
        uint64_t step = (uint64_t)p->format.rate * 10;
        uint64_t at = position_frames(p);
        seek_to(p, (sym == XKB_KEY_Right) ? at + step
            : (at > step ? at - step : 0));
        return true;
    }
    case XKB_KEY_plus:
    case XKB_KEY_equal:
        if (p->volume < 16) {
            p->volume++;
        }
        return true;
    case XKB_KEY_minus:
        if (p->volume > 0) {
            p->volume--;
        }
        return true;
    default:
        return false;
    }
}

static void player_scroll(void *user, double delta) {
    struct recon_player *p = user;
    p->scroll -= (int)(delta * 3);
    if (p->scroll < 0) {
        p->scroll = 0;
    }
}

static void player_describe(void *user, char *out, size_t size) {
    struct recon_player *p = user;
    char now[16], end[16];
    as_time(position_frames(p), p->format.rate, now, sizeof(now));
    as_time(p->format.frames, p->format.rate, end, sizeof(end));

    snprintf(out, size,
        "  tracks: %d  selected: %d  playing: %d\n"
        "  file: %s\n"
        "  codec: %s  %d Hz  %d ch\n"
        "  position: %s of %s (%llu frames)\n"
        "  paused: %s  volume: %d/16\n"
        "  status: %s\n",
        p->count, p->selected, p->playing,
        (p->playing >= 0 && p->playing < p->count)
            ? p->tracks[p->playing].name : "(none)",
        p->codec != NULL ? p->codec->name : "(none)",
        p->format.rate, p->format.channels,
        now, end, (unsigned long long)position_frames(p),
        (p->stream != NULL && recon_audio_paused(p->stream)) ? "yes" : "no",
        p->volume, p->status);
}

static void player_destroy(void *user) {
    struct recon_player *p = user;
    stop_playing(p);
    free(p);
}

static const struct recon_appwin_impl PLAYER_IMPL = {
    .title = PLAYER_APPLICATION,
    .help = "Writing",
    .icon = RECON_ICON_PLAYER,
    .default_width = 520,
    .default_height = 420,
    .min_width = 380,
    .min_height = 240,
    .draw = player_draw,
    .click = player_click,
    .key = player_key,
    .scroll = player_scroll,
    .describe = player_describe,
    .destroy = player_destroy,
};

struct recon_appwin *recon_player_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_player *p = calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }

    p->font = font;
    p->loop = wl_display_get_event_loop(server->wl_display);
    p->playing = -1;
    p->selected = 0;
    p->volume = 16;

    refill_playlist(p);

    /*
     * Said here rather than when somebody presses play. A machine with no
     * sound device should say so while the window is being looked at, not
     * after a button has been pressed and nothing happened.
     */
    char why[160];
    if (!recon_audio_available(why, sizeof(why))) {
        set_status(p, true, "%s", why);
    }

    p->win = recon_appwin_create(server, font, &PLAYER_IMPL, p);
    if (p->win == NULL) {
        free(p);
        return NULL;
    }
    return p->win;
}
