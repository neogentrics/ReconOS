/*
 * Photos: looking at the pictures on this machine.
 *
 * There has been a Pictures folder since accounts existed and nothing that
 * could open what is in it. A wallpaper could be set from a picture and a
 * picture could be copied, renamed and deleted -- but not *seen*, which is
 * the one thing a picture is for.
 *
 * It is deliberately a viewer and not an editor. Nothing here changes a file:
 * no crop, no rotate that writes, no save. A viewer that can damage the
 * original is a viewer people are careful with, and being careful is the
 * opposite of what looking through photographs is.
 *
 * What it does have is the folder it was opened in. Opening one picture puts
 * every picture beside it within reach, because "show me that one" and "show
 * me the next one" are the same request separated by a second.
 */

#ifndef RECON_PHOTOS_H
#define RECON_PHOTOS_H

#include <stdbool.h>

struct recon_server;
struct recon_appwin;

/* Build the window. Registered as a built-in like the rest. */
struct recon_appwin *recon_photos_create(struct recon_server *server,
    struct recon_font *font);

/*
 * Show this picture, and take the folder it is in as the set to move through.
 *
 * Called by the shell when somebody opens an image from the file explorer.
 * False when the path is not a picture this system can decode, which the
 * explorer reports rather than opening an empty window.
 */
bool recon_photos_open_path(struct recon_appwin *win, const char *path);

#endif /* RECON_PHOTOS_H */
