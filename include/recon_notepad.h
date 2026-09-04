/*
 * ReconOS Notepad.
 *
 * A plain text editor. Beyond being useful, it is the first thing in ReconOS
 * that takes sustained keyboard input, so it is what proves typing works
 * without a borrowed client in the way.
 */

#ifndef RECON_NOTEPAD_H
#define RECON_NOTEPAD_H

struct recon_server;
struct recon_font;
struct recon_appwin;

/*
 * Put a file in front of the reader, as if they had opened it themselves.
 *
 * Unsaved work is not discarded for it: when there is something unsaved the
 * file is refused rather than the work being lost to a double click
 * somewhere else on the screen.
 */
bool recon_notepad_open_path(struct recon_appwin *win, const char *path);

struct recon_appwin *recon_notepad_create(struct recon_server *server,
    struct recon_font *font);

#endif
