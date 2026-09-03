/*
 * Reading settings. See include/recon_access.h.
 */

#define _POSIX_C_SOURCE 200809L

#include "recon_access.h"
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
