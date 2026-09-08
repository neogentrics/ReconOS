/*
 * Photos. See include/recon_photos.h.
 *
 * One picture is decoded at a time and held as RGBA. That is a deliberate
 * ceiling: a folder of forty photographs at twelve megapixels each would be
 * two gigabytes if they were all kept, and the window shows one.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "stb_image.h"

#include "ReconOS.h"
#include "recon_appwin.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_ocr_match.h"
#include "recon_photos.h"
#include "recon_image.h"
#include "recon_png.h"
#include "recon_server.h"
#include "recon_shell.h"
#include "recon_server.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_widget.h"

#define COLOR_BG THEME(SURFACE)
#define COLOR_TEXT THEME(SURFACE_TEXT)
#define COLOR_DIM THEME(SURFACE_TEXT_DIM)
#define COLOR_WARNING THEME(WARNING)
#define COLOR_BAR THEME(BAR)

/*
 * The mat the picture sits on.
 *
 * Deliberately not the window's surface colour. A photograph judged against a
 * light grey panel reads differently from the same photograph on a dark one,
 * and every gallery in the world settled on dark for the same reason.
 */
#define COLOR_MAT RECON_RGB(0x1A, 0x1A, 0x1E)

#define BAR_HEIGHT 30
#define PADDING 8

#define HIT_PREVIOUS (RECON_APPWIN_HIT_USER + 1)
#define HIT_NEXT (RECON_APPWIN_HIT_USER + 2)
#define HIT_FIT (RECON_APPWIN_HIT_USER + 3)
#define HIT_PICTURE (RECON_APPWIN_HIT_USER + 4)
#define HIT_READ (RECON_APPWIN_HIT_USER + 5)
#define HIT_CONVERT (RECON_APPWIN_HIT_USER + 6)
#define HIT_SMALLER (RECON_APPWIN_HIT_USER + 7)
#define HIT_BIGGER (RECON_APPWIN_HIT_USER + 8)

/*
 * How small and how large a picture is allowed to be made.
 *
 * The floor is so that halving repeatedly cannot produce a picture with no
 * pixels in it, which is not a picture. The ceiling is memory: a resample
 * holds the old picture and the new one at once, so doubling a large
 * photograph twice is asking for the better part of a gigabyte -- and the
 * refusal is at the point of asking rather than at the point of running out.
 */
#define RESIZE_MIN 16
#define RESIZE_MAX 12000

/*
 * Above this, ask first.
 *
 * The read is synchronous and the desktop is single-threaded, so whatever it
 * costs is time nothing else answers for. Measured rather than guessed: a
 * 1920x1080 screenshot reads in about forty milliseconds and a 2400-pixel-wide
 * page in about a hundred, both including opening the typeface and decoding the
 * file. Nothing at those sizes is worth interrupting somebody for.
 *
 * Twelve megapixels is roughly a quarter of a second by that rate -- the point
 * at which a person would notice the machine stop rather than merely fail to
 * notice it running. A photograph from a camera is the picture that crosses it.
 */
#define ASK_ABOVE_PIXELS 12000000LL

/*
 * Long enough for the "Reading" line to be on screen before the reading starts.
 *
 * The work happens on a timer rather than on the click for exactly this: a
 * synchronous read inside the click handler would freeze with the *previous*
 * status showing, so the one moment the machine is busy is the one moment it
 * has not said so.
 */
#define READ_SETTLE_MS 40

#define PICTURES_MAX 512

struct recon_photos {
    struct recon_font *font;
    struct recon_appwin *win;

    /* The folder being looked through, and everything in it that decodes. */
    char folder[RECON_PATH_MAX];
    char names[PICTURES_MAX][RECON_NAME_MAX];
    int count;
    int at;

    /* The one on screen. NULL when nothing has loaded. */
    unsigned char *pixels;
    int width, height;

    /*
     * Whether what is on screen is still what is in the file.
     *
     * Photos had no unsaved state at all until it could resize, and a window
     * whose contents no longer match the file it names has to say so
     * somewhere. Cleared by loading anything, because loading is what makes
     * them agree again.
     */
    bool resized;

    /*
     * False shows the picture at its own size, which for anything from a
     * camera means a corner of it. True fits the whole thing in the window.
     */
    bool fit;

    char message[192];
    bool message_is_warning;

    /* --- Reading the text in a picture --- */

    /* Kept rather than reached for through the window, because tearing down
     * has to cancel a question and must not depend on what is still alive. */
    struct recon_shell *shell;
    struct wl_event_source *read_timer;

    /* Opened the first time somebody reads, and held. Parsing a typeface is
     * the expensive part of a small read, and it does not change. */
    struct recon_ocr_font *ocr;

    /*
     * Which picture the pending read was asked for, and what it was called.
     *
     * Forty milliseconds is enough to press Next, and reading the picture
     * somebody has moved on from is worse than reading none: the file would be
     * named after one picture and hold the text of another.
     */
    int reading_at;                     /* -1 when nothing is pending */
    char reading_name[RECON_NAME_MAX];

