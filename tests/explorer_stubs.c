/*
 * What a headless file manager needs, on top of the other two stub files.
 *
 * --- Why these are stubs and not links ---
 *
 * Everything on the path this suite tests is real: `recon_explorer.c` itself,
 * `recon_fs.c` underneath it, `recon_appwin.c` and the widget layer around it,
 * and `recon_manifest.c` for the package questions the explorer asks about a
 * `.reconos` file.
 *
 * What is stubbed is everything the explorer can *reach* but this suite does
 * not go near — installing a module, adding a wallpaper, decoding an icon,
 * opening a file in another application. Each of those is a whole subsystem
 * with a suite of its own or a reason not to have one, and linking them to
 * test a delete would mean a file manager test that fails when the theme
 * changes.
 *
 * --- The rule they all follow ---
 *
 * **Every one answers "nothing here", never "it worked".** A stub that
 * reported a successful install or a wallpaper that was set would have the
 * explorer take a path this suite never meant to exercise, and report success
 * about work nothing did. Where a name is asked for, they answer empty; where
 * a question is asked, they answer no.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ReconOS.h"
#include "recon_codec.h"
#include "recon_fonts.h"
#include "recon_modules.h"
#include "recon_registry.h"
#include "recon_shell.h"
#include "recon_sniff.h"
#include "recon_theme.h"
#include "recon_ui.h"
#include "recon_wallpaper.h"

/* --- the shell and the screen ------------------------------------------- */

void recon_background_reload(struct recon_server *server) { (void)server; }

void recon_draw_image(struct recon_panel *panel, int x, int y, int w, int h,
        const unsigned char *rgba, int image_width, int image_height) {
    (void)panel; (void)x; (void)y; (void)w; (void)h; (void)rgba;
    (void)image_width; (void)image_height;
}

/*
 * Opening a file elsewhere is refused.
 *
 * `recon_shell.c` is 7,264 lines and is the taskbar, the Start menu and the
 * desktop; linking it to find out whether a double-click opens Notepad would
 * put the whole shell under a file manager's test. Answering false is also the
 * truthful answer here -- there is no shell in this program to open anything.
 */
bool recon_shell_open_file(struct recon_shell *shell, const char *path) {
    (void)shell; (void)path;
    return false;
}

/* --- the registry ------------------------------------------------------- */

/*
 * Remembers nothing, and says so by answering the fallback.
 *
 * That is the state of an account that has never changed a setting, which is
 * exactly the state a test wants: a remembered column width or sort order is a
 * second thing deciding what the window does.
 */
const char *recon_registry_get(enum recon_registry_scope scope,
        const char *key, const char *fallback) {
    (void)scope; (void)key;
    return fallback;
}

bool recon_registry_has(enum recon_registry_scope scope, const char *key) {
    (void)scope; (void)key;
    return false;
}

bool recon_registry_set(enum recon_registry_scope scope, const char *key,
        const char *value) {
    (void)scope; (void)key; (void)value;
    return true;
}

bool recon_registry_remove(enum recon_registry_scope scope, const char *key) {
    (void)scope; (void)key;
    return false;
}

/* --- what a file is, according to its bytes ----------------------------- */

enum recon_format recon_sniff(const uint8_t *bytes, size_t length) {
    (void)bytes; (void)length;
    return RECON_FORMAT_UNKNOWN;
}

bool recon_sniff_agrees_with_name(enum recon_format kind, const char *name) {
    (void)kind; (void)name;
    return true;
}

const char *recon_sniff_extension(enum recon_format kind) {
    (void)kind;
    return "";
}

const char *recon_sniff_name(enum recon_format kind) {
    (void)kind;
    return "";
}

bool recon_codec_handles_extension(const char *name) {
    (void)name;
    return false;
}

unsigned char *recon_ico_decode(const unsigned char *data, size_t size,
        int preferred_size, int *width_out, int *height_out) {
    (void)data; (void)size; (void)preferred_size;
    if (width_out != NULL) { *width_out = 0; }
    if (height_out != NULL) { *height_out = 0; }
    return NULL;
}

/* --- installed applications --------------------------------------------- */

int recon_installed_app_count(void) { return 0; }

bool recon_installed_app_at(int index, struct recon_installed_app *out) {
    (void)index; (void)out;
    return false;
}

const char *recon_installed_app_for_file(const char *filename) {
    (void)filename;
    return NULL;
}

const char *recon_installed_app_resolve(const char *target) {
    (void)target;
    return NULL;
}

/* --- modules, wallpapers, fonts, themes --------------------------------- */

bool recon_modules_install(const char *reconos_path) {
    (void)reconos_path;
    return false;
}

bool recon_modules_load(const char *reconos_path) {
    (void)reconos_path;
    return false;
}

bool recon_modules_unload(const char *name) {
    (void)name;
    return false;
}

const char *recon_modules_last_error(void) {
    return "there are no modules in this suite";
}

bool recon_wallpaper_add(const char *cwd, const char *path, char *name_out,
        size_t name_size) {
    (void)cwd; (void)path;
    if (name_out != NULL && name_size > 0) { name_out[0] = '\0'; }
    return false;
}

bool recon_wallpaper_set(const char *name) {
    (void)name;
    return false;
}

bool recon_fonts_add(const char *cwd, const char *path, char *name_out,
        size_t name_size) {
    (void)cwd; (void)path;
    if (name_out != NULL && name_size > 0) { name_out[0] = '\0'; }
    return false;
}

const char *recon_fonts_last_error(void) {
    return "there are no fonts in this suite";
}

const char *recon_theme_current(void) {
    return "";
}

unsigned recon_theme_generation(void) {
    return 1;
}

/* --- icons -------------------------------------------------------------- */

/*
 * `recon_icons.c` is not linked because `appwin_stubs.c` already answers
 * `recon_icon_draw_in`, and the real file would pull an image decoder in to
 * draw nothing. These two come with it.
 */
bool recon_icon_draw(struct recon_panel *panel, const char *name, int x,
        int y, int size) {
    (void)panel; (void)name; (void)x; (void)y; (void)size;
    return false;
}

void recon_icons_forget(void) { }
