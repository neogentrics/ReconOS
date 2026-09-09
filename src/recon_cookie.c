/*
 * Cookies. See include/recon_cookie.h, where the four refusals are written
 * down and argued for.
 *
 * Nothing here opens anything. It is string handling on headers a server sent
 * and a table in memory, which is exactly the shape of thing that wants a
 * suite -- the same reason recon_url.c and recon_form.c are their own files.
 */

#define _POSIX_C_SOURCE 200809L

/*
 * The longest a cookie may be asked to last, in seconds: four hundred days.
 *
 * Not a number chosen here. RFC 6265bis says a user agent must clamp to it,
 * and browsers do -- so a server asking for ten years gets four hundred days
 * everywhere, and this behaving differently would be this being wrong.
 *
 * It is also what stops the arithmetic overflowing. `Max-Age=99999999999999`
 * saturates `atoll` at LLONG_MAX, and `now + that` is undefined behaviour --
 * which is exactly what the sanitizer said when the fuzzer sent one. A bound
 * that had to exist for correctness turning out to be the standard's is the
 * happy version of that.
 */
#define COOKIE_LONGEST ((long long)400 * 24 * 60 * 60)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "recon_cookie.h"

struct cookie {
    char host[RECON_COOKIE_HOST_MAX];
    char path[RECON_COOKIE_PATH_MAX];
    char name[RECON_COOKIE_NAME_MAX];
    char value[RECON_COOKIE_VALUE_MAX];
    bool secure;
    bool http_only;
    bool was_narrowed;
    time_t expires;                    /* 0 for the life of the window */

    /* When it arrived, which is the tie-break RFC 6265 asks for when two
     * cookies have paths of the same length. */
    unsigned long long serial;
};

struct recon_cookie_jar {
    struct cookie *at;
    int count;
    unsigned long long next_serial;
};

struct recon_cookie_jar *recon_cookie_jar_new(void) {
    struct recon_cookie_jar *jar = calloc(1, sizeof(*jar));
    if (jar == NULL) {
        return NULL;
    }
    jar->at = calloc(RECON_COOKIE_MAX, sizeof(*jar->at));
    if (jar->at == NULL) {
        free(jar);
        return NULL;
    }
    return jar;
}

void recon_cookie_jar_free(struct recon_cookie_jar *jar) {
    if (jar == NULL) {
        return;
    }
    /*
     * Cleared before it is freed. Every value in here is a key to somebody's
     * account, and this is the one place they all are -- so the buffer is not
     * handed back to the allocator still holding them.
     *
     * `memset` and not `recon_secure_erase`, deliberately: the memory is about
     * to be freed and read by nothing, so an optimiser is entitled to delete
     * this store and probably does. What it costs is nothing and what it buys
     * is that the intent is written down where the next person looks. The
     * secrets that must genuinely be gone go through the keyring, which uses
     * the volatile write.
     */
    memset(jar->at, 0, RECON_COOKIE_MAX * sizeof(*jar->at));
    free(jar->at);
    free(jar);
}

/* --- Dates --- */

static const char *const MONTHS[12] = {
    "jan", "feb", "mar", "apr", "may", "jun",
    "jul", "aug", "sep", "oct", "nov", "dec",
};

/*
 * Days since 1970 for a civil date, by Howard Hinnant's algorithm.
 *
 * Written out rather than calling `timegm`, which is not in any standard, and
 * rather than `mktime`, which reads the machine's time zone -- a cookie expiry
 * is in UTC and interpreting it as local time gets it wrong by up to half a
 * day in a direction that depends on where the computer is. That is the kind
 * of fault that only appears on somebody else's machine.
 */
static long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + (long long)doe - 719468LL;
}

