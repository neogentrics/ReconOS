/*
 * The Media Player window.
 *
 * A list of what is in the Music folder, transport controls, and a position
 * bar that can be dragged. It plays what recon_codec can decode, which today
 * is WAV and MP3, and says plainly when it cannot decode something rather than
 * opening an empty window.
 *
 * It is called a Media Player and it plays audio. Video is not here, and
 * recon_codec.h says why at length -- the short version is that sound is a
 * stream of numbers and a speaker, and video is that plus a container, plus a
 * clock keeping two streams together, plus a decoder whose output has to be
 * scaled and colour-converted before anything can look at it.
 */

#ifndef RECON_PLAYER_H
#define RECON_PLAYER_H

#include <stdbool.h>

struct recon_server;
struct recon_font;
struct recon_appwin;

struct recon_appwin *recon_player_create(struct recon_server *server,
    struct recon_font *font);

/* Play one file, for a double-click in the explorer. */
bool recon_player_open_path(struct recon_appwin *win, const char *path);

#endif /* RECON_PLAYER_H */