    /* A guess, held while its question is on screen. */
    struct recon_ocr_result held;
    bool holding;
};

static void set_message(struct recon_photos *ph, bool warning, const char *fmt,
    ...) __attribute__((format(printf, 3, 4)));

static void set_message(struct recon_photos *ph, bool warning,
        const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(ph->message, sizeof(ph->message), fmt, args);
    va_end(args);
    ph->message_is_warning = warning;
}

/* Whether the name ends in something stb_image can decode. */
static int on_read_tick(void *user);
static void begin_reading(struct recon_photos *ph);
static void on_size_answer(void *user, int choice);

static bool looks_like_a_picture(const char *name) {
    static const char *const KINDS[] = { ".png", ".jpg", ".jpeg", ".bmp",
        ".gif", ".tga", ".psd", NULL };

    size_t length = strlen(name);
    for (int i = 0; KINDS[i] != NULL; i++) {
        size_t kind = strlen(KINDS[i]);
        if (length > kind &&
                strcasecmp(name + length - kind, KINDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void forget_picture(struct recon_photos *ph) {
    if (ph->pixels != NULL) {
        stbi_image_free(ph->pixels);
        ph->pixels = NULL;
    }
    ph->width = 0;
    ph->height = 0;
}

/*
 * Decode whichever picture `at` points to.
 *
 * A picture that will not decode is reported and skipped over rather than
 * emptying the window: a folder with one damaged file in it should still be
 * something you can page through.
 */
/*
 * The window's name, and a mark when what is on screen is not what is in the
 * file.
 *
 * The same star Notepad uses, for the same reason and deliberately not a
 * different sign: two applications with unsaved work should not need to be
 * learned separately. Photos had nothing to put here until it could resize --
 * every other thing it does either changes nothing or writes a new file.
 */
static void retitle(struct recon_photos *ph) {
    if (ph == NULL || ph->win == NULL) {
        return;
    }

    char title[RECON_NAME_MAX + 32];
    const char *name = (ph->at >= 0 && ph->at < ph->count)
        ? ph->names[ph->at] : NULL;

    if (name == NULL) {
        snprintf(title, sizeof(title), "Photos");
    } else {
        snprintf(title, sizeof(title), "%s%s - Photos", name,
            ph->resized ? " *" : "");
    }
    recon_appwin_set_title(ph->win, title);
}

static bool load_current(struct recon_photos *ph) {
    forget_picture(ph);

    /* Whatever was on screen is gone, so any resize of it is gone with it. */
    ph->resized = false;

    if (ph->at < 0 || ph->at >= ph->count) {
        return false;
    }

    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), ph->folder, ph->names[ph->at])) {
        set_message(ph, true, "That path is too long to open.");
        return false;
    }

    /* stb_image reads from the host's filesystem, so the ReconOS path has to
     * be resolved first -- the same translation the font loader needs. */
    char host[RECON_PATH_MAX];
    char canonical[RECON_PATH_MAX];
    if (!recon_fs_resolve("/", path, host, sizeof(host), canonical,
            sizeof(canonical))) {
        set_message(ph, true, "'%s' could not be found.", ph->names[ph->at]);
        return false;
    }

    int channels = 0;
    ph->pixels = stbi_load(host, &ph->width, &ph->height, &channels, 4);
    if (ph->pixels == NULL) {
        ph->width = 0;
        ph->height = 0;
        set_message(ph, true, "'%s' could not be read as a picture.",
            ph->names[ph->at]);
        return false;
    }

    set_message(ph, false, "%s   %d by %d   %d of %d", ph->names[ph->at],
        ph->width, ph->height, ph->at + 1, ph->count);
    retitle(ph);
    return true;
}

/* Everything in the folder that might be a picture, in the order the
 * filesystem lists it. */
static void scan_folder(struct recon_photos *ph) {
    ph->count = 0;

    struct recon_dirent entries[PICTURES_MAX];
    int found = recon_fs_list("/", ph->folder, entries, PICTURES_MAX);
    if (found < 0) {
        return;
    }
    if (found > PICTURES_MAX) {
        found = PICTURES_MAX;
    }

    for (int i = 0; i < found && ph->count < PICTURES_MAX; i++) {
        if (entries[i].kind == RECON_FILE_DIRECTORY) {
            continue;
        }
        if (!looks_like_a_picture(entries[i].name)) {
            continue;
        }
        if (strlen(entries[i].name) >= RECON_NAME_MAX) {
            continue;
        }
        recon_text_copy(ph->names[ph->count], RECON_NAME_MAX,
            entries[i].name);
        ph->count++;
    }
}

static void step(struct recon_photos *ph, int by) {
    if (ph->count == 0) {
        return;
    }

    /*
     * Wraps round. A gallery that stops at the last picture makes somebody
     * click back through forty of them to reach the first, and there is no
     * harm in the loop -- the status line says which number this is, so
     * nobody loses their place.
     */
    ph->at = (ph->at + by + ph->count) % ph->count;
    load_current(ph);
}

bool recon_photos_open_path(struct recon_appwin *win, const char *path) {
    if (win == NULL || path == NULL) {
        return false;
    }

    struct recon_photos *ph = recon_appwin_user(win);
    if (ph == NULL) {
        return false;
    }

    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return false;
    }

    size_t folder_length = (size_t)(slash - path);
    if (folder_length == 0) {
        folder_length = 1;   /* The root itself. */
    }
    if (folder_length >= sizeof(ph->folder)) {
        return false;
    }
    memcpy(ph->folder, path, folder_length);
    ph->folder[folder_length] = '\0';

    scan_folder(ph);

    /* Find the one that was asked for, so the set opens where somebody
     * pointed rather than at whatever happens to be first. */
    const char *leaf = slash + 1;
    ph->at = 0;
    for (int i = 0; i < ph->count; i++) {
        if (strcmp(ph->names[i], leaf) == 0) {
            ph->at = i;
            break;
        }
    }

    bool ok = load_current(ph);
    recon_appwin_refresh(win);
    return ok;
}