/*
 * Read an expiry date the way RFC 6265 says to: find the pieces, in any order,
 * separated by anything that is not part of one.
 *
 * That is deliberately loose, and it is loose in the standard for a reason --
 * servers send `Wed, 09 Jun 2021 10:18:14 GMT`, `Wednesday, 09-Jun-21
 * 10:18:14 GMT` and `Wed Jun 09 10:18:14 2021`, and a parser that insisted on
 * one of them would silently treat the other two as "no expiry" and keep a
 * cookie a server had asked to be forgotten.
 *
 * **Returns whether it read one, rather than returning the time.** The first
 * version returned 0 for "could not read this", which collides with a real
 * date -- and not an obscure one: `Thu, 01 Jan 1970 00:00:00 GMT` is epoch
 * zero and is what servers send to delete a cookie. So the commonest deletion
 * header on the web was being read as "no expiry" and the cookie kept, which
 * means signing out would have left somebody signed in. Found by the suite,
 * before any of this had run against a server.
 *
 * A date it genuinely cannot read leaves the cookie without an expiry rather
 * than refusing it, which is what the standard says.
 */
static bool read_expiry(const char *text, time_t *out) {
    int hour = -1, minute = -1, second = -1;
    int day = -1, month = -1, year = -1;

    const char *at = text;
    while (*at != '\0') {
        /* Skip anything that cannot begin a piece. */
        while (*at != '\0' && !isalnum((unsigned char)*at)) {
            at++;
        }
        if (*at == '\0') {
            break;
        }

        /*
         * A token is letters, digits and colons. **Not hyphens**: RFC 6265
         * lists "-" among the characters that separate one piece from the
         * next, and treating it as part of one turns `09-Jun-21` into a
         * single token that is not a day, not a month and not a year -- so
         * the entire second date format was read as no date at all.
         */
        const char *start = at;
        while (isalnum((unsigned char)*at) || *at == ':') {
            at++;
        }
        size_t length = (size_t)(at - start);
        if (length == 0 || length > 31) {
            continue;
        }

        char piece[32];
        memcpy(piece, start, length);
        piece[length] = '\0';

        int h = 0, m = 0, s = 0;
        if (hour < 0 && sscanf(piece, "%d:%d:%d", &h, &m, &s) == 3) {
            hour = h;
            minute = m;
            second = s;
            continue;
        }

        if (month < 0 && isalpha((unsigned char)piece[0]) && length >= 3) {
            for (int i = 0; i < 12; i++) {
                if (strncasecmp(piece, MONTHS[i], 3) == 0) {
                    month = i + 1;
                    break;
                }
            }
            if (month > 0) {
                continue;
            }
        }

        if (isdigit((unsigned char)piece[0])) {
            int number = atoi(piece);
            if (day < 0 && length <= 2 && number >= 1 && number <= 31) {
                day = number;
                continue;
            }
            if (year < 0 && (length == 2 || length == 4)) {
                year = number;
                continue;
            }
        }
    }

    if (day < 0 || month < 0 || year < 0 || hour < 0) {
        return false;
    }

    /*
     * Two-digit years, which are still sent. The standard's rule, not a guess:
     * 0-69 is 2000-2069 and 70-99 is 1970-1999.
     */
    if (year >= 0 && year <= 69) {
        year += 2000;
    } else if (year >= 70 && year <= 99) {
        year += 1900;
    }

    if (hour > 23 || minute > 59 || second > 60 || year < 1601) {
        return false;
    }

    long long days = days_from_civil(year, (unsigned)month, (unsigned)day);
    long long when = days * 86400LL + hour * 3600LL + minute * 60LL + second;
    if (when < 0) {
        /* Before 1970. Not a date this can hold, and a cookie dated then is
         * one asking to be deleted -- which is what the caller does with it. */
        when = 0;
    }
    *out = (time_t)when;
    return true;
}

/* --- Matching --- */

/* Trim spaces and tabs from both ends, in place. */
static void trim(char *text) {
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
            text[length - 1] == '\r' || text[length - 1] == '\n')) {
        text[--length] = '\0';
    }
    size_t lead = 0;
    while (text[lead] == ' ' || text[lead] == '\t') {
        lead++;
    }
    if (lead > 0) {
        memmove(text, text + lead, length - lead + 1);
    }
}

/*
 * The default path: everything up to the last "/" of the request's path, per
 * RFC 6265 section 5.1.4.
 *
 * Not the request path itself. A cookie set while reading `/help/index.html`
 * belongs to `/help`, and storing the file name would mean it was never sent
 * again -- the next page in that directory has a different one.
 */
