/*
 * Reading CSS. See include/recon_css.h for what this does and does not do.
 *
 * The shape of it: a stylesheet is a list of rules, a rule is one selector and
 * a set of declarations, and matching an element walks the rules and keeps the
 * most specific answer for each property. There is no tree here and no
 * computed style -- the caller has an element and its ancestors and asks what
 * applies, which is the only question a flat document reader can ask.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "recon_css.h"

/*
 * Bounds, not policy. A stylesheet is somebody else's file, and a reader that
 * grows to fit whatever it is handed is one a hostile page can exhaust.
 *
 * Two thousand rules is more than any page this can usefully render; the
 * fourth part of a descendant selector is past the point where a flat document
 * model can honour it anyway.
 */
#define RULES_MAX 2048

/*
 * How many custom properties one sheet's palette may hold.
 *
 * A bound, not a prediction: gaming.recontowers.com defines 157 on `:root`
 * alone. Past this the rest are dropped and the values that used them fall
 * back, which is a page with some of its colours rather than none.
 */
#define CUSTOMS_MAX 512
#define PARTS_MAX 4
#define NAME_MAX_LEN 64

/* One compound selector: a tag, an id and up to two classes, all optional. */
struct part {
    char tag[NAME_MAX_LEN];
    char id[NAME_MAX_LEN];
    char class_one[NAME_MAX_LEN];
    char class_two[NAME_MAX_LEN];
};

struct rule {
    struct part parts[PARTS_MAX];
    int part_count;
    /*
     * (ids, classes, tags), packed so a plain comparison orders them the way
     * the specification does. Order within a specificity is the file order,
     * which is why `index` is here too.
     */
    unsigned specificity;
    int index;
    struct recon_css_style style;
};

/*
 * One custom property -- `--rt-bg: #08090c`.
 *
 * These are how every stylesheet written since about 2018 states its palette,
 * and a reader without them reads a modern sheet and finds almost no colours
 * at all: gaming.recontowers.com declares 157 of them and then says
 * `background: var(--rt-bg)` and `color: var(--rt-ink)` everywhere, so the
 * page came out in the skin's black on the skin's white with none of its own
 * design.
 */
struct custom {
    char name[64];
    char value[192];
};

struct recon_css_sheet {
    struct rule rules[RULES_MAX];
    int count;
    int next_index;

    /*
     * The palette, taken from `:root`, `html` and `body` only.
     *
     * Custom properties inherit and can be set on any element, so a complete
     * reading needs one table per element rather than one per sheet. This
     * takes the page-wide set, which is what the great majority of them are
     * -- 91 of the 157 on that page -- and leaves component-scoped overrides
     * reading the page-wide value. That is a value from the same palette,
     * which is wrong in shade and never wrong in contrast; a missing one is
     * wrong in both.
     */
    struct custom customs[CUSTOMS_MAX];
    int custom_count;
};

static const char *custom_value(const struct recon_css_sheet *sheet,
        const char *name, size_t length) {
    if (sheet == NULL) {
        return NULL;
    }
    for (int i = 0; i < sheet->custom_count; i++) {
        if (strlen(sheet->customs[i].name) == length &&
                strncmp(sheet->customs[i].name, name, length) == 0) {
            return sheet->customs[i].value;
        }
    }
    return NULL;
}

/*
 * Rewrite every `var(--name)` and `var(--name, fallback)` in a value.
 *
 * `depth` stops `--a: var(--b)` where `--b: var(--a)`. A name that is not in
 * the palette becomes its fallback, and with no fallback becomes nothing --
 * which is what a browser does, and which leaves the property unset rather
 * than set to something invented.
 */
