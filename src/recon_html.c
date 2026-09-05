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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

    struct recon_html_link *links;
    int link_count;

    char title[256];
    bool saw_script;
};

/* --- Building --- */

struct builder {
    struct recon_html_document *d;

    /* The block being filled in, if any. */
    bool in_block;
    enum recon_html_block kind;
    int level;
    int first_run;

    /* Open styles, so </b> closes the nearest <b> and not something else. */
    unsigned style;
    int link;
    int list_depth;

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
    if (length == 0 || !b->in_block) {
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
    if (last != NULL && last->style == b->style && last->link == b->link &&
            last->text + last->length == b->d->text + at) {
        last->length += length;
        return;
    }

    if (b->d->run_count >= RUNS_MAX) {
        return;
    }
    struct recon_html_run *run = &b->d->runs[b->d->run_count++];
    run->text = b->d->text + at;
    run->length = length;
    run->style = b->style;
    run->link = b->link;
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
    bool keep_empty = (b->kind == RECON_HTML_PRE);
    if ((count <= 0 && !keep_empty) || b->d->block_count >= BLOCKS_MAX) {
        b->d->run_count = b->first_run;
        return;
    }

    struct recon_html_block_entry *block = &b->d->blocks[b->d->block_count++];
    block->kind = b->kind;
    block->level = b->level;
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
    if (b->d->block_count < BLOCKS_MAX) {
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

struct recon_html_document *recon_html_parse(const char *html, size_t length) {
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
                for (size_t j = after; j + 8 < length; j++) {
                    if (strncasecmp(html + j, want, strlen(want)) == 0) {
                        end = memchr(html + j, '>', length - j);
                        break;
                    }
                }
                i = (end != NULL) ? (size_t)(end - html) + 1 : length;
                continue;
            }

            if (named(tag, name_length, "title")) {
                in_title = !closing;
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

            /* An image becomes its alt text, which is what alt text is for. */
            if (named(tag, name_length, "img")) {
                char alt[256];
                if (attribute(attrs, attrs_length, "alt", alt, sizeof(alt)) &&
                        alt[0] != '\0') {
                    flush_space(&b);
                    unsigned was = b.style;
                    b.style |= RECON_HTML_ITALIC;
                    emit(&b, "[", 1);
                    emit(&b, alt, strlen(alt));
                    emit(&b, "]", 1);
                    b.style = was;
                    b.pending_space = true;
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
                            sizeof(href)) && href[0] != '\0' &&
                            d->link_count < LINKS_MAX) {
                        snprintf(d->links[d->link_count].href,
                            sizeof(d->links[d->link_count].href), "%s", href);
                        b.link = d->link_count++;
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
    while (at < length && d->block_count < BLOCKS_MAX) {
        const char *newline = memchr(text + at, '\n', length - at);
        size_t line = (newline != NULL) ? (size_t)(newline - (text + at))
                                        : length - at;
        while (line > 0 && text[at + line - 1] == '\r') {
            line--;
        }

        if (d->text_used + line + 1 >= TEXT_MAX) {
            break;
        }

        struct recon_html_block_entry *block = &d->blocks[d->block_count++];
        block->kind = RECON_HTML_PRE;
        block->level = 0;
        block->first_run = d->run_count;
        block->run_count = 0;

        if (line > 0 && d->run_count < RUNS_MAX) {
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
