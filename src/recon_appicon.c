/*
 * Which icon belongs to a Wayland client. See include/recon_appicon.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */

#include "recon_appicon.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_modules.h"

#define MAPPINGS_MAX 64
#define NAME_MAX_LEN 64

struct mapping {
    char app_id[128];
    char icon[NAME_MAX_LEN];
};

static struct mapping g_mappings[MAPPINGS_MAX];
static int g_count;
static bool g_loaded;

/* The answer, held until the next call. See the note in the header about who
 * owns this. */
static char g_answer[NAME_MAX_LEN];

void recon_appicon_forget(void) {
    g_loaded = false;
    g_count = 0;
}

/*
 * The written-down mapping.
 *
 * One `app_id<TAB>icon` per line, `#` for a comment. Read once and kept,
 * because this is asked every time a client's title bar is drawn and that is
 * every time a terminal prints a line.
 *
 * A missing file is not an error. It is the ordinary case on a system where
 * nobody has needed to say anything.
 */
static void load(void) {
    g_loaded = true;
    g_count = 0;

    size_t size = 0;
    char *text = recon_fs_read("/", RECON_APPICON_FILE, &size);
    if (text == NULL) {
        return;
    }

    char *saveptr = NULL;
    for (char *line = strtok_r(text, "\n", &saveptr);
            line != NULL && g_count < MAPPINGS_MAX;
            line = strtok_r(NULL, "\n", &saveptr)) {

        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '#' || *line == '\0') {
            continue;
        }

        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            continue;
        }
        *tab = '\0';

        const char *icon = tab + 1;
        while (*icon == ' ' || *icon == '\t') {
            icon++;
        }
        if (*icon == '\0') {
            continue;
        }

        snprintf(g_mappings[g_count].app_id,
            sizeof(g_mappings[g_count].app_id), "%s", line);
        snprintf(g_mappings[g_count].icon,
            sizeof(g_mappings[g_count].icon), "%s", icon);
        g_count++;
    }

    free(text);
}

/*
 * The part after the last dot, lowercased.
 *
 * "org.gnome.Calculator" gives "calculator"; "foot" gives "foot", because a
 * name with no dot in it is already its own last part. Lowercased because icon
 * files are named in lower case and a client's app_id is capitalised however
 * its author felt.
 */
static void short_name(const char *app_id, char *out, size_t size) {
    const char *dot = strrchr(app_id, '.');
    const char *tail = (dot != NULL && dot[1] != '\0') ? dot + 1 : app_id;

    size_t i = 0;
    for (; tail[i] != '\0' && i + 1 < size; i++) {
        char c = tail[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[i] = c;
    }
    out[i] = '\0';
}

/*
 * A file to edit, written once and never again.
 *
 * Unlike the help pages, which are rewritten every start because help that
 * describes an older version is worse than none, this is somebody's list. It
 * is created when it is missing and left alone forever after -- a mapping
 * added by hand that the system helpfully replaced on the next boot would be
 * a file nobody bothers to edit twice.
 *
 * It ships with the format written down and a few entries that are true when
 * the programs are installed and inert when they are not: a mapping only fires
 * for an app_id that actually appears.
 */
void recon_appicon_write_default(void) {
    if (recon_fs_exists("/", RECON_APPICON_FILE)) {
        return;
    }

    static const char CONTENTS[] =
        "# Which icon a Wayland client's window gets.\n"
        "#\n"
        "# One line per program: its app_id, a TAB, then an icon name\n"
        "# from /System/Icons without the extension.\n"
        "#\n"
        "# You only need a line here when ReconOS guesses wrong, or does\n"
        "# not guess at all. Without one it tries, in order: an\n"
        "# application it has registered under that name, then the last\n"
        "# dotted part of the name if an icon by it exists, then the\n"
        "# generic application icon.\n"
        "#\n"
        "# A line here beats all of that, which is how to say \"no, this\n"
        "# one is different\".\n"
        "\n"
        "weston-terminal\tterminal\n"
        "foot\tterminal\n"
        "org.gnome.Console\tterminal\n"
        "firefox\tweb\n"
        "org.mozilla.firefox\tweb\n";

    recon_fs_write("/", RECON_APPICON_FILE, CONTENTS, sizeof(CONTENTS) - 1);
}

const char *recon_appicon_for(const char *app_id) {
    if (app_id == NULL || *app_id == '\0') {
        return RECON_ICON_APP;
    }

    if (!g_loaded) {
        load();
    }

    /*
     * What somebody wrote down, first and above everything.
     *
     * This is the only rule that is a statement rather than a deduction, so it
     * is the only one allowed to contradict the others -- it is how to say
     * "no, this client is not the thing its name reduces to".
     */
    for (int i = 0; i < g_count; i++) {
        if (strcasecmp(g_mappings[i].app_id, app_id) == 0) {
            snprintf(g_answer, sizeof(g_answer), "%s", g_mappings[i].icon);
            return g_answer;
        }
    }

    char shortened[NAME_MAX_LEN];
    short_name(app_id, shortened, sizeof(shortened));

    /*
     * An application ReconOS has registered under this name.
     *
     * It already carries an icon it chose for itself, and a client calling
     * itself by the same name is almost always the same program -- this is how
     * a ReconOS application that also speaks Wayland gets its own picture
     * rather than the generic one.
     */
    const char *registered = recon_installed_app_resolve(app_id);
    if (registered == NULL) {
        registered = recon_installed_app_resolve(shortened);
    }
    if (registered != NULL) {
        const char *icon = recon_installed_app_icon(registered);
        if (icon != NULL && icon[0] != '\0') {
            snprintf(g_answer, sizeof(g_answer), "%s", icon);
            return g_answer;
        }
    }

    /*
     * The shortened name, IF there is an icon by it.
     *
     * This is the rule that looks like guessing and is not, because the answer
     * has to exist before it is used: recon_icon_get returns NULL when there
     * is no such file, and then this falls through to the generic icon --
     * which is exactly where it started. It cannot produce a wrong picture,
     * only a right one or none.
     */
    if (shortened[0] != '\0' &&
            recon_icon_get(shortened, NULL, NULL) != NULL) {
        snprintf(g_answer, sizeof(g_answer), "%s", shortened);
        return g_answer;
    }

    return RECON_ICON_APP;
}