/* --- Saving a picture as a PNG --- */

/*
 * The same picture, in a format that has not thrown anything away.
 *
 * Photos decodes seven formats and this system writes one, so this is the join
 * between them rather than anything new. It lands beside the original under a
 * name nothing else has, because a converter that overwrites is a converter
 * that loses the thing it converted.
 *
 * PNG out and nothing else, deliberately. The direction people want is almost
 * always this one -- a photograph arrives compressed and is wanted lossless to
 * work on -- and offering the other direction would mean a button that quietly
 * costs a little of the picture every time it is pressed.
 */
static void save_as_png(struct recon_photos *ph) {
    if (ph->pixels == NULL) {
        set_message(ph, false, "Nothing open to convert.");
        return;
    }

    /*
     * RGBA bytes into the packed colours the writer takes. Alpha is kept: a
     * picture with a transparent corner should still have one afterwards, and
     * being able to hold that is half the reason somebody converts.
     */
    size_t count = (size_t)ph->width * (size_t)ph->height;
    unsigned int *packed = malloc(count * sizeof(*packed));
    if (packed == NULL) {
        set_message(ph, true, "Not enough memory to convert that.");
        return;
    }
    for (size_t i = 0; i < count; i++) {
        const unsigned char *px = ph->pixels + i * 4;
        packed[i] = ((unsigned int)px[3] << 24) | ((unsigned int)px[0] << 16) |
            ((unsigned int)px[1] << 8) | (unsigned int)px[2];
    }

    char base[RECON_NAME_MAX];
    recon_text_copy(base, sizeof(base), ph->names[ph->at]);
    char *dot = strrchr(base, '.');
    if (dot != NULL && dot != base) {
        *dot = '\0';
    }
    if (base[0] == '\0') {
        recon_text_copy(base, sizeof(base), "Picture");
    }

    char leaf[RECON_NAME_MAX];
    if (!recon_fs_unique_name("/", ph->folder, base, ".png", leaf,
            sizeof(leaf))) {
        free(packed);
        set_message(ph, true, "%s", recon_fs_last_error());
        return;
    }

    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), ph->folder, leaf)) {
        free(packed);
        set_message(ph, true, "That name is too long to save as a picture.");
        return;
    }

    /* The writer goes through recon_fs itself, so the picture lands inside the
     * system and under the account rules like everything else here. */
    bool wrote = recon_png_write(path, packed, ph->width, ph->height, true);
    free(packed);

    if (!wrote) {
        set_message(ph, true, "%s", recon_png_last_error());
        return;
    }

    /*
     * The folder has one more picture in it than the list says. Rescanned, and
     * then the position moved back onto the picture that is on screen -- the
     * new file may sort before it, and stepping "next" should carry on from
     * what is being looked at rather than from wherever the index landed.
     */
    char was[RECON_NAME_MAX];
    recon_text_copy(was, sizeof(was), ph->names[ph->at]);
    scan_folder(ph);
    for (int i = 0; i < ph->count; i++) {
        if (strcmp(ph->names[i], was) == 0) {
            ph->at = i;
            break;
        }
    }

    set_message(ph, false, "Saved '%s' beside it.", leaf);
}

/* --- Making it a different size --- */

/*
 * Resample the open picture to a new size, in memory.
 *
 * Nothing is written. What is on screen changes and the file on disk does not,
 * so a resize is something somebody looks at before deciding to keep -- and
 * keeping it is Save as PNG, which already exists and already refuses to
 * overwrite. That split is deliberate: a resize that wrote straight to the
 * file would be the one operation in this application that destroys what it
 * was given.
 *
 * Which is also why the title bar says the picture has been changed. Photos
 * has never had unsaved state before, and a window whose contents no longer
 * match its file has to say so somewhere.
 */
