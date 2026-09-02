/*
 * The ReconOS terminal window: a view onto the command interpreter.
 *
 * It emulates nothing. A line goes to the interpreter and text comes back,
 * which means the terminal is one client of the interpreter rather than the
 * only way to reach it -- see recon_control.h for the other.
 */

#ifndef RECON_TERMINAL_H
#define RECON_TERMINAL_H

struct recon_server;
struct recon_font;
struct recon_appwin;

struct recon_appwin *recon_terminal_create(struct recon_server *server,
    struct recon_font *font);

#endif
