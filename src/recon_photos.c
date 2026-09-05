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
#include "recon_photos.h"
#include "recon_server.h"
#include "recon_theme.h"
#include "recon_ui.h"

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
     * False shows the picture at its own size, which for anything from a
     * camera means a corner of it. True fits the whole thing in the window.
     */
    bool fit;

    char message[192];
    bool message_is_warning;
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
static bool load_current(struct recon_photos *ph) {
    forget_picture(ph);

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

/* --- Drawing --- */

static void photos_draw(void *user, struct recon_panel *panel,
        int x, int y, int w, int h) {
    struct recon_photos *ph = user;
    int ascent = recon_font_ascent(ph->font);

    int view_h = h - BAR_HEIGHT;
    if (view_h < 1) {
        view_h = 1;
    }

    recon_fill_rect(panel, x, y, w, view_h, COLOR_MAT);
    recon_hit_add(panel, x, y, w, view_h, HIT_PICTURE);

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
        int py = y + (view_h - draw_h) / 2;
        recon_draw_image(panel, px, py, draw_w, draw_h, ph->pixels,
            ph->width, ph->height);
    } else {
        const char *nothing = ph->count == 0
            ? "No pictures here. Open one from the File Explorer."
            : "Nothing to show.";
        int width = recon_text_width(ph->font, nothing);
        recon_draw_text(panel, ph->font, x + (w - width) / 2,
            y + view_h / 2, w, nothing, COLOR_DIM);
    }

    /* The bar along the bottom: what this is, and how to reach the rest. */
    int by = y + view_h;
    recon_fill_rect(panel, x, by, w, BAR_HEIGHT, COLOR_BG);
    recon_fill_rect(panel, x, by, w, 1, COLOR_BAR);

    int baseline = by + (BAR_HEIGHT + ascent) / 2 - 2;

    int bx = x + PADDING;
    int button_w = 30;
    bool many = ph->count > 1;

    recon_fill_rect(panel, bx, by + 4, button_w, BAR_HEIGHT - 9, COLOR_BG);
    recon_draw_button_edge(panel, bx, by + 4, button_w, BAR_HEIGHT - 9, false,
        COLOR_BAR);
    recon_draw_text(panel, ph->font, bx + 11, baseline, button_w, "<",
        many ? COLOR_TEXT : COLOR_DIM);
    if (many) {
        recon_hit_add(panel, bx, by + 4, button_w, BAR_HEIGHT - 9, HIT_PREVIOUS);
        recon_hit_tip(panel, "The one before");
    }
    bx += button_w + 4;

    recon_fill_rect(panel, bx, by + 4, button_w, BAR_HEIGHT - 9, COLOR_BG);
    recon_draw_button_edge(panel, bx, by + 4, button_w, BAR_HEIGHT - 9, false,
        COLOR_BAR);
    recon_draw_text(panel, ph->font, bx + 11, baseline, button_w, ">",
        many ? COLOR_TEXT : COLOR_DIM);
    if (many) {
        recon_hit_add(panel, bx, by + 4, button_w, BAR_HEIGHT - 9, HIT_NEXT);
        recon_hit_tip(panel, "The next one");
    }
    bx += button_w + 10;

    const char *how = ph->fit ? "Actual Size" : "Fit to Window";
    int how_w = recon_text_width(ph->font, how) + 16;
    recon_fill_rect(panel, bx, by + 4, how_w, BAR_HEIGHT - 9, COLOR_BG);
    recon_draw_button_edge(panel, bx, by + 4, how_w, BAR_HEIGHT - 9, false,
        COLOR_BAR);
    recon_draw_text(panel, ph->font, bx + 8, baseline, how_w, how, COLOR_TEXT);
    recon_hit_add(panel, bx, by + 4, how_w, BAR_HEIGHT - 9, HIT_FIT);
    bx += how_w + 12;

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
    forget_picture(ph);
    free(ph);
}

static const struct recon_appwin_impl PHOTOS_IMPL = {
    .title = "Photos",
    .help = "Writing",
    .icon = RECON_ICON_PHOTOS,
    .default_width = 640,
    .default_height = 480,
    .min_width = 280,
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
    return ph->win;
}
