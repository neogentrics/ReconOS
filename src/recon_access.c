/*
 * Reading settings. See include/recon_access.h.
 */

#define _POSIX_C_SOURCE 200809L

#include "recon_access.h"
#include "recon_fs.h"
#include "recon_registry.h"
#include "recon_ui.h"

void recon_access_apply(struct recon_font *font) {
    recon_text_set_spacing(
        recon_registry_get_int(RECON_REG_USER, RECON_ACCESS_LETTER_KEY, 0),
        recon_registry_get_int(RECON_REG_USER, RECON_ACCESS_LINE_KEY, 0));

    if (font == NULL) {
        return;
    }

    /*
     * A chosen font, or the system's usual one. Passing NULL to the loader
     * means "find something", which is what an empty setting should mean too.
     */
    const char *path = recon_registry_get(RECON_REG_USER,
        RECON_ACCESS_FONT_KEY, NULL);
    if (path != NULL && *path == '\0') {
        path = NULL;
    }

    /*
     * Turned into a path the loader can open.
     *
     * `recon_font_load` reads the file directly, so it wants where the host
     * keeps it -- and a font installed into ReconOS is named by its place
     * inside ReconOS, which is not that. Setting one used to appear to work:
     * the key was written, the page said which font was on, and the loader
     * quietly failed to find it and left the old typeface drawing.
     *
     * A path that does not resolve is passed through untouched, so a host
     * path somebody typed still works.
     */
    char host[RECON_PATH_MAX];
    if (path != NULL) {
        char canonical[RECON_PATH_MAX];
        if (recon_fs_resolve("/", path, host, sizeof(host), canonical,
                sizeof(canonical))) {
            path = host;
        }
    }

    int size = recon_registry_get_int(RECON_REG_USER,
        RECON_ACCESS_FONT_SIZE_KEY, RECON_ACCESS_FONT_SIZE_DEFAULT);
    if (size < 8) {
        size = 8;
    }
    if (size > 24) {
        size = 24;
    }

    /* A font file that cannot be read leaves the old one in place: a desktop
     * with the wrong typeface is usable, one with none is not. */
    recon_font_reload(font, path, size);
}
