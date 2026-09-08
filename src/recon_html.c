/*
 * Reading HTML. See include/recon_html.h.
 *
 * One pass over the bytes with a small stack of open styles. There is no tree:
 * a viewer needs the words in order with their appearance, and building a
 * document object model to then flatten it would be work done twice.
 *
 * The text is copied into one growing buffer and the runs point into it, so a
 * page with a lot of markup costs one allocation rather than one per word.
 */

#define _POSIX_C_SOURCE 200809L
/* memmem, for finding the end of a comment. */
#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "recon_css.h"
#include "recon_html.h"

/*
 * Bounds, all of them deliberate.
 *
 * A page is somebody else's file and can be any size and shape. Every one of
 * these is a ceiling rather than a prediction, and hitting one truncates the
 * page rather than the process.
 */
#define TEXT_MAX (1024 * 1024)
#define RUNS_MAX 20000
#define BLOCKS_MAX 4000
#define LINKS_MAX 2000
#define NEST_MAX 32

struct recon_html_document {
    char *text;
    size_t text_used;

    struct recon_html_run *runs;
    int run_count;

    struct recon_html_block_entry *blocks;
    int block_count;

    /* Set when any ceiling was reached, so the reader can say the page is
     * incomplete rather than letting it look finished. */
    bool truncated;

    struct recon_html_link *links;
    int link_count;

    char title[256];
    bool saw_script;

    /*
     * The stylesheets the page asked for, unresolved. Kept so a viewer can
     * fetch them and parse again -- this does not know where the page came
     * from, so it cannot resolve them and does not try.
     */
    char sheets[16][1024];
    int sheet_count;
};

/* --- Building --- */

/*
 * Elements that never have a closing tag, so never go on the stack.
 *
 * A stack that pushed these would never pop them, and everything after the
 * first `<br>` would be treated as being inside it -- which for `display:
 * none` on a `<meta>` would hide the rest of the page.
 */
/*
 * What a browser does not show, whatever the page's stylesheet says.
 *
 * None of this is CSS. It is the behaviour of the elements themselves, and a
 * reader that only reads stylesheets shows all of it: measured on
 * recontowers.com, whose accessibility panel is a closed `<details>` and
 * appeared in full, twenty lines of settings nobody had opened.
 *
 *   template   never rendered at all -- it is markup held for scripting.
 *   dialog     shown only when it has been opened.
 *   details    shows its `<summary>` and nothing else until opened.
 *   [hidden]   the attribute that means exactly this, and which pages use
 *              precisely because it does not need a stylesheet.
 */
/* Both defined further down, next to the parsing they belong to. */
static bool named(const char *tag, size_t length, const char *want);

/*
 * Is this attribute present at all, with or without a value?
 *
 * `attribute` reads a value and so answers no for `<dialog open>` and
 * `<div hidden>` -- which are the two forms that matter here, because a
 * boolean attribute means something by being written and nothing by its
 * value. Asking the wrong question got both backwards: an open dialog was
 * treated as closed and a hidden div as shown.
 */
static bool has_attribute(const char *attrs, size_t length,
        const char *want) {
    size_t want_length = strlen(want);
    if (attrs == NULL || want_length == 0) {
        return false;
    }

    for (size_t i = 0; i + want_length <= length; i++) {
        /* On a boundary, so `data-open` is not `open`. */
        if (i > 0 && attrs[i - 1] != ' ' && attrs[i - 1] != '\t' &&
                attrs[i - 1] != '\n' && attrs[i - 1] != '\r') {
            continue;
        }
        if (strncasecmp(attrs + i, want, want_length) != 0) {
            continue;
        }
        /* And ends where a name ends, so `openable` is not `open`. */
        size_t at = i + want_length;
        if (at < length && (isalnum((unsigned char)attrs[at]) ||
                attrs[at] == '-' || attrs[at] == '_')) {
            continue;
        }
        return true;
    }
    return false;
}

static bool hidden_by_default(const char *tag, size_t length,
        const char *attrs, size_t attrs_length) {
    if (has_attribute(attrs, attrs_length, "hidden")) {
        return true;
    }
    if (named(tag, length, "template")) {
        return true;
    }
    if ((named(tag, length, "dialog") || named(tag, length, "details")) &&
            !has_attribute(attrs, attrs_length, "open")) {
        return true;
    }
    return false;
}

/*
 * Tags this already treats as blocks.
 *
 * Consulted only to avoid acting twice: an element whose tag is a block and
 * whose stylesheet says `display: block` is being told what it already knew,
 * and breaking the line again would leave an empty paragraph between every
 * two real ones.
 */