static void resize_to(struct recon_photos *ph, int want_w, int want_h) {
    if (ph->pixels == NULL) {
        set_message(ph, false, "Nothing open to resize.");
        return;
    }

    if (want_w < RESIZE_MIN || want_h < RESIZE_MIN) {
        set_message(ph, true, "That is as small as this goes.");
        return;
    }
    if (want_w > RESIZE_MAX || want_h > RESIZE_MAX) {
        set_message(ph, true, "That is as large as this goes.");
        return;
    }

    unsigned char *bigger = malloc((size_t)want_w * want_h * 4);
    if (bigger == NULL) {
        set_message(ph, true, "Not enough memory for a picture that size.");
        return;
    }

    if (!recon_image_scale(ph->pixels, ph->width, ph->height, bigger,
            want_w, want_h)) {
        free(bigger);
        set_message(ph, true, "That size could not be worked out.");
        return;
    }

    /*
     * The old pixels are freed only after the new ones exist. A resize that
     * fails halfway should leave the picture that was on screen still on
     * screen, rather than an application showing nothing and a file it can no
     * longer be bothered to reload.
     */
    free(ph->pixels);
    ph->pixels = bigger;
    ph->width = want_w;
    ph->height = want_h;
    ph->resized = true;

    /*
     * Short, because two more buttons left the message strip narrow enough to
     * clip "Save as PNG keeps it." to "Sa...". How to keep it is on the Save
     * button's own tooltip and in the star in the title bar; a sentence that
     * runs out halfway is worse than the two words that fit.
     */
    set_message(ph, false, "Now %d by %d, unsaved", want_w, want_h);
    retitle(ph);
    recon_appwin_refresh(ph->win);
}

/* --- Drawing --- */


/* --- Reading the text in a picture --- */

/*
 * The provenance line, written into every file this produces.
 *
 * Always, not only when the reading was doubtful. A note that appears solely on
 * uncertain files makes its *absence* a claim of confidence -- and absence is
 * erased by one edit, by one copy and paste, by anything at all. A line that is
 * always there says what it says and nothing more.
 */
static int describe_reading(char *out, size_t size, const char *from,
        const struct recon_ocr_result *r, bool a_reading) {
    int named = r->characters - r->unrecognised;

    int used = snprintf(out, size,
        "Read from %s by ReconOS. %d of %d marks named, confidence %d.\n",
        from, named, r->characters, r->confidence);

    if (r->unrecognised > 0 && used > 0 && (size_t)used < size) {
        used += snprintf(out + used, size - (size_t)used,
            "The %d it could not name are written as \xEF\xBF\xBD, not guessed "
            "at.\n", r->unrecognised);
    }
    if (!a_reading && used > 0 && (size_t)used < size) {
        used += snprintf(out + used, size - (size_t)used,
            "This did not meet the bar for a reading. Every line of it is a "
            "guess.\n");
    }
    if (r->truncated && used > 0 && (size_t)used < size) {
        used += snprintf(out + used, size - (size_t)used,
            "The picture held more than could be read in one go. The rest was "
            "not looked at.\n");
    }
    if (used > 0 && (size_t)used < size) {
        used += snprintf(out + used, size - (size_t)used, "\n");
    }
    return used;
}

/*
 * Write it out, and open it.
 *
 * Into Documents rather than beside the picture: Documents is made with the
 * account and is always writable, and the picture may be sitting on a volume
 * this account cannot write to. A new name rather than an overwrite, because
 * this is a button somebody will press twice and the second press must not
 * take the first result away.
 */
