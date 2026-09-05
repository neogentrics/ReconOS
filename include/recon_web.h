/*
 * The web viewer window.
 *
 * An address bar, back and forward, and a page. It is called a viewer and not
 * a browser throughout, in the code and on the screen, because the difference
 * matters: there is no CSS, no JavaScript, no images and no forms, and a
 * window labelled "browser" makes a promise none of that keeps.
 *
 * What it is good for: documentation, plain-text files, RFCs, articles,
 * anything written as text with structure. What it is bad at: a page that is a
 * program, which it will say rather than showing blank.
 */

#ifndef RECON_WEB_H
#define RECON_WEB_H

#include <stdbool.h>

struct recon_server;
struct recon_font;
struct recon_appwin;

struct recon_appwin *recon_web_create(struct recon_server *server,
    struct recon_font *font);

/*
 * Show a file from this machine rather than one from the network.
 *
 * `path` is a ReconOS path. The document is read and shown; there is no
 * address to resolve links against, so a relative link in it says so rather
 * than guessing a server to ask.
 *
 * False when the file cannot be read, with the window saying why.
 */
bool recon_web_open_path(struct recon_appwin *win, const char *path);

#endif /* RECON_WEB_H */
