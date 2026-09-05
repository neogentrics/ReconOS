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

struct recon_server;
struct recon_font;
struct recon_appwin;

struct recon_appwin *recon_web_create(struct recon_server *server,
    struct recon_font *font);

#endif /* RECON_WEB_H */