static bool save_reading(struct recon_photos *ph,
        const struct recon_ocr_result *r, bool a_reading) {
    char documents[RECON_PATH_MAX];
    recon_text_copy(documents, sizeof(documents),
        recon_fs_user_dir("Documents"));

    char base[RECON_NAME_MAX];
    recon_text_copy(base, sizeof(base), ph->reading_name);
    char *dot = strrchr(base, '.');
    if (dot != NULL && dot != base) {
        *dot = '\0';
    }
    if (base[0] == '\0') {
        recon_text_copy(base, sizeof(base), "Reading");
    }

    char leaf[RECON_NAME_MAX];
    if (!recon_fs_unique_name("/", documents, base, ".txt", leaf,
            sizeof(leaf))) {
        set_message(ph, true, "%s", recon_fs_last_error());
        return false;
    }

    char path[RECON_PATH_MAX];
    if (!recon_fs_join(path, sizeof(path), documents, leaf)) {
        set_message(ph, true, "That name is too long to save as text.");
        return false;
    }

    char head[512];
    int head_len = describe_reading(head, sizeof(head), ph->reading_name, r,
        a_reading);
    if (head_len < 0) {
        head_len = 0;
    }

    /*
     * One buffer and one write. A header written separately would leave a file
     * containing nothing but a provenance note if the second write failed --
     * which is a file that says a reading happened and does not contain one.
     */
    size_t total = (size_t)head_len + r->length + 2;
    char *body = malloc(total);
    if (body == NULL) {
        set_message(ph, true, "Not enough memory to save that.");
        return false;
    }

    memcpy(body, head, (size_t)head_len);
    memcpy(body + head_len, r->text, r->length);
    size_t at = (size_t)head_len + r->length;
    if (at == 0 || body[at - 1] != '\n') {
        body[at++] = '\n';
    }

    bool wrote = recon_fs_write("/", path, body, at);
    free(body);

    if (!wrote) {
        set_message(ph, true, "%s", recon_fs_last_error());
        return false;
    }

    int named = r->characters - r->unrecognised;
    if (a_reading) {
        if (r->unrecognised > 0) {
            set_message(ph, false, "Read %d of %d marks. '%s' is in your "
                "Documents.", named, r->characters, leaf);
        } else {
            set_message(ph, false, "Read %d characters. '%s' is in your "
                "Documents.", named, leaf);
        }
    } else {
        set_message(ph, true, "Saved the guess as '%s' in your Documents.",
            leaf);
    }

    /* Written first, opened second. If opening fails the text is on disk and
     * the line above says where. */
    recon_shell_open_file(ph->shell, path);
    return true;
}

static void on_guess_answer(void *user, int choice) {
    struct recon_photos *ph = user;

    if (ph->holding) {
        if (choice == 0) {
            save_reading(ph, &ph->held, false);
        } else {
            set_message(ph, false, "Not saved.");
        }
        recon_ocr_result_free(&ph->held);
        ph->holding = false;
    }
    recon_appwin_refresh(ph->win);
}

/*
 * What came back, and whether to keep it.
 *
 * Four outcomes, and three of them write nothing. That ratio is the feature: a
 * wrong transcription that looks right is worse than none, so the only case
 * that saves without asking is the one the engine itself calls a reading.
 */
static void handle_result(struct recon_photos *ph, struct recon_ocr_result *r) {
    int named = r->characters - r->unrecognised;

    if (named == 0) {
        /*
         * Marks were found and none could be named. A file whose entire
         * contents are replacement characters is not a result, and offering to
         * save one would be offering somebody a page of nothing.
         */
        /* Short enough to fit the bar. The bar truncates with an ellipsis,
         * and a refusal somebody has to widen a window to read is a refusal
         * they will not read. */
        set_message(ph, true, "Found %d marks and could name none. Nothing "
            "saved.", r->characters);
        recon_ocr_result_free(r);
        return;
    }

    if (recon_ocr_result_is_a_reading(r) && !r->truncated) {
        save_reading(ph, r, true);
        recon_ocr_result_free(r);
        return;
    }

    /*
     * A guess, or cut short. Held, and asked about, with the real numbers --
     * and deliberately without a sample of the text.
     *
     * A plausible-looking excerpt turns the question into a rubber stamp: it
     * reads as evidence, and the whole reason to ask is that the engine cannot
     * tell whether it is. There is no setting to skip this and no remembered
     * answer, because it was a guess every time.
     */
    char question[400];
    if (r->truncated && !recon_ocr_result_is_a_reading(r)) {
        snprintf(question, sizeof(question),
            "This picture holds more text than can be read in one go, and of "
            "what was read only %d of %d marks could be named -- confidence "
            "%d out of 100. That is a guess, not a reading, and letters it did "
            "name may still be wrong.",
            named, r->characters, r->confidence);
    } else if (r->truncated) {
        snprintf(question, sizeof(question),
            "This picture holds more text than can be read in one go. What is "
            "here was read; the rest was not looked at.");
    } else {
        snprintf(question, sizeof(question),
            "Only %d of %d marks could be named, and confidence is %d out of "
            "100. That is a guess, not a reading -- letters it did name may "
            "still be wrong. Saving writes that warning into the file.",
            named, r->characters, r->confidence);
    }

    ph->held = *r;
    ph->holding = true;

    /* "Don't Save" last, so it is what Return and Escape both do. */
    static const char *const CHOICES[] = { "Save the Guess", "Don't Save" };
    recon_appwin_ask(ph->win, r->truncated ? "Only part of it" : "This is a "
        "guess", question, CHOICES, 2, on_guess_answer);
}