static void resolve_vars(const struct recon_css_sheet *sheet, const char *in,
        char *out, size_t size, int depth) {
    size_t at = 0;
    if (size == 0) {
        return;
    }
    out[0] = '\0';
    if (in == NULL) {
        return;
    }

    for (size_t i = 0; in[i] != '\0' && at + 1 < size; ) {
        if (depth > 0 && strncasecmp(in + i, "var(", 4) == 0) {
            /* To the matching bracket, so a fallback that is itself a
             * function -- var(--a, rgb(0,0,0)) -- is not cut in half. */
            size_t j = i + 4;
            int nested = 1;
            while (in[j] != '\0' && nested > 0) {
                if (in[j] == '(') {
                    nested++;
                } else if (in[j] == ')') {
                    nested--;
                    if (nested == 0) {
                        break;
                    }
                }
                j++;
            }
            if (in[j] != ')') {
                break;                         /* unclosed; take no more */
            }

            /* Split the arguments at the first comma at this level. */
            size_t name_start = i + 4;
            size_t comma = 0;
            int level = 0;
            for (size_t k = name_start; k < j; k++) {
                if (in[k] == '(') {
                    level++;
                } else if (in[k] == ')') {
                    level--;
                } else if (in[k] == ',' && level == 0) {
                    comma = k;
                    break;
                }
            }
            size_t name_end = comma > 0 ? comma : j;
            while (name_end > name_start &&
                    isspace((unsigned char)in[name_end - 1])) {
                name_end--;
            }
            while (name_start < name_end &&
                    isspace((unsigned char)in[name_start])) {
                name_start++;
            }

            const char *found =
                custom_value(sheet, in + name_start, name_end - name_start);

            char piece[192];
            if (found != NULL) {
                /* A palette entry may itself be written with var(). */
                resolve_vars(sheet, found, piece, sizeof(piece), depth - 1);
            } else if (comma > 0) {
                size_t f = comma + 1;
                while (f < j && isspace((unsigned char)in[f])) {
                    f++;
                }
                char raw[192];
                size_t take = (j - f) < sizeof(raw) - 1 ? (j - f)
                                                        : sizeof(raw) - 1;
                memcpy(raw, in + f, take);
                raw[take] = '\0';
                resolve_vars(sheet, raw, piece, sizeof(piece), depth - 1);
            } else {
                piece[0] = '\0';
            }

            for (size_t k = 0; piece[k] != '\0' && at + 1 < size; k++) {
                out[at++] = piece[k];
            }
            i = j + 1;
            continue;
        }

        out[at++] = in[i++];
    }
    out[at] = '\0';
}

/* --- Small helpers --- */

static bool name_char(char c) {
    return isalnum((unsigned char)c) || c == '-' || c == '_';
}

/*
 * Case-insensitive substring, written out rather than borrowed.
 *
 * `strcasestr` is a GNU extension: available in the desktop build because
 * something else in it asks for the extensions, and not available when this
 * file is compiled on its own for its tests. A four-line function is a better
 * answer than a feature macro whose effect depends on what else was included.
 */
