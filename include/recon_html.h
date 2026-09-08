/*
 * Reading HTML, as much of it as a viewer needs.
 *
 * This turns a page into a list of blocks, each holding a list of runs of
 * text. That is not a document tree and it is not trying to be: what a viewer
 * has to answer is "what words, in what order, in what shape, and which of
 * them are links", and a flat list answers all four.
 *
 * --- Where the line is ---
 *
 * **CSS, as far as it decides what is readable.** A stylesheet can hide an
 * element, colour it, weight it, slant it, size it and align it -- see
 * recon_css.h for exactly how much and why no more. Structure still decides
 * most of it: a heading is large because it is a heading, and a stylesheet
 * that only moves things about changes nothing here, because there is nothing
 * to move things about in.
 *
 * The one that matters is `display: none`. Most of what makes a real page
 * unreadable in a structural reader is not layout, it is the parts of the
 * page that were never meant to be seen at once.
 *
 * **No JavaScript.** A page that builds itself at run time arrives empty, and
 * says so rather than showing a blank window.
 *
 * **Images are named, not fetched.** An `<img>` becomes a block carrying the
 * address of the picture and its alt text. Whether the picture is ever asked
 * for is the viewer's business, not the parser's -- and a block that keeps its
 * alt text is one that degrades to exactly what it used to be when the picture
 * does not arrive, which is what alt text is for.
 *
 * **No tables or forms.** A table becomes its cells in order, which is wrong
 * for a spreadsheet and right for the tables that are really layout. A form is
 * shown and cannot be submitted -- a viewer that could submit one could change
 * something on somebody's server.
 *
 * Saying all that here rather than in a release note, because the gap between
 * "a viewer for simple pages" and "a browser" is a decade of work and the
 * difference should be legible from the header.
 */

#ifndef RECON_HTML_H
#define RECON_HTML_H

#include <stdbool.h>
#include <stddef.h>

#include "recon_css.h"

/* What kind of block this is, which is what decides how it is drawn. */
enum recon_html_block {
    RECON_HTML_PARAGRAPH,
    RECON_HTML_HEADING,        /* `level` says 1 to 6 */
    RECON_HTML_LIST_ITEM,      /* `level` says how deeply nested */
    RECON_HTML_PRE,            /* whitespace kept, drawn fixed-width */
    RECON_HTML_QUOTE,
    RECON_HTML_RULE,           /* a horizontal line; no runs */
    /*
     * A picture. `source` says where it is; the runs are its alt text, drawn
     * when the picture is not there -- which is the whole of how this
     * degrades to what it did before.
     */
    RECON_HTML_IMAGE,
};

/* How a run of text is drawn, as flags because they combine. */
enum recon_html_style {
    RECON_HTML_PLAIN = 0,
    RECON_HTML_BOLD = 1 << 0,
    RECON_HTML_ITALIC = 1 << 1,
    RECON_HTML_MONO = 1 << 2,
    RECON_HTML_LINK = 1 << 3,
};

/*
 * One stretch of text with one appearance.
 *
 * `text` points into the document's own storage and is not NUL-terminated on
 * its own -- `length` is the truth. Splitting the text into separate strings
 * would mean an allocation per word on a page with a lot of markup.
 */
struct recon_html_run {
    const char *text;
    size_t length;
    unsigned style;
    /* Which link this run belongs to, or -1. An index rather than a pointer,
     * so the array can grow without leaving anything dangling. */
    int link;

    /*
     * What a stylesheet said to write this in, as 0xRRGGBB. Unset means the
     * skin decides, which is what should happen for the overwhelming majority
     * of text -- a reader that took every page's colours would be a reader
     * whose dark skin is undone by the first page written for a light one.
     */
    bool has_colour;
    unsigned colour;
};

struct recon_html_link {
    char href[2048];
};

struct recon_html_block_entry {
    enum recon_html_block kind;
    int level;
    int first_run;
    int run_count;

    /*
     * For an image, where the picture is -- as an index into the same table
     * the links use, and -1 for every other kind of block.
     *
     * The same table because the two are the same thing: an address written
     * in a page, resolved against the page it was written in. A second table
     * of strings differing only in which attribute they came from would be a
     * second set of bounds to get right.
     */
    int source;

    /* What a stylesheet said about the block as a whole. 0 means unset, and
     * then the kind decides -- which is what happens on every page that has
     * no stylesheet, and on most blocks of every page that has one. */
    int size_percent;

    /*
     * Read and recorded; not yet drawn. Centring a line means knowing its
     * width before placing the first word on it, which is a second pass the
     * flow does not make -- so this is here, honest and unused, rather than
     * absent and then discovered to be missing by whoever adds the pass.
     */
    int align;                 /* enum recon_css_align, 0 unset */
};

struct recon_html_document;

/*
 * Read a page.
 *
 * `html` is borrowed for the duration of the call and may be freed afterwards;
 * everything the document needs is copied. NULL only when there is no memory,
 * because there is no such thing as HTML this refuses -- a page of nonsense
 * parses to a document with no blocks in it, and a viewer showing "there is
 * nothing here" is more use than one showing an error about markup.
 */