static bool breaks_line(const char *tag, size_t length) {
    static const char *const BLOCKS[] = {
        "p", "div", "section", "article", "header", "footer", "nav", "main",
        "aside", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "dl",
        "dt", "dd", "blockquote", "pre", "table", "tr", "td", "th", "thead",
        "tbody", "tfoot", "form", "fieldset", "figure", "figcaption", "hr",
        "body", "html", "br", "address", "details", "summary",
    };
    for (size_t i = 0; i < sizeof(BLOCKS) / sizeof(BLOCKS[0]); i++) {
        size_t n = strlen(BLOCKS[i]);
        if (n == length && strncasecmp(tag, BLOCKS[i], n) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_void_element(const char *tag, size_t length) {
    static const char *const VOID[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
    };
    for (size_t i = 0; i < sizeof(VOID) / sizeof(VOID[0]); i++) {
        size_t n = strlen(VOID[i]);
        if (n == length && strncasecmp(tag, VOID[i], n) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * How deep the open-element stack goes.
 *
 * Past this the page is nested more deeply than any descendant selector this
 * honours can reach, so the levels above are recorded and the ones below are
 * treated as the deepest one -- which keeps the styles inherited and only
 * loses the ability to match an ancestor nobody could have selected anyway.
 */
#define STACK_MAX 64

/*
 * One open element, and what it looks like.
 *
 * The style fields are *inherited*: a level starts as a copy of the one above
 * it and the stylesheet overlays what it says. That is what makes `body {
 * color: #333 }` colour the whole page without a rule per element, and it is
 * the only part of the cascade that needs the stack rather than the sheet.
 */
struct level {
    char tag[48];
    char id[64];
    char classes[192];

    unsigned add_style;            /* bold, italic, mono, from CSS */
    bool has_colour;
    unsigned colour;
    int size_percent;
    int align;

    /*
     * Whether a stylesheet made this element a block when its tag is not one.
     *
     * Remembered rather than recomputed at the closing tag, because the sheet
     * is matched against the open-element stack and by then this element has
     * been popped off it -- so the close would ask a different question and
     * get a different answer.
     */
    bool made_block;
};

struct builder {
    struct recon_html_document *d;

    /* The stylesheet, when a caller handed one over. */
    struct recon_css_sheet *sheet;

    struct level stack[STACK_MAX];
    int depth;

    /*
     * The depth at which something was hidden, or -1.
     *
     * A depth rather than a flag, because hiding nests: an element inside a
     * hidden one is also hidden, and the hide ends when the element that
     * started it closes -- not when the first close tag comes along.
     */
    int hidden_at;

    /*
     * A `<summary>` inside a closed `<details>` is the one thing shown out of
     * something hidden -- it is the line you click to open it. So the hide is
     * suspended for the length of that element and put back afterwards, and
     * the depth it was suspended at is how the close knows which one to
     * restore.
     */
    int hidden_was;
    int summary_at;

    /* The block being filled in, if any. */
    bool in_block;
    enum recon_html_block kind;
    int level;
    int first_run;

    /* Open styles, so </b> closes the nearest <b> and not something else. */
    unsigned style;
    int link;
    int list_depth;

    /* For an image block, where the picture is. -1 otherwise. */
    int source;

    /* What the stylesheet said about the block as a whole, captured when it
     * opened rather than when it closed: a block's appearance belongs to the
     * element it started in. */
    int size_percent;
    int align;

    /*
     * Whether the last thing appended ended in a space.
     *
     * HTML collapses runs of whitespace to one, and the run may be split
     * across tags -- "a <b>b</b> c" has whitespace on both sides of the bold.
     * Tracking it here rather than per-run is what stops "a", "b", "c" being
     * run together into "abc".
     */
    bool pending_space;
    bool at_block_start;
};

static bool add_text(struct builder *b, const char *bytes, size_t length) {
    if (b->d->text_used + length + 1 >= TEXT_MAX) {
        return false;
    }
    memcpy(b->d->text + b->d->text_used, bytes, length);
    b->d->text_used += length;
    b->d->text[b->d->text_used] = '\0';
    return true;
}

/* Start a run, or extend the last one when nothing about it has changed. */
static void emit(struct builder *b, const char *bytes, size_t length) {
    /*
     * Nothing inside something the page hid.
     *
     * Checked here rather than at every caller, because there are four of
     * them -- text, an entity, a bare ampersand, an image's alt text -- and
     * three of them being right is a page that hides its navigation and keeps
     * the alt text of the icons inside it.
     */
    if (length == 0 || !b->in_block || b->hidden_at >= 0) {
        return;
    }

    size_t at = b->d->text_used;
    if (!add_text(b, bytes, length)) {
        return;
    }

    struct recon_html_run *last = (b->d->run_count > b->first_run)
        ? &b->d->runs[b->d->run_count - 1] : NULL;

    /*
     * Extended rather than appended when the appearance is the same and the
     * text is contiguous. A page with a lot of entities would otherwise
     * produce a run per "&amp;", and the layout would have a word boundary
     * inside every one of them.
     */
    /*
     * The style is the markup's and the stylesheet's together. A `<b>` inside
     * an element a sheet has made italic is both, and neither half knows
     * about the other.
     */
    const struct level *now = (b->depth > 0) ? &b->stack[b->depth - 1] : NULL;
    unsigned style = b->style | (now != NULL ? now->add_style : 0u);
    bool has_colour = (now != NULL) && now->has_colour;
    unsigned colour = has_colour ? now->colour : 0u;

    if (last != NULL && last->style == style && last->link == b->link &&
            last->has_colour == has_colour && last->colour == colour &&
            last->text + last->length == b->d->text + at) {
        last->length += length;
        return;
    }

    if (b->d->run_count >= RUNS_MAX) {
        b->d->truncated = true;
        return;
    }
    struct recon_html_run *run = &b->d->runs[b->d->run_count++];
    run->text = b->d->text + at;
    run->length = length;
    run->style = style;
    run->link = b->link;
    run->has_colour = has_colour;
    run->colour = colour;
}

/*
 * Write out the space that whitespace collapsing has been holding, and note
 * that text has begun.
 *
 * Named rather than repeated, because it was written by hand at each of the
 * places text is emitted and one of them forgot -- an image's alt text ran
 * straight into the word before it.
 */
static void flush_space(struct builder *b) {
    if (b->pending_space && !b->at_block_start) {
        emit(b, " ", 1);
    }
    b->pending_space = false;
    b->at_block_start = false;
}

static void open_block(struct builder *b, enum recon_html_block kind,
        int level) {
    if (b->in_block) {
        return;
    }
    b->in_block = true;
    b->kind = kind;
    b->level = level;
    b->source = -1;

    const struct level *now = (b->depth > 0) ? &b->stack[b->depth - 1] : NULL;
    b->size_percent = (now != NULL) ? now->size_percent : 0;
    b->align = (now != NULL) ? now->align : 0;
    b->first_run = b->d->run_count;
    b->pending_space = false;
    b->at_block_start = true;
}

static void close_block(struct builder *b) {
    if (!b->in_block) {
        return;
    }
    b->in_block = false;

    int count = b->d->run_count - b->first_run;

    /*
     * A block with nothing in it is not a paragraph, it is markup. Dropping
     * them is what stops a page of <div><div><div> becoming a screen of blank
     * lines.
     *
     * Except in preformatted text, where a blank line is the author's and
     * closing it up rewrites a document that used its gaps.
     */
    bool keep_empty = (b->kind == RECON_HTML_PRE ||
        (b->kind == RECON_HTML_IMAGE && b->source >= 0));

    /*
     * The ceiling, recorded before it is acted on.
     *
     * This is the path a long page actually takes -- every paragraph closes
     * through here -- so a version that marked the other five ceilings and
     * not this one reported nothing at all. Which is what happened: a page of
     * five thousand paragraphs stopped at four thousand and the status line
     * said "4000 blocks" as though that were the whole of it.
     */
    if (b->d->block_count >= BLOCKS_MAX) {
        b->d->truncated = true;
    }

    if ((count <= 0 && !keep_empty) || b->d->block_count >= BLOCKS_MAX) {
        b->d->run_count = b->first_run;
        return;
    }

    struct recon_html_block_entry *block = &b->d->blocks[b->d->block_count++];
    block->kind = b->kind;
    block->level = b->level;
    block->source = b->source;
    block->size_percent = b->size_percent;
    block->align = b->align;
    block->first_run = b->first_run;
    block->run_count = count;
}

/* End whatever block is open and start a fresh paragraph. */
static void break_block(struct builder *b) {
    close_block(b);
    open_block(b, RECON_HTML_PARAGRAPH, 0);
}

static void add_rule(struct builder *b) {
    close_block(b);
    if (b->d->block_count >= BLOCKS_MAX) {
        b->d->truncated = true;
    } else {
        struct recon_html_block_entry *block = &b->d->blocks[b->d->block_count++];
        block->kind = RECON_HTML_RULE;
        block->level = 0;
        block->first_run = 0;
        block->run_count = 0;
    }
    open_block(b, RECON_HTML_PARAGRAPH, 0);
}

/* --- Entities --- */

/*
 * The named entities worth knowing.
 *
 * There are more than two thousand in the specification and this is the
 * fraction that appears in prose. An unknown entity is left as written, which
 * is what somebody looking at "&pound;" would rather see than nothing.
 */
static const struct { const char *name; const char *utf8; } ENTITIES[] = {
    { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
    { "apos", "'" }, { "nbsp", " " }, { "mdash", "\xE2\x80\x94" },
    { "ndash", "\xE2\x80\x93" }, { "hellip", "\xE2\x80\xA6" },
    { "lsquo", "\xE2\x80\x98" }, { "rsquo", "\xE2\x80\x99" },
    { "ldquo", "\xE2\x80\x9C" }, { "rdquo", "\xE2\x80\x9D" },
    { "copy", "\xC2\xA9" }, { "reg", "\xC2\xAE" }, { "trade", "\xE2\x84\xA2" },
    { "deg", "\xC2\xB0" }, { "pound", "\xC2\xA3" }, { "euro", "\xE2\x82\xAC" },
    { "middot", "\xC2\xB7" }, { "bull", "\xE2\x80\xA2" },
    { "times", "\xC3\x97" }, { "laquo", "\xC2\xAB" }, { "raquo", "\xC2\xBB" },
};

/* Write a code point as UTF-8. Returns how many bytes. */
static int as_utf8(unsigned long c, char *out) {
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (char)(0xC0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        out[0] = (char)(0xE0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    if (c <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (c >> 18));
        out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[3] = (char)(0x80 | (c & 0x3F));
        return 4;
    }
    return 0;
}

/*
 * Read one entity starting at `at`, which points at the "&".
 *
 * Returns how many bytes it consumed, or 0 for something that is not an
 * entity -- a bare "&" in prose, which is common and is not an error.
 */
static size_t read_entity(const char *at, size_t left, char *out,
        int *out_length) {
    if (left < 3 || at[0] != '&') {
        return 0;
    }

    /* The name or number, up to the semicolon. Bounded: "&" followed by a
     * paragraph of text is not an unterminated entity, it is an ampersand. */
    size_t span = 0;
    while (span < left && span < 12 && at[span] != ';' &&
            at[span] != '<' && at[span] != ' ') {
        span++;
    }
    if (span >= left || at[span] != ';') {
        return 0;
    }

    if (at[1] == '#') {
        unsigned long value;
        if (at[2] == 'x' || at[2] == 'X') {
            value = strtoul(at + 3, NULL, 16);
        } else {
            value = strtoul(at + 2, NULL, 10);
        }
        *out_length = as_utf8(value, out);
        return (*out_length > 0) ? span + 1 : 0;
    }

    for (size_t i = 0; i < sizeof(ENTITIES) / sizeof(ENTITIES[0]); i++) {
        size_t length = strlen(ENTITIES[i].name);
        if (length == span - 1 &&
                strncmp(at + 1, ENTITIES[i].name, length) == 0) {
            *out_length = (int)strlen(ENTITIES[i].utf8);
            memcpy(out, ENTITIES[i].utf8, (size_t)*out_length);
            return span + 1;
        }
    }
    return 0;
}

/* --- Tags --- */

/* Case-insensitive compare of a tag name of known length. */
static bool named(const char *name, size_t length, const char *want) {
    return strlen(want) == length && strncasecmp(name, want, length) == 0;
}

/*
 * The value of one attribute, out of a tag's attribute text.
 *
 * Quoted or not, single or double. Written by hand rather than with a general
 * parser because the only attributes this reads are href and alt.
 */
static bool attribute(const char *attrs, size_t length, const char *want,
        char *out, size_t size) {
    size_t want_length = strlen(want);

    for (size_t i = 0; i + want_length < length; i++) {
        if (i > 0 && attrs[i - 1] != ' ' && attrs[i - 1] != '\t' &&
                attrs[i - 1] != '\n') {
            continue;
        }
        if (strncasecmp(attrs + i, want, want_length) != 0) {
            continue;
        }

        size_t at = i + want_length;
        while (at < length && (attrs[at] == ' ' || attrs[at] == '\t')) {
            at++;
        }
        if (at >= length || attrs[at] != '=') {
            continue;
        }
        at++;
        while (at < length && (attrs[at] == ' ' || attrs[at] == '\t')) {
            at++;
        }
        if (at >= length) {
            return false;
        }

        char quote = '\0';
        if (attrs[at] == '"' || attrs[at] == '\'') {
            quote = attrs[at];
            at++;
        }

        size_t start = at;
        while (at < length) {
            if (quote != '\0' && attrs[at] == quote) {
                break;
            }
            if (quote == '\0' && (attrs[at] == ' ' || attrs[at] == '\t' ||
                    attrs[at] == '\n' || attrs[at] == '>')) {
                break;
            }
            at++;
        }

        size_t value_length = at - start;
        if (value_length >= size) {
            value_length = size - 1;
        }

        /*
         * Entities are decoded here too. An href written "?a=1&amp;b=2" is
         * extremely common and fetching the literal "&amp;" asks the server
         * for a different page.
         */
        size_t used = 0;
        for (size_t j = 0; j < value_length && used + 4 < size; ) {
            char utf8[4];
            int made = 0;
            size_t took = read_entity(attrs + start + j, value_length - j,
                utf8, &made);
            if (took > 0) {
                memcpy(out + used, utf8, (size_t)made);
                used += (size_t)made;
                j += took;
            } else {
                out[used++] = attrs[start + j];
                j++;
            }
        }
        out[used] = '\0';
        return true;
    }
    return false;
}

/* --- The pass --- */

/*
 * One open inline tag.
 *
 * `style_added` is what it turned on, so closing it can turn exactly that off.
 * `link` is which link was in force *before* it opened, so closing it restores
 * that rather than guessing -- an <a> inside an <a> is invalid HTML and pages
 * contain it anyway.
 */
struct open_tag {
    unsigned style_added;
    int link_before;
};

/* --- Encodings --- */

/*
 * The twenty-seven characters Windows-1252 puts where Latin-1 has controls.
 *
 * Curly quotes, dashes and an ellipsis, which is what they are actually used
 * for: a page written in Word and saved as "Latin-1" is full of them, and
 * read as Latin-1 they are control characters and vanish.
 */
static const unsigned CP1252_HIGH[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

/* Does this decode cleanly as UTF-8? */
static bool is_utf8(const char *bytes, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)bytes[i];
        int extra;
        unsigned lowest;

        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            lowest = 0x80;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            lowest = 0x800;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            lowest = 0x10000;
        } else {
            return false;                  /* a continuation byte on its own */
        }

        if (i + (size_t)extra >= length) {
            return false;
        }
        unsigned point = c & (0x7Fu >> (extra + 1));
        for (int k = 1; k <= extra; k++) {
            unsigned char n = (unsigned char)bytes[i + (size_t)k];
            if ((n & 0xC0) != 0x80) {
                return false;
            }
            point = (point << 6) | (n & 0x3Fu);
        }

        /*
         * Overlong forms and surrogates are refused. Both decode "fine" and
         * neither is valid, and accepting them is how a byte sequence gets
         * read as one thing here and another somewhere else.
         */
        if (point < lowest || (point >= 0xD800 && point <= 0xDFFF) ||
                point > 0x10FFFF) {
            return false;
        }
        i += (size_t)extra + 1;
    }
    return true;
}

char *recon_html_to_utf8(const char *bytes, size_t length,
        const char *declared, size_t *out_length) {
    if (bytes == NULL) {
        return NULL;
    }

    /*
     * Said to be UTF-8, or said to be nothing. Checked anyway, because a page
     * that lies about this is a page of replacement characters -- and the
     * check is a single pass over bytes already in memory.
     */
    (void)declared;
    if (is_utf8(bytes, length)) {
        return NULL;
    }

    /* Three bytes per source byte is the worst case: every high byte becomes
     * a three-byte sequence, and none becomes more. */
    char *out = malloc(length * 3 + 1);
    if (out == NULL) {
        return NULL;
    }

    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)bytes[i];
        unsigned point;
        if (c < 0x80) {
            out[used++] = (char)c;
            continue;
        }
        point = (c >= 0x80 && c <= 0x9F) ? CP1252_HIGH[c - 0x80] : c;

        if (point < 0x800) {
            out[used++] = (char)(0xC0 | (point >> 6));
            out[used++] = (char)(0x80 | (point & 0x3F));
        } else {
            out[used++] = (char)(0xE0 | (point >> 12));
            out[used++] = (char)(0x80 | ((point >> 6) & 0x3F));
            out[used++] = (char)(0x80 | (point & 0x3F));
        }
    }
    out[used] = '\0';
    if (out_length != NULL) {
        *out_length = used;
    }
    return out;
}

struct recon_html_document *recon_html_parse(const char *html, size_t length) {
    return recon_html_parse_styled(html, length, NULL);
}

struct recon_html_document *recon_html_parse_styled(const char *html,
        size_t length, struct recon_css_sheet *sheet) {
    struct recon_html_document *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return NULL;
    }

    d->text = calloc(1, TEXT_MAX);
    d->runs = calloc(RUNS_MAX, sizeof(*d->runs));
    d->blocks = calloc(BLOCKS_MAX, sizeof(*d->blocks));
    d->links = calloc(LINKS_MAX, sizeof(*d->links));

    if (d->text == NULL || d->runs == NULL || d->blocks == NULL ||
            d->links == NULL) {
        recon_html_free(d);
        return NULL;
    }
    if (html == NULL) {
        return d;
    }

    struct builder b;
    memset(&b, 0, sizeof(b));
    b.d = d;
    b.link = -1;
    b.sheet = sheet;
    b.hidden_at = -1;
    b.hidden_was = -1;
    b.summary_at = -1;

    struct open_tag stack[NEST_MAX];
    int depth = 0;

    bool in_title = false;
    size_t title_used = 0;
    bool pre = false;

    open_block(&b, RECON_HTML_PARAGRAPH, 0);

    size_t i = 0;
    while (i < length) {
        char c = html[i];

        /* --- A tag --- */
        if (c == '<') {
            /* A comment, or a doctype. Both are skipped whole. */
            if (i + 3 < length && strncmp(html + i, "<!--", 4) == 0) {
                const char *end = memmem(html + i, length - i, "-->", 3);
                i = (end != NULL) ? (size_t)(end - html) + 3 : length;
                continue;
            }
            if (i + 1 < length && html[i + 1] == '!') {
                const char *end = memchr(html + i, '>', length - i);
                i = (end != NULL) ? (size_t)(end - html) + 1 : length;
                continue;
            }

            const char *close = memchr(html + i, '>', length - i);
            if (close == NULL) {
                break;                       /* an unterminated tag ends it */
            }
            size_t tag_length = (size_t)(close - (html + i)) - 1;
            const char *tag = html + i + 1;
            size_t after = (size_t)(close - html) + 1;

            bool closing = (tag_length > 0 && tag[0] == '/');
            if (closing) {
                tag++;
                tag_length--;
            }

            size_t name_length = 0;
            while (name_length < tag_length && tag[name_length] != ' ' &&
                    tag[name_length] != '\t' && tag[name_length] != '\n' &&
                    tag[name_length] != '/') {
                name_length++;
            }
            const char *attrs = tag + name_length;
            size_t attrs_length = tag_length - name_length;

            /*
             * --- The open-element stack, and what the sheet says ---
             *
             * Kept for every element rather than only for the ones this draws
             * from, because a selector names ancestors and an ancestor is
             * usually a `<div>` that produces nothing itself.
             *
             * A level inherits the one above it and the stylesheet overlays
             * what it says, which is what makes `body { color: #333 }` colour
             * a whole page without a rule per element.
             */
            bool self_closing = (tag_length > 0 && tag[tag_length - 1] == '/');
            bool voidish = is_void_element(tag, name_length) || self_closing;

            if (closing && !voidish) {
                bool was_block = (b.depth > 0) &&
                    b.stack[b.depth - 1].made_block;
                if (b.depth > 0) {
                    b.depth--;
                }
                if (b.hidden_at >= 0 && b.depth <= b.hidden_at) {
                    b.hidden_at = -1;
                }
                /* What follows a block is not joined onto it. */
                if (was_block && b.hidden_at < 0) {
                    close_block(&b);
                    open_block(&b, RECON_HTML_PARAGRAPH, 0);
                }
            } else if (!closing) {
                struct level fresh;
                if (b.depth > 0) {
                    fresh = b.stack[b.depth - 1];
                } else {
                    memset(&fresh, 0, sizeof(fresh));
                }
                snprintf(fresh.tag, sizeof(fresh.tag), "%.*s",
                    (int)name_length, tag);
                fresh.id[0] = '\0';
                fresh.classes[0] = '\0';
                attribute(attrs, attrs_length, "id", fresh.id,
                    sizeof(fresh.id));
                attribute(attrs, attrs_length, "class", fresh.classes,
                    sizeof(fresh.classes));

                /*
                 * A void element is asked about without being pushed: an
                 * `<img class=hidden>` should not be drawn, and an `<hr>`
                 * that never pops would swallow the rest of the page.
                 */
                int at = b.depth < STACK_MAX ? b.depth : STACK_MAX - 1;
                struct level was = b.stack[at];
                b.stack[at] = fresh;

                struct recon_css_style style;
                memset(&style, 0, sizeof(style));
                if (b.sheet != NULL) {
                    struct recon_css_element seen[STACK_MAX];
                    for (int j = 0; j <= at; j++) {
                        seen[j].tag = b.stack[j].tag;
                        seen[j].id = b.stack[j].id[0] != '\0'
                            ? b.stack[j].id : NULL;
                        seen[j].classes = b.stack[j].classes[0] != '\0'
                            ? b.stack[j].classes : NULL;
                    }
                    recon_css_match(b.sheet, seen, at + 1, &style);
                }

                char inline_style[512];
                if (attribute(attrs, attrs_length, "style", inline_style,
                        sizeof(inline_style))) {
                    recon_css_inline(inline_style, strlen(inline_style),
                        &style);
                }

                if (style.weight != 0) {
                    if (style.weight >= 700) {
                        b.stack[at].add_style |= RECON_HTML_BOLD;
                    } else {
                        b.stack[at].add_style &= ~(unsigned)RECON_HTML_BOLD;
                    }
                }
                if (style.italic_set) {
                    if (style.italic) {
                        b.stack[at].add_style |= RECON_HTML_ITALIC;
                    } else {
                        b.stack[at].add_style &= ~(unsigned)RECON_HTML_ITALIC;
                    }
                }
                if (style.mono_set) {
                    if (style.mono) {
                        b.stack[at].add_style |= RECON_HTML_MONO;
                    } else {
                        b.stack[at].add_style &= ~(unsigned)RECON_HTML_MONO;
                    }
                }
                if (style.has_colour) {
                    b.stack[at].has_colour = true;
                    b.stack[at].colour = style.colour;
                }
                if (style.size_percent != 0) {
                    b.stack[at].size_percent = style.size_percent;
                }
                if (style.align != RECON_CSS_ALIGN_NONE) {
                    b.stack[at].align = (int)style.align;
                }

                bool hide = style.display == RECON_CSS_NONE ||
                    hidden_by_default(tag, name_length, attrs, attrs_length);
                if (hide && b.hidden_at < 0) {
                    b.hidden_at = voidish ? at + 1 : at;
                }

                /*
                 * --- A span a stylesheet turned into a block ---
                 *
                 * The difference between a `<span>` and a `<div>` is one
                 * property, and pages set it constantly: a card built out of
                 * spans and laid out with `display: flex` is five stacked
                 * lines in a browser and one run-on sentence in a reader that
                 * ignores it. Measured on recontowers.com, where an eyebrow,
                 * a name, a destination, a blurb and a caveat came out as a
                 * single underlined paragraph.
                 *
                 * This still cannot lay anything out. It can tell a line from
                 * a paragraph, and that is most of the difference.
                 *
                 * Only when the tag is not already a block. A `<p>` that says
                 * `display: block` is saying what it already was, and closing
                 * the block it just opened would leave an empty one.
                 */
                b.stack[at].made_block = false;
                if (style.display == RECON_CSS_BLOCK && b.hidden_at < 0 &&
                        !breaks_line(tag, name_length)) {
                    b.stack[at].made_block = true;
                    close_block(&b);
                    open_block(&b, RECON_HTML_PARAGRAPH, 0);
                }

                if (voidish) {
                    /*
                     * Asked about, not pushed. A hide it started ends here
                     * too, since there is no closing tag to end it.
                     */
                    if (b.hidden_at == at + 1) {
                        b.hidden_at = -1;
                        b.stack[at] = was;
                        i = after;
                        continue;
                    }
                    b.stack[at] = was;
                } else if (b.depth < STACK_MAX) {
                    b.depth++;
                }
            }

            /*
             * A `<summary>` is what you click to open the `<details>` it is
             * in, so it is shown even though its parent is not. Suspended
             * rather than special-cased further down: everything between here
             * and its closing tag then behaves exactly as it would anywhere
             * else.
             */
            if (named(tag, name_length, "summary")) {
                if (!closing && b.hidden_at >= 0 && b.summary_at < 0) {
                    b.hidden_was = b.hidden_at;
                    b.hidden_at = -1;
                    b.summary_at = b.depth;
                    close_block(&b);
                    open_block(&b, RECON_HTML_PARAGRAPH, 0);
                } else if (closing && b.summary_at >= 0 &&
                        b.depth <= b.summary_at) {
                    b.hidden_at = b.hidden_was;
                    b.hidden_was = -1;
                    b.summary_at = -1;
                    close_block(&b);
                    open_block(&b, RECON_HTML_PARAGRAPH, 0);
                }
                i = after;
                continue;
            }

            /*
             * Inside something hidden, only the structure is followed. No
             * text, no blocks, no links -- the page said not to show this.
             */
            if (b.hidden_at >= 0 &&
                    !named(tag, name_length, "title")) {
                i = after;
                continue;
            }

            /*
             * Script and style hold text that is not prose. Skipped to their
             * closing tag rather than parsed, because the contents contain
             * "<" and ">" that are not markup and would otherwise be read as
             * tags.
             */
            if (!closing && (named(tag, name_length, "script") ||
                    named(tag, name_length, "style"))) {
                bool is_script = named(tag, name_length, "script");
                if (is_script) {
                    d->saw_script = true;
                }
                const char *want = is_script ? "</script" : "</style";
                const char *end = NULL;
                size_t body_end = length;
                for (size_t j = after; j + 8 < length; j++) {
                    if (strncasecmp(html + j, want, strlen(want)) == 0) {
                        body_end = j;
                        end = memchr(html + j, '>', length - j);
                        break;
                    }
                }

                /*
                 * A `<style>` is not prose, but it is not nothing either. Its
                 * contents go to the sheet in the order the page named them,
                 * because order is half of the cascade.
                 */
                if (!is_script && b.sheet != NULL && body_end > after) {
                    recon_css_add(b.sheet, html + after, body_end - after);
                }

                i = (end != NULL) ? (size_t)(end - html) + 1 : length;
                continue;
            }

            /*
             * A stylesheet the page asked for. Recorded rather than followed:
             * resolving an address against the page it was written in needs
             * to know where the page came from, and this does not.
             */
            if (!closing && named(tag, name_length, "link")) {
                char rel[64];
                char href[1024];
                if (attribute(attrs, attrs_length, "rel", rel, sizeof(rel)) &&
                        strcasecmp(rel, "stylesheet") == 0 &&
                        attribute(attrs, attrs_length, "href", href,
                            sizeof(href)) && href[0] != '\0' &&
                        d->sheet_count <
                            (int)(sizeof(d->sheets) / sizeof(d->sheets[0]))) {
                    snprintf(d->sheets[d->sheet_count],
                        sizeof(d->sheets[d->sheet_count]), "%s", href);
                    d->sheet_count++;
                }
                i = after;
                continue;
            }

            if (named(tag, name_length, "title")) {
                /*
                 * The first one wins, and the rest are ignored.
                 *
                 * `<title>` is not only the document's -- SVG uses it for the
                 * accessible name of a drawing, so an icon in the page can
                 * carry one. wikipedia.org has several, and collecting them
                 * all put "Wikipedia Close" on the window's title bar: the
                 * document's title, then the label on a close button inside
                 * an inline SVG.
                 *
                 * Which one is the document's cannot be told from the tag, so
                 * it is told from the order: a document's title is in its
                 * head, and the head comes first.
                 */
                in_title = !closing && title_used == 0;
                i = after;
                continue;
            }

            /* --- Blocks --- */
            if (named(tag, name_length, "p") ||
                    named(tag, name_length, "div") ||
                    named(tag, name_length, "section") ||
                    named(tag, name_length, "article") ||
                    named(tag, name_length, "header") ||
                    named(tag, name_length, "footer") ||
                    named(tag, name_length, "nav") ||
                    named(tag, name_length, "main") ||
                    named(tag, name_length, "tr") ||
                    named(tag, name_length, "table") ||
                    named(tag, name_length, "form") ||
                    named(tag, name_length, "figure")) {
                break_block(&b);
                i = after;
                continue;
            }

            if (named(tag, name_length, "br")) {
                break_block(&b);
                i = after;
                continue;
            }

            if (named(tag, name_length, "hr")) {
                add_rule(&b);
                i = after;
                continue;
            }

            /* A table cell is a word boundary, not a new paragraph -- a table
             * used for layout reads as a line, which is what it was. */
            if (named(tag, name_length, "td") ||
                    named(tag, name_length, "th")) {
                b.pending_space = true;
                i = after;
                continue;
            }

            if (name_length == 2 && (tag[0] == 'h' || tag[0] == 'H') &&
                    tag[1] >= '1' && tag[1] <= '6') {
                close_block(&b);
                if (!closing) {
                    open_block(&b, RECON_HTML_HEADING, tag[1] - '0');
                } else {
                    open_block(&b, RECON_HTML_PARAGRAPH, 0);
                }
                i = after;
                continue;
            }

            if (named(tag, name_length, "ul") ||
                    named(tag, name_length, "ol")) {
                close_block(&b);
                b.list_depth += closing ? -1 : 1;
                if (b.list_depth < 0) {
                    b.list_depth = 0;
                }
                open_block(&b, RECON_HTML_PARAGRAPH, 0);
                i = after;
                continue;
            }

            if (named(tag, name_length, "li")) {
                close_block(&b);
                open_block(&b, closing ? RECON_HTML_PARAGRAPH
                    : RECON_HTML_LIST_ITEM,
                    b.list_depth > 0 ? b.list_depth : 1);
                i = after;
                continue;
            }

            if (named(tag, name_length, "blockquote")) {
                close_block(&b);
                open_block(&b, closing ? RECON_HTML_PARAGRAPH
                    : RECON_HTML_QUOTE, 0);
                i = after;
                continue;
            }

            if (named(tag, name_length, "pre")) {
                close_block(&b);
                pre = !closing;
                open_block(&b, pre ? RECON_HTML_PRE : RECON_HTML_PARAGRAPH, 0);
                i = after;
                continue;
            }

            /*
             * --- An image ---
             *
             * A block of its own, carrying where the picture is and the alt
             * text as its runs. Whether the picture is fetched is the
             * viewer's decision; if it never arrives, what is left is the alt
             * text -- which is exactly what this used to draw, and is what alt
             * text is for.
             *
             * Its own block rather than something inline, because a picture
             * has a height and a line of text does not. That splits a
             * paragraph an image sits inside, which is what every renderer
             * without a real layout engine does, and reads as a picture
             * between two paragraphs rather than one lost inside a line.
             */
            if (named(tag, name_length, "img")) {
                char src[2048];
                bool have_src = attribute(attrs, attrs_length, "src", src,
                    sizeof(src)) && src[0] != '\0';

                char alt[256];
                bool have_alt = attribute(attrs, attrs_length, "alt", alt,
                    sizeof(alt)) && alt[0] != '\0';

                if (have_src || have_alt) {
                    unsigned was_style = b.style;
                    int was_link = b.link;

                    close_block(&b);
                    open_block(&b, RECON_HTML_IMAGE, 0);

                    if (have_src && d->link_count < LINKS_MAX) {
                        snprintf(d->links[d->link_count].href,
                            sizeof(d->links[d->link_count].href), "%s", src);
                        b.source = d->link_count++;
                    }

                    if (have_alt) {
                        b.style = RECON_HTML_ITALIC;
                        b.link = -1;
                        emit(&b, alt, strlen(alt));
                    }

                    close_block(&b);
                    b.style = was_style;
                    b.link = was_link;
                    open_block(&b, RECON_HTML_PARAGRAPH, 0);
                }
                i = after;
                continue;
            }

            /* --- Inline styles --- */
            unsigned add = 0;
            bool is_link = false;

            if (named(tag, name_length, "b") ||
                    named(tag, name_length, "strong")) {
                add = RECON_HTML_BOLD;
            } else if (named(tag, name_length, "i") ||
                    named(tag, name_length, "em")) {
                add = RECON_HTML_ITALIC;
            } else if (named(tag, name_length, "code") ||
                    named(tag, name_length, "tt") ||
                    named(tag, name_length, "kbd") ||
                    named(tag, name_length, "samp")) {
                add = RECON_HTML_MONO;
            } else if (named(tag, name_length, "a")) {
                add = RECON_HTML_LINK;
                is_link = true;
            }

            if (add == 0) {
                i = after;                    /* a tag nothing here cares about */
                continue;
            }

            if (!closing) {
                if (depth < NEST_MAX) {
                    struct open_tag *open = &stack[depth++];
                    open->style_added = add;
                    open->link_before = b.link;
                }
                b.style |= add;

                if (is_link) {
                    char href[2048];
                    if (attribute(attrs, attrs_length, "href", href,
                            sizeof(href)) && href[0] != '\0') {
                        if (d->link_count >= LINKS_MAX) {
                            /* The ceiling stands; the silence does not. A
                             * page whose links stop working two thousand in
                             * looks like a page with broken links. */
                            d->truncated = true;
                        } else {
                            snprintf(d->links[d->link_count].href,
                                sizeof(d->links[d->link_count].href), "%s",
                                href);
                            b.link = d->link_count++;
                        }
                    }
                }
            } else {
                /*
                 * Unwind to the nearest tag that turned this style on, rather
                 * than popping one.
                 *
                 * Pages close tags out of order constantly -- "<b><i>x</b></i>"
                 * is everywhere -- and popping blindly would leave bold on for
                 * the rest of the document. Searching down for the match and
                 * dropping what sits above it is what a browser does; the
                 * worst result is a style ending early rather than never.
                 */
                int found = -1;
                for (int j = depth - 1; j >= 0; j--) {
                    if (stack[j].style_added == add) {
                        found = j;
                        break;
                    }
                }
                if (found >= 0) {
                    /* Whatever link was in force before that tag opened. Read
                     * before the stack is unwound past it. */
                    b.link = stack[found].link_before;
                    depth = found;

                    b.style = 0;
                    for (int j = 0; j < depth; j++) {
                        b.style |= stack[j].style_added;
                    }
                }
            }

            i = after;
            continue;
        }

        /* --- Text --- */
        if (c == '&') {
            char utf8[4];
            int made = 0;
            size_t took = read_entity(html + i, length - i, utf8, &made);
            if (took > 0) {
                flush_space(&b);
                if (in_title) {
                    if (title_used + (size_t)made < sizeof(d->title) - 1) {
                        memcpy(d->title + title_used, utf8, (size_t)made);
                        title_used += (size_t)made;
                        d->title[title_used] = '\0';
                    }
                } else {
                    emit(&b, utf8, (size_t)made);
                }
                i += took;
                continue;
            }

            /*
             * Not an entity, so it is an ampersand somebody wrote. Emitted
             * here rather than left to the text loop below, which stops at
             * "&" and would skip over it without consuming it -- "tom & jerry"
             * came out as "tom  jerry".
             */
            if (in_title) {
                if (title_used + 1 < sizeof(d->title)) {
                    if (b.pending_space && title_used > 0) {
                        d->title[title_used++] = ' ';
                    }
                    d->title[title_used++] = '&';
                    d->title[title_used] = '\0';
                }
                b.pending_space = false;
            } else {
                flush_space(&b);
                emit(&b, "&", 1);
            }
            i++;
            continue;
        }

        /*
         * Inside <pre>, a newline is a line break rather than whitespace to
         * collapse. One block per line, which is what recon_html_plain does
         * for a text file and what the layout already draws correctly --
         * without this the newline stayed in the text and the whole of a
         * <pre> was drawn as one line thousands of pixels wide.
         */
        if (pre && (c == '\n' || c == '\r')) {
            if (c == '\n') {
                close_block(&b);
                open_block(&b, RECON_HTML_PRE, 0);
            }
            i++;
            continue;
        }

        if (!pre && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
            /*
             * Collapsed. HTML says a run of whitespace of any length and kind
             * is one space, and the space is remembered rather than written --
             * so a paragraph that begins with a newline does not begin with a
             * space, and trailing whitespace before a tag does not become a
             * space at the end of a line.
             */
            b.pending_space = true;
            i++;
            continue;
        }

        /* A run of ordinary characters, taken in one go. */
        size_t run = 0;
        while (i + run < length && html[i + run] != '<' &&
                html[i + run] != '&' &&
                html[i + run] != '\n' && html[i + run] != '\r' &&
                (pre || (html[i + run] != ' ' && html[i + run] != '\t'))) {
            run++;
        }
        if (run == 0) {
            i++;
            continue;
        }

        if (in_title) {
            size_t take = run;
            if (title_used + take >= sizeof(d->title)) {
                take = sizeof(d->title) - title_used - 1;
            }
            if (take > 0) {
                if (b.pending_space && title_used > 0) {
                    d->title[title_used++] = ' ';
                }
                memcpy(d->title + title_used, html + i, take);
                title_used += take;
                d->title[title_used] = '\0';
            }
            b.pending_space = false;
        } else {
            flush_space(&b);
            emit(&b, html + i, run);
        }
        i += run;
    }

    close_block(&b);
    return d;
}

struct recon_html_document *recon_html_plain(const char *text, size_t length) {
    struct recon_html_document *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return NULL;
    }

    d->text = calloc(1, TEXT_MAX);
    d->runs = calloc(RUNS_MAX, sizeof(*d->runs));
    d->blocks = calloc(BLOCKS_MAX, sizeof(*d->blocks));
    d->links = calloc(LINKS_MAX, sizeof(*d->links));

    if (d->text == NULL || d->runs == NULL || d->blocks == NULL ||
            d->links == NULL) {
        recon_html_free(d);
        return NULL;
    }
    if (text == NULL || length == 0) {
        return d;
    }

    /*
     * One block per line, preformatted.
     *
     * Not one block for the whole thing: a paragraph wraps, and wrapping a
     * text file destroys the one thing its author controlled. A line each
     * keeps the shape and lets the viewer scroll it.
     */
    size_t at = 0;
    /* The ceiling is deliberate; being quiet about reaching it was not. */
    while (at < length) {
        if (d->block_count >= BLOCKS_MAX) {
            d->truncated = true;
            break;
        }
        const char *newline = memchr(text + at, '\n', length - at);
        size_t line = (newline != NULL) ? (size_t)(newline - (text + at))
                                        : length - at;
        while (line > 0 && text[at + line - 1] == '\r') {
            line--;
        }

        if (d->text_used + line + 1 >= TEXT_MAX) {
            d->truncated = true;
            break;
        }

        struct recon_html_block_entry *block = &d->blocks[d->block_count++];
        block->kind = RECON_HTML_PRE;
        block->level = 0;
        block->first_run = d->run_count;
        block->run_count = 0;

        if (line > 0 && d->run_count >= RUNS_MAX) {
            d->truncated = true;
        } else if (line > 0) {
            size_t start = d->text_used;
            memcpy(d->text + start, text + at, line);
            d->text_used += line;
            d->text[d->text_used] = '\0';

            struct recon_html_run *run = &d->runs[d->run_count++];
            run->text = d->text + start;
            run->length = line;
            run->style = RECON_HTML_MONO;
            run->link = -1;
            block->run_count = 1;
        }

        at += (newline != NULL) ? line + 1 : line;
        if (newline != NULL && at <= (size_t)(newline - text)) {
            at = (size_t)(newline - text) + 1;
        }
    }

    return d;
}

void recon_html_free(struct recon_html_document *document) {
    if (document == NULL) {
        return;
    }
    free(document->text);
    free(document->runs);
    free(document->blocks);
    free(document->links);
    free(document);
}

const char *recon_html_title(const struct recon_html_document *document) {
    return document != NULL ? document->title : "";
}

bool recon_html_was_truncated(const struct recon_html_document *document) {
    return document != NULL && document->truncated;
}

int recon_html_block_count(const struct recon_html_document *document) {
    return document != NULL ? document->block_count : 0;
}

const struct recon_html_block_entry *recon_html_block_at(
        const struct recon_html_document *document, int index) {
    if (document == NULL || index < 0 || index >= document->block_count) {
        return NULL;
    }
    return &document->blocks[index];
}

const struct recon_html_run *recon_html_run_at(
        const struct recon_html_document *document, int index) {
    if (document == NULL || index < 0 || index >= document->run_count) {
        return NULL;
    }
    return &document->runs[index];
}

int recon_html_stylesheet_count(const struct recon_html_document *document) {
    return document != NULL ? document->sheet_count : 0;
}

const char *recon_html_stylesheet_at(const struct recon_html_document *document,
        int index) {
    if (document == NULL || index < 0 || index >= document->sheet_count) {
        return NULL;
    }
    return document->sheets[index];
}

const char *recon_html_link_at(const struct recon_html_document *document,
        int link) {
    if (document == NULL || link < 0 || link >= document->link_count) {
        return NULL;
    }
    return document->links[link].href;
}

bool recon_html_needs_scripting(const struct recon_html_document *document) {
    if (document == NULL || !document->saw_script) {
        return false;
    }
    /*
     * "Almost no text" rather than "no text", because a page that builds
     * itself usually still has a noscript line, a copyright, or a menu in it.
     * Two hundred characters is well under a screenful and well over a
     * boilerplate footer.
     */
    return document->text_used < 200;
}