static void default_path(const char *request_path, char *out, size_t size) {
    if (request_path == NULL || request_path[0] != '/') {
        snprintf(out, size, "/");
        return;
    }
    const char *last = strrchr(request_path, '/');
    if (last == NULL || last == request_path) {
        snprintf(out, size, "/");
        return;
    }
    size_t keep = (size_t)(last - request_path);
    if (keep >= size) {
        keep = size - 1;
    }
    memcpy(out, request_path, keep);
    out[keep] = '\0';
}

/*
 * Does a request for `path` reach a cookie stored at `cookie_path`?
 *
 * RFC 6265 section 5.1.4, and the third case is the one everybody gets wrong:
 * `/foo` matches `/foo` and `/foo/bar`, and does **not** match `/foobar`. A
 * plain prefix test sends a cookie scoped to `/admin` to `/administrators`.
 */
static bool path_matches(const char *cookie_path, const char *path) {
    size_t length = strlen(cookie_path);
    if (length == 0) {
        return true;
    }
    if (strncmp(path, cookie_path, length) != 0) {
        return false;
    }
    if (path[length] == '\0') {
        return true;
    }
    if (cookie_path[length - 1] == '/') {
        return true;                   /* the stored path ended in a slash */
    }
    return path[length] == '/';
}

/*
 * The whole of the domain policy, in one place so it can be read at once.
 *
 * A cookie is kept under the host that set it. `Domain` is honoured when it
 * names that host -- with or without the leading dot servers still write --
 * and is otherwise ignored, which narrows the cookie rather than widening it.
 *
 * Narrowing is always safe: the cookie goes back to exactly where it came
 * from. Widening is what needs the Public Suffix List, and the header says why
 * a copy of that list is worse than not having one.
 */
static bool domain_names_the_host(const char *domain, const char *host) {
    const char *want = domain;
    if (want[0] == '.') {
        want++;
    }
    return strcasecmp(want, host) == 0;
}

/* --- Storing --- */

int recon_cookie_sweep(struct recon_cookie_jar *jar, time_t now) {
    if (jar == NULL) {
        return 0;
    }
    int dropped = 0;
    for (int i = jar->count - 1; i >= 0; i--) {
        if (jar->at[i].expires != 0 && jar->at[i].expires <= now) {
            memmove(&jar->at[i], &jar->at[i + 1],
                sizeof(*jar->at) * (size_t)(jar->count - i - 1));
            jar->count--;
            dropped++;
        }
    }
    if (dropped > 0) {
        memset(&jar->at[jar->count], 0,
            sizeof(*jar->at) * (size_t)dropped);
    }
    return dropped;
}

