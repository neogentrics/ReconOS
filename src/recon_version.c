#include "recon_version.h"

#include <stdio.h>
#include <string.h>

/*
 * A version part: digits, and a bound.
 *
 * Bounded at four digits rather than parsed into an int and checked afterwards,
 * because "99999999999999999999" overflows on the way in and the check would
 * be reading a number that already went wrong. Four digits is more releases
 * than this will ever have and is a number that cannot overflow.
 */
#define PART_DIGITS_MAX 4

static bool read_part(const char **at, int *out) {
    const char *p = *at;
    int digits = 0;
    int value = 0;

    while (*p >= '0' && *p <= '9') {
        if (++digits > PART_DIGITS_MAX) {
            return false;
        }
        value = value * 10 + (*p - '0');
        p++;
    }

    if (digits == 0) {
        return false;
    }

    *at = p;
    *out = value;
    return true;
}

bool recon_version_parse(const char *text, struct recon_version *out) {
    if (text == NULL || out == NULL) {
        return false;
    }

    /* A tag's leading v. Somebody will paste one in eventually. */
    if (*text == 'v' || *text == 'V') {
        text++;
    }

    struct recon_version got = { 0, 0, 0 };
    int *parts[3] = { &got.major, &got.minor, &got.patch };

    for (int i = 0; i < 3; i++) {
        if (!read_part(&text, parts[i])) {
            return false;
        }
        if (*text != '.') {
            break;
        }
        text++;
        /*
         * A trailing dot -- "1.2." -- is refused by the read_part above on the
         * next turn, because there are no digits after it. Which is right: it
         * is somebody's half-finished edit, not a version.
         */
    }

    if (*text != '\0') {
        return false;
    }

    *out = got;
    return true;
}

int recon_version_compare(const struct recon_version *a,
        const struct recon_version *b) {
    if (a->major != b->major) {
        return a->major < b->major ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return a->minor < b->minor ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return a->patch < b->patch ? -1 : 1;
    }
    return 0;
}

void recon_version_format(const struct recon_version *version, char *out,
        size_t size) {
    if (out == NULL || size == 0) {
        return;
    }
    if (version == NULL) {
        snprintf(out, size, "0.0.0");
        return;
    }
    snprintf(out, size, "%d.%d.%d", version->major, version->minor,
        version->patch);
}

int recon_version_compare_text(const char *a, const char *b,
        bool *unparseable) {
    struct recon_version left, right;

    if (!recon_version_parse(a, &left) || !recon_version_parse(b, &right)) {
        if (unparseable != NULL) {
            *unparseable = true;
        }
        return 0;
    }

    if (unparseable != NULL) {
        *unparseable = false;
    }
    return recon_version_compare(&left, &right);
}
