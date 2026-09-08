/*
 * The install path's dependencies, for a suite that does not install.
 *
 * `recon_package.c` is one file doing two jobs. Reading a manifest and taking
 * digests of what it names is arithmetic on bytes; installing loads a shared
 * object into the process and rebuilds the icon cache. The signature tests
 * need the first and must never reach the second -- so the second is stubbed
 * here rather than linked, which would pull in the module loader, the icon
 * cache, wlroots and the compositor behind them.
 *
 * Every one of these aborts rather than returning a polite failure. A stub
 * that quietly succeeded would let a test pass while having done something
 * this suite has no business doing, and the point of stubbing is that these
 * are never called. If one ever is, the suite should stop and say so rather
 * than report a green tick over a silently different code path.
 *
 * The honest reading of this file is that recon_package.c wants splitting the
 * way recon_http.c did -- an address parser that needed a socket went untested
 * for the same reason, and the fragment bug lived there. This is the smaller
 * of the two moves and it is deliberate: the split is a refactor of working
 * install code, and doing it in the same change as adding signing would mean
 * neither could be reviewed on its own.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static void never(const char *what) {
    fprintf(stderr,
        "\nThe signature tests called %s, which is the install path.\n"
        "These tests take digests and check signatures; they do not install\n"
        "anything, and reaching here means something now does.\n", what);
    abort();
}

bool recon_modules_load(const char *path) {
    (void)path;
    never("recon_modules_load");
    return false;
}

bool recon_modules_unload(const char *name) {
    (void)name;
    never("recon_modules_unload");
    return false;
}

const char *recon_modules_last_error(void) {
    never("recon_modules_last_error");
    return "";
}

void recon_icons_forget(void) {
    never("recon_icons_forget");
}

int recon_version_compare_text(const char *a, const char *b) {
    (void)a;
    (void)b;
    never("recon_version_compare_text");
    return 0;
}
