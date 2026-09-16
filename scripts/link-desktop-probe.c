/*
 * What a ReconOS desktop program does on its first frame.
 *
 * The subject of `scripts/link-desktop.sh`. It is deliberately the smallest
 * honest program rather than the smallest program: read the theme off the
 * disk, fill the screen, frame a window, write a word in it, and make the
 * folder the settings live in.
 *
 * "Honest" is doing work there. A probe that only called `recon_fill_rect`
 * would link today and would be measuring nothing, because what actually pulls
 * the filesystem into a desktop is not the drawing -- it is that the drawing
 * has to know what colour to be, and the colours are on the disk.
 *
 * Written against the real headers rather than against declarations invented
 * here, so it cannot be satisfied by something this file made up.
 *
 * `_start` rather than `main`, because there is no C runtime under this yet.
 * The question is whether the symbols resolve, and a missing `__libc_start`
 * would be noise on top of the answer.
 */

#include "recon_ui.h"
#include "recon_theme.h"
#include "recon_fs.h"

void _start(void)
{
    struct recon_font *font = recon_font_system(14);
    struct recon_panel *panel = 0;

    recon_theme_init();
    recon_fill_rect(panel, 0, 0, 1920, 1080, THEME(WINDOW_FRAME));
    recon_fill_rect(panel, 80, 60, 640, 400, THEME(TITLE_ACTIVE));
    recon_draw_text(panel, font, 90, 70, 620, "ReconOS", THEME(TITLE_TEXT));
    recon_fs_mkdir("/", "/Users/x/.config");

    for (;;) {
    }
}
