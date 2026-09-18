/*
 * Keeping a cookie jar across a restart. See include/recon_cookie.h.
 *
 * --- Why this is not in src/recon_cookie.c ---
 *
 * Sealing a file pulls in a cipher, the keyring, the accounts file and the
 * filesystem. `src/recon_cookie.c` is eight hundred lines of policy about what
 * a server may ask this machine to remember, and it compiles with no libc
 * under it -- it is one of the sixty-seven. Putting a file format and a cipher
 * into it to gain persistence would have taken the policy off that list to get
 * a format.
 *
 * So the jar stays what it is and this is separate, and the only thing that
 * crosses between them is `recon_cookie_restore`, which applies every refusal
 * a cookie arriving from a server meets. A saved file is a file somebody with
 * the disk can write; what it can put in a jar is exactly what a server could.
 *
 * --- The format ---
 *
 *     # ReconOS cookies
 *     <expires>\t<flags>\t<host>\t<path>\t<name>\t<value>
 *
 * Tab-separated, with the value last so it is the only field that can hold
 * anything surprising -- and it cannot hold a tab, because `recon_cookie_set`
 * refuses a control character in a name or a value. **That refusal is what
 * makes this format safe**, and it is written down at both ends on purpose: a
 * format made safe by a check somewhere else needs the check to be findable
 * from the format.
 *
 * `flags` is letters rather than a number: `s` secure, `h` http-only, `n`
 * narrowed, `-` none. A number would have to be read back as the same number
 * by a later version that had added a fourth flag; letters do not have that
 * problem, because an unknown one is ignored and a missing one is false.
 *
 * There is no version marker. The sealed file's own magic carries the format's
 * identity -- see `src/recon_sealed.c` -- and a version number inside a format
 * is a thing that can disagree with the bytes around it. `src/recon_package.c`
 * reached the same conclusion about receipts.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_cookie.h"
#include "recon_sealed.h"

#define COOKIE_FILE "browser/cookies"
#define HEADER      "# ReconOS cookies. Sealed: see include/recon_sealed.h.\n"

bool recon_cookie_jar_save(const struct recon_cookie_jar *jar) {
    /*
     * The worst case, sized rather than grown: three hundred cookies of the
     * longest each field may be. About half a megabyte, which is why this is
     * on the heap.
     */
    size_t capacity = sizeof(HEADER) +
        (size_t)RECON_COOKIE_MAX * (RECON_COOKIE_HOST_MAX +
            RECON_COOKIE_PATH_MAX + RECON_COOKIE_NAME_MAX +
            RECON_COOKIE_VALUE_MAX + 40);
    char *text = malloc(capacity);

    if (text == NULL) {
        return false;
    }

    int n = snprintf(text, capacity, HEADER);

    if (n < 0) {
        free(text);
        return false;
    }

    size_t used = (size_t)n;
    int count = recon_cookie_count(jar);

    for (int i = 0; i < count; i++) {
        struct recon_cookie_view c;

        if (!recon_cookie_at(jar, i, &c)) {
            continue;
        }

        /*
         * No expiry means the server said "until the window closes". Writing
         * it down would be keeping something it asked not to be kept, which
         * is not a stricter policy, it is a different one.
         */
        if (c.expires == 0) {
            continue;
        }

        char flags[4];
        size_t f = 0;

        if (c.secure) {
            flags[f++] = 's';
        }
        if (c.http_only) {
            flags[f++] = 'h';
        }
        if (c.was_narrowed) {
            flags[f++] = 'n';
        }
        if (f == 0) {
            flags[f++] = '-';
        }
        flags[f] = 0;

        n = snprintf(text + used, capacity - used, "%lld\t%s\t%s\t%s\t%s\t%s\n",
            (long long)c.expires, flags, c.host, c.path, c.name, c.value);
        if (n < 0 || (size_t)n >= capacity - used) {
            break;
        }
        used += (size_t)n;
    }

    /*
     * Written even when nothing was kept.
     *
     * An empty jar saved as an empty file is the difference between "I signed
     * out" and "I signed out and it came back": skipping the write would leave
     * yesterday's sealed jar on the disk for the next start to find.
     */
    bool ok = recon_sealed_write(COOKIE_FILE, text, used);

    free(text);
    return ok;
}

int recon_cookie_jar_load(struct recon_cookie_jar *jar, time_t now) {
    if (jar == NULL) {
        return 0;
    }

    size_t size = 0;
    char *text = recon_sealed_read(COOKIE_FILE, &size);

    if (text == NULL) {
        return 0;
    }

    int restored = 0;
    char *save = NULL;

    for (char *line = strtok_r(text, "\n", &save); line != NULL;
            line = strtok_r(NULL, "\n", &save)) {
        if (*line == '#' || *line == 0) {
            continue;
        }

        /*
         * Five tabs, and the value is everything after the last one -- so a
         * value is never cut at something inside it, and a value containing a
         * tab could not have got here anyway.
         *
         * A line with fewer is one this does not understand, and it is skipped
         * rather than guessed at. Half a cookie sent to a server is worse than
         * no cookie: it is a different cookie, under a name somebody's session
         * is keyed on.
         */
        char *field[6];
        int found = 0;
        char *walk = line;

        while (found < 5) {
            char *tab = strchr(walk, '\t');

            if (tab == NULL) {
                break;
            }
            *tab = 0;
            field[found++] = walk;
            walk = tab + 1;
        }
        if (found < 5) {
            continue;
        }
        field[5] = walk;

        struct recon_cookie_view c;

        memset(&c, 0, sizeof(c));
        c.expires = (time_t)atoll(field[0]);

        for (const char *f = field[1]; *f != 0; f++) {
            if (*f == 's') {
                c.secure = true;
            } else if (*f == 'h') {
                c.http_only = true;
            } else if (*f == 'n') {
                c.was_narrowed = true;
            }
        }

        snprintf(c.host, sizeof(c.host), "%s", field[2]);
        snprintf(c.path, sizeof(c.path), "%s", field[3]);
        snprintf(c.name, sizeof(c.name), "%s", field[4]);
        snprintf(c.value, sizeof(c.value), "%s", field[5]);

        /*
         * Every refusal lives in `recon_cookie_restore`, including dropping
         * one whose expiry has passed. Repeating any of them here would be two
         * places to keep in step, and the one that drifted would be this one.
         */
        if (recon_cookie_restore(jar, &c, now)) {
            restored++;
        }
    }

    free(text);
    return restored;
}

bool recon_cookie_jar_forget_saved(void) {
    return recon_sealed_forget(COOKIE_FILE);
}