static int on_read_tick(void *user) {
    struct recon_photos *ph = user;

    int asked_for = ph->reading_at;
    ph->reading_at = -1;

    if (ph->pixels == NULL || asked_for != ph->at) {
        set_message(ph, true, "The picture changed, so nothing was read.");
        recon_appwin_refresh(ph->win);
        return 0;
    }

    if (ph->ocr == NULL) {
        ph->ocr = recon_ocr_font_find();
    }
    if (ph->ocr == NULL) {
        /*
         * Reported here rather than by dimming the button. Finding out costs
         * parsing a typeface, and a machine that gains one should not need
         * this window reopened.
         */
        set_message(ph, true, "There is no typeface on this machine to read "
            "with.");
        recon_appwin_refresh(ph->win);
        return 0;
    }

    struct recon_ocr_result result;
    if (!recon_ocr_read(ph->pixels, ph->width, ph->height, ph->ocr, &result)) {
        /* The engine's own sentence. It knows which of several things went
         * wrong and this does not. */
        set_message(ph, true, "%s", recon_ocr_match_last_error());
        recon_appwin_refresh(ph->win);
        return 0;
    }

    handle_result(ph, &result);
    recon_appwin_refresh(ph->win);
    return 0;
}

static void begin_reading(struct recon_photos *ph) {
    ph->reading_at = ph->at;
    recon_text_copy(ph->reading_name, sizeof(ph->reading_name),
        ph->names[ph->at]);

    set_message(ph, false, "Reading.");
    recon_appwin_refresh(ph->win);

    wl_event_source_timer_update(ph->read_timer, READ_SETTLE_MS);
}

static void on_size_answer(void *user, int choice) {
    struct recon_photos *ph = user;

    if (choice == 0) {
        begin_reading(ph);
    } else {
        set_message(ph, false, "Not read.");
        recon_appwin_refresh(ph->win);
    }
}

static void photos_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct recon_photos *ph = user;
    int ascent = recon_font_ascent(ph->font);

    /*
     * The controls across the top, the picture under them.
     *
     * They were along the bottom, which is where a *status* line goes -- and
     * this row is not status, it is the only way to do anything here. Every
     * other window in ReconOS puts what you can do above what you are looking
     * at, and a picture is the one thing on screen big enough that a row
     * underneath it is genuinely far away.
     */
    int bar_y = y;
    int view_y = y + BAR_HEIGHT;
    int view_h = h - BAR_HEIGHT;
    if (view_h < 1) {
        view_h = 1;
    }

    recon_fill_rect(panel, x, view_y, w, view_h, COLOR_MAT);
    recon_hit_add(panel, x, view_y, w, view_h, HIT_PICTURE);

    if (ph->pixels != NULL && ph->width > 0 && ph->height > 0) {
        int draw_w = ph->width;
        int draw_h = ph->height;

        /*
         * Fitted by the tighter of the two ratios, so the whole picture is
         * inside the window and its shape is unchanged. Never enlarged past
         * its own size: a 200-pixel thumbnail blown up to fill a window is
         * not more of the picture, it is the same picture with the detail
         * spread thinner.
         */
        if (ph->fit && (draw_w > w - PADDING * 2 || draw_h > view_h - PADDING * 2)) {
            int room_w = w - PADDING * 2;
            int room_h = view_h - PADDING * 2;
            if (room_w < 1) {
                room_w = 1;
            }
            if (room_h < 1) {
                room_h = 1;
            }

            /* Compared as a cross-multiplication rather than as a ratio,
             * because integer division would round both sides to zero on
             * anything narrower than its room. */
            if ((long long)draw_w * room_h > (long long)draw_h * room_w) {
                draw_h = (int)((long long)draw_h * room_w / draw_w);
                draw_w = room_w;
            } else {
                draw_w = (int)((long long)draw_w * room_h / draw_h);
                draw_h = room_h;
            }
            if (draw_w < 1) {
                draw_w = 1;
            }
            if (draw_h < 1) {
                draw_h = 1;
            }
        }

        /* Centred, and clipped by the panel when it is larger than the
         * window -- which is what "actual size" means for a photograph. */
        int px = x + (w - draw_w) / 2;
        int py = view_y + (view_h - draw_h) / 2;
        recon_draw_image(panel, px, py, draw_w, draw_h, ph->pixels,
            ph->width, ph->height);
    } else {
        const char *nothing = ph->count == 0
            ? "No pictures here. Open one from the File Explorer."
            : "Nothing to show.";
        int width = recon_text_width(ph->font, nothing);
        recon_draw_text(panel, ph->font, x + (w - width) / 2,
            view_y + view_h / 2, w, nothing, COLOR_DIM);
    }

    /* The bar along the top: what this is, and how to reach the rest. The
     * rule sits under it, against the picture, rather than over it. */
    int by = bar_y;
    recon_fill_rect(panel, x, by, w, BAR_HEIGHT, COLOR_BG);
    recon_fill_rect(panel, x, by + BAR_HEIGHT - 1, w, 1, COLOR_BAR);

    int baseline = by + (BAR_HEIGHT + ascent) / 2 - 2;

    /*
     * The bar, as a list of what the buttons are rather than as drawing.
     *
     * Every one of these used to be five lines -- a fill, an edge, a label, a
     * hit region and a tooltip -- and the five had drifted: two of them
     * clipped their label at the button width measured from an indented
     * start, so a long label ran over its neighbour, and the two arrows
     * registered no region at all when there was nowhere to step to, which
     * made them vanish under the pointer rather than explain themselves.
     *
     * None of that is decided here any more. This says what the buttons are;
     * recon_widget decides what a button is.
     */
    const char *how = ph->fit ? "Actual Size" : "Fit to Window";
    const char *png_label = "Save as PNG";

    bool many = ph->count > 1;
    bool can_read = ph->pixels != NULL && ph->reading_at < 0 && !ph->holding;
    bool can_shrink = ph->pixels != NULL &&
        ph->width / 2 >= RESIZE_MIN && ph->height / 2 >= RESIZE_MIN;
    bool can_grow = ph->pixels != NULL &&
        ph->width * 2 <= RESIZE_MAX && ph->height * 2 <= RESIZE_MAX;

    const struct {
        const char *label;
        uint32_t id;
        int width;      /* Zero measures the label. */
        int gap;        /* To the next one. */
        bool enabled;
        const char *tip;
    } BUTTONS[] = {
        { "<", HIT_PREVIOUS, 30, 4, many,
          many ? "The one before" : "This is the only one here" },
        { ">", HIT_NEXT, 30, 10, many,
          many ? "The next one" : "This is the only one here" },
        { how, HIT_FIT, 0, 12, true, NULL },
        { "Read Text", HIT_READ, 0, 12, can_read,
          ph->pixels == NULL ? "Nothing open to read"
          : can_read ? "Read the text in this picture into a file in Documents"
                     : "Still reading" },
        { png_label, HIT_CONVERT, 0, 12, ph->pixels != NULL,
          ph->pixels == NULL ? "Nothing open to convert"
              : "Write this picture out as a PNG, next to the original" },
        { "Half", HIT_SMALLER, 0, 6, can_shrink,
          can_shrink ? "Make the picture half this size, on screen only"
                     : "Already as small as this goes" },
        { "Double", HIT_BIGGER, 0, 12, can_grow,
          can_grow ? "Make the picture twice this size, on screen only"
                   : "Already as large as this goes" },
    };

    int bx = x + PADDING;
    for (size_t i = 0; i < sizeof(BUTTONS) / sizeof(BUTTONS[0]); i++) {
        int bw = BUTTONS[i].width > 0 ? BUTTONS[i].width
            : recon_text_width(ph->font, BUTTONS[i].label) + 16;

        struct recon_widget_button button = {
            .x = bx, .y = by + 4, .w = bw, .h = BAR_HEIGHT - 9,
            .id = BUTTONS[i].id,
            .label = BUTTONS[i].label,
            .font = ph->font,
            .tip = BUTTONS[i].tip,
            .behind = COLOR_BAR,
            .disabled = !BUTTONS[i].enabled,
        };
        recon_widget_button(panel, &button);

        bx += bw + BUTTONS[i].gap;
    }

    if (ph->message[0] != '\0') {
        recon_draw_text(panel, ph->font, bx, baseline, x + w - bx - PADDING,
            ph->message,
            ph->message_is_warning ? COLOR_WARNING : COLOR_DIM);
    }
}