/*
 * A page's bytes as UTF-8, when they are not already.
 *
 * Returns a NUL-terminated copy the caller owns, or NULL when `bytes` is
 * already usable and should be read as it stands.
 *
 * --- How it decides ---
 *
 * By looking, not by being told. If the bytes are valid UTF-8 they are UTF-8:
 * that is not a guess, because the encoding is self-checking -- a sequence of
 * bytes that decodes cleanly as UTF-8 is essentially never anything else. And
 * if they are not, they are read as Windows-1252, which is what every browser
 * does with a page that declares Latin-1 and is what the remaining
 * single-byte web actually is.
 *
 * `declared` -- from the Content-Type header -- is used only to skip the
 * check when it says UTF-8, which is most of the time. It is deliberately not
 * trusted the other way: a page that *says* Latin-1 and is really UTF-8 is
 * common enough that believing the label would break pages that currently
 * work.
 */
char *recon_html_to_utf8(const char *bytes, size_t length,
    const char *declared, size_t *out_length);

struct recon_html_document *recon_html_parse(const char *html, size_t length);

/*
 * The same, with stylesheets already in hand.
 *
 * `sheet` is added to as the page's own `<style>` elements are met, in the
 * order the page names them -- so a caller fetches the `<link>` sheets from a
 * first pass, puts them in a sheet in that order, and parses again. Order is
 * half of the cascade, which is why this takes a sheet rather than a list of
 * texts: the caller already knows the order and this does not.
 *
 * NULL `sheet` is exactly recon_html_parse.
 */
struct recon_html_document *recon_html_parse_styled(const char *html,
    size_t length, struct recon_css_sheet *sheet);

/*
 * The stylesheets a page asked for, so a caller can fetch them.
 *
 * `<link rel=stylesheet href=...>` only, unresolved -- resolving an address
 * against the page it was written in is the viewer's job, and this does not
 * know where the page came from.
 */
int recon_html_stylesheet_count(const struct recon_html_document *document);
const char *recon_html_stylesheet_at(const struct recon_html_document *document,
    int index);
void recon_html_free(struct recon_html_document *document);

/* Everything between <title> and </title>, or "" for a page with none. */
const char *recon_html_title(const struct recon_html_document *document);

/*
 * What the page paints behind everything, if it said so.
 *
 * False when it did not, which is most pages of the old web and no page of
 * the new one. A viewer that gets false should keep its own paper rather than
 * assume white: the paper belongs to whoever is reading, until the page has
 * an opinion.
 */
bool recon_html_page_background(const struct recon_html_document *document,
    unsigned *colour_out);

int recon_html_block_count(const struct recon_html_document *document);

/*
 * Whether the page was bigger than this reader will hold.
 *
 * The ceilings in recon_html.c are deliberate -- a page is somebody else's
 * file and can be any size, and a reader that grows to fit whatever it is
 * handed is one a hostile page can exhaust. Truncating is the right answer.
 *
 * Being quiet about it is not. A page cut off at four thousand blocks looks
 * exactly like a page that ended, and somebody reading it has no way to know
 * the rest is missing -- the same failure the help had, where the change log
 * stopped a third of the way through and said nothing.
 */
bool recon_html_was_truncated(const struct recon_html_document *document);
const struct recon_html_block_entry *recon_html_block_at(
    const struct recon_html_document *document, int index);

/*
 * How many runs and how many links the document has.
 *
 * A caller walking the runs of one block knows where they end, because the
 * block says. A caller walking the document does not, and had no way to ask:
 * `recon_html_run_at` returns NULL past the end, so the only way to find the
 * end was to run off it and be told. That is fine for a loop and useless for
 * a bounds check, which is what wanted them -- an index into these tables is
 * exactly the kind of thing that is wrong by one and reads whatever follows
 * the array.
 */
/*
 * Which block a named place is in, or -1.
 *
 * `name` is what followed the "#" in a link, without it. This is what makes
 * "#install" go somewhere: an id is not a thing to draw, it is a place to
 * arrive at, and a block is the finest thing a viewer can scroll to.
 */
int recon_html_anchor_block(const struct recon_html_document *document,
    const char *name);
int recon_html_anchor_count(const struct recon_html_document *document);

int recon_html_run_count(const struct recon_html_document *document);
int recon_html_link_count(const struct recon_html_document *document);

const struct recon_html_run *recon_html_run_at(
    const struct recon_html_document *document, int index);

/* The address a link run points at, or NULL. */
const char *recon_html_link_at(const struct recon_html_document *document,
    int link);

/*
 * True when the page had a <script> in it and almost no text.
 *
 * The signature of a page that builds itself at run time. Worth telling apart
 * from an empty page, because "this page needs JavaScript, which this does
 * not have" is an explanation and a blank window is not.
 */
bool recon_html_needs_scripting(const struct recon_html_document *document);

/*
 * Read a page that is not HTML at all: plain text.
 *
 * Same document shape, one preformatted block. text/plain is a large fraction
 * of what is worth reading and it would be strange to fetch it and refuse it.
 */
struct recon_html_document *recon_html_plain(const char *text, size_t length);

#endif /* RECON_HTML_H */