/* Which entry is this cookie, by the three things that identify one. */
static int find(const struct recon_cookie_jar *jar, const char *host,
        const char *path, const char *name) {
    for (int i = 0; i < jar->count; i++) {
        if (strcasecmp(jar->at[i].host, host) == 0 &&
                strcmp(jar->at[i].path, path) == 0 &&
                strcmp(jar->at[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void drop_at(struct recon_cookie_jar *jar, int index) {
    memmove(&jar->at[index], &jar->at[index + 1],
        sizeof(*jar->at) * (size_t)(jar->count - index - 1));
    jar->count--;
    memset(&jar->at[jar->count], 0, sizeof(*jar->at));
}

/*
 * Make room within one host, by dropping the one that expires soonest.
 *
 * Within the host rather than across the jar, so a site that sets fifty
 * cookies cannot push another site's session out. That is the difference
 * between a ceiling and a weapon.
 */
static bool make_room(struct recon_cookie_jar *jar, const char *host) {
    int here = 0;
    int soonest = -1;
    for (int i = 0; i < jar->count; i++) {
        if (strcasecmp(jar->at[i].host, host) != 0) {
            continue;
        }
        here++;
        if (soonest < 0) {
            soonest = i;
            continue;
        }
        /* A cookie with no expiry outlives one with any expiry at all. */
        time_t a = jar->at[i].expires;
        time_t b = jar->at[soonest].expires;
        if (b == 0 && a != 0) {
            soonest = i;
        } else if (a != 0 && b != 0 && a < b) {
            soonest = i;
        }
    }

    if (here < RECON_COOKIE_PER_HOST && jar->count < RECON_COOKIE_MAX) {
        return true;
    }
    if (here >= RECON_COOKIE_PER_HOST && soonest >= 0) {
        drop_at(jar, soonest);
        return true;
    }
    /* The jar itself is full and this host is not the reason. Dropping
     * somebody else's session to make room for this one would let any site
     * sign you out of every other, so this one is refused instead. */
    return false;
}

const char *recon_cookie_set(struct recon_cookie_jar *jar, const char *host,
        const char *path, bool secure, const char *set_cookie, time_t now) {
    if (jar == NULL || host == NULL || host[0] == '\0' ||
            set_cookie == NULL) {
        return "there was nothing to store";
    }

    recon_cookie_sweep(jar, now);

    /* --- The name and the value, which are everything before the first ";" --- */
    const char *semicolon = strchr(set_cookie, ';');
    size_t pair_length = (semicolon != NULL)
        ? (size_t)(semicolon - set_cookie) : strlen(set_cookie);

    char pair[RECON_COOKIE_NAME_MAX + RECON_COOKIE_VALUE_MAX + 4];
    if (pair_length >= sizeof(pair)) {
        return "that cookie is longer than this keeps";
    }
    memcpy(pair, set_cookie, pair_length);
    pair[pair_length] = '\0';

    char *equals = strchr(pair, '=');
    if (equals == NULL) {
        /*
         * RFC 6265 allows a cookie with no name at all -- the whole string is
         * the value. It is refused here: nothing sets one on purpose, a server
         * cannot tell it apart from any other nameless cookie, and accepting
         * it means a malformed header quietly becomes state.
         */
        return "a cookie with no name is not stored";
    }
    *equals = '\0';

    /*
     * Both lengths are checked before either is copied.
     *
     * A name cut to fit is a *different cookie*: it would be stored under a
     * name no server ever set, sent back under that name, and would never
     * replace the one it was meant to be. Same for a value, where half a
     * session token is not a shorter token. Neither is truncated; both are
     * refused, and the refusal is shown.
     */
    if (strlen(pair) >= RECON_COOKIE_NAME_MAX) {
        return "that cookie's name is longer than this keeps";
    }
    if (strlen(equals + 1) >= RECON_COOKIE_VALUE_MAX) {
        return "that cookie's value is longer than this keeps";
    }

    /*
     * Copied by the length already checked, rather than by snprintf.
     *
     * snprintf here is correct and the compiler cannot see it: it only knows
     * the source could be 1,155 bytes and the destination is 128, so it warns
     * about a truncation the two returns above have made impossible. Copying
     * the measured length says the same thing in a form that is checkable --
     * and this is a file where a silent truncation would store a cookie under
     * a name no server ever set.
     */
    size_t name_length = strlen(pair);
    size_t value_length = strlen(equals + 1);

    char name[RECON_COOKIE_NAME_MAX];
    char value[RECON_COOKIE_VALUE_MAX];
    memcpy(name, pair, name_length);
    name[name_length] = '\0';
    memcpy(value, equals + 1, value_length);
    value[value_length] = '\0';
    trim(name);
    trim(value);

    if (name[0] == '\0') {
        return "a cookie with no name is not stored";
    }

    /* --- The attributes --- */
    char stored_path[RECON_COOKIE_PATH_MAX];
    default_path(path, stored_path, sizeof(stored_path));

    bool want_secure = false;
    bool http_only = false;
    bool was_narrowed = false;
    bool have_max_age = false;
    long long max_age = 0;
    bool have_expires = false;
    time_t expires = 0;

    const char *walk = semicolon;
    while (walk != NULL) {
        walk++;                        /* past the ";" */
        const char *next = strchr(walk, ';');
        size_t length = (next != NULL) ? (size_t)(next - walk) : strlen(walk);

        char attribute[RECON_COOKIE_PATH_MAX + 64];
        if (length >= sizeof(attribute)) {
            length = sizeof(attribute) - 1;
        }
        memcpy(attribute, walk, length);
        attribute[length] = '\0';
        trim(attribute);

        char *value_at = strchr(attribute, '=');
        char *attribute_value = NULL;
        if (value_at != NULL) {
            *value_at = '\0';
            attribute_value = value_at + 1;
            trim(attribute);
            trim(attribute_value);
        }

        if (strcasecmp(attribute, "secure") == 0) {
            want_secure = true;
        } else if (strcasecmp(attribute, "httponly") == 0) {
            http_only = true;
        } else if (attribute_value == NULL) {
            /* An attribute with no value that means nothing here. */
        } else if (strcasecmp(attribute, "path") == 0) {
            if (attribute_value[0] == '/') {
                snprintf(stored_path, sizeof(stored_path), "%s",
                    attribute_value);
            }
            /* A Path that does not begin with "/" is ignored and the default
             * stands, which is what the standard says. */
        } else if (strcasecmp(attribute, "domain") == 0) {
            if (attribute_value[0] != '\0' &&
                    !domain_names_the_host(attribute_value, host)) {
                was_narrowed = true;
            }
        } else if (strcasecmp(attribute, "max-age") == 0) {
            have_max_age = true;
            max_age = atoll(attribute_value);
        } else if (strcasecmp(attribute, "expires") == 0) {
            have_expires = read_expiry(attribute_value, &expires);
        }

        walk = next;
    }

    /*
     * Max-Age beats Expires when both are sent, which the standard is explicit
     * about and which matters: a server that wants a cookie gone sends both,
     * and reading only the second leaves somebody signed in.
     */
    time_t when = 0;
    if (have_max_age) {
        /* One, not zero: zero means "no expiry" everywhere else in this file,
         * and a Max-Age of zero means the opposite of that. */
        if (max_age <= 0) {
            when = (time_t)1;
        } else {
            if (max_age > COOKIE_LONGEST) {
                max_age = COOKIE_LONGEST;
            }
            when = (time_t)(now + max_age);
        }
    } else if (have_expires) {
        /*
         * Epoch zero is a date, and the deletion below reads it as one.
         * Nudged to 1 only because 0 is this file's "no expiry", and a cookie
         * dated 1970 is being deleted either way.
         */
        when = (expires == 0) ? (time_t)1 : expires;
    }

    /* And a date far enough ahead comes back to the same bound, so a server
     * cannot reach past it by writing an Expires instead of a Max-Age. */
    if (when > now && (long long)when - (long long)now > COOKIE_LONGEST) {
        when = (time_t)(now + COOKIE_LONGEST);
    }

    /*
     * A `Secure` cookie over a connection that is not.
     *
     * The standard used to permit this and browsers stopped permitting it,
     * because it is how a server on the clear side of a site is used to
     * overwrite the session set on the encrypted side. Refused, and said --
     * the same rule, in the same shape, as recon_http refusing a redirect that
     * goes from https back to http.
     */
    if (want_secure && !secure) {
        return "a Secure cookie arrived over an unencrypted connection, "
            "which is how one side of a site overwrites the other's session "
            "-- refused";
    }

    int at = find(jar, host, stored_path, name);

    /* A cookie already over is a deletion, whether or not one is there. */
    if (when != 0 && when <= now) {
        if (at >= 0) {
            drop_at(jar, at);
        }
        return NULL;
    }

    if (at < 0) {
        if (!make_room(jar, host)) {
            return "this is holding as many cookies as it will";
        }
        at = jar->count++;
        memset(&jar->at[at], 0, sizeof(jar->at[at]));
        snprintf(jar->at[at].host, sizeof(jar->at[at].host), "%s", host);
        snprintf(jar->at[at].path, sizeof(jar->at[at].path), "%s",
            stored_path);
        snprintf(jar->at[at].name, sizeof(jar->at[at].name), "%s", name);
    }

    /*
     * Replacing keeps the original serial, so a cookie a server refreshes on
     * every request does not walk to the end of the send order. RFC 6265 asks
     * for creation time, not last-write time, and this is the difference.
     */
    if (jar->at[at].serial == 0) {
        jar->at[at].serial = ++jar->next_serial;
    }
    snprintf(jar->at[at].value, sizeof(jar->at[at].value), "%s", value);
    jar->at[at].secure = want_secure;
    jar->at[at].http_only = http_only;
    jar->at[at].was_narrowed = was_narrowed;
    jar->at[at].expires = when;
    return NULL;
}

/* --- Sending --- */

/*
 * The order RFC 6265 section 5.4 asks for: longer paths first, and among
 * equal paths the one that arrived first.
 *
 * It matters because a server reading a repeated name reads one of them, and
 * which one it reads is decided here. A more specific path is the more
 * specific answer, so it goes first.
 */
static int more_specific(const struct cookie *a, const struct cookie *b) {
    size_t pa = strlen(a->path);
    size_t pb = strlen(b->path);
    if (pa != pb) {
        return pa > pb ? -1 : 1;
    }
    if (a->serial != b->serial) {
        return a->serial < b->serial ? -1 : 1;
    }
    return 0;
}

size_t recon_cookie_header(struct recon_cookie_jar *jar, const char *host,
        const char *path, bool secure, time_t now, char *out, size_t size) {
    if (out == NULL || size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (jar == NULL || host == NULL || host[0] == '\0') {
        return 0;
    }

    recon_cookie_sweep(jar, now);

    const char *want_path = (path != NULL && path[0] == '/') ? path : "/";

    /* Which ones match, as indices, so the sort moves four bytes rather than
     * a kilobyte and a half. */
    int order[RECON_COOKIE_MAX];
    int found = 0;
    for (int i = 0; i < jar->count && found < RECON_COOKIE_MAX; i++) {
        const struct cookie *c = &jar->at[i];
        if (strcasecmp(c->host, host) != 0) {
            continue;                  /* host-only, always; see the header */
        }
        if (c->secure && !secure) {
            continue;
        }
        if (!path_matches(c->path, want_path)) {
            continue;
        }
        order[found++] = i;
    }

    /* An insertion sort: `found` is at most fifty, and a comparison here is
     * two string lengths. */
    for (int i = 1; i < found; i++) {
        int hold = order[i];
        int j = i - 1;
        while (j >= 0 &&
                more_specific(&jar->at[hold], &jar->at[order[j]]) < 0) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = hold;
    }

    size_t used = 0;
    for (int i = 0; i < found; i++) {
        const struct cookie *c = &jar->at[order[i]];
        size_t need = strlen(c->name) + 1 + strlen(c->value) +
            (used > 0 ? 2 : 0);
        if (used + need + 1 > size) {
            /*
             * Stopped rather than cut mid-cookie. Half a session token is not
             * a shorter session token, it is a different one -- and a server
             * given one may answer as though somebody signed out.
             */
            break;
        }
        if (used > 0) {
            out[used++] = ';';
            out[used++] = ' ';
        }
        int wrote = snprintf(out + used, size - used, "%s=%s", c->name,
            c->value);
        if (wrote < 0) {
            break;
        }
        used += (size_t)wrote;
    }
    out[used] = '\0';
    return used;
}

/* --- Looking at what is kept --- */

int recon_cookie_count(const struct recon_cookie_jar *jar) {
    return jar != NULL ? jar->count : 0;
}

bool recon_cookie_at(const struct recon_cookie_jar *jar, int index,
        struct recon_cookie_view *out) {
    if (jar == NULL || out == NULL || index < 0 || index >= jar->count) {
        return false;
    }
    const struct cookie *c = &jar->at[index];
    memset(out, 0, sizeof(*out));
    snprintf(out->host, sizeof(out->host), "%s", c->host);
    snprintf(out->path, sizeof(out->path), "%s", c->path);
    snprintf(out->name, sizeof(out->name), "%s", c->name);
    snprintf(out->value, sizeof(out->value), "%s", c->value);
    out->secure = c->secure;
    out->http_only = c->http_only;
    out->expires = c->expires;
    out->was_narrowed = c->was_narrowed;
    return true;
}

int recon_cookie_forget_host(struct recon_cookie_jar *jar, const char *host) {
    if (jar == NULL || host == NULL) {
        return 0;
    }
    int gone = 0;
    for (int i = jar->count - 1; i >= 0; i--) {
        if (strcasecmp(jar->at[i].host, host) == 0) {
            drop_at(jar, i);
            gone++;
        }
    }
    return gone;
}

int recon_cookie_forget_all(struct recon_cookie_jar *jar) {
    if (jar == NULL) {
        return 0;
    }
    int gone = jar->count;
    memset(jar->at, 0, RECON_COOKIE_MAX * sizeof(*jar->at));
    jar->count = 0;
    return gone;
}