static bool photos_click(void *user, uint32_t hit_id, int cx, int cy,
        bool pressed) {
    struct recon_photos *ph = user;
    (void)cx;
    (void)cy;

    if (!pressed) {
        return false;
    }

    switch (hit_id) {
    case HIT_PREVIOUS:
        step(ph, -1);
        return true;
    case HIT_NEXT:
        step(ph, 1);
        return true;
    case HIT_FIT:
        ph->fit = !ph->fit;
        set_message(ph, false, ph->fit
            ? "Fitted to the window."
            : "At its own size. The window shows as much as it holds.");
        return true;
    case HIT_CONVERT:
        save_as_png(ph);
        return true;

    case HIT_SMALLER:
        resize_to(ph, ph->width / 2, ph->height / 2);
        return true;

    case HIT_BIGGER:
        resize_to(ph, ph->width * 2, ph->height * 2);
        return true;
    case HIT_READ:
        if (ph->pixels == NULL) {
            set_message(ph, false, "Nothing open to read.");
            return true;
        }
        if (ph->reading_at >= 0 || ph->holding) {
            set_message(ph, false, "Still reading.");
            return true;
        }
        if (ph->read_timer == NULL) {
            set_message(ph, true, "This machine could not start the reader.");
            return true;
        }
        if ((long long)ph->width * ph->height > ASK_ABOVE_PIXELS) {
            /*
             * Big enough to be noticed. The read is synchronous, so for the
             * time it takes nothing else on the desktop answers -- worth
             * mentioning before it happens rather than explaining afterwards.
             *
             * "Not Now" last, so Return and Escape both decline.
             */
            static const char *const CHOICES[] = { "Read It Anyway",
                "Not Now" };
            char question[256];
            snprintf(question, sizeof(question),
                "This picture is %d by %d. Reading something that size takes "
                "long enough that the desktop will stop answering while it "
                "works. Nothing is harmed by waiting.",
                ph->width, ph->height);
            recon_appwin_ask(ph->win, "Read Text", question, CHOICES, 2,
                on_size_answer);
            return true;
        }
        begin_reading(ph);
        return true;

    case HIT_PICTURE:
        /* Clicking the picture moves on, the way a slideshow does. */
        step(ph, 1);
        return true;
    default:
        return false;
    }
}

