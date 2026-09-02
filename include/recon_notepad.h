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

struct recon_appwin *recon_notepad_create(struct recon_server *server,
    struct recon_font *font);

#endif