static bool contains_fold(const char *haystack, const char *needle) {
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    size_t n = strlen(needle);
    for (const char *at = haystack; *at != '\0'; at++) {
        if (strncasecmp(at, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static void copy_name(char *out, size_t size, const char *from, size_t length) {
    if (length >= size) {
        length = size - 1;
    }
    memcpy(out, from, length);
    out[length] = '\0';
}

/* --- Colours --- */

/*
 * The named colours worth having.
 *
 * Not all hundred and forty. These are the ones pages actually write, and a
 * name this does not know leaves the colour unset -- which is the right
 * answer, because the alternative is guessing at somebody's brand colour and
 * drawing text in it.
 */
static const struct {
    const char *name;
    unsigned value;
} NAMED[] = {
    { "black",   0x000000 }, { "white",   0xFFFFFF },
    { "red",     0xFF0000 }, { "green",   0x008000 },
    { "blue",    0x0000FF }, { "yellow",  0xFFFF00 },
    { "gray",    0x808080 }, { "grey",    0x808080 },
    { "silver",  0xC0C0C0 }, { "maroon",  0x800000 },
    { "olive",   0x808000 }, { "lime",    0x00FF00 },
    { "aqua",    0x00FFFF }, { "cyan",    0x00FFFF },
    { "teal",    0x008080 }, { "navy",    0x000080 },
    { "fuchsia", 0xFF00FF }, { "magenta", 0xFF00FF },
    { "purple",  0x800080 }, { "orange",  0xFFA500 },
    { "brown",   0xA52A2A }, { "pink",    0xFFC0CB },
    { "gold",    0xFFD700 }, { "beige",   0xF5F5DC },
    { "ivory",   0xFFFFF0 }, { "khaki",   0xF0E68C },
    { "crimson", 0xDC143C }, { "indigo",  0x4B0082 },
    { "violet",  0xEE82EE }, { "salmon",  0xFA8072 },
    { "tan",     0xD2B48C }, { "plum",    0xDDA0DD },
    { "orchid",  0xDA70D6 }, { "coral",   0xFF7F50 },
    { "darkblue",  0x00008B }, { "darkgreen", 0x006400 },
    { "darkred",   0x8B0000 }, { "darkgray",  0xA9A9A9 },
    { "darkgrey",  0xA9A9A9 }, { "lightgray", 0xD3D3D3 },
    { "lightgrey", 0xD3D3D3 }, { "lightblue", 0xADD8E6 },
    { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC },
};

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

bool recon_css_colour(const char *text, unsigned *out) {
    if (text == NULL || out == NULL) {
        return false;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }

    if (*text == '#') {
        const char *d = text + 1;
        int digits = 0;
        while (digits < 8 && hex_digit(d[digits]) >= 0) {
            digits++;
        }
        if (digits == 3 || digits == 4) {
            /* #rgb, and #rgba whose alpha this has nowhere to put. */
            int r = hex_digit(d[0]), g = hex_digit(d[1]), b = hex_digit(d[2]);
            *out = (unsigned)((r * 17) << 16 | (g * 17) << 8 | (b * 17));
            return true;
        }
        if (digits == 6 || digits == 8) {
            unsigned v = 0;
            for (int i = 0; i < 6; i++) {
                v = (v << 4) | (unsigned)hex_digit(d[i]);
            }
            *out = v;
            return true;
        }
        return false;
    }

    if (strncasecmp(text, "rgb", 3) == 0) {
        const char *open = strchr(text, '(');
        if (open == NULL) {
            return false;
        }
        int channel[3] = {0, 0, 0};
        const char *at = open + 1;
        for (int i = 0; i < 3; i++) {
            while (*at == ' ' || *at == ',') {
                at++;
            }
            if (!isdigit((unsigned char)*at)) {
                return false;
            }
            int value = 0;
            while (isdigit((unsigned char)*at)) {
                value = value * 10 + (*at++ - '0');
            }
            /* A percentage channel, which is legal and rare. */
            if (*at == '%') {
                at++;
                value = value * 255 / 100;
            }
            channel[i] = value > 255 ? 255 : value;
        }
        *out = (unsigned)(channel[0] << 16 | channel[1] << 8 | channel[2]);
        return true;
    }

    for (size_t i = 0; i < sizeof(NAMED) / sizeof(NAMED[0]); i++) {
        if (strcasecmp(text, NAMED[i].name) == 0) {
            *out = NAMED[i].value;
            return true;
        }
    }
    return false;
}

/* --- Declarations --- */

/*
 * One `name: value` pair, folded into a style.
 *
 * Everything not listed is ignored on purpose. A property this does not
 * understand is a property that changes nothing, which is the failure that
 * leaves a page looking like its markup -- and is a great deal better than
 * one that changes the wrong thing.
 */
static void apply_declaration(struct recon_css_style *style,
        const char *name, const char *value) {
    if (strcasecmp(name, "display") == 0) {
        if (strcasecmp(value, "none") == 0) {
            style->display = RECON_CSS_NONE;
        } else if (contains_fold(value, "inline") ||
                strcasecmp(value, "contents") == 0) {
            /*
             * `inline-block` and `inline-flex` count as inline here. They lay
             * their *contents* out as a box and sit in the line themselves,
             * and only the second half of that is a thing this can act on.
             */
            style->display = RECON_CSS_INLINE;
        } else {
            /*
             * Everything else starts a line: block, flex, grid, table,
             * list-item, flow-root. Lumped together on purpose -- what they
             * have in common is the only part of them a reader with no layout
             * can honour, which is that the thing before them ends.
             */
            style->display = RECON_CSS_BLOCK;
        }
        return;
    }

    /*
     * `visibility: hidden` leaves the space and shows nothing. A reader with
     * no layout has no space to leave, so it is the same answer as display
     * none -- and treating it as "shown" would put back exactly the parts of
     * a page somebody hid.
     */
    if (strcasecmp(name, "visibility") == 0) {
        if (strcasecmp(value, "hidden") == 0 ||
                strcasecmp(value, "collapse") == 0) {
            style->display = RECON_CSS_NONE;
        }
        return;
    }

    if (strcasecmp(name, "color") == 0 || strcasecmp(name, "colour") == 0) {
        unsigned rgb = 0;
        if (recon_css_colour(value, &rgb)) {
            style->colour = rgb;
            style->has_colour = true;
        }
        return;
    }

    if (strcasecmp(name, "background-color") == 0 ||
            strcasecmp(name, "background") == 0) {
        unsigned rgb = 0;
        /*
         * `background` is a shorthand and may carry an image, a position and
         * more. Only a bare colour is taken: parsing the rest would be
         * parsing it wrong.
         */
        if (recon_css_colour(value, &rgb)) {
            style->background = rgb;
            style->has_background = true;
        }
        return;
    }

    if (strcasecmp(name, "font-weight") == 0) {
        if (strcasecmp(value, "bold") == 0 ||
                strcasecmp(value, "bolder") == 0) {
            style->weight = 700;
        } else if (strcasecmp(value, "normal") == 0 ||
                strcasecmp(value, "lighter") == 0) {
            style->weight = 400;
        } else if (isdigit((unsigned char)value[0])) {
            int n = atoi(value);
            style->weight = n >= 600 ? 700 : 400;
        }
        return;
    }

    if (strcasecmp(name, "font-style") == 0) {
        style->italic_set = true;
        style->italic = (strcasecmp(value, "italic") == 0 ||
            strcasecmp(value, "oblique") == 0);
        return;
    }

    if (strcasecmp(name, "font-family") == 0) {
        /* Anywhere in the list, because a page writes the fallbacks too and
         * "Consolas, monospace" is a request for a fixed pitch. */
        style->mono_set = true;
        style->mono = contains_fold(value, "monospace") ||
            contains_fold(value, "mono") ||
            contains_fold(value, "courier") ||
            contains_fold(value, "consol");
        return;
    }

    if (strcasecmp(name, "font-size") == 0) {
        /*
         * As a percentage of whatever size the element would otherwise have,
         * because that is the only thing a reader with one base size can act
         * on. A pixel value is turned into one against sixteen, which is the
         * size a browser's default text is and so the size a page written in
         * pixels was written against.
         */
        static const struct { const char *name; int percent; } WORDS[] = {
            { "xx-small", 60 }, { "x-small", 75 }, { "small", 88 },
            { "medium", 100 }, { "large", 120 }, { "x-large", 150 },
            { "xx-large", 200 }, { "smaller", 85 }, { "larger", 120 },
        };
        for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
            if (strcasecmp(value, WORDS[i].name) == 0) {
                style->size_percent = WORDS[i].percent;
                return;
            }
        }
        if (!isdigit((unsigned char)value[0]) && value[0] != '.') {
            return;
        }
        double n = atof(value);
        if (strstr(value, "%") != NULL) {
            style->size_percent = (int)n;
        } else if (contains_fold(value, "em") ||
                contains_fold(value, "rem")) {
            style->size_percent = (int)(n * 100.0);
        } else if (contains_fold(value, "px") || strchr(value, '.') != NULL
                || isdigit((unsigned char)value[0])) {
            style->size_percent = (int)(n * 100.0 / 16.0);
        }
        /* A size nobody could read, or one that would fill the window, is not
         * a size. Clamped rather than refused: the rest of the rule is fine. */
        if (style->size_percent > 0 && style->size_percent < 40) {
            style->size_percent = 40;
        }
        if (style->size_percent > 400) {
            style->size_percent = 400;
        }
        return;
    }

    if (strcasecmp(name, "text-align") == 0) {
        if (strcasecmp(value, "center") == 0 ||
                strcasecmp(value, "centre") == 0) {
            style->align = RECON_CSS_ALIGN_CENTRE;
        } else if (strcasecmp(value, "right") == 0) {
            style->align = RECON_CSS_ALIGN_RIGHT;
        } else if (strcasecmp(value, "left") == 0 ||
                strcasecmp(value, "start") == 0) {
            style->align = RECON_CSS_ALIGN_LEFT;
        }
        return;
    }
}

/*
 * A block of declarations, `a: b; c: d`.
 *
 * The braces are the caller's business; this reads what is between them.
 */
/*
 * Record a custom property, replacing one of the same name.
 *
 * Later wins, which is what the cascade does when specificity ties -- and
 * every rule this collects from is `:root`, `html` or `body`, so ties are the
 * usual case rather than the exception.
 */
static void set_custom(struct recon_css_sheet *sheet, const char *name,
        const char *value) {
    for (int i = 0; i < sheet->custom_count; i++) {
        if (strcmp(sheet->customs[i].name, name) == 0) {
            snprintf(sheet->customs[i].value,
                sizeof(sheet->customs[i].value), "%s", value);
            return;
        }
    }
    if (sheet->custom_count >= CUSTOMS_MAX) {
        return;
    }
    struct custom *c = &sheet->customs[sheet->custom_count++];
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->value, sizeof(c->value), "%s", value);
}

/*
 * Is this selector the page itself?
 *
 * `:root`, `html` and `body` -- and only those three exactly, so `.dark :root`
 * and `body.landing` do not contribute. A page-wide palette is the thing being
 * collected; a scoped one is a different question this does not answer.
 */
static bool is_page_selector(const char *text, size_t length) {
    while (length > 0 && isspace((unsigned char)*text)) {
        text++;
        length--;
    }
    while (length > 0 && isspace((unsigned char)text[length - 1])) {
        length--;
    }
    static const char *const PAGE[] = { ":root", "html", "body" };
    for (size_t i = 0; i < sizeof(PAGE) / sizeof(PAGE[0]); i++) {
        size_t n = strlen(PAGE[i]);
        if (n == length && strncasecmp(text, PAGE[i], n) == 0) {
            return true;
        }
    }
    return false;
}

static void read_declarations(const char *text, size_t length,
        struct recon_css_style *out, const struct recon_css_sheet *sheet,
        struct recon_css_sheet *palette) {
    size_t i = 0;
    while (i < length) {
        while (i < length && (isspace((unsigned char)text[i]) ||
                text[i] == ';')) {
            i++;
        }
        size_t name_start = i;
        while (i < length && text[i] != ':' && text[i] != ';' &&
                text[i] != '}') {
            i++;
        }
        if (i >= length || text[i] != ':') {
            /* No colon: not a declaration. Skip to the next one rather than
             * giving up on the block -- one broken line in a stylesheet is
             * normal and the rest of it is still worth having. */
            while (i < length && text[i] != ';') {
                i++;
            }
            continue;
        }

        size_t name_end = i;
        while (name_end > name_start &&
                isspace((unsigned char)text[name_end - 1])) {
            name_end--;
        }

        i++;                                   /* past the colon */
        size_t value_start = i;
        while (i < length && text[i] != ';') {
            i++;
        }
        size_t value_end = i;

        /* `!important` is read and then ignored: this has one cascade level,
         * so honouring it would mean implementing the other. Trimmed off so
         * it does not end up inside a colour. */
        const char *bang = NULL;
        for (size_t j = value_start; j + 10 <= value_end; j++) {
            if (text[j] == '!' && strncasecmp(text + j, "!important", 10) == 0) {
                bang = text + j;
                break;
            }
        }
        if (bang != NULL) {
            value_end = (size_t)(bang - text);
        }

        while (value_start < value_end &&
                isspace((unsigned char)text[value_start])) {
            value_start++;
        }
        while (value_end > value_start &&
                isspace((unsigned char)text[value_end - 1])) {
            value_end--;
        }

        char name[64];
        char value[192];
        if (name_end > name_start && value_end > value_start) {
            copy_name(name, sizeof(name), text + name_start,
                name_end - name_start);
            copy_name(value, sizeof(value), text + value_start,
                value_end - value_start);

            /*
             * A custom property is recorded, not applied. `--rt-bg` is not a
             * property this draws anything with; it is a name other
             * declarations refer to.
             */
            if (name[0] == '-' && name[1] == '-') {
                if (palette != NULL) {
                    set_custom(palette, name, value);
                }
                continue;
            }

            if (strstr(value, "var(") != NULL) {
                char resolved[192];
                resolve_vars(sheet, value, resolved, sizeof(resolved), 4);
                apply_declaration(out, name, resolved);
            } else {
                apply_declaration(out, name, value);
            }
        }
    }
}

void recon_css_inline(const char *declarations, size_t length,
        struct recon_css_style *out) {
    if (declarations == NULL || out == NULL) {
        return;
    }
    read_declarations(declarations, length, out, NULL, NULL);
}

/* --- Selectors --- */

/*
 * One compound selector, `div#main.wide`.
 *
 * Returns false for anything with a combinator this cannot honour or a
 * construct it does not read -- an attribute test, a pseudo-class, a
 * universal with something attached. A rule that fires when it should not is
 * worse than one that never fires, because the first hides text.
 */
static bool read_part(const char *text, size_t length, struct part *out,
        unsigned *specificity) {
    memset(out, 0, sizeof(*out));
    size_t i = 0;

    while (i < length) {
        char lead = text[i];

        if (lead == '*') {
            i++;
            continue;                          /* matches anything: no cost */
        }
        if (lead == '[' || lead == ':' || lead == '(' || lead == ')') {
            return false;                      /* not read, so not matched */
        }

        if (lead == '.' || lead == '#') {
            i++;
        }
        size_t start = i;
        while (i < length && name_char(text[i])) {
            i++;
        }
        if (i == start) {
            return false;                      /* a bare . or # */
        }

        if (lead == '#') {
            if (out->id[0] != '\0') {
                return false;
            }
            copy_name(out->id, sizeof(out->id), text + start, i - start);
            *specificity += 1u << 16;
        } else if (lead == '.') {
            char *slot = out->class_one[0] == '\0' ? out->class_one
                : (out->class_two[0] == '\0' ? out->class_two : NULL);
            if (slot == NULL) {
                return false;                  /* three classes: not held */
            }
            copy_name(slot, NAME_MAX_LEN, text + start, i - start);
            *specificity += 1u << 8;
        } else {
            if (out->tag[0] != '\0') {
                return false;
            }
            copy_name(out->tag, sizeof(out->tag), text + start, i - start);
            *specificity += 1u;
        }
    }
    return true;
}

/* One selector, which may be several compounds separated by whitespace. */
static bool read_selector(const char *text, size_t length, struct rule *out) {
    out->part_count = 0;
    out->specificity = 0;

    size_t i = 0;
    while (i < length) {
        while (i < length && isspace((unsigned char)text[i])) {
            i++;
        }
        if (i >= length) {
            break;
        }
        /* A combinator this cannot honour. The whole selector goes. */
        if (text[i] == '>' || text[i] == '+' || text[i] == '~') {
            return false;
        }

        size_t start = i;
        while (i < length && !isspace((unsigned char)text[i])) {
            i++;
        }
        if (out->part_count >= PARTS_MAX) {
            return false;
        }
        if (!read_part(text + start, i - start,
                &out->parts[out->part_count], &out->specificity)) {
            return false;
        }
        out->part_count++;
    }
    return out->part_count > 0;
}

/* --- Reading a sheet --- */

struct recon_css_sheet *recon_css_new(void) {
    struct recon_css_sheet *sheet = calloc(1, sizeof(*sheet));
    return sheet;
}

void recon_css_free(struct recon_css_sheet *sheet) {
    free(sheet);
}

int recon_css_rule_count(const struct recon_css_sheet *sheet) {
    return sheet != NULL ? sheet->count : 0;
}

/* Past a comment, if one starts here. */
static size_t skip_comment(const char *text, size_t length, size_t i) {
    if (i + 1 < length && text[i] == '/' && text[i + 1] == '*') {
        i += 2;
        while (i + 1 < length && !(text[i] == '*' && text[i + 1] == '/')) {
            i++;
        }
        i = (i + 1 < length) ? i + 2 : length;
    }
    return i;
}

/*
 * One walk over a stylesheet, in one of two modes.
 *
 * Collecting takes the palette and adds no rules; the second pass adds the
 * rules and resolves `var()` against what the first found. Two passes rather
 * than one because a sheet may say `background: var(--bg)` above the `:root`
 * that defines `--bg`, and a single pass would read the first as unset and
 * the answer would depend on the order somebody happened to write the file
 * in. The alternative -- keeping every value unresolved and resolving at
 * match time -- means storing the text of every declaration, which is the
 * whole sheet a second time.
 *
 * Both passes skip `@media` whole, exactly as before, so a dark-mode block
 * does not quietly become the page's palette.
 */
static void walk(struct recon_css_sheet *sheet, const char *text,
        size_t length, bool collecting) {
    size_t i = 0;
    while (i < length) {
        size_t was = i;
        i = skip_comment(text, length, i);
        if (i != was) {
            continue;
        }
        if (isspace((unsigned char)text[i])) {
            i++;
            continue;
        }

        /*
         * An at-rule. `@media` and friends are skipped whole -- their
         * condition needs a viewport this does not model, and applying a
         * print stylesheet to a screen is worse than applying neither.
         */
        if (text[i] == '@') {
            int depth = 0;
            bool opened = false;
            while (i < length) {
                if (text[i] == '{') {
                    depth++;
                    opened = true;
                } else if (text[i] == '}') {
                    depth--;
                    if (depth <= 0) {
                        i++;
                        break;
                    }
                } else if (text[i] == ';' && !opened) {
                    i++;
                    break;                     /* @import, @charset */
                }
                i++;
            }
            continue;
        }

        size_t selector_start = i;
        while (i < length && text[i] != '{' && text[i] != '}') {
            i = skip_comment(text, length, i);
            if (i < length && text[i] != '{' && text[i] != '}') {
                i++;
            }
        }
        if (i >= length || text[i] != '{') {
            break;                             /* a selector with no block */
        }
        size_t selector_end = i;
        i++;

        size_t body_start = i;
        while (i < length && text[i] != '}') {
            i++;
        }
        size_t body_end = i;
        if (i < length) {
            i++;
        }

        if (collecting) {
            /*
             * A selector list may put the palette on more than one of the
             * three, `html, body { --bg: ... }`, so each part is asked.
             */
            bool page = false;
            size_t from = selector_start;
            for (size_t j = selector_start; j <= selector_end; j++) {
                if (j != selector_end && text[j] != ',') {
                    continue;
                }
                if (is_page_selector(text + from, j - from)) {
                    page = true;
                }
                from = j + 1;
            }
            if (page) {
                struct recon_css_style ignored;
                memset(&ignored, 0, sizeof(ignored));
                read_declarations(text + body_start, body_end - body_start,
                    &ignored, sheet, sheet);
            }
            continue;
        }

        struct recon_css_style style;
        memset(&style, 0, sizeof(style));
        read_declarations(text + body_start, body_end - body_start, &style,
            sheet, NULL);

        /*
         * A selector list, `a, b, c`, is that many rules with one body. Split
         * here rather than stored as a list, because matching then has one
         * shape instead of two.
         */
        size_t part_start = selector_start;
        for (size_t j = selector_start; j <= selector_end; j++) {
            bool last = (j == selector_end);
            if (!last && text[j] != ',') {
                continue;
            }
            if (sheet->count < RULES_MAX) {
                struct rule *rule = &sheet->rules[sheet->count];
                memset(rule, 0, sizeof(*rule));
                if (read_selector(text + part_start, j - part_start, rule)) {
                    rule->style = style;
                    rule->index = sheet->next_index++;
                    sheet->count++;
                }
            }
            part_start = j + 1;
        }
    }
}

bool recon_css_add(struct recon_css_sheet *sheet, const char *text,
        size_t length) {
    if (sheet == NULL || text == NULL) {
        return false;
    }
    walk(sheet, text, length, true);
    walk(sheet, text, length, false);
    return true;
}

/* --- Matching --- */

/* Is `want` one of the space-separated names in `list`? */
static bool has_class(const char *list, const char *want) {
    if (list == NULL || want == NULL || want[0] == '\0') {
        return false;
    }
    size_t want_length = strlen(want);
    const char *at = list;
    while (*at != '\0') {
        while (*at == ' ' || *at == '\t' || *at == '\n') {
            at++;
        }
        const char *start = at;
        while (*at != '\0' && *at != ' ' && *at != '\t' && *at != '\n') {
            at++;
        }
        if ((size_t)(at - start) == want_length &&
                strncasecmp(start, want, want_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool part_matches(const struct part *p,
        const struct recon_css_element *e) {
    if (p->tag[0] != '\0' && (e->tag == NULL ||
            strcasecmp(p->tag, e->tag) != 0)) {
        return false;
    }
    if (p->id[0] != '\0' && (e->id == NULL ||
            strcasecmp(p->id, e->id) != 0)) {
        return false;
    }
    if (p->class_one[0] != '\0' && !has_class(e->classes, p->class_one)) {
        return false;
    }
    if (p->class_two[0] != '\0' && !has_class(e->classes, p->class_two)) {
        return false;
    }
    return true;
}

/*
 * Does this rule match the element at the end of the stack?
 *
 * The last part must match the element itself; the parts before it must match
 * ancestors, in order, but not necessarily adjacent ones -- which is what a
 * descendant combinator means. Walked from the end backwards, which is how
 * every implementation does it and is why: the last part fails for almost
 * every rule, so testing it first is the whole of the performance.
 */
static bool rule_matches(const struct rule *r,
        const struct recon_css_element *stack, int depth) {
    if (depth <= 0 || r->part_count > depth) {
        return false;
    }
    if (!part_matches(&r->parts[r->part_count - 1], &stack[depth - 1])) {
        return false;
    }

    int at = depth - 2;
    for (int p = r->part_count - 2; p >= 0; p--) {
        bool found = false;
        while (at >= 0) {
            if (part_matches(&r->parts[p], &stack[at--])) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

/* Fold one style over another, taking only what the newer one mentions. */
static void merge(struct recon_css_style *out,
        const struct recon_css_style *in) {
    if (in->has_colour) {
        out->colour = in->colour;
        out->has_colour = true;
    }
    if (in->has_background) {
        out->background = in->background;
        out->has_background = true;
    }
    if (in->weight != 0) {
        out->weight = in->weight;
    }
    if (in->italic_set) {
        out->italic_set = true;
        out->italic = in->italic;
    }
    if (in->mono_set) {
        out->mono_set = true;
        out->mono = in->mono;
    }
    if (in->size_percent != 0) {
        out->size_percent = in->size_percent;
    }
    if (in->align != RECON_CSS_ALIGN_NONE) {
        out->align = in->align;
    }
    if (in->display != 0) {
        out->display = in->display;
    }
}

void recon_css_match(const struct recon_css_sheet *sheet,
        const struct recon_css_element *stack, int depth,
        struct recon_css_style *out) {
    if (sheet == NULL || stack == NULL || out == NULL || depth <= 0) {
        return;
    }

    /*
     * Applied in order of specificity, then file order, so the last thing
     * merged is the winner. An insertion pass rather than a sort: the matches
     * for one element are a handful out of thousands, and sorting the
     * thousands to find them would be the wrong way round.
     */
    for (unsigned pass = 0; pass < 2; pass++) {
        /*
         * Two passes, low specificity then high, rather than a full ordering.
         * Within a pass, file order. That is exact for the overwhelmingly
         * common case -- a type rule then a class rule overriding it -- and
         * wrong only where two rules of *different* specificity within the
         * same pass disagree, where it falls back to file order, which is
         * what a stylesheet author usually meant anyway.
         */
        for (int i = 0; i < sheet->count; i++) {
            const struct rule *r = &sheet->rules[i];
            bool high = r->specificity >= (1u << 8);
            if ((pass == 0) == high) {
                continue;
            }
            if (rule_matches(r, stack, depth)) {
                merge(out, &r->style);
            }
        }
    }
}