static bool photos_key(void *user, xkb_keysym_t sym, uint32_t modifiers) {
    struct recon_photos *ph = user;
    (void)modifiers;

    switch (sym) {
    case XKB_KEY_Left:
    case XKB_KEY_Up:
        step(ph, -1);
        return true;
    case XKB_KEY_Right:
    case XKB_KEY_Down:
    case XKB_KEY_space:
        step(ph, 1);
        return true;
    case XKB_KEY_Home:
        if (ph->count > 0) {
            ph->at = 0;
            load_current(ph);
        }
        return true;
    case XKB_KEY_End:
        if (ph->count > 0) {
            ph->at = ph->count - 1;
            load_current(ph);
        }
        return true;
    case XKB_KEY_f:
    case XKB_KEY_F:
        ph->fit = !ph->fit;
        return true;
    default:
        return false;
    }
}

static void photos_scroll(void *user, double delta) {
    step(user, delta > 0 ? 1 : -1);
}

static void photos_describe(void *user, char *out, size_t size) {
    struct recon_photos *ph = user;
    snprintf(out, size,
        "  folder: %s\n"
        "  pictures: %d\n"
        "  showing: %d (%s)\n"
        "  decoded: %s, %d by %d\n"
        "  fit: %s\n",
        ph->folder[0] != '\0' ? ph->folder : "(none)",
        ph->count, ph->at + 1,
        ph->at >= 0 && ph->at < ph->count ? ph->names[ph->at] : "(none)",
        ph->pixels != NULL ? "yes" : "no", ph->width, ph->height,
        ph->fit ? "yes" : "no");
}

static void photos_destroy(void *user) {
    struct recon_photos *ph = user;

    /*
     * The question goes first.
     *
     * Photos is the first application here that can both ask something and be
     * closed while it asks. A dialog left standing holds a callback into this
     * struct, and the struct is two lines from being freed.
     */
    recon_shell_cancel_dialog(ph->shell, ph);

    if (ph->holding) {
        recon_ocr_result_free(&ph->held);
    }
    if (ph->read_timer != NULL) {
        wl_event_source_remove(ph->read_timer);
    }
    if (ph->ocr != NULL) {
        recon_ocr_font_close(ph->ocr);
    }

    forget_picture(ph);
    free(ph);
}

static const struct recon_appwin_impl PHOTOS_IMPL = {
    .title = "Photos",
    .help = "Pictures",
    .icon = RECON_ICON_PHOTOS,
    .default_width = 640,
    .default_height = 480,
    .min_width = 760,
    .min_height = 200,
    .draw = photos_draw,
    .click = photos_click,
    .key = photos_key,
    .scroll = photos_scroll,
    .describe = photos_describe,
    .destroy = photos_destroy,
};

struct recon_appwin *recon_photos_create(struct recon_server *server,
        struct recon_font *font) {
    struct recon_photos *ph = calloc(1, sizeof(*ph));
    if (ph == NULL) {
        return NULL;
    }

    ph->font = font;
    ph->fit = true;

    /* Zero is a real picture index, so idle has to be said explicitly. */
    ph->reading_at = -1;

    /*
     * Opens on the account's own Pictures folder, because that is where a
     * person's pictures are and an empty window asking them to go and find
     * some is a window that has not tried.
     */
    recon_text_copy(ph->folder, sizeof(ph->folder),
        recon_fs_user_dir("Pictures"));
    scan_folder(ph);
    if (ph->count > 0) {
        load_current(ph);
    }

    ph->win = recon_appwin_create(server, font, &PHOTOS_IMPL, ph);
    if (ph->win == NULL) {
        forget_picture(ph);
        free(ph);
        return NULL;
    }

    ph->shell = server->shell;

    /*
     * Named after the window exists, not while it is being loaded.
     *
     * load_current retitles, and at that point in creation there is no window
     * to retitle -- so the first picture opened came up in a window called
     * "Photos" and only got its name on the second. The title is set again
     * here rather than moving the load, because the load is also what the
     * arrow keys and the file dialog call and those do have a window.
     */
    retitle(ph);
    ph->read_timer = wl_event_loop_add_timer(
        wl_display_get_event_loop(server->wl_display), on_read_tick, ph);

    return ph->win;
}
