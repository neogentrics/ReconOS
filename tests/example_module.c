/*
 * A real module, built as a real shared object, for the loader to load.
 *
 * --- Why this file exists ---
 *
 * There was no module anywhere in this tree. `src/recon_modules.c` is 1,514
 * lines whose whole job is to `dlopen` somebody else's shared object and
 * register what it declares, and until now **nothing in the repository had
 * ever handed it one.** Every test of the install path stood the loader in,
 * and the stand-in returned a bool.
 *
 * --- The constructor, which is the point of it ---
 *
 * `__attribute__((constructor))` runs when the file is `dlopen`ed: before the
 * loader has looked at the descriptor, before the ABI is checked, before
 * anything in ReconOS gets a turn. That is not a quirk of this file -- it is
 * how every shared object on this machine works, and it is the entire reason
 * the integrity gate had to go in front of `dlopen` rather than beside the
 * ABI check it used to sit next to.
 *
 * So it writes a marker, and the suite reads it. A module that was opened
 * leaves one; a module refused *before it was opened* does not. That is the
 * difference between a gate and a complaint, and from outside the process it
 * is not otherwise observable.
 *
 * The marker's path comes from the environment and is written with plain
 * `fopen`, so it says one thing only -- that code in this file ran -- without
 * going through any of the machinery under test.
 *
 * --- Built twice ---
 *
 * `EXAMPLE_MODULE_MARK` names which build this is, and the suite builds two.
 * Both are valid, loadable modules; the second is what a swapped file looks
 * like when the swap is competent. A tamper test that replaced a module with
 * rubbish would pass with no gate at all, because `dlopen` would refuse the
 * rubbish by itself.
 */

#include <stdio.h>
#include <stdlib.h>

#include "recon_module.h"

#ifndef EXAMPLE_MODULE_MARK
#define EXAMPLE_MODULE_MARK "one"
#endif

#define MARKER_VAR "RECON_EXAMPLE_MODULE_MARKER"

__attribute__((constructor))
static void it_ran(void) {
    const char *where = getenv(MARKER_VAR);

    if (where == NULL || *where == '\0') {
        return;
    }

    FILE *f = fopen(where, "w");

    if (f == NULL) {
        return;
    }
    fputs(EXAMPLE_MODULE_MARK, f);
    fclose(f);
}

/*
 * Nothing registered and nothing drawn.
 *
 * A module that contributed an application would drag the window layer into
 * the suite that loads it, and what this is for is the loading itself --
 * whether the bytes on the disk are weighed before they are executed.
 * Registering an applet would test something else, in a suite that needed a
 * compositor.
 */
static bool load(void) {
    return true;
}

static void unload(void) {
}

RECON_MODULE(
    .name = "Example",
    .version = "1.0",
    .description = "A module that exists so the loader has something to load",
    .load = load,
    .unload = unload
);
