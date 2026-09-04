/*
 * Account pictures. See include/recon_avatar.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "ReconOS.h"
#include "recon_avatar.h"
#include "recon_fs.h"
#include "recon_icons.h"
#include "recon_registry.h"
#include "recon_ui.h"

#define AVATARS_MAX 32
#define AVATAR_NAME_MAX 64

/*
 * The set is found by listing /System/Icons and keeping the names that begin
 * with the prefix, rather than from a table.
 *
 * That is what makes a picture somebody adds themselves a real choice: drop
 * avatar-whatever.png in there and it is offered, with no code that has to
 * hear about it. A table would have made the drawn eight special and
 * everything else invisible.
 */
static char g_names[AVATARS_MAX][AVATAR_NAME_MAX];
static int g_count;
static bool g_scanned;

static void scan(void) {
    g_count = 0;
    g_scanned = true;

    struct recon_dirent entries[128];
    int found = recon_fs_list("/", RECON_DIR_SYSTEM_ICONS, entries, 128);
    if (found < 0) {
        return;
    }
    if (found > 128) {
        found = 128;
    }

    size_t prefix = strlen(RECON_AVATAR_PREFIX);

    for (int i = 0; i < found && g_count < AVATARS_MAX; i++) {
        if (entries[i].kind == RECON_FILE_DIRECTORY) {
            continue;
        }
        if (strncmp(entries[i].name, RECON_AVATAR_PREFIX, prefix) != 0) {
            continue;
        }

        /* Stored without its extension, because that is how an icon is asked
         * for -- the icon layer tries .ico and .png itself. */
        /* Too long to hold is left out rather than cut down: this name is
         * what the picture is asked for by, and a shortened one names
         * nothing. */
        if (strlen(entries[i].name) >= AVATAR_NAME_MAX) {
            continue;
        }
        recon_text_copy(g_names[g_count], AVATAR_NAME_MAX, entries[i].name);
        char *dot = strrchr(g_names[g_count], '.');
        if (dot != NULL) {
            *dot = '\0';
        }

        /* The same picture in two formats is one choice, not two. */
        bool duplicate = false;
        for (int j = 0; j < g_count; j++) {
            if (strcmp(g_names[j], g_names[g_count]) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            g_count++;
        }
    }
}

int recon_avatar_count(void) {
    if (!g_scanned) {
        scan();
    }
    return g_count;
}

bool recon_avatar_at(int index, char *out, size_t size) {
    if (!g_scanned) {
        scan();
    }
    if (index < 0 || index >= g_count || out == NULL) {
        return false;
    }
    snprintf(out, size, "%s", g_names[index]);
    return true;
}

/* Where one account's choice is kept. */
static void avatar_key(const char *account, char *out, size_t size) {
    snprintf(out, size, "users/%s/avatar", account != NULL ? account : "");
}

const char *recon_avatar_of(const char *account) {
    if (account == NULL || *account == '\0') {
        return "";
    }
    char key[RECON_REGISTRY_KEY_MAX];
    avatar_key(account, key, sizeof(key));
    return recon_registry_get(RECON_REG_SYSTEM, key, "");
}

bool recon_avatar_set(const char *account, const char *avatar) {
    if (account == NULL || *account == '\0') {
        return false;
    }
    char key[RECON_REGISTRY_KEY_MAX];
    avatar_key(account, key, sizeof(key));

    if (avatar == NULL || *avatar == '\0') {
        recon_registry_remove(RECON_REG_SYSTEM, key);
        return true;
    }
    return recon_registry_set(RECON_REG_SYSTEM, key, avatar);
}

/*
 * A colour for a name.
 *
 * Worked out from the name rather than assigned, so it is the same every time
 * without being stored anywhere, and two people rarely land on the same one.
 * The palette is fixed and hand-picked: hashing straight to RGB gives muddy
 * colours and occasionally an unreadable one.
 */
static recon_color colour_for(const char *account) {
    static const recon_color PALETTE[] = {
        RECON_RGB(0x3E, 0x6E, 0x8E),  /* slate blue */
        RECON_RGB(0x6E, 0x3E, 0x5E),  /* plum */
        RECON_RGB(0x4E, 0x7C, 0x4E),  /* moss */
        RECON_RGB(0x8B, 0x4A, 0x1A),  /* rust */
        RECON_RGB(0x3A, 0x5E, 0x6E),  /* teal */
        RECON_RGB(0x5E, 0x4E, 0x8E),  /* iris */
        RECON_RGB(0x7C, 0x5E, 0x2A),  /* bronze */
        RECON_RGB(0x2A, 0x5E, 0x4E),  /* pine */
    };
    const int count = (int)(sizeof(PALETTE) / sizeof(PALETTE[0]));

    unsigned hash = 2166136261u;
    for (const char *c = account != NULL ? account : ""; *c != '\0'; c++) {
        hash ^= (unsigned char)*c;
        hash *= 16777619u;
    }
    return PALETTE[hash % (unsigned)count];
}

/* A filled circle. */
static void fill_disc(struct recon_panel *panel, int cx, int cy, int radius,
        recon_color colour) {
    for (int y = -radius; y <= radius; y++) {
        /* The half-width of the circle at this row, by Pythagoras. Done as a
         * span per row rather than a test per pixel: it is the same shape and
         * a great deal less work. */
        int half = 0;
        while ((half + 1) * (half + 1) + y * y <= radius * radius) {
            half++;
        }
        recon_fill_rect(panel, cx - half, cy + y, half * 2 + 1, 1, colour);
    }
}

void recon_avatar_draw(struct recon_panel *panel, struct recon_font *font,
        const char *account, int x, int y, int size) {
    if (panel == NULL || size <= 0) {
        return;
    }

    const char *chosen = recon_avatar_of(account);
    if (*chosen != '\0' && recon_icon_draw(panel, chosen, x, y, size)) {
        return;
    }

    /*
     * Nothing chosen, or the file it named is gone. Either way there is still
     * a person here, so they get a disc with their initial on it rather than
     * a gap. A picture that has been deleted should degrade to this quietly:
     * it is a decoration, and losing it is not worth an error.
     */
    int radius = size / 2;
    recon_color disc = colour_for(account);
    fill_disc(panel, x + radius, y + radius, radius, disc);

    if (font == NULL || account == NULL || *account == '\0') {
        return;
    }

    char initial[2] = { account[0], '\0' };
    if (initial[0] >= 'a' && initial[0] <= 'z') {
        initial[0] = (char)(initial[0] - 'a' + 'A');
    }

    int width = recon_text_width(font, initial);
    int ascent = recon_font_ascent(font);
    recon_draw_text(panel, font, x + radius - width / 2,
        y + radius + ascent / 2 - 1, size, initial,
        RECON_RGB(0xF4, 0xF4, 0xF8));
}
